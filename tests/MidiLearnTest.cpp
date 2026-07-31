// MIDI-learn tests: command parsing, which message gets bound, and the
// noise rejection that keeps a chattering controller from winning the race
// against a human foot. Pure logic -- no JACK, no hardware.

#include "../src/midi/MidiLearn.h"

#include <cstdio>

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

sf::MidiEvent Event(sf::MidiEventType type, int channel, int number, int value)
{
	sf::MidiEvent event;
	event.type = type;
	event.channel = channel;
	event.number = number;
	event.value = value;

	return event;
}

// Mirrors exactly what the live loop does: every incoming event is
// Observe()d first, then offered. Getting this order wrong in the test is
// what let a real noise-rejection bug through -- the offered event's own
// arrival was moving the very timestamps the rejection compared against.
sf::MidiLearn::Result ObserveThenOffer(
	sf::MidiLearn& learn, const sf::MidiEvent& event, uint64_t now_ms, sf::MidiBinding& out)
{
	learn.Observe(event, now_ms);
	return learn.Offer(event, now_ms, out);
}

sf::MidiBinding Target(const char* command)
{
	sf::MidiBinding target;
	std::string error;

	if (!sf::ParseLearnCommand(command, target, error))
	{
		std::printf("FAIL  could not parse '%s': %s\n", command, error.c_str());
		++g_failures;
	}

	return target;
}

void ExpectParseFails(const char* command, const char* description)
{
	sf::MidiBinding target;
	std::string error;

	if (sf::ParseLearnCommand(command, target, error))
	{
		std::printf("FAIL  %-44s expected a parse failure\n", description);
		++g_failures;
		return;
	}

	std::printf("ok    %-44s rejected: %s\n", description, error.c_str());
}

void TestParsing()
{
	std::printf("\n-- learn command parsing --\n");

	sf::MidiBinding next = Target(" next_preset");
	Check(next.action == sf::MidiAction::NextPreset, "next_preset");

	sf::MidiBinding load = Target(" load_preset 02-rhythm");
	Check(load.action == sf::MidiAction::LoadPreset && load.preset == "02-rhythm", "load_preset with a name");

	// Preset names come from filenames and may well contain spaces.
	sf::MidiBinding spaced = Target(" load_preset Crunch Rhythm 2");
	Check(spaced.preset == "Crunch Rhythm 2", "load_preset name with spaces");

	sf::MidiBinding bypass = Target(" toggle_bypass 2");
	Check(bypass.action == sf::MidiAction::ToggleBypass && bypass.stage == 2, "toggle_bypass with a stage");

	sf::MidiBinding control = Target(" set_control 0 input_level -20 20");
	Check(control.action == sf::MidiAction::SetControl && control.stage == 0 && control.port == "input_level" &&
			control.minimum == -20.0f && control.maximum == 20.0f,
		"set_control with an explicit range");

	// Without a range, the caller fills in the port's real bounds.
	sf::MidiBinding ranged = Target(" set_control 1 gain");
	Check(ranged.port == "gain" && ranged.minimum == 0.0f && ranged.maximum == 1.0f,
		"set_control range defaults when omitted");

	ExpectParseFails("", "empty command");
	ExpectParseFails(" explode", "unknown action");
	ExpectParseFails(" load_preset", "load_preset without a name");
	ExpectParseFails(" toggle_bypass", "toggle_bypass without a stage");
	ExpectParseFails(" toggle_bypass -1", "toggle_bypass with a negative stage");
	ExpectParseFails(" toggle_bypass two", "toggle_bypass with a non-numeric stage");
	ExpectParseFails(" set_control 0", "set_control without a port");
	ExpectParseFails(" set_control 0 gain 5", "set_control with a min but no max");
	ExpectParseFails(" set_control 0 gain 5 5", "set_control with an empty range");
}

void TestBinding()
{
	std::printf("\n-- what gets bound --\n");

	{
		sf::MidiLearn learn;
		sf::MidiBinding out;

		learn.Arm(Target(" next_preset"), 1000);
		Check(learn.Armed(), "arming sticks");

		// The user's AIRSTEP sends program changes from its switches.
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ProgramChange, 3, 7, 0), 1100, out) ==
				sf::MidiLearn::Result::Bound,
			"a program change binds");
		Check(out.action == sf::MidiAction::NextPreset && out.type == sf::MidiEventType::ProgramChange &&
				out.channel == 3 && out.number == 7,
			"binding keeps the action and the message it arrived on");
		Check(!learn.Armed(), "learning disarms after binding");
	}

	{
		// A footswitch sends press *and* release. Binding the release would
		// record a value of 0 and fire on the wrong edge afterwards.
		sf::MidiLearn learn;
		sf::MidiBinding out;

		learn.Arm(Target(" toggle_bypass 1"), 1000);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 80, 0), 1100, out) ==
				sf::MidiLearn::Result::Ignored,
			"a CC release is not bound");
		Check(learn.Armed(), "still listening after ignoring a release");
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 80, 127), 1200, out) ==
				sf::MidiLearn::Result::Bound,
			"the following press is bound");
		Check(out.number == 80, "bound to the pressed controller");
	}

	{
		sf::MidiLearn learn;
		sf::MidiBinding out;

		learn.Arm(Target(" load_preset lead"), 1000);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::NoteOff, 0, 60, 0), 1100, out) == sf::MidiLearn::Result::Ignored,
			"a note off is not bound");
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::NoteOn, 0, 60, 100), 1200, out) == sf::MidiLearn::Result::Bound,
			"a note on is bound");
	}

	{
		// A knob can't drive a preset change, and a program change carries no
		// value to drive a knob with.
		sf::MidiLearn learn;
		sf::MidiBinding out;

		learn.Arm(Target(" set_control 0 gain 0 10"), 1000);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ProgramChange, 0, 5, 0), 1100, out) ==
				sf::MidiLearn::Result::Ignored,
			"a program change cannot drive set_control");
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::NoteOn, 0, 60, 100), 1150, out) == sf::MidiLearn::Result::Ignored,
			"a note cannot drive set_control");

		// Unlike a switch, a pedal at rest reading 0 is a legitimate message.
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 11, 0), 1200, out) ==
				sf::MidiLearn::Result::Bound,
			"a CC at 0 binds for set_control");
		Check(out.number == 11 && out.minimum == 0.0f && out.maximum == 10.0f,
			"set_control binding keeps its range");
	}

	{
		sf::MidiLearn learn;
		sf::MidiBinding out;

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ProgramChange, 0, 1, 0), 100, out) ==
				sf::MidiLearn::Result::Ignored,
			"offers are ignored when not armed");

		learn.Arm(Target(" next_preset"), 1000);
		learn.Cancel();

		Check(!learn.Armed(), "cancel disarms");
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ProgramChange, 0, 1, 0), 1100, out) ==
				sf::MidiLearn::Result::Ignored,
			"nothing binds after cancelling");
	}
}

void TestNoiseRejection()
{
	std::printf("\n-- noise rejection --\n");

	{
		// The real case: an expression pedal with a bad pot streaming CC 7
		// continuously. It would win every race against a human foot.
		sf::MidiLearn learn;
		sf::MidiBinding out;

		uint64_t now = 0;

		for (int i = 0; i < 40; ++i)
		{
			learn.Observe(Event(sf::MidiEventType::ControlChange, 0, 7, 88 + (i % 4)), now);
			now += 50;
		}

		learn.Arm(Target(" set_control 0 gain 0 10"), now);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 7, 90), now + 10, out) ==
				sf::MidiLearn::Result::Noisy,
			"a chattering controller is rejected");
		Check(learn.Armed(), "still listening after rejecting noise");

		// The pedal the user actually moves is silent until they move it.
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 11, 64), now + 500, out) ==
				sf::MidiLearn::Result::Bound,
			"the quiet controller binds");
		Check(out.number == 11, "bound to the one the user moved, not the noisy one");
	}

	{
		// Noise rejection must not swallow a control the user deliberately
		// swept before arming -- only one that is streaming on its own.
		sf::MidiLearn learn;
		sf::MidiBinding out;

		learn.Observe(Event(sf::MidiEventType::ControlChange, 0, 11, 10), 0);
		learn.Observe(Event(sf::MidiEventType::ControlChange, 0, 11, 20), 100);

		learn.Arm(Target(" set_control 0 gain 0 10"), 1000);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 11, 30), 1100, out) ==
				sf::MidiLearn::Result::Bound,
			"a few earlier messages are not treated as chatter");
	}

	{
		// A controller that chattered a while ago and then went quiet is
		// fine -- the user may have since unplugged and replugged it.
		sf::MidiLearn learn;
		sf::MidiBinding out;

		uint64_t now = 0;

		for (int i = 0; i < 40; ++i)
		{
			learn.Observe(Event(sf::MidiEventType::ControlChange, 0, 7, 88), now);
			now += 50;
		}

		// ...then silence for well past the window.
		learn.Arm(Target(" set_control 0 gain 0 10"), now + 10 * sf::MidiLearn::kNoiseWindowMs);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 7, 90),
				  now + 10 * sf::MidiLearn::kNoiseWindowMs + 100, out) == sf::MidiLearn::Result::Bound,
			"a controller that has since gone quiet can be bound");
	}

	{
		// Switch bindings need the same protection: a chattering CC would
		// otherwise be learned as a footswitch.
		sf::MidiLearn learn;
		sf::MidiBinding out;

		uint64_t now = 0;

		for (int i = 0; i < 40; ++i)
		{
			learn.Observe(Event(sf::MidiEventType::ControlChange, 0, 7, 127), now);
			now += 50;
		}

		learn.Arm(Target(" next_preset"), now);

		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ControlChange, 0, 7, 127), now + 10, out) ==
				sf::MidiLearn::Result::Noisy,
			"chatter is rejected for switch targets too");
		Check(ObserveThenOffer(learn, Event(sf::MidiEventType::ProgramChange, 0, 3, 0), now + 500, out) ==
				sf::MidiLearn::Result::Bound,
			"the switch press still binds");
	}
}

void TestRebinding()
{
	std::printf("\n-- rebinding --\n");

	sf::MidiMap map;
	std::string error;

	sf::MidiBinding first;
	first.type = sf::MidiEventType::ProgramChange;
	first.channel = 0;
	first.number = 3;
	first.action = sf::MidiAction::NextPreset;

	Check(map.Add(first, error), "initial binding added");

	// Re-learning the same switch must move it, not fail as a duplicate --
	// pressing it again is exactly how a user says "make it do this instead".
	sf::MidiBinding relearned = first;
	relearned.action = sf::MidiAction::PrevPreset;

	Check(map.Bind(relearned) == 1, "rebinding the same message replaces one binding");
	Check(map.Size() == 1, "no duplicate is left behind");
	Check(map.Bindings()[0].action == sf::MidiAction::PrevPreset, "the new action won");

	sf::MidiBinding other = first;
	other.number = 4;

	Check(map.Bind(other) == 0, "a different message replaces nothing");
	Check(map.Size() == 2, "and is appended");
}

}  // namespace

int main()
{
	TestParsing();
	TestBinding();
	TestNoiseRejection();
	TestRebinding();

	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);

	return g_failures == 0 ? 0 : 1;
}
