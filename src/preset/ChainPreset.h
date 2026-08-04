#pragma once

#include <memory>
#include <string>

#include "../lv2/Chain.h"
#include "../lv2/UridMap.h"
#include "Preset.h"

namespace sf
{

// Bridges the LV2 hosting layer and the preset data model. Lives here, in
// the preset layer, so lv2/ stays a standalone plugin-hosting library with
// no knowledge of our file format.

// Snapshots a live chain into a preset: each stage's URI, its input control
// port values, and its serialized LV2 state (model/IR paths and any other
// plugin-internal state).
Preset CaptureChainPreset(const Chain& chain, UridMap& urid_map, const std::string& name);

// The same snapshot, taken *into* the preset the chain was built from, so the
// board keeps its shape.
//
// CaptureChainPreset above can only ever produce a series: a running Chain is
// a flat list of stages, and the split/merge structure it was built from
// isn't recoverable from it. Editing a board means capturing what the player
// has changed by hand (knobs, bypasses, a loaded model) *without* flattening
// their splits, so the source preset supplies the shape and the chain
// supplies the values.
//
// `skipped_indices` is ChainBuildReport's: those flat positions had no plugin
// installed, so there is no stage to read them back from and the file's own
// values are left alone.
Preset CaptureChainPresetInto(const Preset& source, const Chain& chain, UridMap& urid_map,
	const std::vector<size_t>& skipped_indices);

// What a build had to leave out. A preset naming a plugin that isn't
// installed is not an error the player can do anything about mid-song, so the
// chain is built without that stage and this says which ones went (SF-31).
struct ChainBuildReport
{
	// Flat (signal-order) positions in the preset's graph, ascending, of the
	// stages that were skipped,
	// and their URIs in the same order. Positions, because the surviving
	// stages have shifted down and the file still has them where they were.
	std::vector<size_t> skipped_indices;
	std::vector<std::string> skipped_uris;

	// How many splits were collapsed back to a series because dropping a
	// missing plugin emptied one of their lanes (SF-58).
	//
	// **An authored empty lane is not this.** A branch with nothing in it is a
	// dry wire and a perfectly ordinary way to spell a parallel blend, so it
	// is kept. What is collapsed is a lane that *had* something and lost it:
	// the split it belonged to would otherwise sum a dry copy against the
	// other lane at +6 dB, which is not the board anyone saved.
	size_t collapsed_splits = 0;

	bool Degraded() const { return !skipped_indices.empty(); }
};

// Instantiates the preset's chain and applies each stage's saved state and
// control values. Returns nullptr on failure, with a reason in `error`.
//
// Stages whose plugin isn't installed are skipped rather than fatal: a board
// missing one pedal still plays, and the alternative is a preset that can't be
// loaded at all. They are named on stderr, and in `report` when one is
// supplied -- a caller with a UI has to be able to say so on screen. A preset
// where *every* plugin is missing still fails; there is no chain left.
//
// Note the ordering: LV2 state is restored first, then the preset's explicit
// controls, so `controls` in the file is authoritative if a state blob also
// happens to carry port values.
//
// Worker-based loading (models, IRs) is asynchronous; this calls
// Chain::Settle() so the returned chain is ready to process.
std::unique_ptr<Chain> BuildChainFromPreset(LilvWorld* world, const Preset& preset, double sample_rate,
	uint32_t max_block_size, UridMap& urid_map, std::string& error, ChainBuildReport* report = nullptr,
	unsigned board_inputs = kAutoBoardInputs);

}  // namespace sf
