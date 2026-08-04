#pragma once

#include <string>

#include "BoardView.h"

namespace sf
{

// One stage of the board, drawn as a card (SF-70).
//
// Replaces the wired canvas's tile plus the single selected-pedal panel that
// sat under it: every card is expandable in place, so the board shows as many
// stages' controls at once as the player wants rather than exactly one.

// Where a card sits, which is all the card needs to know about the shape
// around it. Computed by the chain area, because "first" means first *in its
// own lane* and a card has no way to find that out for itself.
struct StageCardPlace
{
	// The code in the card's top-right corner: "0", "1", "A0", "B1".
	//
	// **A position, not a MIDI index.** For a stage on the trunk the two agree,
	// but a stage in a lane is `A0` here and some other number to a binding --
	// the flat index counts all of lane A, then all of lane B, then the trunk
	// resumes. The flat index is in the card's tooltip, where the discrepancy
	// can be seen rather than guessed at.
	std::string code;

	// Whether `^` and `v` have anywhere to go. A card moves within its own
	// lane; crossing into the sibling lane is a different gesture and is not
	// this one.
	bool first = false;
	bool last = false;

	// Removing this one would leave the chain empty, which the engine has no
	// ports for.
	bool only_stage = false;

	// Where in the running chain to find this node's controls, or kNoPreset
	// when the preset names a plugin that isn't installed.
	size_t stage = kNoPreset;
};

// Draws one card. Reports any edit through `state`, at most one per frame, the
// way every other edit on the board does.
void DrawStageCard(BoardViewState& state, BoardViewUi& ui, BoardNodeId node, const StageCardPlace& place);

// The two halves of an expanded card's body, on their own so the plugin-UI
// modal can show the *same* rows rather than a second set beside them (SF-79).
//
// Shared rather than reimplemented deliberately: these report edits through the
// same `state` fields, so a knob moved in the modal reaches the running plugin
// by the path that already works. Two implementations would be two chances for
// one of them to stop.
//
// `probe_prefix` keeps the two contexts' widgets apart. **No two widgets may
// answer to the same probe name** -- a test clicks whichever it finds first,
// and with the card expanded behind the modal that is the one nobody can
// reach. The card passes nothing; the modal passes its own prefix.
void DrawStagePaths(BoardViewState& state, BoardViewUi& ui, size_t stage_index, const BoardStage& stage,
	const char* probe_prefix = "");
void DrawStageControls(
	BoardViewState& state, size_t stage_index, const BoardStage& stage, const char* probe_prefix = "");

}  // namespace sf
