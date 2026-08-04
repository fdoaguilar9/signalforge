// Mapping table tests: parsing, round-trip, and event resolution. Pure
// logic -- no JACK, no audio hardware, no plugins, so this runs anywhere.

#include "../src/midi/MidiMap.h"

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;

void Check(bool condition, const char* description)
{
	if (condition)
	{
		std::printf("ok    %s\n", description);
		return;
	}

	std::printf("FAIL  %s\n", description);
	++g_failures;
}

sf::MidiMap MapFrom(const std::string& text, const char* description)
{
	sf::MidiMap map;
	std::string error;

	if (!sf::MidiMap::FromJson(nlohmann::json::parse(text), map, error))
	{
		std::printf("FAIL  %s: unexpected load failure: %s\n", description, error.c_str());
		++g_failures;
	}

	return map;
}

void ExpectRejected(const std::string& text, const char* description)
{
	sf::MidiMap map;
	std::string error;

	if (sf::MidiMap::FromJson(nlohmann::json::parse(text), map, error))
	{
		std::printf("FAIL  %-46s expected rejection, loaded %zu binding(s)\n", description, map.Size());
		++g_failures;
		return;
	}

	std::printf("ok    %-46s rejected: %s\n", description, error.c_str());
}

sf::MidiEvent Event(sf::MidiEventType type, int channel, int number, int value)
{
	sf::MidiEvent event;
	event.type = type;
	event.channel = channel;
	event.number = number;
	event.value = value;

	return event;
}

// Resolves an event and reports whether it fired, for terse assertions.
bool Fires(sf::MidiMap& map, const sf::MidiEvent& event, sf::MidiActionInvocation& out)
{
	return map.Resolve(event, out);
}

bool Fires(sf::MidiMap& map, const sf::MidiEvent& event)
{
	sf::MidiActionInvocation invocation;
	return map.Resolve(event, invocation);
}

// PLAN.md §5.2's example table, in the shape we actually settled on.
const char* kSampleMap = R"([
  { "type": "program_change", "channel": 0, "number": 0, "action": "load_preset", "preset": "clean" },
  { "type": "cc", "channel": 0, "number": 64, "action": "next_preset" },
  { "type": "cc", "channel": 0, "number": 65, "action": "prev_preset" },
  { "type": "cc", "channel": 0, "number": 66, "action": "toggle_bypass", "stage": 1 },
  { "type": "cc", "channel": 0, "number": 20, "action": "set_control", "stage": 0, "port": "quality",
    "min": 0.0, "max": 1.0 },
  { "type": "note_on", "channel": 3, "number": 60, "action": "load_preset", "preset": "lead" }
])";

void TestParsing()
{
	std::printf("\n-- parsing --\n");

	sf::MidiMap map = MapFrom(kSampleMap, "sample map");
	Check(map.Size() == 6, "sample map loads 6 bindings");

	const sf::MidiBinding& pc = map.Bindings()[0];
	Check(pc.type == sf::MidiEventType::ProgramChange && pc.channel == 0 && pc.number == 0 &&
			pc.action == sf::MidiAction::LoadPreset && pc.preset == "clean",
		"program_change -> load_preset parsed");

	const sf::MidiBinding& control = map.Bindings()[4];
	Check(control.action == sf::MidiAction::SetControl && control.stage == 0 && control.port == "quality",
		"set_control target parsed");

	// An omitted channel is the forgiving default: listen on all 16.
	sf::MidiMap omni = MapFrom(R"([{ "type": "cc", "number": 64, "action": "next_preset" }])", "omni map");
	Check(omni.Size() == 1 && omni.Bindings()[0].channel == sf::kAnyChannel, "omitted channel means any");

	ExpectRejected(R"([{ "type": "cc", "number": 64 }])", "missing action");
	ExpectRejected(R"([{ "type": "cc", "action": "next_preset" }])", "missing number");
	ExpectRejected(R"([{ "type": "aftertouch", "number": 1, "action": "next_preset" }])", "unknown type");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "explode" }])", "unknown action");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "load_preset" }])", "load_preset without preset");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "set_control", "stage": 0 }])",
		"set_control without port");
	ExpectRejected(
		R"([{ "type": "cc", "number": 1, "action": "set_control", "port": "q", "min": 1, "max": 1 }])",
		"set_control with empty range");
	ExpectRejected(R"([{ "type": "program_change", "number": 1, "action": "set_control", "port": "q" }])",
		"set_control driven by program_change");
	ExpectRejected(R"([{ "type": "cc", "channel": 42, "number": 1, "action": "next_preset" }])",
		"channel out of range");
	ExpectRejected(R"([{ "type": "cc", "number": 999, "action": "next_preset" }])", "number out of range");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "toggle_bypass", "stage": -2 }])",
		"negative stage");
	ExpectRejected(R"({ "type": "cc", "number": 1, "action": "next_preset" })", "object instead of array");

	// Two bindings on one message would make behaviour depend on file order.
	ExpectRejected(R"([
		{ "type": "cc", "channel": 0, "number": 64, "action": "next_preset" },
		{ "type": "cc", "channel": 0, "number": 64, "action": "prev_preset" }
	])",
		"duplicate matcher");

	// ...including when one of them is omni, since it subsumes the other.
	ExpectRejected(R"([
		{ "type": "cc", "number": 64, "action": "next_preset" },
		{ "type": "cc", "channel": 5, "number": 64, "action": "prev_preset" }
	])",
		"omni binding shadowing a specific one");
}

void TestRoundTrip()
{
	std::printf("\n-- round-trip --\n");

	sf::MidiMap map = MapFrom(kSampleMap, "sample map");

	sf::MidiMap reloaded;
	std::string error;
	Check(sf::MidiMap::FromJson(map.ToJson(), reloaded, error), "serialized map reloads");
	Check(reloaded.Bindings() == map.Bindings(), "round-trip preserves every binding");

	// An omni binding must not come back as channel -1 masquerading as real.
	sf::MidiMap omni = MapFrom(R"([{ "type": "cc", "number": 64, "action": "next_preset" }])", "omni map");
	Check(!omni.ToJson()[0].contains("channel"), "omni channel is omitted, not written as -1");

	// A next_preset binding shouldn't imply it cares about a stage or port.
	Check(!omni.ToJson()[0].contains("stage") && !omni.ToJson()[0].contains("port"),
		"unused target fields are not emitted");
}

void TestMatching()
{
	std::printf("\n-- matching --\n");

	sf::MidiMap map = MapFrom(kSampleMap, "sample map");
	sf::MidiActionInvocation invocation;

	Check(Fires(map, Event(sf::MidiEventType::ProgramChange, 0, 0, 0), invocation) &&
			invocation.action == sf::MidiAction::LoadPreset && invocation.binding->preset == "clean",
		"program change 0 loads 'clean'");

	Check(!Fires(map, Event(sf::MidiEventType::ProgramChange, 1, 0, 0)), "wrong channel does not fire");
	Check(!Fires(map, Event(sf::MidiEventType::ProgramChange, 0, 7, 0)), "unmapped program does not fire");
	Check(!Fires(map, Event(sf::MidiEventType::ControlChange, 0, 0, 127)),
		"right number but wrong type does not fire");

	// A program change repeats freely -- there is no release to wait for.
	Check(Fires(map, Event(sf::MidiEventType::ProgramChange, 0, 0, 0)), "program change refires immediately");

	sf::MidiMap omni = MapFrom(R"([{ "type": "cc", "number": 64, "action": "next_preset" }])", "omni map");
	Check(Fires(omni, Event(sf::MidiEventType::ControlChange, 11, 64, 127)),
		"omni binding fires on an arbitrary channel");
}

void TestSwitchEdges()
{
	std::printf("\n-- switch edges --\n");

	sf::MidiMap map = MapFrom(kSampleMap, "sample map");

	// The bug this exists to prevent: a momentary footswitch sends 127 on
	// press and 0 on release. Firing on both advances two presets per stomp.
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 127)), "press fires");
	Check(!Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 0)), "release does not fire");
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 127)), "second press fires");

	// A switch held down may repeat its message; that isn't a new press.
	Check(!Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 127)), "repeat while held does not fire");
	Check(!Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 100)), "still-held value does not fire");
	Check(!Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 10)), "falling below threshold re-arms");
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 64, 64)), "press at the threshold fires");

	// Held state is per binding, not global.
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 65, 127)), "a different switch is independent");

	// Note-driven switches: press on NoteOn, re-arm on NoteOff. (SF-19 already
	// turns NoteOn/velocity-0 into NoteOff before we ever see it.)
	Check(Fires(map, Event(sf::MidiEventType::NoteOn, 3, 60, 100)), "note on fires");
	Check(!Fires(map, Event(sf::MidiEventType::NoteOn, 3, 60, 100)), "held note does not refire");
	Check(!Fires(map, Event(sf::MidiEventType::NoteOff, 3, 60, 0)), "note off does not fire");
	Check(Fires(map, Event(sf::MidiEventType::NoteOn, 3, 60, 100)), "note on after release fires again");
}

void TestControlScaling()
{
	std::printf("\n-- control scaling --\n");

	sf::MidiMap map = MapFrom(kSampleMap, "sample map");
	sf::MidiActionInvocation invocation;

	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 20, 0), invocation) &&
			invocation.control_value == 0.0f,
		"cc 0 maps to min");
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 20, 127), invocation) &&
			invocation.control_value == 1.0f,
		"cc 127 maps to max");

	// Unlike a switch, a knob reports every value -- including a repeat, and
	// including 0, which is a legitimate position rather than a release.
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 20, 127), invocation), "repeated cc still fires");
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 20, 0), invocation) &&
			invocation.control_value == 0.0f,
		"cc 0 fires rather than reading as a release");

	sf::MidiMap ranged = MapFrom(
		R"([{ "type": "cc", "number": 7, "action": "set_control", "stage": 0, "port": "gain",
		      "min": -20.0, "max": 20.0 }])",
		"ranged map");

	Check(Fires(ranged, Event(sf::MidiEventType::ControlChange, 0, 7, 0), invocation) &&
			invocation.control_value == -20.0f,
		"custom range: 0 -> min");
	Check(Fires(ranged, Event(sf::MidiEventType::ControlChange, 0, 7, 127), invocation) &&
			invocation.control_value == 20.0f,
		"custom range: 127 -> max");

	// An inverted range is a legitimate way to reverse a pedal's direction.
	sf::MidiMap inverted = MapFrom(
		R"([{ "type": "cc", "number": 7, "action": "set_control", "port": "gain", "min": 1.0, "max": 0.0 }])",
		"inverted map");

	Check(Fires(inverted, Event(sf::MidiEventType::ControlChange, 0, 7, 0), invocation) &&
			invocation.control_value == 1.0f,
		"inverted range: 0 -> 1");
}

// The ten actions Milestone 10 added (SF-68). Each one is a target the
// redesign put a learn button beside, and each has to survive the same
// round-trip and the same refusals as the five that came before.
void TestBoardWideActions()
{
	std::printf("\n-- board-wide actions (SF-68) --\n");

	// Every new action, in one map, so a name that stopped parsing shows up
	// here rather than as a mapping file that mysteriously won't load.
	sf::MidiMap map = MapFrom(
		R"([
			{ "type": "cc", "number": 90, "action": "next_bank" },
			{ "type": "cc", "number": 91, "action": "prev_bank" },
			{ "type": "cc", "number": 92, "action": "toggle_metronome" },
			{ "type": "cc", "number": 20, "action": "set_input_gain",  "min": -24.0, "max": 24.0 },
			{ "type": "cc", "number": 21, "action": "set_output_gain", "min": -24.0, "max": 24.0 },
			{ "type": "cc", "number": 22, "action": "set_tempo", "min": 40.0, "max": 240.0 },
			{ "type": "cc", "number": 23, "action": "set_splitter", "port": "send_a",    "min": -60.0, "max": 6.0 },
			{ "type": "cc", "number": 24, "action": "set_splitter", "port": "send_b",    "min": -60.0, "max": 6.0 },
			{ "type": "cc", "number": 25, "action": "set_splitter", "port": "crossover", "min": 20.0, "max": 20000.0 },
			{ "type": "cc", "number": 26, "action": "set_mixer", "port": "a", "min": -60.0, "max": 6.0 },
			{ "type": "cc", "number": 27, "action": "set_mixer", "port": "b", "min": -60.0, "max": 6.0 }
		])",
		"board-wide map");

	Check(map.Size() == 11, "every new action name parses");

	Check(map.Bindings()[0].action == sf::MidiAction::NextBank, "next_bank parsed");
	Check(map.Bindings()[1].action == sf::MidiAction::PrevBank, "prev_bank parsed");
	Check(map.Bindings()[2].action == sf::MidiAction::ToggleMetronome, "toggle_metronome parsed");
	Check(map.Bindings()[3].action == sf::MidiAction::SetInputGain, "set_input_gain parsed");
	Check(map.Bindings()[4].action == sf::MidiAction::SetOutputGain, "set_output_gain parsed");
	Check(map.Bindings()[5].action == sf::MidiAction::SetTempo, "set_tempo parsed");

	Check(map.Bindings()[6].action == sf::MidiAction::SetSplitterControl && map.Bindings()[6].port == "send_a",
		"set_splitter names its control through \"port\"");
	Check(map.Bindings()[9].action == sf::MidiAction::SetMixerLevel && map.Bindings()[9].port == "a",
		"set_mixer names its channel through \"port\"");

	// Round-trip. The reason this matters for the new actions specifically is
	// that each writes a different subset of the target fields, and a subset
	// that doesn't match what the loader requires is a map that saves and
	// then won't load.
	sf::MidiMap reloaded;
	std::string error;

	Check(sf::MidiMap::FromJson(map.ToJson(), reloaded, error), "board-wide map reloads after serializing");
	Check(reloaded.Bindings() == map.Bindings(), "round-trip preserves every board-wide binding");

	// None of these lives in the chain, so none of them should imply a stage.
	Check(!map.ToJson()[0].contains("stage") && !map.ToJson()[6].contains("stage"),
		"board-wide actions do not emit a stage");

	// The refusals. Each of these is a hand-editable mistake that would
	// otherwise fail silently at stomp time.
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "set_splitter", "min": 0, "max": 1 }])",
		"set_splitter without a port");
	ExpectRejected(
		R"([{ "type": "cc", "number": 1, "action": "set_splitter", "port": "wobble", "min": 0, "max": 1 }])",
		"set_splitter with an unknown port");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "set_mixer", "port": "c", "min": 0, "max": 1 }])",
		"set_mixer with a channel that isn't a or b");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "set_input_gain", "min": 1, "max": 1 }])",
		"set_input_gain with an empty range");
	ExpectRejected(R"([{ "type": "cc", "number": 1, "action": "set_tempo", "min": 120, "max": 120 }])",
		"set_tempo with an empty range");

	// A program change carries no value, so it cannot drive any of the
	// continuous actions -- not just set_control, which was the only one that
	// existed when that check was first written.
	ExpectRejected(
		R"([{ "type": "program_change", "number": 1, "action": "set_input_gain", "min": -24, "max": 24 }])",
		"program_change cannot drive set_input_gain");
	ExpectRejected(
		R"([{ "type": "program_change", "number": 1, "action": "set_mixer", "port": "a", "min": -60, "max": 6 }])",
		"program_change cannot drive set_mixer");

	// ...but it is fine for the switch-like ones, which take no value at all.
	sf::MidiMap stomps = MapFrom(
		R"([{ "type": "program_change", "number": 5, "action": "next_bank" },
		    { "type": "program_change", "number": 6, "action": "toggle_metronome" }])",
		"program_change stomps");

	Check(stomps.Size() == 2, "program_change still drives the actions that carry no value");

	// Scaling, on one of the new continuous actions: the range in the file is
	// the range the pedal sweeps.
	sf::MidiActionInvocation invocation;

	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 20, 0), invocation) &&
			invocation.control_value == -24.0f,
		"set_input_gain: 0 -> -24 dB");
	Check(Fires(map, Event(sf::MidiEventType::ControlChange, 0, 20, 127), invocation) &&
			invocation.control_value == 24.0f,
		"set_input_gain: 127 -> +24 dB");
}

}  // namespace

int main()
{
	TestParsing();
	TestRoundTrip();
	TestMatching();
	TestSwitchEdges();
	TestControlScaling();
	TestBoardWideActions();

	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);

	return g_failures == 0 ? 0 : 1;
}
