#include "JackEngine.h"

#include "audio/Gain.h"

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

// The one refusal both host swaps can hit, worded in a single place so the
// player and the console are told the same thing.
bool PortLayoutMatches(const AudioHost& next, size_t inputs, size_t outputs, std::string* reason)
{
	size_t next_inputs = 0;
	size_t next_outputs = 0;
	CountAudioPorts(next, next_inputs, next_outputs);

	if (next_inputs == inputs && next_outputs == outputs)
	{
		return true;
	}

	char message[160];
	std::snprintf(message, sizeof(message),
		"Cannot switch: new chain is %zu in / %zu out, but the JACK ports are %zu in / %zu out.", next_inputs,
		next_outputs, inputs, outputs);

	std::fprintf(stderr, "%s\n", message);

	if (reason)
	{
		*reason = message;
	}

	return false;
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
	input_scratch_.assign(inputs_.size(), std::vector<float>(block_size, 0.0f));

	// The tuner's two windows, allocated here with everything else the audio
	// thread will touch -- it can allocate nothing itself.
	tap_buffer_[0].assign(kInputTapSamples, 0.0f);
	tap_buffer_[1].assign(kInputTapSamples, 0.0f);
	tap_fill_ = 0;
	tap_writing_ = 0;
	tap_ready_.store(-1, std::memory_order_relaxed);

	// The gate's coefficients are all times, so they need the sample rate --
	// which is first known here, and again after a restart at a different one.
	// Reset keeps the settings and clears only the runtime state.
	gate_.Reset(SampleRate());

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

	// Hardware playback ports are JACK *inputs*; our out_N ports feed them.
	const std::vector<std::string> playback_ports = find_physical(JackPortIsInput, output_filter);

	for (size_t i = 0; i < outputs_.size() && i < playback_ports.size(); ++i)
	{
		const char* our_name = jack_port_name(outputs_[i].jack_port);
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

	for (std::vector<float>& buffer : input_scratch_)
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
}

void JackEngine::SetCrossfade(double milliseconds, CrossfadeCurve curve)
{
	// The sample rate may not be known yet (Open() hasn't been called, or this
	// is a plain configuration step before Start()); Start() reconfigures with
	// the real rate, so recording the intent here is enough.
	crossfade_.Configure(client_ ? SampleRate() : 0.0, milliseconds, curve);
}

bool JackEngine::SwitchHost(AudioHost* next, std::string* reason)
{
	// Check compatibility first: a rejected switch shouldn't disturb audio.
	if (!PortLayoutMatches(*next, inputs_.size(), outputs_.size(), reason))
	{
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

bool JackEngine::SwapHost(AudioHost* next, std::string* reason)
{
	if (!PortLayoutMatches(*next, inputs_.size(), outputs_.size(), reason))
	{
		return false;
	}

	host_.store(next, std::memory_order_release);

	return true;
}

bool JackEngine::TakeInputWindow(float* out)
{
	const int ready = tap_ready_.load(std::memory_order_acquire);

	if (ready < 0 || !out)
	{
		return false;
	}

	const std::vector<float>& from = tap_buffer_[static_cast<size_t>(ready)];

	for (size_t i = 0; i < from.size(); ++i)
	{
		out[i] = from[i];
	}

	// Released only after the copy: this is what tells the audio thread the
	// buffer is free again, and saying so early is saying it while the copy is
	// still reading from it.
	tap_ready_.store(-1, std::memory_order_release);

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

void JackEngine::SetInputGainDb(float db)
{
	input_gain_db_.store(db, std::memory_order_relaxed);
	input_gain_.store(DbToLinear(db), std::memory_order_relaxed);
}

void JackEngine::SetOutputGainDb(float db)
{
	output_gain_db_.store(db, std::memory_order_relaxed);
	output_gain_.store(DbToLinear(db), std::memory_order_relaxed);
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
	// When this cycle started, for the DSP figure (SF-65). jack_get_time is the
	// server's own monotonic microsecond clock: it is documented as callable
	// from the process callback, it does not allocate, and it is what JACK
	// measures itself with -- so this figure and jackd's own agree about what
	// a microsecond is.
	const jack_time_t started_us = jack_get_time();

	AudioHost* host = host_.load(std::memory_order_acquire);

	if (!host)
	{
		for (const AudioPortBinding& binding : outputs_)
		{
			float* buffer = static_cast<float*>(jack_port_get_buffer(binding.jack_port, nframes));
			std::memset(buffer, 0, nframes * sizeof(float));
		}

		// Counted like any other cycle. An idle board reading 0% is the useful
		// baseline; skipping these would leave the last real figure standing
		// after the chain went away.
		RecordDspLoad(started_us, nframes);

		process_count_.fetch_add(1, std::memory_order_release);

		return 0;
	}

	AudioHost* incoming = incoming_.load(std::memory_order_acquire);

	// Both hosts read the same input buffers; they only read, so sharing is
	// fine.
	float input_peak = 0.0f;

	// The input trim, ramped once for the whole block and then applied per
	// channel, so a stereo pair walks the same ramp and stays coherent.
	const float input_gain_target = input_gain_.load(std::memory_order_relaxed);

	for (size_t k = 0; k < inputs_.size(); ++k)
	{
		const AudioPortBinding& binding = inputs_[k];

		const float* incoming_samples =
			static_cast<const float*>(jack_port_get_buffer(binding.jack_port, nframes));

		// Trimmed into scratch rather than in place: the port buffer belongs
		// to the server and is ours to read only.
		float* buffer = input_scratch_[k].data();

		RampGain(buffer, incoming_samples, nframes, input_gain_applied_, input_gain_target);

		host->ConnectAudioPort(binding.plugin_port_index, buffer);

		if (incoming)
		{
			incoming->ConnectAudioPort(binding.plugin_port_index, buffer);
		}

		// Measured *after* the trim, so the meter shows what the chain is
		// actually being fed. That is the reading the trim is set against --
		// a meter that didn't move as you dragged the slider beside it would
		// be useless for the one job it has here.
		bool ignored = false;
		input_peak = std::max(input_peak, BlockPeak(buffer, nframes, ignored));
	}

	input_gain_applied_ = input_gain_target;

	RaisePeak(input_peak_, input_peak);

	// The tuner's tap (SF-81). Channel 0 only: a tuner listens to the guitar,
	// and summing a stereo input would fold two strings' worth of signal into
	// one window for no gain.
	//
	// Filling stops the moment a window is complete and does not start again
	// until the reader takes it. A tuner that overwrote the window it was
	// being read from would hand over half of one and half of the next, and
	// that is not noise -- it is a confident wrong note.
	if (tap_enabled_.load(std::memory_order_relaxed) && !inputs_.empty() &&
		tap_ready_.load(std::memory_order_acquire) < 0)
	{
		std::vector<float>& into = tap_buffer_[tap_writing_];
		const float* source = input_scratch_[0].data();

		const uint32_t room = static_cast<uint32_t>(into.size()) - tap_fill_;
		const uint32_t take = std::min(room, nframes);

		for (uint32_t i = 0; i < take; ++i)
		{
			into[tap_fill_ + i] = source[i];
		}

		tap_fill_ += take;

		if (tap_fill_ >= into.size())
		{
			tap_ready_.store(static_cast<int>(tap_writing_), std::memory_order_release);

			tap_writing_ = 1 - tap_writing_;
			tap_fill_ = 0;
		}
	}

	// The gate (SF-85), in place on the buffers the chain is about to read.
	//
	// **Here, and not in the loop above, for two reasons that are invisible
	// once it works.** The input meter was measured before this point, so it
	// keeps showing the *ungated* signal -- which matters because the meter is
	// what the threshold is set against, and one that read silence exactly
	// when you needed to see what you were cutting would be useless. And the
	// tuner tapped before this point, so tuning is never gated: a quietly
	// plucked string is precisely the signal a gate removes, and a tuner that
	// went blank when the gate came up would be blamed on the tuner.
	//
	// No restructuring is needed to sit here because ConnectAudioPort above
	// handed over a *pointer* to input_scratch_. Both hosts read these same
	// buffers when they run, so one gate upstream serves a crossfade rather
	// than two independent ones fighting inside it.
	if (!input_scratch_.empty())
	{
		float* gated[2] = {nullptr, nullptr};
		const unsigned count = static_cast<unsigned>(std::min<size_t>(input_scratch_.size(), 2));

		for (unsigned k = 0; k < count; ++k)
		{
			gated[k] = input_scratch_[k].data();
		}

		gate_.Process(gated, count, nframes);
	}

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

	// The output trim, before the meter reads -- so the meter and the clip
	// light describe what actually leaves the machine.
	//
	// In place, unlike the input's: these buffers are ours to write.
	const float output_gain_target = output_gain_.load(std::memory_order_relaxed);

	for (const AudioPortBinding& binding : outputs_)
	{
		float* buffer = static_cast<float*>(jack_port_get_buffer(binding.jack_port, nframes));

		RampGain(buffer, buffer, nframes, output_gain_applied_, output_gain_target);
	}

	output_gain_applied_ = output_gain_target;

	// The click, **after the chain and after the trim** (SF-82). The board's
	// gain staging therefore cannot change how loud it is -- a metronome that
	// got quieter when you turned the amp down would be one more thing to
	// fight mid-song -- and it can never reach the tuner, which listens to the
	// input.
	if (metronome_)
	{
		float* left = outputs_.empty()
			? nullptr
			: static_cast<float*>(jack_port_get_buffer(outputs_[0].jack_port, nframes));

		float* right = outputs_.size() > 1
			? static_cast<float*>(jack_port_get_buffer(outputs_[1].jack_port, nframes))
			: nullptr;

		metronome_->Mix(left, right, nframes);
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

	RecordDspLoad(started_us, nframes);

	// Release-ordered so a control thread that sees this count knows the
	// work above (including any use of the previous host) has finished.
	process_count_.fetch_add(1, std::memory_order_release);

	return 0;
}

// How much of this block's time budget the callback just used, folded into a
// running average (SF-65).
//
// Audio thread. Two clock reads, some arithmetic and a relaxed store -- no
// allocation, no lock, no syscall beyond jack_get_time's clock read.
void JackEngine::RecordDspLoad(uint64_t started_us, uint32_t nframes)
{
	const double rate = jack_get_sample_rate(client_);

	if (rate <= 0.0 || nframes == 0)
	{
		return;
	}

	// What the block was worth in wall-clock time. This is the budget: spend
	// longer than this and the next block is late.
	const double budget_us = (static_cast<double>(nframes) / rate) * 1e6;
	const double spent_us = static_cast<double>(jack_get_time() - started_us);

	const float instant = static_cast<float>(spent_us / budget_us);

	// A one-pole average. Without it the reading is unreadable: block times
	// scatter by a factor of two from scheduling alone, and a gauge that
	// flickers between 20% and 60% tells a player nothing. The coefficient is
	// per block rather than per second, which means the smoothing is longer in
	// wall-clock terms at a small buffer size -- correct, since that is also
	// where the scatter is worst.
	constexpr float kSmoothing = 0.05f;

	const float previous = dsp_load_.load(std::memory_order_relaxed);
	const float smoothed = previous + (instant - previous) * kSmoothing;

	dsp_load_.store(std::isfinite(smoothed) ? smoothed : 0.0f, std::memory_order_relaxed);
}

}  // namespace sf
