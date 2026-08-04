#include "Preset.h"

#include <algorithm>

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

}  // namespace

nlohmann::json Preset::ToJson() const
{
	nlohmann::json json;
	json["name"] = name;

	if (graph.IsLinear())
	{
		// Ids are written only once they stop being positional -- i.e. once
		// the board has been edited in a way that shuffled them. Until then a
		// preset file looks exactly as it did before SF-46.
		json["chain"] = graph.ToFlatJson(!graph.HasDefaultIds());
	}
	else
	{
		json["graph"] = graph.ToJson();
	}

	// Written only when it says something. A gate equal to the default is what
	// an absent one means, so writing it out would add a block to every preset
	// ever saved and change none of their meaning -- the same rule ids (SF-46)
	// and the splitter (SF-75) follow.
	if (gate != NoiseGateSettings())
	{
		json["gate"] = nlohmann::json{
			{"enabled", gate.enabled},
			{"threshold_db", gate.threshold_db},
			{"release_ms", gate.release_ms},
		};
	}

	// The channel layout a collapse left behind (SF-86). Written only when
	// there is one, so a preset that has never been collapsed is untouched.
	if (collapsed_split.present)
	{
		nlohmann::json lanes = nlohmann::json::array();

		for (unsigned b = 0; b < 2; ++b)
		{
			nlohmann::json lane;
			lane["gain_db"] = collapsed_split.gain_db[b];
			lane["muted"] = collapsed_split.muted[b];
			lane["nodes"] = collapsed_split.lane[b];

			lanes.push_back(std::move(lane));
		}

		nlohmann::json remembered;
		remembered["at"] = collapsed_split.trunk_index;
		remembered["branches"] = std::move(lanes);

		// The splitter travels with it, so a crossover survives the round trip
		// rather than coming back at full range.
		if (collapsed_split.splitter != Splitter())
		{
			remembered["splitter"] = SplitterToJson(collapsed_split.splitter);
		}

		json["collapsed_split"] = std::move(remembered);
	}

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

	const bool has_graph = json.contains("graph");
	const bool has_chain = json.contains("chain");

	// Both would leave which one is the board to chance, and the two would
	// drift the moment either is hand-edited.
	if (has_graph && has_chain)
	{
		error = "preset has both \"chain\" and \"graph\" -- it can only have one";
		return false;
	}

	if (!has_graph && !has_chain)
	{
		error = "preset needs a \"chain\" array (or a \"graph\" one)";
		return false;
	}

	if (has_graph)
	{
		if (!ChainGraph::FromJson(json["graph"], out.graph, error))
		{
			return false;
		}
	}
	else if (!ChainGraph::FromFlatJson(json["chain"], out.graph, error))
	{
		return false;
	}

	// An absent gate is a disabled one with defaults, which is what every
	// preset written before SF-85 means.
	if (json.contains("gate"))
	{
		const nlohmann::json& gate_json = json["gate"];

		if (!gate_json.is_object())
		{
			error = "\"gate\" must be an object";
			return false;
		}

		if (gate_json.contains("enabled"))
		{
			if (!gate_json["enabled"].is_boolean())
			{
				error = "\"gate.enabled\" must be a boolean";
				return false;
			}

			out.gate.enabled = gate_json["enabled"].get<bool>();
		}

		// The two numbers are **clamped, not refused**. A threshold has silly
		// values rather than wrong ones -- somebody typing -200 meant "as low
		// as it goes" -- and taking the preset away over it would be a worse
		// answer than giving them the bottom of the range. A value of the
		// wrong *type* is a different thing and is still refused.
		if (gate_json.contains("threshold_db"))
		{
			if (!gate_json["threshold_db"].is_number())
			{
				error = "\"gate.threshold_db\" must be a number";
				return false;
			}

			out.gate.threshold_db = std::min(
				std::max(gate_json["threshold_db"].get<float>(), kGateMinThresholdDb), kGateMaxThresholdDb);
		}

		if (gate_json.contains("release_ms"))
		{
			if (!gate_json["release_ms"].is_number())
			{
				error = "\"gate.release_ms\" must be a number";
				return false;
			}

			out.gate.release_ms =
				std::min(std::max(gate_json["release_ms"].get<float>(), kGateMinReleaseMs), kGateMaxReleaseMs);
		}
	}

	// An absent one simply means nothing was collapsed. A malformed one is
	// **ignored rather than refused**, unlike everything else here: this is a
	// convenience the board left for itself, not something the player wrote,
	// and losing a preset over it would be a poor trade for losing a layout.
	if (json.contains("collapsed_split") && json["collapsed_split"].is_object())
	{
		const nlohmann::json& remembered = json["collapsed_split"];

		RememberedSplit parsed;
		parsed.present = true;

		if (remembered.contains("at") && remembered["at"].is_number_unsigned())
		{
			parsed.trunk_index = remembered["at"].get<size_t>();
		}

		if (remembered.contains("splitter"))
		{
			std::string ignored;

			if (!ParseSplitterJson(remembered["splitter"], "collapsed_split.splitter", parsed.splitter, ignored))
			{
				parsed.splitter = Splitter();
			}
		}

		if (remembered.contains("branches") && remembered["branches"].is_array() &&
			remembered["branches"].size() == 2)
		{
			for (unsigned b = 0; b < 2; ++b)
			{
				const nlohmann::json& lane = remembered["branches"][b];

				if (!lane.is_object())
				{
					continue;
				}

				if (lane.contains("gain_db") && lane["gain_db"].is_number())
				{
					parsed.gain_db[b] = lane["gain_db"].get<float>();
				}

				if (lane.contains("muted") && lane["muted"].is_boolean())
				{
					parsed.muted[b] = lane["muted"].get<bool>();
				}

				if (lane.contains("nodes") && lane["nodes"].is_array())
				{
					for (const nlohmann::json& node : lane["nodes"])
					{
						if (node.is_number_unsigned())
						{
							parsed.lane[b].push_back(node.get<NodeId>());
						}
					}
				}
			}
		}

		// Nothing to put back is the same as nothing remembered.
		if (!parsed.lane[0].empty() || !parsed.lane[1].empty())
		{
			out.collapsed_split = std::move(parsed);
		}
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
