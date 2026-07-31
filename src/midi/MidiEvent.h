#pragma once

#include <cstddef>
#include <cstdint>

namespace sf
{

// The message types a foot controller realistically sends to switch
// presets or drive a parameter. Anything else is reported as Other and
// ignored by the mapping layer.
enum class MidiEventType
{
	ProgramChange,
	ControlChange,
	NoteOn,
	NoteOff,
	Other
};

struct MidiEvent
{
	MidiEventType type = MidiEventType::Other;

	// 0-15 as on the wire; the UI/config layer is where 1-16 display
	// numbering belongs, if we ever want it.
	int channel = 0;

	// Program number, controller number, or note number depending on type.
	int number = 0;

	// Controller value or note velocity. Always 0 for Program Change.
	int value = 0;
};

const char* ToString(MidiEventType type);

// Parses one raw MIDI message. Returns false for anything malformed,
// truncated, or not one of the types above (system messages, clock, ...).
//
// A Note On with velocity 0 is reported as NoteOff, per the MIDI spec's
// running-status convention -- controllers commonly send it that way, and
// treating it as a press would double-trigger every footswitch.
bool ParseMidiEvent(const uint8_t* data, size_t size, MidiEvent& out);

}  // namespace sf
