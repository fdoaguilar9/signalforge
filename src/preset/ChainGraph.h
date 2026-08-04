#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "PresetStage.h"

namespace sf
{

// The shape of a board: an ordered trunk of nodes, where a node is either a
// plugin or a split into two branches that run in parallel and sum back
// together. Replaces the flat `std::vector<PresetStage>` a preset used to
// carry (PLAN.md §4.2, SF-45).
//
// It is a tree, not a patch bay: no arbitrary node-to-node cables, no cycles,
// no side-chain inputs into a branch. Everything expressible here therefore
// has a defined signal order and can always be built and drawn -- which is
// the property that makes the graph view (Milestone 9, Phase C) a pure
// function of this structure, with no coordinates stored anywhere.
//
// A branch may be *empty*, and that is not a degenerate case to be rejected:
// an empty branch is a straight wire, which is how a dry parallel path is
// spelled. Parallel compression and wet/dry blends are the main reason to
// reach for a split at all, and requiring a placeholder plugin to express one
// would be a worse model, not a stricter one.

using NodeId = uint32_t;

// "No node". Ids start at 1 so that a zeroed struct never names one.
inline constexpr NodeId kNoNode = 0;

// "Not in the chain" from the flat-index lookups.
inline constexpr size_t kNoFlatIndex = static_cast<size_t>(-1);

// "Not a split in this graph" from MergeIndexOf.
inline constexpr size_t kNoMerge = static_cast<size_t>(-1);

enum class NodeKind
{
	Plugin,
	Split
};

struct GraphNode;

using NodeList = std::vector<GraphNode>;

// One side of a split: what it runs through, and how it comes back in.
//
// Gain and mute live on the branch rather than in a separate merge object
// because per-branch level *is* the merge -- a dry/wet blend across a split
// is two gains, and giving it a second concept of its own would only make
// the two ways of saying it disagree.
struct GraphBranch
{
	NodeList nodes;

	float gain_db = 0.0f;
	bool muted = false;

	bool operator==(const GraphBranch& other) const;
	bool operator!=(const GraphBranch& other) const { return !(*this == other); }
};

// How a split divides the signal, and how hard it drives each lane (SF-75).
//
// **Sends live here rather than on the branch beside `gain_db`.** They are two
// halves of one question -- what the splitter does -- and keeping them
// together is what lets the whole thing be absent from a file that never
// chose any of it.
enum class SplitterMode
{
	Full,
	Crossover,
	Stereo
};

enum class SplitterSource
{
	Left,
	Right,
	Sum
};

struct Splitter
{
	SplitterMode mode = SplitterMode::Full;

	// Into each lane, before its plugins. The return side is the branch's own
	// `gain_db`: a send drives what is in the lane, a return decides how much
	// of the result comes back, and with a distorting pedal in between those
	// are not the same knob.
	float send_db[2] = {0.0f, 0.0f};

	// Crossover only, in Hz.
	float frequency = 800.0f;

	// Stereo only, per lane.
	SplitterSource source[2] = {SplitterSource::Left, SplitterSource::Right};

	bool operator==(const Splitter& other) const;
	bool operator!=(const Splitter& other) const { return !(*this == other); }
};

struct GraphNode
{
	NodeId id = kNoNode;
	NodeKind kind = NodeKind::Plugin;

	// Plugin nodes only.
	PresetStage stage;

	// Split nodes only: exactly two, in lane order (branches[0] is drawn
	// above the trunk line, branches[1] below).
	std::vector<GraphBranch> branches;

	// Split nodes only. **An absent `splitter` in a file reads as this**, and
	// a splitter equal to this is written back out as absent -- so every split
	// preset written before SF-75 round-trips byte-identical, the same rule
	// SF-46 established for ids.
	Splitter splitter;

	bool operator==(const GraphNode& other) const;
	bool operator!=(const GraphNode& other) const { return !(*this == other); }
};

// Where in the graph an edit lands: which list, and how far into it.
// `split == kNoNode` means the trunk, in which case `branch` is ignored.
struct GraphPosition
{
	NodeId split = kNoNode;
	unsigned branch = 0;
	size_t index = 0;
};

// One plugin node in signal order, as Flatten() reports it.
struct FlatEntry
{
	NodeId id = kNoNode;
	GraphPosition position;

	// 0 on the trunk, 1 inside a split, 2 inside a split inside a split.
	unsigned depth = 0;
};

// The splitter object's two halves of JSON, shared so that the remembered
// channel layout (SF-86) can carry a splitter through a collapse without a
// second parser that would drift from this one.
//
// `ParseSplitterJson` refuses a bad *value* with the path named; the caller
// decides what that means. `SplitterToJson` writes only the fields that differ
// from the default, so a splitter that chose one thing does not acquire the
// others.
bool ParseSplitterJson(const nlohmann::json& json, const std::string& context, Splitter& out, std::string& error);
nlohmann::json SplitterToJson(const Splitter& splitter);

class ChainGraph
{
public:
	// The linear board every preset written before SF-46 describes. Ids are
	// assigned 1..N in order.
	static ChainGraph FromStages(const std::vector<PresetStage>& stages);

	const NodeList& Trunk() const { return trunk_; }
	NodeList& Trunk() { return trunk_; }

	bool Empty() const { return trunk_.empty(); }

	// True when the board is a plain series of plugins -- no splits at any
	// depth. This is what decides whether a preset saves in the flat `chain`
	// form or the nested `graph` one (SF-46).
	bool IsLinear() const;

	// Every plugin node, in signal order: the trunk in order, and at a split,
	// all of branch 0 then all of branch 1 before the trunk resumes.
	//
	// This ordering is the definition of a "stage index" now that the chain
	// isn't a list: console commands, MIDI bindings, and the degraded-build
	// report all still speak in flat indices, and they all mean this one.
	std::vector<FlatEntry> Flatten() const;

	size_t PluginCount() const;

	// By position in Flatten(). Null when out of range.
	const PresetStage* PluginAt(size_t flat_index) const;
	PresetStage* PluginAt(size_t flat_index);

	// The two directions of the index<->id translation. Together they are what
	// lets the persisted, cross-preset meaning of a binding stay positional
	// while the UI and engine address nodes by identity.
	NodeId NodeAtFlatIndex(size_t flat_index) const;
	size_t FlatIndexOf(NodeId id) const;

	const GraphNode* Find(NodeId id) const;
	GraphNode* Find(NodeId id);

	// Which merge a split node's is, counting splits in a pre-order walk: a
	// split before either of its own branches', the trunk in order. This is
	// the number the built chain knows the same merge by, and the two walks
	// have to stay the same walk -- it is what lets a lane's level reach the
	// audio thread without rebuilding anything (SF-72).
	//
	// kNoMerge when `id` isn't a split in this graph.
	size_t MergeIndexOf(NodeId id) const;

	// Hands out the next id. Ids are never reused within a graph, so a
	// dangling reference to a removed node stays dangling rather than
	// silently coming to mean some later node.
	NodeId AllocateId();

	// Appends a plugin to the end of the trunk -- what "add stage" has always
	// done. Returns the new node's id.
	NodeId AppendPlugin(const PresetStage& stage);

	// Inserts at an explicit position. `at.index` may equal the target list's
	// size (append). Returns false with a reason if the position names a split
	// that isn't there, a branch that isn't 0 or 1, or an index past the end.
	bool InsertPlugin(const GraphPosition& at, const PresetStage& stage, NodeId& out_id, std::string& error);

	// Inserts an empty split -- two dry wires -- at `at`. Both branches start
	// empty on purpose: the split exists first, then pedals go into it.
	bool InsertSplit(const GraphPosition& at, NodeId& out_id, std::string& error);

	// Removes a node by id, wherever it is. Removing a split removes
	// everything in both its branches with it.
	bool Remove(NodeId id, std::string& error);

	// Detaches a node and hands it back **with its identity intact**, so it can
	// be put somewhere else and still be the same pedal (SF-86).
	//
	// This is what `Remove` plus `InsertPlugin` is not: that pair copies the
	// stage's *values* into a freshly numbered node, which is fine when a
	// pedal is being created and wrong when one is being moved. A collapse
	// that renumbered every pedal in a split would leave nothing able to say
	// which channel any of them came from.
	bool TakeNode(NodeId id, GraphNode& out, std::string& error);

	// Puts a node back, keeping the id it already has. The counterpart of
	// TakeNode, and the two are always used together.
	bool InsertNode(const GraphPosition& at, GraphNode node, std::string& error);

	// Moves a node one place earlier (-1) or later (+1) within the list it is
	// already in. Refuses at either end rather than hopping into a neighbouring
	// lane -- moving *across* lanes is a different gesture and is Phase C's
	// (SF-55), not something a "move up" should do by accident.
	bool Move(NodeId id, int delta, std::string& error);

	// Structural checks that the parsers and the editors both have to pass:
	// exactly two branches per split, ids unique and non-zero, and next_id
	// past every id in use.
	bool Validate(std::string& error) const;

	// True when ids are exactly 1..N in flat order on a linear graph, i.e.
	// what FromStages would have produced. When that holds, the flat `chain`
	// form can leave ids out entirely and a preset file that predates SF-46
	// round-trips byte-identical.
	bool HasDefaultIds() const;

	// The nested `graph` form: an array of node objects. Plugin nodes carry
	// `uri` (and the rest of a stage); split nodes carry `branches`.
	nlohmann::json ToJson() const;
	static bool FromJson(const nlohmann::json& json, ChainGraph& out, std::string& error);

	// The flat `chain` form, kept for linear graphs. `with_ids` writes an
	// `id` per entry; callers pass HasDefaultIds() == false.
	nlohmann::json ToFlatJson(bool with_ids) const;
	static bool FromFlatJson(const nlohmann::json& json, ChainGraph& out, std::string& error);

	// Structure only -- `next_id` is deliberately not compared. Two graphs
	// that describe the same board are equal even if one has had a node
	// removed and so hands out higher ids next; otherwise a round-trip
	// through JSON, which recomputes next_id from the ids present, could
	// never compare equal to what it came from.
	bool operator==(const ChainGraph& other) const { return trunk_ == other.trunk_; }
	bool operator!=(const ChainGraph& other) const { return !(*this == other); }

private:
	NodeList trunk_;
	NodeId next_id_ = 1;
};

}  // namespace sf
