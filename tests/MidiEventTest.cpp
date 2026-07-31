// Pure parsing tests for MidiEvent -- no JACK, no audio hardware, no
// plugins, so this runs anywhere.

#include "../src/midi/MidiEvent.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

namespace
{

int g_failures = 0;

void ExpectParsed(std::initializer_list<uint8_t> bytes, sf::MidiEventType type, int channel, int number, int value,
	const char* description)
{
	const std::vector<uint8_t> data(bytes);
	sf::MidiEvent event;

	const bool parsed = sf::ParseMidiEvent(data.data(), data.size(), event);

	if (!parsed || event.type != type || event.channel != channel || event.number != number || event.value != value)
	{
		std::printf("FAIL  %-34s parsed=%d %s ch=%d number=%d value=%d\n", description, parsed,
			sf::ToString(event.type), event.channel, event.number, event.value);
		++g_failures;
		return;
	}

	std::printf("ok    %-34s %s ch=%d number=%d value=%d\n", description, sf::ToString(event.type), event.channel,
		event.number, event.value);
}

void ExpectRejected(std::initializer_list<uint8_t> bytes, const char* description)
{
	const std::vector<uint8_t> data(bytes);
	sf::MidiEvent event;

	if (sf::ParseMidiEvent(data.data(), data.size(), event))
	{
		std::printf("FAIL  %-34s expected rejection, got %s\n", description, sf::ToString(event.type));
		++g_failures;
		return;
	}

	std::printf("ok    %-34s rejected\n", description);
}

}  // namespace

int main()
{
	ExpectParsed({0xC0, 0x03}, sf::MidiEventType::ProgramChange, 0, 3, 0, "PC ch0 program 3");
	ExpectParsed({0xC9, 0x7F}, sf::MidiEventType::ProgramChange, 9, 127, 0, "PC ch9 program 127");

	ExpectParsed({0xB0, 0x40, 0x7F}, sf::MidiEventType::ControlChange, 0, 64, 127, "CC ch0 cc64 = 127");
	ExpectParsed({0xBF, 0x14, 0x00}, sf::MidiEventType::ControlChange, 15, 20, 0, "CC ch15 cc20 = 0");

	ExpectParsed({0x90, 0x3C, 0x64}, sf::MidiEventType::NoteOn, 0, 60, 100, "NoteOn ch0 note60");
	ExpectParsed({0x85, 0x3C, 0x40}, sf::MidiEventType::NoteOff, 5, 60, 64, "NoteOff ch5 note60");

	// Controllers commonly send NoteOn/velocity-0 for a release; treating
	// that as a press would double-trigger every footswitch.
	ExpectParsed({0x90, 0x3C, 0x00}, sf::MidiEventType::NoteOff, 0, 60, 0, "NoteOn velocity 0 -> NoteOff");

	ExpectRejected({0xC0}, "PC truncated");
	ExpectRejected({0xB0, 0x40}, "CC truncated");
	ExpectRejected({0x90, 0x3C}, "NoteOn truncated");
	ExpectRejected({0x3C, 0x40}, "no status byte");
	ExpectRejected({0xF8, 0x00}, "realtime clock ignored");
	ExpectRejected({0xE0, 0x00, 0x40}, "pitch bend ignored");

	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);

	return g_failures == 0 ? 0 : 1;
}
