#include "Preset.h"

#include <fstream>

namespace sf
{

namespace
{

bool ParseMidiTrigger(const nlohmann::json& json, MidiTrigger& out, std::string& error)
{
	if (!json.is_object())
	{
		error = "\"midi.trigger\" must be an object";
		return false;
	}

	if (!json.contains("type") || !json["type"].is_string())
	{
		error = "\"midi.trigger\" needs a string \"type\"";
		return false;
	}

	out.type = json["type"].get<std::string>();

	if (json.contains("channel"))
	{
		if (!json["channel"].is_number_integer())
		{
			error = "\"midi.trigger.channel\" must be an integer";
			return false;
		}

		out.channel = json["channel"].get<int>();
	}

	if (json.contains("number"))
	{
		if (!json["number"].is_number_integer())
		{
			error = "\"midi.trigger.number\" must be an integer";
			return false;
		}

		out.number = json["number"].get<int>();
	}

	return true;
}

bool ParseStage(const nlohmann::json& json, size_t index, PresetStage& out, std::string& error)
{
	if (!json.is_object())
	{
		error = "chain[" + std::to_string(index) + "] must be an object";
		return false;
	}

	if (!json.contains("uri") || !json["uri"].is_string())
	{
		error = "chain[" + std::to_string(index) + "] needs a string \"uri\"";
		return false;
	}

	out.uri = json["uri"].get<std::string>();

	if (json.contains("bypassed"))
	{
		if (!json["bypassed"].is_boolean())
		{
			error = "chain[" + std::to_string(index) + "].bypassed must be a boolean";
			return false;
		}

		out.bypassed = json["bypassed"].get<bool>();
	}

	if (json.contains("controls"))
	{
		if (!json["controls"].is_object())
		{
			error = "chain[" + std::to_string(index) + "].controls must be an object";
			return false;
		}

		for (const auto& [symbol, value] : json["controls"].items())
		{
			if (!value.is_number())
			{
				error = "chain[" + std::to_string(index) + "].controls[\"" + symbol + "\"] must be a number";
				return false;
			}

			out.controls[symbol] = value.get<float>();
		}
	}

	if (json.contains("state"))
	{
		if (!json["state"].is_string())
		{
			error = "chain[" + std::to_string(index) + "].state must be a string";
			return false;
		}

		out.state = json["state"].get<std::string>();
	}

	return true;
}

}  // namespace

nlohmann::json Preset::ToJson() const
{
	nlohmann::json json;
	json["name"] = name;

	nlohmann::json chain_json = nlohmann::json::array();

	for (const PresetStage& stage : chain)
	{
		nlohmann::json stage_json;
		stage_json["uri"] = stage.uri;
		stage_json["bypassed"] = stage.bypassed;

		// Always emit "controls" (even empty) so the shape is predictable
		// for anyone hand-editing a preset.
		nlohmann::json controls_json = nlohmann::json::object();

		for (const auto& [symbol, value] : stage.controls)
		{
			controls_json[symbol] = value;
		}

		stage_json["controls"] = controls_json;

		// Omit an empty state blob rather than writing "": nothing to
		// restore is better expressed by absence.
		if (!stage.state.empty())
		{
			stage_json["state"] = stage.state;
		}

		chain_json.push_back(stage_json);
	}

	json["chain"] = chain_json;

	if (midi_trigger)
	{
		nlohmann::json trigger_json;
		trigger_json["type"] = midi_trigger->type;
		trigger_json["channel"] = midi_trigger->channel;
		trigger_json["number"] = midi_trigger->number;

		json["midi"] = nlohmann::json{{"trigger", trigger_json}};
	}

	return json;
}

bool Preset::FromJson(const nlohmann::json& json, Preset& out, std::string& error)
{
	out = Preset();

	if (!json.is_object())
	{
		error = "preset must be a JSON object";
		return false;
	}

	if (json.contains("name"))
	{
		if (!json["name"].is_string())
		{
			error = "\"name\" must be a string";
			return false;
		}

		out.name = json["name"].get<std::string>();
	}

	if (!json.contains("chain"))
	{
		error = "preset needs a \"chain\" array";
		return false;
	}

	if (!json["chain"].is_array())
	{
		error = "\"chain\" must be an array";
		return false;
	}

	for (size_t i = 0; i < json["chain"].size(); ++i)
	{
		PresetStage stage;

		if (!ParseStage(json["chain"][i], i, stage, error))
		{
			return false;
		}

		out.chain.push_back(std::move(stage));
	}

	if (json.contains("midi"))
	{
		if (!json["midi"].is_object())
		{
			error = "\"midi\" must be an object";
			return false;
		}

		if (json["midi"].contains("trigger"))
		{
			MidiTrigger trigger;

			if (!ParseMidiTrigger(json["midi"]["trigger"], trigger, error))
			{
				return false;
			}

			out.midi_trigger = trigger;
		}
	}

	return true;
}

bool LoadPresetFile(const std::string& path, Preset& out, std::string& error)
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
		// Second arg: no callback. Third: throw on error. Fourth: allow
		// comments, so the plan's .jsonc-style annotated examples parse.
		json = nlohmann::json::parse(file, nullptr, true, true);
	}
	catch (const nlohmann::json::parse_error& e)
	{
		error = "invalid JSON in '" + path + "': " + e.what();
		return false;
	}

	if (!Preset::FromJson(json, out, error))
	{
		error = "in '" + path + "': " + error;
		return false;
	}

	return true;
}

bool SavePresetFile(const std::string& path, const Preset& preset, std::string& error)
{
	std::ofstream file(path);

	if (!file)
	{
		error = "cannot write '" + path + "'";
		return false;
	}

	file << preset.ToJson().dump(2) << "\n";

	if (!file)
	{
		error = "error while writing '" + path + "'";
		return false;
	}

	return true;
}

}  // namespace sf
