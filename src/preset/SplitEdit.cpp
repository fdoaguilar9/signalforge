#include "SplitEdit.h"

#include <algorithm>
#include <functional>

namespace sf
{

// Undoing a split: lane 0's pedals, then lane 1's, spliced into the trunk
// where the split was.
//
// Concatenating rather than discarding is the whole point. A "rejoin" that
// kept one lane and dropped the other would silently delete pedals -- and the
// lane it dropped would be a choice the user never made. Ordering lane 0
// before lane 1 matches how they read on screen, top before bottom.
// Collapses a split to a series, remembering the layout so the trip back can
// restore it (SF-58, SF-86).
//
// `remembered` is filled in with which pedal was on which channel, where the
// split sat, and what the splitter and the two returns were set to. Null asks
// for the collapse without the memory.
bool RejoinSplit(sf::ChainGraph& graph, sf::NodeId id, sf::RememberedSplit* remembered, std::string& error)
{
	const sf::GraphNode* split = graph.Find(id);

	if (split == nullptr || split->kind != sf::NodeKind::Split)
	{
		error = "that isn't a split";
		return false;
	}

	// **Ids, not copies.** The nodes are moved out and put back, so every pedal
	// is still the same pedal afterwards. That is what lets the memory below
	// name them -- a `Remove` plus `InsertPlugin` mints fresh ids, and a
	// collapse that renumbered every pedal could never be undone.
	std::vector<sf::NodeId> rescued;

	std::function<void(const sf::NodeList&)> collect = [&](const sf::NodeList& nodes)
	{
		for (const sf::GraphNode& node : nodes)
		{
			if (node.kind == sf::NodeKind::Plugin)
			{
				rescued.push_back(node.id);
				continue;
			}

			// A nested split flattens too. Rejoining the outer one and leaving
			// an inner one dangling in a trunk that no longer has lanes would
			// be a shape nobody asked for.
			for (const sf::GraphBranch& branch : node.branches)
			{
				collect(branch.nodes);
			}
		}
	};

	if (remembered)
	{
		*remembered = sf::RememberedSplit();
		remembered->present = true;
		remembered->splitter = split->splitter;
	}

	for (unsigned b = 0; b < split->branches.size(); ++b)
	{
		const size_t before = rescued.size();
		collect(split->branches[b].nodes);

		if (!remembered || b > 1)
		{
			continue;
		}

		remembered->gain_db[b] = split->branches[b].gain_db;
		remembered->muted[b] = split->branches[b].muted;
		remembered->lane[b].assign(rescued.begin() + static_cast<long>(before), rescued.end());
	}

	const size_t position = graph.FlatIndexOf(id);
	(void)position;

	// Where the split sat, so what comes out of it lands in the same place.
	sf::GraphPosition at;
	bool found = false;

	std::function<bool(const sf::NodeList&, sf::NodeId, unsigned)> locate =
		[&](const sf::NodeList& nodes, sf::NodeId owner, unsigned branch)
	{
		for (size_t i = 0; i < nodes.size(); ++i)
		{
			if (nodes[i].id == id)
			{
				at.split = owner;
				at.branch = branch;
				at.index = i;
				return true;
			}

			for (unsigned b = 0; b < nodes[i].branches.size(); ++b)
			{
				if (locate(nodes[i].branches[b].nodes, nodes[i].id, b))
				{
					return true;
				}
			}
		}

		return false;
	};

	found = locate(graph.Trunk(), sf::kNoNode, 0);

	if (!found)
	{
		error = "could not find where that split sits";
		return false;
	}

	if (remembered)
	{
		remembered->trunk_index = at.index;
	}

	// Detached before the split goes, since removing a split takes both lanes'
	// contents with it -- and these are the very nodes being kept.
	std::vector<sf::GraphNode> nodes;
	nodes.reserve(rescued.size());

	for (sf::NodeId node_id : rescued)
	{
		sf::GraphNode taken;

		if (!graph.TakeNode(node_id, taken, error))
		{
			return false;
		}

		nodes.push_back(std::move(taken));
	}

	if (!graph.Remove(id, error))
	{
		return false;
	}

	for (size_t i = 0; i < nodes.size(); ++i)
	{
		sf::GraphPosition where = at;
		where.index = at.index + i;

		if (!graph.InsertNode(where, std::move(nodes[i]), error))
		{
			return false;
		}
	}

	return true;
}

// The trip back: rebuilds the split a collapse left a memory of (SF-86).
//
// Every pedal whose id is still on the trunk goes back to the lane it came
// from, in the order it was in. Ones that have since been removed are skipped
// and ones added while collapsed stay on the trunk -- so editing a flattened
// board degrades into "as much of the old layout as still makes sense" rather
// than into a refusal.
//
// Returns false when there is nothing left to restore, which is the caller's
// cue to make a plain empty split instead.
bool RestoreSplit(sf::ChainGraph& graph, const sf::RememberedSplit& remembered, std::string& error)
{
	if (!remembered.present)
	{
		return false;
	}

	// Detached first, and only the ones still there. Taking them before the
	// split is inserted keeps the trunk indices below meaningful.
	std::vector<sf::GraphNode> lanes[2];
	size_t found = 0;

	for (unsigned b = 0; b < 2; ++b)
	{
		for (sf::NodeId id : remembered.lane[b])
		{
			sf::GraphNode taken;
			std::string ignored;

			if (graph.TakeNode(id, taken, ignored))
			{
				lanes[b].push_back(std::move(taken));
				++found;
			}
		}
	}

	if (found == 0)
	{
		return false;
	}

	sf::GraphPosition at;
	at.index = std::min(remembered.trunk_index, graph.Trunk().size());

	sf::NodeId split_id = sf::kNoNode;

	if (!graph.InsertSplit(at, split_id, error))
	{
		return false;
	}

	sf::GraphNode* split = graph.Find(split_id);

	if (split == nullptr)
	{
		error = "the restored split went missing";
		return false;
	}

	split->splitter = remembered.splitter;

	for (unsigned b = 0; b < 2; ++b)
	{
		split->branches[b].gain_db = remembered.gain_db[b];
		split->branches[b].muted = remembered.muted[b];

		for (size_t i = 0; i < lanes[b].size(); ++i)
		{
			sf::GraphPosition where;
			where.split = split_id;
			where.branch = b;
			where.index = i;

			if (!graph.InsertNode(where, std::move(lanes[b][i]), error))
			{
				return false;
			}
		}
	}

	return true;
}

}  // namespace sf
