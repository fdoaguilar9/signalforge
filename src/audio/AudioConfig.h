#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sf
{

// One ALSA hardware card, as found in /proc/asound/cards. `id` is what a
// driver argument to jackd/ALSA actually wants ("hw:1"), `name` is what a
// human picks from in a list ("AIRSTEP").
struct AudioDevice
{
	std::string id;
	std::string name;
	unsigned card_index = 0;
};

// Reads /proc/asound/cards. Returns an empty list (not an error) on a
// machine with no sound cards, or where the file doesn't exist -- both are
// normal, not exceptional.
std::vector<AudioDevice> EnumerateAlsaDevices();

// True if a JACK server is already reachable. Side-effect-free: opens a
// throwaway client with JackNoStartServer (so this never launches one) and
// closes it immediately.
bool IsJackServerRunning();

// One individual *physical* JACK audio port, as the running server actually
// names it.
//
// Deliberately one entry per port rather than per device: the chain is mono
// in this version, so which of an interface's inputs the guitar is actually
// plugged into is precisely the choice being made. A Scarlett Solo offering
// "2 inputs" as a single row would hide the only thing worth picking.
struct PhysicalPort
{
	std::string name;    // full JACK name, "client:port" -- what gets matched
	std::string client;  // the part before the colon
	std::string port;    // the part after it, which is what distinguishes channels

	// True for a hardware source (a capture port -- feeds the board). JACK
	// calls these *output* ports, since data flows out of them into the
	// graph; the naming here follows the guitarist's meaning, not JACK's.
	bool is_capture = false;
};

// Live enumeration (SF-39): every physical audio port in the currently
// reachable server's graph, sorted by client then port name. Requires a
// reachable server; returns empty (not an error) if there isn't one -- same
// probe-only posture as IsJackServerRunning, and this never starts one
// either.
std::vector<PhysicalPort> EnumeratePhysicalPorts();

// What SignalForge needs to know to either connect to an existing JACK
// server (device/rate/buffer are then informational only -- the server
// already decided them) or start its own.
//
// Deliberately just ALSA-via-JACK for now (SF-34/35 scope). A native
// ALSA-without-JACK backend is a later milestone.
struct AudioConfig
{
	// "alsa": SignalForge starts its own jackd with this device/rate/buffer
	// if none is already running (JackServer, SF-35).
	// "external": don't manage a server at all -- one was already found
	// running at configuration time, and its device/rate/buffer are the
	// server's business, not ours.
	std::string driver = "alsa";

	// An AudioDevice::id, e.g. "hw:1". Empty lets jackd pick the ALSA
	// default.
	std::string device;

	double sample_rate = 48000.0;
	uint32_t buffer_size = 256;

	// SF-39: which physical ports to auto-connect to, as a case-insensitive
	// substring match on the JACK port name -- same convention as
	// JackEngine::ConnectMidiInputs's device_filter. Empty means "every
	// physical port of the right direction", same default behaviour as MIDI.
	std::string input_ports;
	std::string output_ports;

	// Whether this config has actually been chosen (by a user, or by
	// discovering an already-running server) rather than being untouched
	// defaults. Drives SF-36's first-run gate: a config that has never been
	// configured triggers the config step; one that has, however plain,
	// does not.
	bool configured = false;

	// Sample rate: any of the common rates, clamped to the nearest if a
	// hand-edited file has something odd. Buffer size: clamped to a
	// power-of-two-ish range jackd/ALSA can actually run at.
	void Clamp();

	nlohmann::json ToJson() const;
	static AudioConfig FromJson(const nlohmann::json& json);
};

}  // namespace sf
