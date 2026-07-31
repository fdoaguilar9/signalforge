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
	return static_cast<double>(jack_get_sample_rate(client_));
}

uint32_t JackEngine::MaxBlockSize() const
{
	return jack_get_buffer_size(client_);
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
	for (const AudioPortBinding& binding : inputs_)
	{
		float* buffer = static_cast<float*>(jack_port_get_buffer(binding.jack_port, nframes));
		host->ConnectAudioPort(binding.plugin_port_index, buffer);

		if (incoming)
		{
			incoming->ConnectAudioPort(binding.plugin_port_index, buffer);
		}
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

	for (const MirrorBinding& mirror : mirrors_)
	{
		const float* source =
			static_cast<const float*>(jack_port_get_buffer(outputs_[mirror.source_output_index].jack_port, nframes));
		float* dest = static_cast<float*>(jack_port_get_buffer(mirror.jack_port, nframes));

		std::memcpy(dest, source, nframes * sizeof(float));
	}

	// Release-ordered so a control thread that sees this count knows the
	// work above (including any use of the previous host) has finished.
	process_count_.fetch_add(1, std::memory_order_release);

	return 0;
}

}  // namespace sf
