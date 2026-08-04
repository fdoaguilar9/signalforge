#include "ChainGraph.h"

#include <algorithm>
#include <functional>

namespace sf
{

namespace
{

// Errors name the exact JSON path they came from ("graph[1].branches[0].
// nodes[2].uri"), because a hand-edited board is expected to have mistakes in
// it and "invalid node" is not enough to find one.
std::string Child(const std::string& context, const std::string& suffix)
{
	return context + suffix;
}

bool ParseStage(const nlohmann::json& json, const std::string& context, PresetStage& out, std::string& error)
{
	if (!json.contains("uri") || !json["uri"].is_string())
	{
		error = context + " needs a string \"uri\"";
		return false;
	}

	out.uri = json["uri"].get<std::string>();

	if (json.contains("bypassed"))
	{
		if (!json["bypassed"].is_boolean())
		{
			error = Child(context, ".bypassed") + " must be a boolean";
			return false;
		}

		out.bypassed = json["bypassed"].get<bool>();
	}

	if (json.contains("controls"))
	{
		if (!json["controls"].is_object())
		{
			error = Child(context, ".controls") + " must be an object";
			return false;
		}

		for (const auto& [symbol, value] : json["controls"].items())
		{
			if (!value.is_number())
			{
				error = Child(context, ".controls[\"" + symbol + "\"]") + " must be a number";
				return false;
			}

			out.controls[symbol] = value.get<float>();
		}
	}

	if (json.contains("state"))
	{
		if (!json["state"].is_string())
		{
			error = Child(context, ".state") + " must be a string";
			return false;
		}

		out.state = json["state"].get<std::string>();
	}

	return true;
}

nlohmann::json StageToJson(const PresetStage& stage)
{
	nlohmann::json json;
	json["uri"] = stage.uri;
	json["bypassed"] = stage.bypassed;

	// Always emit "controls" (even empty) so the shape is predictable for
	// anyone hand-editing a preset.
	nlohmann::json controls_json = nlohmann::json::object();

	for (const auto& [symbol, value] : stage.controls)
	{
		controls_json[symbol] = value;
	}

	json["controls"] = controls_json;

	// Omit an empty state blob rather than writing "": nothing to restore is
	// better expressed by absence.
	if (!stage.state.empty())
	{
		json["state"] = stage.state;
	}

	return json;
}

const GraphNode* FindIn(const NodeList& nodes, NodeId id)
{
	for (const GraphNode& node : nodes)
	{
		if (node.id == id)
		{
			return &node;
		}

		if (node.kind != NodeKind::Split)
		{
			continue;
		}

		for (const GraphBranch& branch : node.branches)
		{
			if (const GraphNode* found = FindIn(branch.nodes, id))
			{
				return found;
			}
		}
	}

	return nullptr;
}

void FlattenInto(const NodeList& nodes, NodeId owner_split, unsigned owner_branch, unsigned depth,
	std::vector<FlatEntry>& out)
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		const GraphNode& node = nodes[i];

		if (node.kind == NodeKind::Plugin)
		{
			FlatEntry entry;
			entry.id = node.id;
			entry.position.split = owner_split;
			entry.position.branch = owner_branch;
			entry.position.index = i;
			entry.depth = depth;

			out.push_back(entry);
			continue;
		}

		// A split contributes nothing itself; both of its branches run before
		// the trunk resumes, which is what makes the flat order a signal order
		// and not just a traversal.
		for (unsigned b = 0; b < node.branches.size(); ++b)
		{
			FlattenInto(node.branches[b].nodes, node.id, b, depth + 1, out);
		}
	}
}

bool AnySplit(const NodeList& nodes)
{
	for (const GraphNode& node : nodes)
	{
		if (node.kind == NodeKind::Split)
		{
			return true;
		}
	}

	return false;
}

void CollectIds(const NodeList& nodes, std::vector<NodeId>& out)
{
	for (const GraphNode& node : nodes)
	{
		out.push_back(node.id);

		for (const GraphBranch& branch : node.branches)
		{
			CollectIds(branch.nodes, out);
		}
	}
}

bool RemoveIn(NodeList& nodes, NodeId id)
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		if (nodes[i].id == id)
		{
			nodes.erase(nodes.begin() + static_cast<long>(i));
			return true;
		}

		for (GraphBranch& branch : nodes[i].branches)
		{
			if (RemoveIn(branch.nodes, id))
			{
				return true;
			}
		}
	}

	return false;
}

// The list a node sits in, plus where in it -- what a move needs and a plain
// Find() can't give, since a GraphNode doesn't know its own container.
bool LocateIn(NodeList& nodes, NodeId id, NodeList*& out_list, size_t& out_index)
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		if (nodes[i].id == id)
		{
			out_list = &nodes;
			out_index = i;
			return true;
		}

		for (GraphBranch& branch : nodes[i].branches)
		{
			if (LocateIn(branch.nodes, id, out_list, out_index))
			{
				return true;
			}
		}
	}

	return false;
}

bool ValidateIn(const NodeList& nodes, const std::string& context, std::string& error)
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		const GraphNode& node = nodes[i];
		const std::string here = context + "[" + std::to_string(i) + "]";

		if (node.id == kNoNode)
		{
			error = here + " has no id";
			return false;
		}

		if (node.kind == NodeKind::Plugin)
		{
			if (!node.branches.empty())
			{
				error = here + " is a plugin node but has branches";
				return false;
			}

			if (node.stage.uri.empty())
			{
				error = here + " has an empty uri";
				return false;
			}

			continue;
		}

		if (node.branches.size() != 2)
		{
			error = here + " is a split and must have exactly 2 branches, has " +
				std::to_string(node.branches.size());
			return false;
		}

		for (unsigned b = 0; b < 2; ++b)
		{
			if (!ValidateIn(node.branches[b].nodes, here + ".branches[" + std::to_string(b) + "].nodes", error))
			{
				return false;
			}
		}
	}

	return true;
}

bool ParseNodeList(const nlohmann::json& json, const std::string& context, NodeList& out, std::string& error);



bool ParseNode(const nlohmann::json& json, const std::string& context, GraphNode& out, std::string& error)
{
	if (!json.is_object())
	{
		error = context + " must be an object";
		return false;
	}

	if (json.contains("id"))
	{
		if (!json["id"].is_number_unsigned() || json["id"].get<uint64_t>() == 0)
		{
			error = Child(context, ".id") + " must be a positive integer";
			return false;
		}

		out.id = json["id"].get<NodeId>();
	}

	// A node is a split if it says which two ways the signal goes, and a
	// plugin otherwise. Discriminating on the field that carries the meaning
	// beats a "kind": "split" string that could disagree with it.
	if (json.contains("branches"))
	{
		out.kind = NodeKind::Split;

		if (!json["branches"].is_array() || json["branches"].size() != 2)
		{
			error = Child(context, ".branches") + " must be an array of exactly 2 branches";
			return false;
		}

		out.branches.resize(2);

		for (unsigned b = 0; b < 2; ++b)
		{
			const nlohmann::json& branch_json = json["branches"][b];
			const std::string here = context + ".branches[" + std::to_string(b) + "]";

			if (!branch_json.is_object())
			{
				error = here + " must be an object";
				return false;
			}

			GraphBranch& branch = out.branches[b];

			if (branch_json.contains("gain_db"))
			{
				if (!branch_json["gain_db"].is_number())
				{
					error = Child(here, ".gain_db") + " must be a number";
					return false;
				}

				branch.gain_db = branch_json["gain_db"].get<float>();
			}

			if (branch_json.contains("muted"))
			{
				if (!branch_json["muted"].is_boolean())
				{
					error = Child(here, ".muted") + " must be a boolean";
					return false;
				}

				branch.muted = branch_json["muted"].get<bool>();
			}

			// An absent or empty "nodes" is a branch that is just a wire --
			// the dry side of a parallel blend. Legal, and common.
			if (branch_json.contains("nodes"))
			{
				if (!ParseNodeList(branch_json["nodes"], here + ".nodes", branch.nodes, error))
				{
					return false;
				}
			}
		}

		// An absent splitter is full range with unity sends, which is what
		// every split written before SF-75 meant.
		if (json.contains("splitter") && !ParseSplitterJson(json["splitter"], Child(context, ".splitter"), out.splitter, error))
		{
			return false;
		}

		return true;
	}

	out.kind = NodeKind::Plugin;

	return ParseStage(json, context, out.stage, error);
}

bool ParseNodeList(const nlohmann::json& json, const std::string& context, NodeList& out, std::string& error)
{
	if (!json.is_array())
	{
		error = context + " must be an array";
		return false;
	}

	out.clear();
	out.reserve(json.size());

	for (size_t i = 0; i < json.size(); ++i)
	{
		GraphNode node;

		if (!ParseNode(json[i], context + "[" + std::to_string(i) + "]", node, error))
		{
			return false;
		}

		out.push_back(std::move(node));
	}

	return true;
}


nlohmann::json NodeToJson(const GraphNode& node, bool with_id)
{
	nlohmann::json json;

	if (node.kind == NodeKind::Plugin)
	{
		json = StageToJson(node.stage);
	}
	else
	{
		nlohmann::json branches_json = nlohmann::json::array();

		for (const GraphBranch& branch : node.branches)
		{
			nlohmann::json branch_json;
			branch_json["gain_db"] = branch.gain_db;
			branch_json["muted"] = branch.muted;

			nlohmann::json nodes_json = nlohmann::json::array();

			for (const GraphNode& child : branch.nodes)
			{
				nodes_json.push_back(NodeToJson(child, true));
			}

			branch_json["nodes"] = nodes_json;
			branches_json.push_back(branch_json);
		}

		json["branches"] = branches_json;

		// Written only when it says something. A splitter equal to the default
		// is what an absent one means, so writing it out would add a line to
		// every split preset ever saved and change none of their meaning --
		// the same rule ids follow (SF-46).
		if (node.splitter != Splitter())
		{
			json["splitter"] = SplitterToJson(node.splitter);
		}
	}

	if (with_id)
	{
		// First, so a node reads id-then-what-it-is rather than burying its
		// identity after a state blob.
		nlohmann::json ordered;
		ordered["id"] = node.id;

		for (const auto& [key, value] : json.items())
		{
			ordered[key] = value;
		}

		return ordered;
	}

	return json;
}

// Ids only have to be assigned to nodes that arrived without one -- the flat
// `chain` form leaves them out when they're positional (SF-46).
void AssignMissingIds(NodeList& nodes, NodeId& next_id)
{
	for (GraphNode& node : nodes)
	{
		if (node.id == kNoNode)
		{
			node.id = next_id++;
		}

		for (GraphBranch& branch : node.branches)
		{
			AssignMissingIds(branch.nodes, next_id);
		}
	}
}

}  // namespace

// Shared with Preset.cpp so the remembered channel layout (SF-86) carries a
// splitter through a collapse without a second parser to drift from this one.
// The splitter object (SF-75). Every field is optional and defaults to what a
// split meant before any of them existed, so a file that says
// `"splitter": {}` is exactly a file that says nothing.
//
// Unknown *values* are refused rather than ignored. A typo in a mode name is
// the difference between two amps and one, and silently reading "crosover" as
// full range would be a board that sounds wrong for a reason nothing reported.
bool ParseSplitterJson(const nlohmann::json& json, const std::string& context, Splitter& out, std::string& error)
{
	if (!json.is_object())
	{
		error = context + " must be an object";
		return false;
	}

	if (json.contains("mode"))
	{
		if (!json["mode"].is_string())
		{
			error = Child(context, ".mode") + " must be a string";
			return false;
		}

		const std::string mode = json["mode"].get<std::string>();

		if (mode == "full")
		{
			out.mode = SplitterMode::Full;
		}
		else if (mode == "crossover")
		{
			out.mode = SplitterMode::Crossover;
		}
		else if (mode == "stereo")
		{
			out.mode = SplitterMode::Stereo;
		}
		else
		{
			error = Child(context, ".mode") + " must be \"full\", \"crossover\" or \"stereo\", not \"" + mode + "\"";
			return false;
		}
	}

	static const char* const kSends[2] = {"send_a", "send_b"};
	static const char* const kSources[2] = {"src_a", "src_b"};

	for (unsigned lane = 0; lane < 2; ++lane)
	{
		if (json.contains(kSends[lane]))
		{
			if (!json[kSends[lane]].is_number())
			{
				error = Child(context, std::string(".") + kSends[lane]) + " must be a number";
				return false;
			}

			out.send_db[lane] = json[kSends[lane]].get<float>();
		}

		if (!json.contains(kSources[lane]))
		{
			continue;
		}

		if (!json[kSources[lane]].is_string())
		{
			error = Child(context, std::string(".") + kSources[lane]) + " must be a string";
			return false;
		}

		const std::string source = json[kSources[lane]].get<std::string>();

		if (source == "left")
		{
			out.source[lane] = SplitterSource::Left;
		}
		else if (source == "right")
		{
			out.source[lane] = SplitterSource::Right;
		}
		else if (source == "sum")
		{
			out.source[lane] = SplitterSource::Sum;
		}
		else
		{
			error = Child(context, std::string(".") + kSources[lane]) +
				" must be \"left\", \"right\" or \"sum\", not \"" + source + "\"";
			return false;
		}
	}

	if (json.contains("freq"))
	{
		if (!json["freq"].is_number())
		{
			error = Child(context, ".freq") + " must be a number";
			return false;
		}

		const float frequency = json["freq"].get<float>();

		// Refused here rather than clamped at the filter. A file is edited
		// once and read forever; a number that means nothing should be
		// reported when it is written, not quietly turned into another number
		// every time the preset loads.
		if (!(frequency > 0.0f))
		{
			error = Child(context, ".freq") + " must be above 0 Hz";
			return false;
		}

		out.frequency = frequency;
	}

	return true;
}

// Only the parts that differ from the default, so a preset that chose a
// crossover frequency doesn't also acquire two stereo sources it never picked.
nlohmann::json SplitterToJson(const Splitter& splitter)
{
	static const char* const kModes[] = {"full", "crossover", "stereo"};
	static const char* const kSourceNames[] = {"left", "right", "sum"};

	const Splitter defaults;

	// An object explicitly, not a default-constructed json, which is `null`.
	// Nothing calls this with an all-default splitter today -- the caller
	// checks first -- but `"splitter": null` would fail to parse on the way
	// back in, and a writer that can emit an unreadable file is a trap for
	// whoever changes that caller.
	nlohmann::json json = nlohmann::json::object();

	if (splitter.mode != defaults.mode)
	{
		json["mode"] = kModes[static_cast<int>(splitter.mode)];
	}

	static const char* const kSends[2] = {"send_a", "send_b"};
	static const char* const kSources[2] = {"src_a", "src_b"};

	for (unsigned lane = 0; lane < 2; ++lane)
	{
		if (splitter.send_db[lane] != defaults.send_db[lane])
		{
			json[kSends[lane]] = splitter.send_db[lane];
		}

		if (splitter.source[lane] != defaults.source[lane])
		{
			json[kSources[lane]] = kSourceNames[static_cast<int>(splitter.source[lane])];
		}
	}

	if (splitter.frequency != defaults.frequency)
	{
		json["freq"] = splitter.frequency;
	}

	return json;
}

bool GraphBranch::operator==(const GraphBranch& other) const
{
	return nodes == other.nodes && gain_db == other.gain_db && muted == other.muted;
}

bool GraphNode::operator==(const GraphNode& other) const
{
	if (id != other.id || kind != other.kind)
	{
		return false;
	}

	if (kind == NodeKind::Plugin)
	{
		return stage == other.stage;
	}

	return branches == other.branches && splitter == other.splitter;
}

bool Splitter::operator==(const Splitter& other) const
{
	// Every field, including the ones the current mode ignores. Two splits
	// that sound the same today but carry different crossover frequencies are
	// not the same board -- switching mode would tell them apart, and a
	// round-trip that dropped the difference would lose a number the player
	// set on purpose.
	return mode == other.mode && send_db[0] == other.send_db[0] && send_db[1] == other.send_db[1] &&
		frequency == other.frequency && source[0] == other.source[0] && source[1] == other.source[1];
}

ChainGraph ChainGraph::FromStages(const std::vector<PresetStage>& stages)
{
	ChainGraph graph;

	for (const PresetStage& stage : stages)
	{
		graph.AppendPlugin(stage);
	}

	return graph;
}

bool ChainGraph::IsLinear() const
{
	return !AnySplit(trunk_);
}

std::vector<FlatEntry> ChainGraph::Flatten() const
{
	std::vector<FlatEntry> out;
	FlattenInto(trunk_, kNoNode, 0, 0, out);

	return out;
}

size_t ChainGraph::PluginCount() const
{
	return Flatten().size();
}

const PresetStage* ChainGraph::PluginAt(size_t flat_index) const
{
	const NodeId id = NodeAtFlatIndex(flat_index);

	if (id == kNoNode)
	{
		return nullptr;
	}

	const GraphNode* node = Find(id);

	return node ? &node->stage : nullptr;
}

PresetStage* ChainGraph::PluginAt(size_t flat_index)
{
	const PresetStage* stage = static_cast<const ChainGraph*>(this)->PluginAt(flat_index);

	return const_cast<PresetStage*>(stage);
}

NodeId ChainGraph::NodeAtFlatIndex(size_t flat_index) const
{
	const std::vector<FlatEntry> flat = Flatten();

	if (flat_index >= flat.size())
	{
		return kNoNode;
	}

	return flat[flat_index].id;
}

size_t ChainGraph::FlatIndexOf(NodeId id) const
{
	const std::vector<FlatEntry> flat = Flatten();

	for (size_t i = 0; i < flat.size(); ++i)
	{
		if (flat[i].id == id)
		{
			return i;
		}
	}

	return kNoFlatIndex;
}

const GraphNode* ChainGraph::Find(NodeId id) const
{
	if (id == kNoNode)
	{
		return nullptr;
	}

	return FindIn(trunk_, id);
}

GraphNode* ChainGraph::Find(NodeId id)
{
	return const_cast<GraphNode*>(static_cast<const ChainGraph*>(this)->Find(id));
}

size_t ChainGraph::MergeIndexOf(NodeId id) const
{
	if (id == kNoNode)
	{
		return kNoMerge;
	}

	size_t next = 0;
	size_t found = kNoMerge;

	std::function<void(const NodeList&)> walk = [&](const NodeList& nodes)
	{
		for (const GraphNode& node : nodes)
		{
			if (node.kind != NodeKind::Split)
			{
				continue;
			}

			// Numbered on the way down, before its branches are walked, which
			// is where the planner numbers it too.
			const size_t index = next++;

			if (node.id == id)
			{
				found = index;
			}

			for (const GraphBranch& branch : node.branches)
			{
				walk(branch.nodes);
			}
		}
	};

	walk(trunk_);

	return found;
}

NodeId ChainGraph::AllocateId()
{
	return next_id_++;
}

NodeId ChainGraph::AppendPlugin(const PresetStage& stage)
{
	GraphNode node;
	node.id = AllocateId();
	node.kind = NodeKind::Plugin;
	node.stage = stage;

	trunk_.push_back(std::move(node));

	return trunk_.back().id;
}

namespace
{

// Resolves a position to the list it names. Shared by both inserts, which
// differ only in what they then put there.
NodeList* ListAt(NodeList& trunk, ChainGraph& graph, const GraphPosition& at, std::string& error)
{
	if (at.split == kNoNode)
	{
		return &trunk;
	}

	GraphNode* split = graph.Find(at.split);

	if (split == nullptr)
	{
		error = "no node with id " + std::to_string(at.split);
		return nullptr;
	}

	if (split->kind != NodeKind::Split)
	{
		error = "node " + std::to_string(at.split) + " is not a split";
		return nullptr;
	}

	if (at.branch >= split->branches.size())
	{
		error = "split " + std::to_string(at.split) + " has no branch " + std::to_string(at.branch);
		return nullptr;
	}

	return &split->branches[at.branch].nodes;
}

}  // namespace

bool ChainGraph::InsertPlugin(const GraphPosition& at, const PresetStage& stage, NodeId& out_id, std::string& error)
{
	NodeList* list = ListAt(trunk_, *this, at, error);

	if (list == nullptr)
	{
		return false;
	}

	if (at.index > list->size())
	{
		error = "index " + std::to_string(at.index) + " is past the end of a list of " +
			std::to_string(list->size());
		return false;
	}

	GraphNode node;
	node.id = AllocateId();
	node.kind = NodeKind::Plugin;
	node.stage = stage;

	out_id = node.id;
	list->insert(list->begin() + static_cast<long>(at.index), std::move(node));

	return true;
}

bool ChainGraph::InsertSplit(const GraphPosition& at, NodeId& out_id, std::string& error)
{
	NodeList* list = ListAt(trunk_, *this, at, error);

	if (list == nullptr)
	{
		return false;
	}

	if (at.index > list->size())
	{
		error = "index " + std::to_string(at.index) + " is past the end of a list of " +
			std::to_string(list->size());
		return false;
	}

	GraphNode node;
	node.id = AllocateId();
	node.kind = NodeKind::Split;
	node.branches.resize(2);

	out_id = node.id;
	list->insert(list->begin() + static_cast<long>(at.index), std::move(node));

	return true;
}

bool ChainGraph::Remove(NodeId id, std::string& error)
{
	if (!RemoveIn(trunk_, id))
	{
		error = "no node with id " + std::to_string(id);
		return false;
	}

	return true;
}

bool ChainGraph::TakeNode(NodeId id, GraphNode& out, std::string& error)
{
	NodeList* list = nullptr;
	size_t index = 0;

	if (!LocateIn(trunk_, id, list, index))
	{
		error = "no node with id " + std::to_string(id);
		return false;
	}

	out = std::move((*list)[index]);
	list->erase(list->begin() + static_cast<long>(index));

	return true;
}

bool ChainGraph::InsertNode(const GraphPosition& at, GraphNode node, std::string& error)
{
	NodeList* list = ListAt(trunk_, *this, at, error);

	if (list == nullptr)
	{
		return false;
	}

	if (at.index > list->size())
	{
		error = "index " + std::to_string(at.index) + " is past the end of a list of " +
			std::to_string(list->size());
		return false;
	}

	// The id comes with the node rather than being handed out here -- that is
	// the whole point of this over InsertPlugin. `next_id_` needs no nudging:
	// the id was already in this graph, so it is already below it.
	list->insert(list->begin() + static_cast<long>(at.index), std::move(node));

	return true;
}

bool ChainGraph::Move(NodeId id, int delta, std::string& error)
{
	if (delta != -1 && delta != 1)
	{
		error = "a move is one place at a time, not " + std::to_string(delta);
		return false;
	}

	NodeList* list = nullptr;
	size_t index = 0;

	if (!LocateIn(trunk_, id, list, index))
	{
		error = "no node with id " + std::to_string(id);
		return false;
	}

	const size_t target = static_cast<size_t>(static_cast<long>(index) + delta);

	if (delta < 0 ? index == 0 : target >= list->size())
	{
		error = delta < 0 ? "already first" : "already last";
		return false;
	}

	std::swap((*list)[index], (*list)[target]);

	return true;
}

bool ChainGraph::Validate(std::string& error) const
{
	if (!ValidateIn(trunk_, "chain", error))
	{
		return false;
	}

	std::vector<NodeId> ids;
	CollectIds(trunk_, ids);

	std::vector<NodeId> sorted = ids;
	std::sort(sorted.begin(), sorted.end());

	for (size_t i = 1; i < sorted.size(); ++i)
	{
		if (sorted[i] == sorted[i - 1])
		{
			error = "two nodes share id " + std::to_string(sorted[i]);
			return false;
		}
	}

	if (!sorted.empty() && next_id_ <= sorted.back())
	{
		error = "next id " + std::to_string(next_id_) + " is not past the highest id in use (" +
			std::to_string(sorted.back()) + ")";
		return false;
	}

	return true;
}

bool ChainGraph::HasDefaultIds() const
{
	if (!IsLinear())
	{
		return false;
	}

	for (size_t i = 0; i < trunk_.size(); ++i)
	{
		if (trunk_[i].id != static_cast<NodeId>(i + 1))
		{
			return false;
		}
	}

	return true;
}

nlohmann::json ChainGraph::ToJson() const
{
	nlohmann::json json = nlohmann::json::array();

	for (const GraphNode& node : trunk_)
	{
		json.push_back(NodeToJson(node, true));
	}

	return json;
}

bool ChainGraph::FromJson(const nlohmann::json& json, ChainGraph& out, std::string& error)
{
	out = ChainGraph();

	if (!ParseNodeList(json, "graph", out.trunk_, error))
	{
		return false;
	}

	std::vector<NodeId> ids;
	CollectIds(out.trunk_, ids);

	out.next_id_ = 1;

	for (NodeId id : ids)
	{
		out.next_id_ = std::max(out.next_id_, static_cast<NodeId>(id + 1));
	}

	// A graph written by hand may leave ids out; a graph we wrote never does.
	AssignMissingIds(out.trunk_, out.next_id_);

	return out.Validate(error);
}

nlohmann::json ChainGraph::ToFlatJson(bool with_ids) const
{
	nlohmann::json json = nlohmann::json::array();

	for (const GraphNode& node : trunk_)
	{
		json.push_back(NodeToJson(node, with_ids));
	}

	return json;
}

bool ChainGraph::FromFlatJson(const nlohmann::json& json, ChainGraph& out, std::string& error)
{
	out = ChainGraph();

	if (!ParseNodeList(json, "chain", out.trunk_, error))
	{
		return false;
	}

	for (const GraphNode& node : out.trunk_)
	{
		if (node.kind == NodeKind::Split)
		{
			error = "the flat \"chain\" form cannot hold a split -- use \"graph\"";
			return false;
		}
	}

	std::vector<NodeId> ids;
	CollectIds(out.trunk_, ids);

	out.next_id_ = 1;

	for (NodeId id : ids)
	{
		out.next_id_ = std::max(out.next_id_, static_cast<NodeId>(id + 1));
	}

	AssignMissingIds(out.trunk_, out.next_id_);

	return out.Validate(error);
}

}  // namespace sf
