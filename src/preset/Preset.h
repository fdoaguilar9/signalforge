#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../audio/NoiseGate.h"
#include "ChainGraph.h"
#include "PresetStage.h"

namespace sf
{

// The channel layout a collapse left behind (SF-86).
//
// Going from two channels to one flattens both lanes into the trunk, which
// throws away the one thing the player cared about: *which* channel each pedal
// was on. This is what makes the trip back a restoration rather than a fresh
// start -- press `2 channels` again and every pedal that is still there returns
// to the lane it came from, in the order it was in, with the splitter and the
// channel levels it had.
//
// Nodes are named by id rather than by position, so pedals added, removed or
// moved while collapsed degrade sensibly: an id that is gone is skipped, and a
// pedal that was never in a lane stays on the trunk.
//
// Absent when there is nothing to remember, and omitted from the file then --
// the same rule ids (SF-46), the splitter (SF-75) and the gate (SF-85) follow,
// so a preset that has never been collapsed is untouched.
struct RememberedSplit
{
	bool present = false;

	// Where the split sat in the trunk, so it comes back in the same place
	// rather than at the end.
	size_t trunk_index = 0;

	Splitter splitter;

	// The return side, per lane.
	float gain_db[2] = {0.0f, 0.0f};
	bool muted[2] = {false, false};

	// The pedals each lane held, in order.
	std::vector<NodeId> lane[2];

	bool operator==(const RememberedSplit& other) const
	{
		return present == other.present && trunk_index == other.trunk_index && splitter == other.splitter &&
			gain_db[0] == other.gain_db[0] && gain_db[1] == other.gain_db[1] && muted[0] == other.muted[0] &&
			muted[1] == other.muted[1] && lane[0] == other.lane[0] && lane[1] == other.lane[1];
	}

	bool operator!=(const RememberedSplit& other) const { return !(*this == other); }
};

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

struct Preset
{
	std::string name;

	// The board. A linear graph is the ordinary case and is what every preset
	// written before SF-46 describes; the split/merge shape is the same field
	// with more in it.
	ChainGraph graph;

	std::optional<MidiTrigger> midi_trigger;

	// The built-in noise gate (SF-85). Per preset rather than board-wide: a
	// high-gain lead needs to gate hard and a clean tone should not gate at
	// all, and one threshold cannot serve both.
	//
	// It is not a stage. A gate is what the board does to its *input*, before
	// anything else, and making it a node would put it on the board where it
	// could be moved somewhere it does not belong.
	NoiseGateSettings gate;

	// The channel layout a collapse left behind (SF-86), or nothing.
	RememberedSplit collapsed_split;

	bool operator==(const Preset& other) const
	{
		return name == other.name && graph == other.graph && midi_trigger == other.midi_trigger &&
			gate == other.gate && collapsed_split == other.collapsed_split;
	}

	bool operator!=(const Preset& other) const { return !(*this == other); }

	// Serializes in whichever of the two chain forms fits: the flat "chain"
	// array when the graph is linear, the nested "graph" array when it isn't.
	// A preset that has never been split therefore keeps the exact file shape
	// it had before this existed, down to the bytes -- the new form costs
	// nothing to anyone not using it.
	nlohmann::json ToJson() const;

	// Returns false with a human-readable reason in `error` rather than
	// throwing: a preset referencing something we can't parse should be
	// reportable to the user, not fatal (see SF-31).
	static bool FromJson(const nlohmann::json& json, Preset& out, std::string& error);
};

bool LoadPresetFile(const std::string& path, Preset& out, std::string& error);
bool SavePresetFile(const std::string& path, const Preset& preset, std::string& error);

}  // namespace sf
