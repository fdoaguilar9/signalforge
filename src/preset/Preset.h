#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sf
{

// What makes a preset fire from the pedal. Mirrors PLAN.md §5.1's
// "midi": { "trigger": {...} } block.
struct MidiTrigger
{
	std::string type;  // "program_change", "cc", "note_on"
	int channel = 0;
	int number = 0;

	bool operator==(const MidiTrigger& other) const
	{
		return type == other.type && channel == other.channel && number == other.number;
	}
};

struct PresetStage
{
	std::string uri;
	bool bypassed = false;

	// Control port values by symbol. std::map (not unordered_map) so
	// serialization order is stable -- presets are meant to be diffable in
	// git, and a reordering diff on every save would be noise.
	std::map<std::string, float> controls;

	// Opaque LV2 state blob (lilv_state_to_string output). Carries
	// atom:Path properties like the loaded .nam model. Empty if the stage
	// has no state to restore. Capturing/restoring this is SF-13/SF-14 --
	// here it's just preserved verbatim.
	std::string state;

	bool operator==(const PresetStage& other) const
	{
		return uri == other.uri && bypassed == other.bypassed && controls == other.controls && state == other.state;
	}
};

struct Preset
{
	std::string name;
	std::vector<PresetStage> chain;
	std::optional<MidiTrigger> midi_trigger;

	bool operator==(const Preset& other) const
	{
		return name == other.name && chain == other.chain && midi_trigger == other.midi_trigger;
	}

	nlohmann::json ToJson() const;

	// Returns false with a human-readable reason in `error` rather than
	// throwing: a preset referencing something we can't parse should be
	// reportable to the user, not fatal (see SF-31).
	static bool FromJson(const nlohmann::json& json, Preset& out, std::string& error);
};

bool LoadPresetFile(const std::string& path, Preset& out, std::string& error);
bool SavePresetFile(const std::string& path, const Preset& preset, std::string& error);

}  // namespace sf
