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

	void Close();

private:
	static int ProcessCallback(jack_nframes_t nframes, void* arg);
	int Process(jack_nframes_t nframes);

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
