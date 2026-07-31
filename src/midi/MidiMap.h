#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "MidiEvent.h"

namespace sf
{

// What an incoming MIDI message does. PLAN.md §5.2.
enum class MidiAction
{
	LoadPreset,   // switch to a named preset
	NextPreset,   // step forward in the preset library
	PrevPreset,   // step back
	ToggleBypass, // flip one chain stage in/out
	SetControl    // drive a control port continuously
};

const char* ToString(MidiAction action);
bool ParseMidiAction(const std::string& text, MidiAction& out);

// A binding whose "channel" is omitted in JSON listens on all 16. Foot
// controllers ship on whatever channel they ship on, and users routinely
// don't know which -- so the forgiving reading is the default, and naming a
// channel explicitly is what narrows it.
inline constexpr int kAnyChannel = -1;

// A CC switch is "pressed" at or above this value. Momentary footswitches
// send 127 down / 0 up, latching ones toggle between 0 and 127; either way
// the halfway point separates the two states.
inline constexpr int kSwitchThreshold = 64;

struct MidiBinding
{
	// The matcher. NoteOff is never a binding type: a note-driven switch
	// binds as note_on, and the release is handled internally (see MidiMap).
	MidiEventType type = MidiEventType::ControlChange;
	int channel = kAnyChannel;
	int number = 0;

	MidiAction action = MidiAction::NextPreset;

	// Which of these matter depends on `action`:
	std::string preset;  // LoadPreset: name, index, or file stem
	int stage = 0;       // ToggleBypass, SetControl: chain stage index
	std::string port;    // SetControl: control port symbol

	// SetControl only: the range MIDI 0..127 is scaled into. Defaults to the
	// unit interval; a real port range comes from the plugin at wiring time
	// (SF-22) when these are left unset.
	float minimum = 0.0f;
	float maximum = 1.0f;

	// True if `event` targets this binding at all. Says nothing about
	// whether it should *fire* -- that's MidiMap::Resolve, which also has to
	// consider press/release state.
	bool Matches(const MidiEvent& event) const;

	bool operator==(const MidiBinding& other) const;

	nlohmann::json ToJson() const;

	// Returns false with a human-readable reason rather than throwing: a
	// hand-edited mapping file is expected to have mistakes in it, and the
	// user needs to be told which entry and why.
	static bool FromJson(const nlohmann::json& json, MidiBinding& out, std::string& error);
};

// One binding that fired, with the value already interpreted.
struct MidiActionInvocation
{
	const MidiBinding* binding = nullptr;
	MidiAction action = MidiAction::NextPreset;

	// SetControl only: `binding->minimum..maximum` scaled from MIDI 0..127.
	float control_value = 0.0f;
};

// The global (not per-preset) mapping table, persisted alongside presets.
//
// Resolve() is deliberately stateful. A momentary footswitch sends two CC
// messages per stomp -- 127 down, 0 up -- and firing on both would advance
// the preset twice per press. So the map tracks which switch bindings are
// currently held and fires only on the press edge.
class MidiMap
{
public:
	bool Load(const std::string& path, std::string& error);
	bool Save(const std::string& path, std::string& error) const;

	nlohmann::json ToJson() const;
	static bool FromJson(const nlohmann::json& json, MidiMap& out, std::string& error);

	// Appends a binding. Returns false if an existing binding already claims
	// the same type/channel/number, since which one wins would be arbitrary.
	bool Add(const MidiBinding& binding, std::string& error);

	// Adds a binding, replacing any existing one that claims the same
	// message. This is what MIDI-learn wants: re-learning a switch should
	// move it, not fail as a duplicate. Returns how many were replaced.
	size_t Bind(const MidiBinding& binding);

	const std::vector<MidiBinding>& Bindings() const { return bindings_; }
	size_t Size() const { return bindings_.size(); }

	// Interprets one incoming event. Returns true and fills `out` when a
	// binding should fire now; false when the event is unmapped, or is the
	// release half of a switch press, or repeats a press already held.
	//
	// Control thread only -- it mutates the held-switch state, and is not
	// safe to call from the audio thread.
	bool Resolve(const MidiEvent& event, MidiActionInvocation& out);

private:
	std::vector<MidiBinding> bindings_;

	// Parallel to bindings_: is this switch currently held down? Unused for
	// SetControl (continuous) and ProgramChange (inherently momentary).
	std::vector<bool> held_;
};

}  // namespace sf
