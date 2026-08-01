#pragma once

#include <cstdint>
#include <string>
#include <vector>

// For WidgetProbe, the same testing affordance the board view uses -- one
// definition rather than a near-identical second one.
#include "BoardView.h"

namespace sf
{

// One selectable device, as the dialog needs it. Not sf::AudioDevice: the
// view doesn't know about ALSA, only about what it's given to list. `filter`
// is a guess at the substring that will identify this device's ports in the
// running JACK graph -- picking a row fills the editable port filter field
// with it, but it's a starting point, not a promise, since the actual port
// names are the server's (or PipeWire's) to decide.
struct AudioSettingsDevice
{
	std::string id;
	std::string name;
	std::string filter;
};

// One individual physical port currently visible in a *reachable* JACK graph
// -- live, not guessed. Not sf::PhysicalPort, same reason AudioSettingsDevice
// isn't sf::AudioDevice: the view doesn't touch JACK directly, only what it's
// handed to list.
//
// One row per port, not per device: the chain is mono, so which of an
// interface's inputs the guitar is in is exactly the choice being made.
struct AudioSettingsJackPort
{
	std::string name;    // full "client:port" -- what gets saved and matched
	std::string client;  // shown as context
	std::string port;    // shown prominently: this is what tells channels apart
	bool is_capture = false;
};

// Everything DrawAudioSettings reads and writes, gathered by the caller.
// Same pattern as BoardViewState: a snapshot in, requests out, so the
// drawing code never touches AudioConfig, JackServer or Settings directly.
struct AudioSettingsViewState
{
	// True while a JACK server is already reachable -- in which case rate and
	// buffer size are that server's own settings, not ours to choose, and
	// those two fields are hidden. Which physical ports to use is still ours
	// to pick either way: that's a client-side connection, independent of who
	// owns the server.
	bool server_running = false;

	// First run: no Cancel button, since there is nothing yet to cancel back
	// to. Menu-invoked (SF-38): Cancel is offered and leaves the running
	// config untouched.
	bool first_run = false;

	// Borrowed; the caller enumerates once, not per frame.
	const std::vector<AudioSettingsDevice>* devices = nullptr;

	// Live JACK port enumeration, borrowed the same way. Only populated when
	// a server is actually reachable to ask (empty otherwise, e.g. during
	// SF-36's first-run step, before anything exists to enumerate) -- when
	// it is, this is what the port filter should actually be picked from:
	// real port names, not a guess derived from an ALSA card's description.
	const std::vector<AudioSettingsJackPort>* jack_ports = nullptr;

	// -1 selects the ALSA default rather than a specific card. Only used when
	// *we* end up spawning the server (see JackServer) -- meaningless when
	// attaching to one that's already running, where the two filters below
	// are what actually decide which hardware gets used.
	int selected_device = -1;

	// Which hardware feeds the board, and which hardware the board feeds.
	// **Two separate fields, deliberately**: the guitar comes in one
	// interface and the sound very often goes out somewhere else entirely
	// (monitors on the same interface, but just as often headphones on the
	// motherboard codec). One shared filter cannot express that, and forcing
	// input and output to be the same device is wrong on most desks.
	//
	// Each holds a full JACK port name picked from the dropdown (which
	// matches exactly that one port), or empty for "any physical port of
	// that direction" -- only a sensible default when one interface is
	// present. Matching is the case-insensitive substring rule
	// ConnectMidiInputs already uses, so a hand-edited settings file can
	// still hold a looser pattern; the dialog just always writes an exact
	// name, since that's the unambiguous thing to write.
	std::string input_filter;
	std::string output_filter;

	double sample_rate = 48000.0;
	uint32_t buffer_size = 256;

	// One line of feedback from the last apply attempt (e.g. why starting
	// jackd failed), or empty.
	std::string status;

	// Set by the view when the user asks to use this configuration. The
	// caller performs the (possibly slow) restart and is free to refuse,
	// leaving `status` to say why on the next frame.
	bool apply_requested = false;

	// Set when Cancel is clicked. Never set during a first run, since
	// first_run hides the button.
	bool cancel_requested = false;

	// Testing aid, off by default -- same contract as BoardViewState's. The
	// port dropdowns only draw their rows while open, so a test has to
	// actually open one to reach that code, which means clicking a widget it
	// can locate by name rather than by coordinate.
	bool record_widgets = false;
	std::vector<WidgetProbe> widgets;
};

// Draws the dialog as its own floating window. Must be called between
// AppWindow::BeginFrame and EndFrame, either alone (SF-36's first-run step,
// before a board exists to draw) or alongside DrawBoard (SF-38, opened from
// the menu, floating over the running board).
void DrawAudioSettings(AudioSettingsViewState& state);

}  // namespace sf
