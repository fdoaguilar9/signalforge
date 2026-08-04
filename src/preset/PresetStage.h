#pragma once

#include <map>
#include <string>

namespace sf
{

// One hosted plugin, as a preset records it. Split out of Preset.h so that
// ChainGraph -- which holds these as its plugin nodes -- can be included by
// Preset.h rather than the other way round (SF-45).
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

	bool operator!=(const PresetStage& other) const { return !(*this == other); }
};

}  // namespace sf
