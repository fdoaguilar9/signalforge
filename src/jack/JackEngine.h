#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "../audio/Crossfade.h"
#include "../lv2/AudioHost.h"
#include "../midi/MidiEvent.h"

namespace sf
{

// Wires an AudioHost's boundary audio ports directly to JACK ports of the
// same count/order. Works the same whether the host is a single PluginStage
// or a multi-stage Chain.
//
// One adaptation is applied automatically: if the host's audio *output* is
// mono (exactly one output port), a second JACK output port is registered
// and fed a copy of the same signal, so a mono amp+IR chain still gives a
// normal stereo pair to connect in a DAW. Anything else (a host with 2+
// outputs already, or mismatched input counts) is passed through as-is --
// this is deliberately just the one final-output special case, not a
// general N-to-M channel mixer.
class JackEngine
{
public:
	~JackEngine();

	// Opens a JACK client named `client_name`. Returns false (with a
	// diagnostic on stderr) on failure.
	bool Open(const std::string& client_name);

	double SampleRate() const;
	uint32_t MaxBlockSize() const;

	// Registers one JACK audio port per boundary audio port on `host` (in
	// the same order Ports() reported them) and starts processing. `host` is
	// not owned and must outlive the engine (or until Close()).
	bool Start(AudioHost* host);

	// Crossfades from the current host to `next`: both run for the length of
	// the fade and are mixed with equal-power (cos/sin) gains, so the
	// transition is continuous instead of dipping through silence.
	//
	// Blocks until the fade completes. The port layout is validated first,
	// so a rejected switch costs nothing audibly.
	//
	// As with SwapHost(), the previous host isn't freed here -- follow with
	// WaitForProcessCycles() before destroying it.
	//
	// Note both chains process audio during the fade, so CPU load briefly
	// roughly doubles.
	bool SwitchHost(AudioHost* next);

	// How long a SwitchHost() crossfade lasts, and the shape of its ramp.
	// Call before Start(): the length is turned into a per-sample step there,
	// once the sample rate is known. Zero milliseconds means switch at the
	// next block boundary.
	void SetCrossfade(double milliseconds, CrossfadeCurve curve);

	double CrossfadeMilliseconds() const { return crossfade_.Milliseconds(); }
	CrossfadeCurve CrossfadeShape() const { return crossfade_.Curve(); }

	// Atomically publishes a different host for the audio thread to run.
	// Call from the control thread only, with a host that is already fully
	// built and activated -- the audio thread picks it up on its very next
	// cycle, so anything slow (instantiating plugins, loading models) must
	// already be done.
	//
	// Returns false, changing nothing, if `next` doesn't have the same audio
	// port layout as the host passed to Start(): JACK ports are registered
	// once and can't be reshuffled underneath a running graph.
	//
	// The previous host is *not* freed here -- the audio thread may still be
	// mid-cycle with it. Use WaitForProcessCycles() before destroying it.
	bool SwapHost(AudioHost* next);

	// Blocks until the audio thread has completed `cycles` more process
	// callbacks, so a host retired by SwapHost() is provably no longer in
	// use. Returns immediately if the engine isn't processing.
	//
	// Control thread only; this blocks.
	void WaitForProcessCycles(unsigned cycles);

	// Total process callbacks completed so far. Sampling this around a slow
	// control-thread operation shows whether audio kept flowing during it.
	uint64_t ProcessCycleCount() const { return process_count_.load(std::memory_order_acquire); }

	// Loudest sample magnitude seen since the previous call, and reset to
	// zero by it -- "peak since you last looked", which is what a meter
	// wants. Control thread only.
	//
	// Read-and-reset rather than a running value because the audio thread
	// runs far faster than the UI redraws: a plain snapshot would show one
	// arbitrary block out of every six and miss the transient that matters.
	float TakeInputPeak() { return input_peak_.exchange(0.0f, std::memory_order_relaxed); }
	float TakeOutputPeak() { return output_peak_.exchange(0.0f, std::memory_order_relaxed); }

	// Monotonic count of blocks in which at least one output sample reached
	// full scale. A count rather than a flag so the reader can tell "clipping
	// right now" from "clipped a while ago" by watching it change, without
	// the engine having to know anything about how long a warning should
	// stay on screen.
	uint64_t OutputClipCount() const { return output_clips_.load(std::memory_order_relaxed); }

	// Round-trip latency in milliseconds, as JACK reports it: hardware in to
	// our input port, plus our output port to hardware out. Falls back to one
	// period when the ports aren't connected to anything and JACK therefore
	// reports zero -- that's the floor no client can beat, not a claim the
	// real figure is that low.
	double RoundTripLatencyMs() const;

	// Asks the server to change its period size, live. Legal on a running
	// JACK server and it affects every client on it, ours included -- there
	// is no per-client buffer size.
	//
	// **The caller must make sure the running chain can already take the new
	// size before calling this**: the change takes effect on the audio thread
	// as soon as the server agrees, while the old chain is still processing,
	// and an LV2 plugin handed more frames than it was instantiated for
	// writes past the end of its buffers. Build (and swap in) a chain whose
	// maximum block covers both sizes first.
	bool SetBufferSize(uint32_t frames);

	// Pops one MIDI event received since the last call, if any. Control
	// thread only -- the audio thread just parses and enqueues, since it
	// can't log or allocate.
	//
	// Returns false when the queue is empty. If more events arrive than the
	// queue holds before the control thread drains it, the oldest are
	// dropped and DroppedMidiEvents() counts them.
	bool PopMidiEvent(MidiEvent& out);

	uint64_t DroppedMidiEvents() const { return midi_dropped_.load(std::memory_order_acquire); }

	// Connects hardware MIDI controllers to our midi_in port so a footswitch
	// works the moment the app starts, with no manual patching.
	//
	// With an empty `device_filter`, every *physical* MIDI source is connected
	// except the ALSA "Midi Through" loopback -- that one is always present,
	// is never a controller, and would echo back anything else routed through
	// it. Software ports (other apps' MIDI outputs) are left alone: they're
	// almost never what a guitarist wants driving their pedalboard, and
	// grabbing them all would be surprising.
	//
	// A non-empty `device_filter` is a case-insensitive substring match on the
	// port name, and it overrides *both* rules -- if you name it, you get it,
	// virtual ports included, since that's the escape hatch for odd setups and
	// for testing without hardware.
	//
	// Returns the port names actually connected. Call after Start(); the
	// engine must be activated for connections to hold.
	std::vector<std::string> ConnectMidiInputs(const std::string& device_filter);

	// Connects our registered audio ports to physical hardware ports (SF-39):
	// our in_N ports to hardware capture ports, our out_N ports to hardware
	// playback ports (the mono->stereo mirror port included). Matched by
	// position after sorting by name, bounded by however many of each exist
	// on either side -- a two-in pedalboard on a four-in interface gets the
	// first two capture ports, not all four.
	//
	// An empty filter matches every physical port of the right direction,
	// same convention as ConnectMidiInputs. A non-empty one is a
	// case-insensitive substring match on the port name and is the only way
	// to reach a non-physical (software) port.
	//
	// Returns the port name pairs actually connected, as "ours -> theirs"
	// strings, for logging. Call after Start().
	std::vector<std::string> ConnectAudioPorts(const std::string& input_filter, const std::string& output_filter);

	// Drops every connection on our audio ports, leaving the engine running
	// and the ports registered. Paired with ConnectAudioPorts to re-point the
	// board at different hardware *without* restarting the engine or the
	// server -- changing which device feeds the board doesn't need either,
	// and doing it this way costs no audio gap beyond the reconnection
	// itself.
	void DisconnectAudioPorts();

	void Close();

private:
	static int ProcessCallback(jack_nframes_t nframes, void* arg);
	int Process(jack_nframes_t nframes);

	// JACK calls this with processing stopped, so it may allocate -- which is
	// the only reason the crossfade scratch buffers can be resized at all.
	static int BufferSizeCallback(jack_nframes_t nframes, void* arg);
	void ResizeScratch(uint32_t frames);

	// Peak magnitude since the control thread last read it, written by the
	// audio thread. Relaxed throughout: a meter that is one block stale is
	// not wrong in any way a player could see, and ordering it against the
	// audio would cost more than it's worth.
	std::atomic<float> input_peak_{0.0f};
	std::atomic<float> output_peak_{0.0f};
	std::atomic<uint64_t> output_clips_{0};

	jack_client_t* client_ = nullptr;

	// Read by the audio thread every cycle, written by the control thread on
	// a swap. Not owned.
	std::atomic<AudioHost*> host_{nullptr};

	// Bumped at the end of every process callback, so the control thread can
	// tell when a retired host is definitely no longer referenced.
	std::atomic<uint64_t> process_count_{0};

	std::atomic<bool> processing_{false};

	// The gain ramp a switch runs down. Both hosts render during the fade,
	// into their own scratch buffers, and are mixed by this -- so the
	// transition is continuous rather than passing through silence. Length and
	// curve are configuration (SF-33), set before Start().
	Crossfade crossfade_;

	// Published by the control thread to start a crossfade; cleared by the
	// audio thread once the fade completes (at which point it has become
	// host_).
	std::atomic<AudioHost*> incoming_{nullptr};


	// Per-output scratch buffers so the two hosts can run without writing
	// over each other. Allocated in Start(), never on the audio thread.
	std::vector<std::vector<float>> outgoing_scratch_;
	std::vector<std::vector<float>> incoming_scratch_;

	// Blocks (control thread) until a published crossfade has completed.
	// Returns false on timeout.
	bool WaitForCrossfade(unsigned timeout_ms);

	// MIDI input. The audio thread parses incoming messages and writes them
	// into a lock-free SPSC ring; the control thread drains it. Sized far
	// larger than a foot controller could ever fill between drains.
	static constexpr size_t kMidiQueueCapacity = 256;

	jack_port_t* midi_in_ = nullptr;
	MidiEvent midi_queue_[kMidiQueueCapacity];
	std::atomic<size_t> midi_head_{0};  // consumer: control thread
	std::atomic<size_t> midi_tail_{0};  // producer: audio thread
	std::atomic<uint64_t> midi_dropped_{0};

	struct AudioPortBinding
	{
		uint32_t plugin_port_index;
		jack_port_t* jack_port;
	};

	std::vector<AudioPortBinding> inputs_;
	std::vector<AudioPortBinding> outputs_;

	// A mono->stereo duplicate: mirrors outputs_[source_output_index]'s
	// buffer into an extra JACK port after Run(), rather than connecting the
	// plugin port to it directly (a plugin port can only be connected to one
	// buffer per block).
	struct MirrorBinding
	{
		size_t source_output_index;
		jack_port_t* jack_port;
	};

	std::vector<MirrorBinding> mirrors_;
};

}  // namespace sf
