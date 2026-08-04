#pragma once

#include <string>
#include <vector>

#include "BoardView.h"

namespace sf
{

// The chain, as the design draws it (SF-71, SF-72): a vertical run of stage
// cards from INPUT to OUTPUT, splitting into two lanes side by side when the
// board has a split in it.
//
// Replaces the wired canvas (SF-53..57). What that could draw and this cannot
// is written down in the milestone preamble; the short version is that the
// board here shows **one** split, at one place on the trunk, and says so when
// it is handed anything else rather than drawing part of it.

// What the chain area made of the graph it was given.
//
// Computed fresh every frame, like the layout it replaced: a preset holds no
// shape metadata, and recomputing is what keeps "the board" and "the drawing
// of the board" from being able to disagree.
struct BoardShape
{
	// False when the graph is legal but undrawable here -- two splits on the
	// trunk, or a split inside a lane. `problem` says which, in the words the
	// warning banner uses.
	bool drawable = true;
	std::string problem;

	bool split = false;
	BoardNodeId split_node = kNoBoardNode;

	// Positions in the trunk, so an insertion can name a slot. `split_index`
	// is where the split node sits; pre is everything before it.
	size_t split_index = 0;
	size_t trunk_size = 0;

	// Borrowed from the graph the caller owns for the frame.
	std::vector<const BoardGraphNode*> pre;
	std::vector<const BoardGraphNode*> lane[2];
	std::vector<const BoardGraphNode*> post;

	float gain_db[2] = {0.0f, 0.0f};
	bool muted[2] = {false, false};

	// How the split divides the signal, for the splitter card (SF-75).
	BoardSplitter splitter;

	// Every plugin node the board is showing, whether or not it is running.
	size_t Stages() const;
};

// Reads a graph into the shape above. Pure and side-effect free, so what the
// board can and cannot draw is a question with an answer rather than something
// discovered by looking at the screen.
BoardShape ReadShape(const BoardGraphNodes& trunk);

// Draws the chain area: the signal-path control, the rails, the cards, and --
// when there is a split -- the splitter and mixer cards and the two lanes.
void DrawChain(BoardViewState& state, BoardViewUi& ui);

}  // namespace sf
