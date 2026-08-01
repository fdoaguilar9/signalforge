#include "JackEngine.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace sf
{

JackEngine::~JackEngine()
{
	Close();
}

bool JackEngine::Open(const std::string& client_name)
{
	jack_status_t status;
	client_ = jack_client_open(client_name.c_str(), JackNoStartServer, &status);

	if (!client_)
	{
		std::fprintf(stderr, "Failed to open JACK client (status 0x%x).\n", status);
		return false;
	}

	return true;
}

double JackEngine::SampleRate() const
{
	// A closed engine (SF-38: between tearing the old one down and a new one
	// coming up during a live audio-settings restart) has no client to ask.
	// Zero rather than a crash -- the same "never the reason a board won't
	// play" posture as Settings.
	return client_ ? static_cast<double>(jack_get_sample_rate(client_)) : 0.0;
}

uint32_t JackEngine::MaxBlockSize() const
{
	return client_ ? jack_get_buffer_size(client_) : 0;
}

namespace
{

// Case-insensitive substring search, so "airstep" matches the port JACK
// actually calls "Midi-Bridge:AIRSTEP MIDI 1 (capture)".
bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
	if (needle.empty())
	{
		return true;
	}

	const auto equal_ignoring_case = [](char a, char b)
	{ return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };

	return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), equal_ignoring_case) !=
		haystack.end();
}

// The ALSA sequencer loopback. It's flagged physical just like a real
// controller, but it's a pipe between applications -- connecting to it by
// default would pick up any MIDI another app happens to route there.
bool IsLoopbackPort(const std::string& port_name)
{
	return ContainsNoCase(port_name, "midi through") || ContainsNoCase(port_name, "midi_through");
}

// Raises `target` to `value` if `value` is larger, and leaves it alone
// otherwise. Called on the audio thread: the loop is uncontended in practice
// (one writer, and a reader that only ever swaps in zero) and never blocks,
// which a mutex around a meter would.
void RaisePeak(std::atomic<float>& target, float value)
{
	float previous = target.load(std::memory_order_relaxed);

	while (value > previous && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed))
	{
		// compare_exchange_weak refreshed `previous`; retry unless it has
		// meanwhile risen above what we were going to store.
	}
}

// Largest magnitude in a block, and whether anything in it reached full
// scale. Full scale is the clip test rather than "> 1.0" because a sample
// sitting exactly at 1.0 is already the top of the range: by the time it is
// past it, the converter has been square-waving for a while.
float BlockPeak(const float* samples, jack_nframes_t frames, bool& clipped)
{
	float peak = 0.0f;

	for (jack_nframes_t i = 0; i < frames; ++i)
	{
		const float magnitude = std::fabs(samples[i]);

		if (magnitude > peak)
		{
			peak = magnitude;
		}
	}

	if (peak >= 1.0f)
	{
		clipped = true;
	}

	return peak;
}

// Counts a host's boundary audio ports, so a swap can check the new host
// matches the JACK port layout we already registered.
void CountAudioPorts(const AudioHost& host, size_t& inputs, size_t& outputs)
{
	inputs = 0;
	outputs = 0;

	for (const PortInfo& port : host.Ports())
	{
		if (port.type != PortType::Audio)
		{
			continue;
		}

		if (port.direction == PortDirection::Input)
		{
			++inputs;
		}
		else
		{
			++outputs;
		}
	}
}

}  // namespace

bool JackEngine::Start(AudioHost* host)
{
	host_.store(host, std::memory_order_release);

	unsigned in_count = 0;
	unsigned out_count = 0;

	for (const PortInfo& port : host->Ports())
	{
		if (port.type != PortType::Audio)
		{
			continue;
		}

		char name[32];

		if (port.direction == PortDirection::Input)
		{
			std::snprintf(name, sizeof(name), "in_%u", ++in_count);
			jack_port_t* jack_port = jack_port_register(client_, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
			inputs_.push_back({port.index, jack_port});
		}
		else
		{
			std::snprintf(name, sizeof(name), "out_%u", ++out_count);
			jack_port_t* jack_port = jack_port_register(client_, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
			outputs_.push_back({port.index, jack_port});
		}
	}

	midi_in_ = jack_port_register(client_, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

	if (!midi_in_)
	{
		std::fprintf(stderr, "Warning: could not register a MIDI input port; MIDI control is unavailable.\n");
	}

	if (outputs_.size() == 1)
	{
		jack_port_t* mirror_port = jack_port_register(client_, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		mirrors_.push_back({0, mirror_port});
	}

	jack_set_process_callback(client_, &JackEngine::ProcessCallback, this);

	// So a live buffer-size change (SetBufferSize) resizes the crossfade
	// scratch instead of overflowing it on the next fade.
	jack_set_buffer_size_callback(client_, &JackEngine::BufferSizeCallback, this);

	if (jack_activate(client_) != 0)
	{
		std::fprintf(stderr, "Failed to activate JACK client.\n");
		return false;
	}

	// The fade's per-sample step, now that the sample rate is known, and the
	// scratch buffers the two hosts render into during one. Allocated here so
	// the audio thread never has to.
	crossfade_.Configure(SampleRate(), crossfade_.Milliseconds(), crossfade_.Curve());

	const uint32_t block_size = MaxBlockSize();
	outgoing_scratch_.assign(outputs_.size(), std::vector<float>(block_size, 0.0f));
	incoming_scratch_.assign(outputs_.size(), std::vector<float>(block_size, 0.0f));

	processing_.store(true, std::memory_order_release);

	return true;
}

std::vector<std::string> JackEngine::ConnectMidiInputs(const std::string& device_filter)
{
	std::vector<std::string> connected;

	if (!midi_in_)
	{
		return connected;
	}

	const char* our_port = jack_port_name(midi_in_);

	// Ask for MIDI *sources*: a controller's port is an output as far as the
	// graph is concerned, since data flows out of it and into us.
	const char** ports = jack_get_ports(client_, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);

	if (!ports)
	{
		std::fprintf(stderr, "Warning: no MIDI sources found; connect a controller manually if you need one.\n");
		return connected;
	}

	unsigned skipped_software = 0;
	unsigned skipped_loopback = 0;

	for (size_t i = 0; ports[i]; ++i)
	{
		const std::string name = ports[i];

		if (name == our_port)
		{
			continue;
		}

		if (!device_filter.empty())
		{
			// An explicit name is a deliberate choice, so it bypasses the
			// physical/loopback filtering entirely.
			if (!ContainsNoCase(name, device_filter))
			{
				continue;
			}
		}
		else
		{
			jack_port_t* port = jack_port_by_name(client_, name.c_str());

			if (!port || (jack_port_flags(port) & JackPortIsPhysical) == 0)
			{
				++skipped_software;
				continue;
			}

			if (IsLoopbackPort(name))
			{
				++skipped_loopback;
				continue;
			}
		}

		const int result = jack_connect(client_, name.c_str(), our_port);

		// EEXIST just means someone already patched it -- that's the state we
		// wanted, so report it as connected.
		if (result == 0 || result == EEXIST)
		{
			connected.push_back(name);
		}
		else
		{
			std::fprintf(stderr, "Warning: could not connect MIDI source '%s' (error %d).\n", name.c_str(), result);
		}
	}

	jack_free(ports);

	for (const std::string& name : connected)
	{
		std::printf("MIDI input connected: %s\n", name.c_str());
	}

	if (connected.empty())
	{
		if (!device_filter.empty())
		{
			std::fprintf(stderr, "Warning: no MIDI source matched '%s'; MIDI control is inactive.\n",
				device_filter.c_str());
		}
		else
		{
			std::fprintf(stderr,
				"Warning: no hardware MIDI controller found (skipped %u software and %u loopback port(s)); "
				"MIDI control is inactive.\n",
				skipped_software, skipped_loopback);
		}
	}

	return connected;
}

std::vector<std::string> JackEngine::ConnectAudioPorts(const std::string& input_filter,
	const std::string& output_filter)
{
	std::vector<std::string> connected;

	const auto find_physical = [this](unsigned long jack_direction_flag, const std::string& filter)
	{
		std::vector<std::string> found;

		const char** ports = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE, jack_direction_flag);

		if (!ports)
		{
			return found;
		}

		for (size_t i = 0; ports[i]; ++i)
		{
			const std::string name = ports[i];

			if (!filter.empty())
			{
				if (ContainsNoCase(name, filter))
				{
					found.push_back(name);
				}

				continue;
			}

			jack_port_t* port = jack_port_by_name(client_, name.c_str());

			if (port && (jack_port_flags(port) & JackPortIsPhysical) != 0)
			{
				found.push_back(name);
			}
		}

		jack_free(ports);
		std::sort(found.begin(), found.end());

		return found;
	};

	// Hardware capture ports are JACK *outputs* (data flows out of the
	// interface into the graph); they feed our in_N ports.
	const std::vector<std::string> capture_ports = find_physical(JackPortIsOutput, input_filter);

	for (size_t i = 0; i < inputs_.size() && i < capture_ports.size(); ++i)
	{
		const char* our_name = jack_port_name(inputs_[i].jack_port);
		const int result = jack_connect(client_, capture_ports[i].c_str(), our_name);

		if (result == 0 || result == EEXIST)
		{
			connected.push_back(capture_ports[i] + " -> " + our_name);
		}
		else
		{
			std::fprintf(stderr, "Warning: could not connect audio input '%s' (error %d).\n",
				capture_ports[i].c_str(), result);
		}
	}

	// Hardware playback ports are JACK *inputs*; our out_N ports (including
	// the mono->stereo mirror) feed them.
	const std::vector<std::string> playback_ports = find_physical(JackPortIsInput, output_filter);

	std::vector<jack_port_t*> our_outputs;

	for (const AudioPortBinding& binding : outputs_)
	{
		our_outputs.push_back(binding.jack_port);
	}

	for (const MirrorBinding& mirror : mirrors_)
	{
		our_outputs.push_back(mirror.jack_port);
	}

	for (size_t i = 0; i < our_outputs.size() && i < playback_ports.size(); ++i)
	{
		const char* our_name = jack_port_name(our_outputs[i]);
		const int result = jack_connect(client_, our_name, playback_ports[i].c_str());

		if (result == 0 || result == EEXIST)
		{
			connected.push_back(std::string(our_name) + " -> " + playback_ports[i]);
		}
		else
		{
			std::fprintf(stderr, "Warning: could not connect audio output '%s' (error %d).\n", our_name, result);
		}
	}

	for (const std::string& pair : connected)
	{
		std::printf("Audio connected: %s\n", pair.c_str());
	}

	if (connected.empty())
	{
		std::fprintf(stderr, "Warning: no physical audio ports matched; connect them manually if you need them.\n");
	}

	return connected;
}

int JackEngine::BufferSizeCallback(jack_nframes_t nframes, void* arg)
{
	static_cast<JackEngine*>(arg)->ResizeScratch(nframes);

	return 0;
}

void JackEngine::ResizeScratch(uint32_t frames)
{
	// JACK guarantees processing is stopped around this callback, which is
	// the only reason allocating here is allowed at all.
	for (std::vector<float>& buffer : outgoing_scratch_)
	{
		buffer.assign(frames, 0.0f);
	}

	for (std::vector<float>& buffer : incoming_scratch_)
	{
		buffer.assign(frames, 0.0f);
	}
}

bool JackEngine::SetBufferSize(uint32_t frames)
{
	if (!client_)
	{
		return false;
	}

	return jack_set_buffer_size(client_, frames) == 0;
}

double JackEngine::RoundTripLatencyMs() const
{
	const double rate = SampleRate();

	if (!client_ || rate <= 0.0)
	{
		return 0.0;
	}

	jack_nframes_t total = 0;

	// What JACK reports already accounts for the hardware's own buffering
	// and for anything patched in front of or behind us, which is why this
	// is asked rather than computed from the period.
	if (!inputs_.empty())
	{
		jack_latency_range_t range{};
		jack_port_get_latency_range(inputs_[0].jack_port, JackCaptureLatency, &range);
		total += range.max;
	}

	if (!outputs_.empty())
	{
		jack_latency_range_t range{};
		jack_port_get_latency_range(outputs_[0].jack_port, JackPlaybackLatency, &range);
		total += range.max;
	}

	// Unconnected ports report zero, which would read as "no latency at all".
	// One period is the floor a client cannot beat, so say that instead.
	if (total == 0)
	{
		total = jack_get_buffer_size(client_);
	}

	return 1000.0 * static_cast<double>(total) / rate;
}

void JackEngine::DisconnectAudioPorts()
{
	if (!client_)
	{
		return;
	}

	// jack_port_disconnect drops *all* connections on the given port, which
	// is exactly the intent here: whatever this port was patched to, it
	// isn't any more, and ConnectAudioPorts decides what it should be now.
	for (const AudioPortBinding& binding : inputs_)
	{
		jack_port_disconnect(client_, binding.jack_port);
	}

	for (const AudioPortBinding& binding : outputs_)
	{
		jack_port_disconnect(client_, binding.jack_port);
	}

	for (const MirrorBinding& mirror : mirrors_)
	{
		jack_port_disconnect(client_, mirror.jack_port);
	}
}

void JackEngine::SetCrossfade(double milliseconds, CrossfadeCurve curve)
{
	// The sample rate may not be known yet (Open() hasn't been called, or this
	// is a plain configuration step before Start()); Start() reconfigures with
	// the real rate, so recording the intent here is enough.
	crossfade_.Configure(client_ ? SampleRate() : 0.0, milliseconds, curve);
}

bool JackEngine::SwitchHost(AudioHost* next)
{
	// Check compatibility first: a rejected switch shouldn't disturb audio.
	size_t next_inputs = 0;
	size_t next_outputs = 0;
	CountAudioPorts(*next, next_inputs, next_outputs);

	if (next_inputs != inputs_.size() || next_outputs != outputs_.size())
	{
		std::fprintf(stderr,
			"Cannot switch: new chain is %zu in / %zu out, but the JACK ports are %zu in / %zu out.\n", next_inputs,
			next_outputs, inputs_.size(), outputs_.size());

		return false;
	}

	if (!processing_.load(std::memory_order_acquire) || !host_.load(std::memory_order_acquire))
	{
		// Nothing is being heard, so there's nothing to smooth over.
		host_.store(next, std::memory_order_release);
		return true;
	}

	crossfade_.Restart();
	incoming_.store(next, std::memory_order_release);

	if (!WaitForCrossfade(500))
	{
		// The audio thread isn't advancing; fall back to a plain swap rather
		// than leaving a crossfade half-published.
		std::fprintf(stderr, "Warning: timed out waiting for the crossfade; switching directly.\n");

		incoming_.store(nullptr, std::memory_order_release);
		host_.store(next, std::memory_order_release);
	}

	return true;
}

bool JackEngine::WaitForCrossfade(unsigned timeout_ms)
{
	for (unsigned elapsed = 0; elapsed < timeout_ms; ++elapsed)
	{
		if (incoming_.load(std::memory_order_acquire) == nullptr)
		{
			return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return incoming_.load(std::memory_order_acquire) == nullptr;
}

bool JackEngine::SwapHost(AudioHost* next)
{
	size_t next_inputs = 0;
	size_t next_outputs = 0;
	CountAudioPorts(*next, next_inputs, next_outputs);

	if (next_inputs != inputs_.size() || next_outputs != outputs_.size())
	{
		std::fprintf(stderr,
			"Cannot switch: new chain is %zu in / %zu out, but the JACK ports are %zu in / %zu out.\n", next_inputs,
			next_outputs, inputs_.size(), outputs_.size());

		return false;
	}

	host_.store(next, std::memory_order_release);

	return true;
}

bool JackEngine::PopMidiEvent(MidiEvent& out)
{
	const size_t head = midi_head_.load(std::memory_order_relaxed);

	if (head == midi_tail_.load(std::memory_order_acquire))
	{
		return false;
	}

	out = midi_queue_[head];
	midi_head_.store((head + 1) % kMidiQueueCapacity, std::memory_order_release);

	return true;
}

void JackEngine::WaitForProcessCycles(unsigned cycles)
{
	if (!processing_.load(std::memory_order_acquire))
	{
		return;
	}

	const uint64_t target = process_count_.load(std::memory_order_acquire) + cycles;

	// Bounded: if the audio thread has stopped calling us (server gone,
	// graph stalled) we'd otherwise wait forever.
	for (int attempt = 0; attempt < 1000; ++attempt)
	{
		if (process_count_.load(std::memory_order_acquire) >= target)
		{
			return;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::fprintf(stderr, "Warning: audio thread did not advance while retiring a chain.\n");
}

void JackEngine::Close()
{
	if (client_)
	{
		jack_client_close(client_);
		client_ = nullptr;
	}

	processing_.store(false, std::memory_order_release);

	inputs_.clear();
	outputs_.clear();
	mirrors_.clear();
	outgoing_scratch_.clear();
	incoming_scratch_.clear();
	midi_in_ = nullptr;
	host_.store(nullptr, std::memory_order_release);
	incoming_.store(nullptr, std::memory_order_release);
}

int JackEngine::ProcessCallback(jack_nframes_t nframes, void* arg)
{
	return static_cast<JackEngine*>(arg)->Process(nframes);
}

int JackEngine::Process(jack_nframes_t nframes)
{
	// Parse incoming MIDI into the queue. Done first so an event and the
	// audio of the cycle it arrived in stay in step; no logging or
	// allocation happens here.
	if (midi_in_)
	{
		void* midi_buffer = jack_port_get_buffer(midi_in_, nframes);
		const jack_nframes_t event_count = jack_midi_get_event_count(midi_buffer);

		for (jack_nframes_t i = 0; i < event_count; ++i)
		{
			jack_midi_event_t raw;

			if (jack_midi_event_get(&raw, midi_buffer, i) != 0)
			{
				continue;
			}

			MidiEvent event;

			if (!ParseMidiEvent(raw.buffer, raw.size, event))
			{
				continue;
			}

			const size_t tail = midi_tail_.load(std::memory_order_relaxed);
			const size_t next = (tail + 1) % kMidiQueueCapacity;

			if (next == midi_head_.load(std::memory_order_acquire))
			{
				// Full: drop rather than block or overwrite mid-read.
				midi_dropped_.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			midi_queue_[tail] = event;
			midi_tail_.store(next, std::memory_order_release);
		}
	}

	// One acquire load per cycle: whichever host this sees, it uses for the
	// whole callback. A swap published mid-cycle simply takes effect next
	// time round.
	AudioHost* host = host_.load(std::memory_order_acquire);

	if (!host)
	{
		for (const AudioPortBinding& binding : outputs_)
		{
			float* buffer = static_cast<float*>(jack_port_get_buffer(binding.jack_port, nframes));
			std::memset(buffer, 0, nframes * sizeof(float));
		}

		process_count_.fetch_add(1, std::memory_order_release);

		return 0;
	}

	AudioHost* incoming = incoming_.load(std::memory_order_acquire);

	// Both hosts read the same input buffers; they only read, so sharing is
	// fine.
	float input_peak = 0.0f;

	for (const AudioPortBinding& binding : inputs_)
	{
		float* buffer = static_cast<float*>(jack_port_get_buffer(binding.jack_port, nframes));
		host->ConnectAudioPort(binding.plugin_port_index, buffer);

		if (incoming)
		{
			incoming->ConnectAudioPort(binding.plugin_port_index, buffer);
		}

		// Measured before the chain runs, so this is what arrived from the
		// interface -- the number that says whether the *guitar* is too hot,
		// independently of what the plugins then do with it.
		bool ignored = false;
		input_peak = std::max(input_peak, BlockPeak(buffer, nframes, ignored));
	}

	RaisePeak(input_peak_, input_peak);

	if (!incoming)
	{
		// Steady state: render straight into the JACK buffers.
		for (const AudioPortBinding& binding : outputs_)
		{
			float* buffer = static_cast<float*>(jack_port_get_buffer(binding.jack_port, nframes));
			host->ConnectAudioPort(binding.plugin_port_index, buffer);
		}

		host->PrepareAtomPorts();
		host->Run(nframes);
	}
	else
	{
		// Crossfading: each host renders into its own scratch buffer, then
		// the two are mixed with equal-power gains.
		for (size_t k = 0; k < outputs_.size(); ++k)
		{
			host->ConnectAudioPort(outputs_[k].plugin_port_index, outgoing_scratch_[k].data());
			incoming->ConnectAudioPort(outputs_[k].plugin_port_index, incoming_scratch_[k].data());
		}

		host->PrepareAtomPorts();
		host->Run(nframes);

		incoming->PrepareAtomPorts();
		incoming->Run(nframes);

		// Every channel walks the same ramp from the same start -- MixChannel
		// doesn't advance the fade -- so the channels stay phase-coherent.
		for (size_t k = 0; k < outputs_.size(); ++k)
		{
			float* out = static_cast<float*>(jack_port_get_buffer(outputs_[k].jack_port, nframes));

			crossfade_.MixChannel(out, outgoing_scratch_[k].data(), incoming_scratch_[k].data(), nframes);
		}

		crossfade_.Advance(nframes);

		if (crossfade_.Done())
		{
			// Fade complete: the incoming host becomes the current one. The
			// control thread sees incoming_ go null and can then retire the
			// old chain (after WaitForProcessCycles).
			host_.store(incoming, std::memory_order_release);
			incoming_.store(nullptr, std::memory_order_release);
		}
	}

	for (const MirrorBinding& mirror : mirrors_)
	{
		const float* source =
			static_cast<const float*>(jack_port_get_buffer(outputs_[mirror.source_output_index].jack_port, nframes));
		float* dest = static_cast<float*>(jack_port_get_buffer(mirror.jack_port, nframes));

		std::memcpy(dest, source, nframes * sizeof(float));
	}

	// Measured last, on the buffers actually going to the interface: after
	// the chain, and after a crossfade mix, since a switch is exactly when
	// two chains summing can push the output into clipping.
	float output_peak = 0.0f;
	bool clipped = false;

	for (const AudioPortBinding& binding : outputs_)
	{
		const float* buffer = static_cast<const float*>(jack_port_get_buffer(binding.jack_port, nframes));
		output_peak = std::max(output_peak, BlockPeak(buffer, nframes, clipped));
	}

	RaisePeak(output_peak_, output_peak);

	if (clipped)
	{
		output_clips_.fetch_add(1, std::memory_order_relaxed);
	}

	// Release-ordered so a control thread that sees this count knows the
	// work above (including any use of the previous host) has finished.
	process_count_.fetch_add(1, std::memory_order_release);

	return 0;
}

}  // namespace sf
