#include "MidiMap.h"

#include <fstream>

namespace sf
{

namespace
{

// The JSON spelling of each event type a binding may match on. "cc" rather
// than "control_change" to match PLAN.md §5.2 and how pedal manuals write it.
struct TypeName
{
	const char* text;
	MidiEventType type;
};

constexpr TypeName kTypeNames[] = {
	{"program_change", MidiEventType::ProgramChange},
	{"cc", MidiEventType::ControlChange},
	{"note_on", MidiEventType::NoteOn},
};

bool ParseBindingType(const std::string& text, MidiEventType& out)
{
	for (const TypeName& entry : kTypeNames)
	{
		if (text == entry.text)
		{
			out = entry.type;
			return true;
		}
	}

	return false;
}

const char* BindingTypeName(MidiEventType type)
{
	for (const TypeName& entry : kTypeNames)
	{
		if (type == entry.type)
		{
			return entry.text;
		}
	}

	return "cc";
}

// Reads an optional integer field, leaving `out` alone when absent.
bool ReadOptionalInt(const nlohmann::json& json, const char* key, int& out, std::string& error)
{
	if (!json.contains(key))
	{
		return true;
	}

	if (!json[key].is_number_integer())
	{
		error = std::string("\"") + key + "\" must be an integer";
		return false;
	}

	out = json[key].get<int>();
	return true;
}

bool ReadOptionalFloat(const nlohmann::json& json, const char* key, float& out, std::string& error)
{
	if (!json.contains(key))
	{
		return true;
	}

	if (!json[key].is_number())
	{
		error = std::string("\"") + key + "\" must be a number";
		return false;
	}

	out = json[key].get<float>();
	return true;
}

bool ReadOptionalString(const nlohmann::json& json, const char* key, std::string& out, std::string& error)
{
	if (!json.contains(key))
	{
		return true;
	}

	if (!json[key].is_string())
	{
		error = std::string("\"") + key + "\" must be a string";
		return false;
	}

	out = json[key].get<std::string>();
	return true;
}

// Does this action fire once per press (a switch), or track a value
// continuously (a knob/expression pedal)?
//
// **Every value-carrying action has to be listed here**, not just the first
// one anyone wrote. A continuous action left out reads as a switch: an
// expression pedal at 0 looks like a release and never fires, and one held
// still repeats nothing -- so the control simply stops responding below
// halfway, which is a symptom nobody would connect back to this function.
bool IsSwitchAction(MidiAction action)
{
	switch (action)
	{
		case MidiAction::SetControl:
		case MidiAction::SetInputGain:
		case MidiAction::SetOutputGain:
		case MidiAction::SetSplitterControl:
		case MidiAction::SetMixerLevel:
		case MidiAction::SetTempo:
			return false;

		case MidiAction::LoadPreset:
		case MidiAction::NextPreset:
		case MidiAction::PrevPreset:
		case MidiAction::ToggleBypass:
		case MidiAction::NextBank:
		case MidiAction::PrevBank:
		case MidiAction::ToggleMetronome:
			break;
	}

	return true;
}

}  // namespace

const char* ToString(MidiAction action)
{
	switch (action)
	{
		case MidiAction::LoadPreset:
			return "load_preset";
		case MidiAction::NextPreset:
			return "next_preset";
		case MidiAction::PrevPreset:
			return "prev_preset";
		case MidiAction::ToggleBypass:
			return "toggle_bypass";
		case MidiAction::SetControl:
			return "set_control";
		case MidiAction::NextBank:
			return "next_bank";
		case MidiAction::PrevBank:
			return "prev_bank";
		case MidiAction::SetInputGain:
			return "set_input_gain";
		case MidiAction::SetOutputGain:
			return "set_output_gain";
		case MidiAction::SetSplitterControl:
			return "set_splitter";
		case MidiAction::SetMixerLevel:
			return "set_mixer";
		case MidiAction::SetTempo:
			return "set_tempo";
		case MidiAction::ToggleMetronome:
			return "toggle_metronome";
	}

	return "unknown";
}

// Every action, in one place, so that adding one to the enum and forgetting to
// make it parseable is a compile-time omission rather than a mapping file that
// mysteriously won't load.
const MidiAction kAllActions[] = {MidiAction::LoadPreset, MidiAction::NextPreset, MidiAction::PrevPreset,
	MidiAction::ToggleBypass, MidiAction::SetControl, MidiAction::NextBank, MidiAction::PrevBank,
	MidiAction::SetInputGain, MidiAction::SetOutputGain, MidiAction::SetSplitterControl, MidiAction::SetMixerLevel,
	MidiAction::SetTempo, MidiAction::ToggleMetronome};

bool ParseMidiAction(const std::string& text, MidiAction& out)
{
	for (MidiAction action : kAllActions)
	{
		if (text == ToString(action))
		{
			out = action;
			return true;
		}
	}

	return false;
}

// The list of names an error message offers when it rejects one. Built rather
// than written out, so it cannot drift from what is actually accepted -- the
// old hand-written list would have gone stale the moment this grew.
std::string KnownActions()
{
	std::string names;

	for (MidiAction action : kAllActions)
	{
		if (!names.empty())
		{
			names += ", ";
		}

		names += ToString(action);
	}

	return names;
}

bool MidiBinding::Matches(const MidiEvent& event) const
{
	if (channel != kAnyChannel && channel != event.channel)
	{
		return false;
	}

	if (number != event.number)
	{
		return false;
	}

	// A note-driven switch binds as note_on, but its NoteOff is what tells us
	// the switch was released -- so it has to reach the same binding.
	if (type == MidiEventType::NoteOn && event.type == MidiEventType::NoteOff)
	{
		return true;
	}

	return type == event.type;
}

bool MidiBinding::operator==(const MidiBinding& other) const
{
	return type == other.type && channel == other.channel && number == other.number && action == other.action &&
		preset == other.preset && stage == other.stage && port == other.port && minimum == other.minimum &&
		maximum == other.maximum;
}

nlohmann::json MidiBinding::ToJson() const
{
	nlohmann::json json;
	json["type"] = BindingTypeName(type);

	// Omit an omni channel rather than writing -1: absence is what the reader
	// understands as "any", and round-tripping through -1 would be a lie
	// about what was configured.
	if (channel != kAnyChannel)
	{
		json["channel"] = channel;
	}

	json["number"] = number;
	json["action"] = ToString(action);

	// Only emit the target fields the action actually uses, so a mapping file
	// doesn't imply that, say, a next_preset binding cares about "stage".
	switch (action)
	{
		case MidiAction::LoadPreset:
			json["preset"] = preset;
			break;

		case MidiAction::ToggleBypass:
			json["stage"] = stage;
			break;

		case MidiAction::SetControl:
			json["stage"] = stage;
			json["port"] = port;
			json["min"] = minimum;
			json["max"] = maximum;
			break;

		// Named controls on board-wide things: which one, and the range a
		// pedal sweeps into. No stage, because none of these lives in the
		// chain.
		case MidiAction::SetSplitterControl:
		case MidiAction::SetMixerLevel:
			json["port"] = port;
			json["min"] = minimum;
			json["max"] = maximum;
			break;

		case MidiAction::SetInputGain:
		case MidiAction::SetOutputGain:
		case MidiAction::SetTempo:
			json["min"] = minimum;
			json["max"] = maximum;
			break;

		case MidiAction::NextPreset:
		case MidiAction::PrevPreset:
		case MidiAction::NextBank:
		case MidiAction::PrevBank:
		case MidiAction::ToggleMetronome:
			break;
	}

	return json;
}

bool MidiBinding::FromJson(const nlohmann::json& json, MidiBinding& out, std::string& error)
{
	out = MidiBinding();

	if (!json.is_object())
	{
		error = "each mapping entry must be an object";
		return false;
	}

	if (!json.contains("type") || !json["type"].is_string())
	{
		error = "mapping entry needs a string \"type\"";
		return false;
	}

	const std::string type_text = json["type"].get<std::string>();

	if (!ParseBindingType(type_text, out.type))
	{
		error = "unknown mapping type \"" + type_text + "\" (expected program_change, cc, or note_on)";
		return false;
	}

	if (!json.contains("action") || !json["action"].is_string())
	{
		error = "mapping entry needs a string \"action\"";
		return false;
	}

	const std::string action_text = json["action"].get<std::string>();

	if (!ParseMidiAction(action_text, out.action))
	{
		error = "unknown action \"" + action_text + "\" (expected one of: " + KnownActions() + ")";
		return false;
	}

	// An omitted channel means "any" -- see kAnyChannel.
	if (!ReadOptionalInt(json, "channel", out.channel, error))
	{
		return false;
	}

	if (out.channel != kAnyChannel && (out.channel < 0 || out.channel > 15))
	{
		error = "channel must be 0-15 (or omitted for any), got " + std::to_string(out.channel);
		return false;
	}

	if (!json.contains("number") || !json["number"].is_number_integer())
	{
		error = "mapping entry needs an integer \"number\"";
		return false;
	}

	out.number = json["number"].get<int>();

	if (out.number < 0 || out.number > 127)
	{
		error = "number must be 0-127, got " + std::to_string(out.number);
		return false;
	}

	if (!ReadOptionalString(json, "preset", out.preset, error) ||
		!ReadOptionalInt(json, "stage", out.stage, error) ||
		!ReadOptionalString(json, "port", out.port, error) ||
		!ReadOptionalFloat(json, "min", out.minimum, error) ||
		!ReadOptionalFloat(json, "max", out.maximum, error))
	{
		return false;
	}

	// Each action needs its own target, and a binding missing one would fail
	// silently at stomp time -- so it's rejected at load time instead.
	switch (out.action)
	{
		case MidiAction::LoadPreset:
			if (out.preset.empty())
			{
				error = "load_preset needs a \"preset\" name";
				return false;
			}
			break;

		case MidiAction::SetControl:
			if (out.port.empty())
			{
				error = "set_control needs a \"port\" symbol";
				return false;
			}

			if (out.minimum == out.maximum)
			{
				error = "set_control needs \"min\" and \"max\" to differ";
				return false;
			}
			break;

		case MidiAction::SetSplitterControl:
			// Which of the splitter's controls, since which ones exist depends
			// on its mode. Checked by name here rather than left to the engine
			// so a typo is a load error with the file in front of you.
			if (out.port != "send_a" && out.port != "send_b" && out.port != "crossover")
			{
				error = "set_splitter needs \"port\" to be send_a, send_b, or crossover, got \"" + out.port + "\"";
				return false;
			}

			if (out.minimum == out.maximum)
			{
				error = "set_splitter needs \"min\" and \"max\" to differ";
				return false;
			}
			break;

		case MidiAction::SetMixerLevel:
			if (out.port != "a" && out.port != "b")
			{
				error = "set_mixer needs \"port\" to be a or b, got \"" + out.port + "\"";
				return false;
			}

			if (out.minimum == out.maximum)
			{
				error = "set_mixer needs \"min\" and \"max\" to differ";
				return false;
			}
			break;

		case MidiAction::SetInputGain:
		case MidiAction::SetOutputGain:
		case MidiAction::SetTempo:
			if (out.minimum == out.maximum)
			{
				error = std::string(ToString(out.action)) + " needs \"min\" and \"max\" to differ";
				return false;
			}
			break;

		case MidiAction::ToggleBypass:
			break;

		case MidiAction::NextPreset:
		case MidiAction::PrevPreset:
		case MidiAction::NextBank:
		case MidiAction::PrevBank:
		case MidiAction::ToggleMetronome:
			break;
	}

	if ((out.action == MidiAction::ToggleBypass || out.action == MidiAction::SetControl) && out.stage < 0)
	{
		error = "\"stage\" must be >= 0, got " + std::to_string(out.stage);
		return false;
	}

	// A program change carries no value, so it can't drive a continuous
	// control. Better to say so than to let it jam the port at min forever.
	// Every action that reads a value has the same problem, so they are all
	// refused rather than only the first one anyone thought of.
	const bool continuous = out.action == MidiAction::SetControl || out.action == MidiAction::SetInputGain ||
		out.action == MidiAction::SetOutputGain || out.action == MidiAction::SetSplitterControl ||
		out.action == MidiAction::SetMixerLevel || out.action == MidiAction::SetTempo;

	if (continuous && out.type == MidiEventType::ProgramChange)
	{
		error = std::string(ToString(out.action)) +
			" cannot be driven by program_change (it carries no value); use cc";
		return false;
	}

	return true;
}

bool MidiMap::Add(const MidiBinding& binding, std::string& error)
{
	for (const MidiBinding& existing : bindings_)
	{
		// Two bindings on the same message would make the outcome depend on
		// file order, which is not something a user can reason about.
		if (existing.type == binding.type && existing.number == binding.number &&
			(existing.channel == binding.channel || existing.channel == kAnyChannel ||
				binding.channel == kAnyChannel))
		{
			error = std::string("two bindings both claim ") + BindingTypeName(binding.type) + " number " +
				std::to_string(binding.number) +
				(binding.channel == kAnyChannel ? " (any channel)" : " on channel " + std::to_string(binding.channel));

			return false;
		}
	}

	bindings_.push_back(binding);
	held_.push_back(false);

	return true;
}

size_t MidiMap::Bind(const MidiBinding& binding)
{
	size_t replaced = 0;

	for (size_t i = bindings_.size(); i > 0; --i)
	{
		const MidiBinding& existing = bindings_[i - 1];

		const bool same_message = existing.type == binding.type && existing.number == binding.number &&
			(existing.channel == binding.channel || existing.channel == kAnyChannel ||
				binding.channel == kAnyChannel);

		if (!same_message)
		{
			continue;
		}

		bindings_.erase(bindings_.begin() + static_cast<long>(i - 1));
		held_.erase(held_.begin() + static_cast<long>(i - 1));

		++replaced;
	}

	bindings_.push_back(binding);
	held_.push_back(false);

	return replaced;
}

nlohmann::json MidiMap::ToJson() const
{
	nlohmann::json json = nlohmann::json::array();

	for (const MidiBinding& binding : bindings_)
	{
		json.push_back(binding.ToJson());
	}

	return json;
}

bool MidiMap::FromJson(const nlohmann::json& json, MidiMap& out, std::string& error)
{
	out = MidiMap();

	if (!json.is_array())
	{
		error = "mapping file must be a JSON array of bindings";
		return false;
	}

	for (size_t i = 0; i < json.size(); ++i)
	{
		MidiBinding binding;

		if (!MidiBinding::FromJson(json[i], binding, error))
		{
			error = "mapping[" + std::to_string(i) + "]: " + error;
			return false;
		}

		if (!out.Add(binding, error))
		{
			error = "mapping[" + std::to_string(i) + "]: " + error;
			return false;
		}
	}

	return true;
}

bool MidiMap::Load(const std::string& path, std::string& error)
{
	std::ifstream file(path);

	if (!file)
	{
		error = "cannot open '" + path + "'";
		return false;
	}

	nlohmann::json json;

	try
	{
		// Same parse flags as presets: throw on error, allow comments, so an
		// annotated hand-written mapping file works.
		json = nlohmann::json::parse(file, nullptr, true, true);
	}
	catch (const nlohmann::json::parse_error& e)
	{
		error = "invalid JSON in '" + path + "': " + e.what();
		return false;
	}

	if (!FromJson(json, *this, error))
	{
		error = "in '" + path + "': " + error;
		return false;
	}

	return true;
}

bool MidiMap::Save(const std::string& path, std::string& error) const
{
	std::ofstream file(path);

	if (!file)
	{
		error = "cannot write '" + path + "'";
		return false;
	}

	file << ToJson().dump(2) << "\n";

	if (!file)
	{
		error = "error while writing '" + path + "'";
		return false;
	}

	return true;
}

bool MidiMap::Resolve(const MidiEvent& event, MidiActionInvocation& out)
{
	for (size_t i = 0; i < bindings_.size(); ++i)
	{
		const MidiBinding& binding = bindings_[i];

		if (!binding.Matches(event))
		{
			continue;
		}

		if (!IsSwitchAction(binding.action))
		{
			// Continuous: every value is meaningful, including 0 and repeats.
			const float unit = static_cast<float>(event.value) / 127.0f;

			out.binding = &binding;
			out.action = binding.action;
			out.control_value = binding.minimum + unit * (binding.maximum - binding.minimum);

			return true;
		}

		// A program change is a single event with no release, so it always
		// fires; there is no edge to track.
		if (binding.type == MidiEventType::ProgramChange)
		{
			out.binding = &binding;
			out.action = binding.action;
			out.control_value = 0.0f;

			return true;
		}

		// Everything else is a switch: fire on the press edge only. Without
		// this, a momentary footswitch (127 down, 0 up) would fire twice per
		// stomp, and a held switch would repeat on every repeat message.
		const bool pressed =
			event.type == MidiEventType::NoteOff ? false : event.value >= kSwitchThreshold;

		const bool was_held = held_[i];
		held_[i] = pressed;

		if (!pressed || was_held)
		{
			return false;
		}

		out.binding = &binding;
		out.action = binding.action;
		out.control_value = 0.0f;

		return true;
	}

	return false;
}

}  // namespace sf
