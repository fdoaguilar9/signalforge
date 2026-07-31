#include "MidiEvent.h"

namespace sf
{

const char* ToString(MidiEventType type)
{
	switch (type)
	{
		case MidiEventType::ProgramChange: return "program_change";
		case MidiEventType::ControlChange: return "cc";
		case MidiEventType::NoteOn: return "note_on";
		case MidiEventType::NoteOff: return "note_off";
		case MidiEventType::Other: return "other";
	}

	return "other";
}

bool ParseMidiEvent(const uint8_t* data, size_t size, MidiEvent& out)
{
	if (size < 2)
	{
		return false;
	}

	const uint8_t status = data[0];

	// Not a status byte at all (we don't track running status: JACK MIDI
	// delivers complete messages).
	if ((status & 0x80) == 0)
	{
		return false;
	}

	const uint8_t kind = status & 0xf0;

	out.channel = status & 0x0f;
	out.value = 0;

	switch (kind)
	{
		case 0xc0:  // Program Change: one data byte
			out.type = MidiEventType::ProgramChange;
			out.number = data[1] & 0x7f;
			return true;

		case 0xb0:  // Control Change
			if (size < 3)
			{
				return false;
			}

			out.type = MidiEventType::ControlChange;
			out.number = data[1] & 0x7f;
			out.value = data[2] & 0x7f;
			return true;

		case 0x90:  // Note On (velocity 0 means Note Off)
			if (size < 3)
			{
				return false;
			}

			out.number = data[1] & 0x7f;
			out.value = data[2] & 0x7f;
			out.type = out.value == 0 ? MidiEventType::NoteOff : MidiEventType::NoteOn;
			return true;

		case 0x80:  // Note Off
			if (size < 3)
			{
				return false;
			}

			out.type = MidiEventType::NoteOff;
			out.number = data[1] & 0x7f;
			out.value = data[2] & 0x7f;
			return true;

		default:
			return false;
	}
}

}  // namespace sf
