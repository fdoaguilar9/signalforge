#pragma once

#include <string>

#include "ChainGraph.h"
#include "Preset.h"

namespace sf
{

// Collapsing a split to a series, and putting it back (SF-58, SF-86).
//
// Here rather than in main.cpp because it is graph surgery: it takes a
// ChainGraph and a remembered layout and touches nothing else -- no engine, no
// plugins, no audio -- which is what lets the round trip a player actually
// cares about ("collapse to one channel and back, and my pedals are where I
// left them") be asserted rather than clicked through.

// Collapses the split `id` into the trunk, lane 0's pedals then lane 1's,
// spliced in where the split was. Nested splits flatten with it.
//
// `remembered`, when given, is filled in with enough to undo this: which pedals
// were on which channel, where the split sat, and what the splitter and the two
// returns were set to. Pass null to collapse without recording.
//
// **Pedals keep their ids.** The nodes are moved rather than copied, which is
// what makes the memory able to name them afterwards.
bool RejoinSplit(ChainGraph& graph, NodeId id, RememberedSplit* remembered, std::string& error);

// Rebuilds the split a collapse remembered.
//
// Every pedal whose id is still on the trunk goes back to the lane it came
// from, in the order it was in. Pedals removed since are skipped; pedals added
// while collapsed stay on the trunk. So a board edited while flattened comes
// back as much of its old shape as still makes sense, rather than refusing.
//
// Returns false when there is nothing left to restore -- the caller's cue to
// make a plain empty split instead.
bool RestoreSplit(ChainGraph& graph, const RememberedSplit& remembered, std::string& error);

}  // namespace sf
