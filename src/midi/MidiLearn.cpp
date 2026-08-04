#include "MidiLearn.h"

#include <cstdlib>
#include <sstream>

namespace sf
{

namespace
{

// Can this message type express the target at all? A knob can't drive a
// preset change usefully, and a program change carries no value to drive a
// control with.
bool TypeSuitsAction(MidiEventType type, MidiAction action)
{
	if (action == MidiAction::SetControl)
	{
		return type == MidiEventType::ControlChange;
	}

	return type == MidiEventType::ProgramChange || type == MidiEventType::ControlChange ||
		type == MidiEventType::NoteOn;
}

// Is this the message a person just produced deliberately, as opposed to the
// other half of it? Learning from a release would bind the wrong edge.
bool IsDeliberate(const MidiEvent& event, MidiAction action)
{
	if (action == MidiAction::SetControl)
	{
		// Any movement counts -- a pedal at rest reads 0 just as legitimately
		// as one at full travel.
		return event.type == MidiEventType::ControlChange;
	}

	switch (event.type)
	{
		case MidiEventType::ProgramChange:
			return true;

		case MidiEventType::ControlChange:
			return event.value >= kSwitchThreshold;

		case MidiEventType::NoteOn:
			return event.value > 0;

		default:
			return false;
	}
}

bool ReadInt(const std::string& text, int& out)
{
	if (text.empty())
	{
		return false;
	}

	char* end = nullptr;
	const long value = std::strtol(text.c_str(), &end, 10);

	if (*end != '\0')
	{
		return false;
	}

	out = static_cast<int>(value);

	return true;
}

bool ReadFloat(const std::string& text, float& out)
{
	if (text.empty())
	{
		return false;
	}

	char* end = nullptr;
	const float value = std::strtof(text.c_str(), &end);

	if (*end != '\0')
	{
		return false;
	}

	out = value;

	return true;
}

}  // namespace

void MidiLearn::Observe(const MidiEvent& event, uint64_t now_ms)
{
	for (Traffic& entry : traffic_)
	{
		if (entry.type != event.type || entry.channel != event.channel || entry.number != event.number)
		{
			continue;
		}

		// Restart the count after a quiet spell, so a control someone used
		// once a minute ago isn't mistaken for one that's chattering now.
		if (now_ms - entry.last_ms > kNoiseWindowMs)
		{
			entry.count = 0;
		}

		++entry.count;
		entry.last_ms = now_ms;

		return;
	}

	traffic_.push_back({event.type, event.channel, event.number, 1, now_ms});
}

bool MidiLearn::WasChattering(const MidiEvent& event) const
{
	for (const Source& source : chattering_)
	{
		if (source.type == event.type && source.channel == event.channel && source.number == event.number)
		{
			return true;
		}
	}

	return false;
}

void MidiLearn::Arm(const MidiBinding& target, uint64_t now_ms)
{
	target_ = target;
	armed_ = true;

	// Snapshot the noisy sources now. Everything from here on is potentially
	// the user pressing what we asked them to press -- including, crucially,
	// the very next message, which the caller Observe()s before offering it.
	chattering_.clear();

	for (const Traffic& entry : traffic_)
	{
		if (entry.count >= kNoiseCount && now_ms >= entry.last_ms && now_ms - entry.last_ms <= kNoiseWindowMs)
		{
			chattering_.push_back({entry.type, entry.channel, entry.number});
		}
	}
}

MidiLearn::Result MidiLearn::Offer(const MidiEvent& event, uint64_t now_ms, MidiBinding& out)
{
	// Kept in the signature so callers stay honest about ordering, and so a
	// time-based rule can return here without churning every call site.
	(void)now_ms;

	if (!armed_)
	{
		return Result::Ignored;
	}

	if (!TypeSuitsAction(event.type, target_.action) || !IsDeliberate(event, target_.action))
	{
		return Result::Ignored;
	}

	// Checked after the type filter so a chattering controller that couldn't
	// have been bound anyway is simply ignored rather than reported.
	if (WasChattering(event))
	{
		return Result::Noisy;
	}

	out = target_;
	out.type = event.type;

	// Bind the channel it actually arrived on. Learning knows exactly what
	// came in, so there's no reason to widen it to omni here -- and a
	// specific channel is what keeps two controllers from colliding.
	out.channel = event.channel;
	out.number = event.number;

	armed_ = false;

	return Result::Bound;
}

bool ParseLearnCommand(const std::string& arguments, MidiBinding& out, std::string& error)
{
	std::istringstream stream(arguments);
	std::string action_text;

	if (!(stream >> action_text))
	{
		error = "learn what? (next_preset, prev_preset, load_preset <name>, toggle_bypass <stage>, "
				"set_control <stage> <port> [min] [max])";
		return false;
	}

	out = MidiBinding();

	if (!ParseMidiAction(action_text, out.action))
	{
		error = "unknown action '" + action_text + "'";
		return false;
	}

	switch (out.action)
	{
		case MidiAction::NextPreset:
		case MidiAction::PrevPreset:
			break;

		case MidiAction::LoadPreset:
		{
			// The rest of the line, so preset names may contain spaces.
			std::string name;
			std::getline(stream, name);

			const size_t start = name.find_first_not_of(" \t");
			const size_t end = name.find_last_not_of(" \t");

			if (start == std::string::npos)
			{
				error = "load_preset needs a preset name";
				return false;
			}

			out.preset = name.substr(start, end - start + 1);
			break;
		}

		case MidiAction::ToggleBypass:
		{
			std::string stage_text;

			if (!(stream >> stage_text) || !ReadInt(stage_text, out.stage) || out.stage < 0)
			{
				error = "toggle_bypass needs a stage index";
				return false;
			}
			break;
		}

		case MidiAction::SetControl:
		{
			std::string stage_text;

			if (!(stream >> stage_text) || !ReadInt(stage_text, out.stage) || out.stage < 0)
			{
				error = "set_control needs a stage index";
				return false;
			}

			if (!(stream >> out.port) || out.port.empty())
			{
				error = "set_control needs a control port symbol";
				return false;
			}

			// The range is optional; callers that know the port's real bounds
			// fill them in afterwards.
			std::string min_text;
			std::string max_text;

			if (stream >> min_text)
			{
				if (!ReadFloat(min_text, out.minimum))
				{
					error = "min must be a number, got '" + min_text + "'";
					return false;
				}

				if (!(stream >> max_text) || !ReadFloat(max_text, out.maximum))
				{
					error = "a min needs a matching max";
					return false;
				}

				if (out.minimum == out.maximum)
				{
					error = "min and max must differ";
					return false;
				}
			}

			break;
		}

		// The board-wide targets (SF-68). None of them names a stage, because
		// none of them is in the chain -- the trims sit outside it, and there
		// is one splitter and one mixer.
		case MidiAction::SetSplitterControl:
		{
			if (!(stream >> out.port) || (out.port != "send_a" && out.port != "send_b" && out.port != "crossover"))
			{
				error = "set_splitter needs one of: send_a, send_b, crossover";
				return false;
			}
			break;
		}

		case MidiAction::SetMixerLevel:
		{
			if (!(stream >> out.port) || (out.port != "a" && out.port != "b"))
			{
				error = "set_mixer needs a channel: a or b";
				return false;
			}
			break;
		}

		case MidiAction::SetInputGain:
		case MidiAction::SetOutputGain:
		case MidiAction::SetTempo:
		case MidiAction::NextBank:
		case MidiAction::PrevBank:
		case MidiAction::ToggleMetronome:
			// No target to name: each of these addresses exactly one thing.
			// The continuous ones get their range from the control they were
			// armed beside, the way SetControl does.
			break;
	}

	return true;
}

}  // namespace sf
