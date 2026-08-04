// Collapsing a split to one channel and going back (SF-58, SF-86).
//
// The claim a player would make is "put my pedals back where they were", and
// the only honest way to check it is to do the round trip and compare the
// shape, pedal by pedal, lane by lane. Pure graph surgery -- no plugins, no
// audio, no files -- so all of it runs anywhere.

#include "../src/preset/SplitEdit.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool condition, const char* description)
{
	if (condition)
	{
		std::printf("ok    %s\n", description);
		return;
	}

	std::printf("FAIL  %s\n", description);
	++g_failures;
}

sf::PresetStage Stage(const char* uri)
{
	sf::PresetStage stage;
	stage.uri = uri;

	return stage;
}

// comp, then a split with (drive, amp) against (clean), then reverb.
sf::ChainGraph Board(sf::NodeId& split_out)
{
	sf::ChainGraph graph;
	graph.AppendPlugin(Stage("comp"));

	std::string error;
	sf::NodeId split = sf::kNoNode;
	graph.InsertSplit({sf::kNoNode, 0, 1}, split, error);

	sf::NodeId ignored = sf::kNoNode;
	graph.InsertPlugin({split, 0, 0}, Stage("drive"), ignored, error);
	graph.InsertPlugin({split, 0, 1}, Stage("amp"), ignored, error);
	graph.InsertPlugin({split, 1, 0}, Stage("clean"), ignored, error);

	graph.AppendPlugin(Stage("reverb"));

	split_out = split;

	return graph;
}

// The uris of one lane, joined -- the compact way to say "these pedals, in
// this order, on this channel".
std::string Lane(const sf::ChainGraph& graph, sf::NodeId split, unsigned branch)
{
	const sf::GraphNode* node = graph.Find(split);

	if (node == nullptr || node->kind != sf::NodeKind::Split || branch >= node->branches.size())
	{
		return "(no lane)";
	}

	std::string text;

	for (const sf::GraphNode& inner : node->branches[branch].nodes)
	{
		text += (text.empty() ? "" : ",") + inner.stage.uri;
	}

	return text;
}

std::string Trunk(const sf::ChainGraph& graph)
{
	std::string text;

	for (const sf::GraphNode& node : graph.Trunk())
	{
		text += (text.empty() ? "" : ",") + (node.kind == sf::NodeKind::Split ? std::string("<split>") : node.stage.uri);
	}

	return text;
}

// The split in a graph, or kNoNode.
sf::NodeId SplitIn(const sf::ChainGraph& graph)
{
	for (const sf::GraphNode& node : graph.Trunk())
	{
		if (node.kind == sf::NodeKind::Split)
		{
			return node.id;
		}
	}

	return sf::kNoNode;
}

void TestCollapse()
{
	sf::NodeId split = sf::kNoNode;
	sf::ChainGraph graph = Board(split);

	Check(Trunk(graph) == "comp,<split>,reverb", "the board starts with a split in the middle");
	Check(Lane(graph, split, 0) == "drive,amp" && Lane(graph, split, 1) == "clean", "...with pedals on both channels");

	// The ids before, so the claim that they survive can be made about the
	// same pedals rather than about pedals with the same names.
	const std::vector<sf::FlatEntry> before = graph.Flatten();

	sf::RememberedSplit remembered;
	std::string error;

	Check(sf::RejoinSplit(graph, split, &remembered, error), "collapsing to one channel succeeds");
	Check(Trunk(graph) == "comp,drive,amp,clean,reverb", "everything ends up in series, lane 0 then lane 1");

	// **Ids survive the collapse.** Without this the memory below could not
	// name anything, and a `Remove` plus `InsertPlugin` -- which is what this
	// used to do -- mints a fresh id for every pedal.
	const std::vector<sf::FlatEntry> after = graph.Flatten();

	bool same_ids = before.size() == after.size();

	for (size_t i = 0; same_ids && i < before.size(); ++i)
	{
		same_ids = before[i].id == after[i].id;
	}

	Check(same_ids, "every pedal is still the same pedal, by id");

	// And the memory says what was where.
	Check(remembered.present, "the collapse left a memory");
	Check(remembered.lane[0].size() == 2 && remembered.lane[1].size() == 1, "...of two pedals on A and one on B");
	Check(remembered.trunk_index == 1, "...and of where the split sat");
}

void TestRoundTrip()
{
	sf::NodeId split = sf::kNoNode;
	sf::ChainGraph graph = Board(split);

	// Settings worth losing, so the round trip is about more than pedals.
	sf::GraphNode* node = graph.Find(split);
	node->splitter.mode = sf::SplitterMode::Crossover;
	node->splitter.frequency = 250.0f;
	node->splitter.send_db[1] = -6.0f;
	node->branches[0].gain_db = -3.0f;
	node->branches[1].muted = true;

	const std::string trunk_before = Trunk(graph);
	const std::string lane_a = Lane(graph, split, 0);
	const std::string lane_b = Lane(graph, split, 1);

	sf::RememberedSplit remembered;
	std::string error;

	Check(sf::RejoinSplit(graph, split, &remembered, error), "collapse succeeds");
	Check(sf::RestoreSplit(graph, remembered, error), "going back to two channels restores rather than refuses");

	const sf::NodeId restored = SplitIn(graph);

	// **The claim the whole feature exists for.**
	Check(Trunk(graph) == trunk_before, "the trunk comes back exactly as it was");
	Check(Lane(graph, restored, 0) == lane_a, "channel A has its own pedals back, in order");
	Check(Lane(graph, restored, 1) == lane_b, "...and so does channel B");

	// Everything about the split, not only the pedals: losing a crossover on
	// the way through would be the same surprise as losing the layout.
	//
	// Guarded rather than dereferenced. A restore that failed leaves no split
	// to find, and reading through the null would segfault here instead of
	// naming the assertion that broke -- which is the difference between a
	// test that reports a bug and one that hides it behind a crash.
	const sf::GraphNode* back = graph.Find(restored);

	if (back == nullptr || back->branches.size() != 2)
	{
		Check(false, "the restored split is there to be inspected");
		return;
	}

	Check(back->splitter.mode == sf::SplitterMode::Crossover && back->splitter.frequency == 250.0f,
		"the splitter's mode and frequency survive the round trip");
	Check(back->splitter.send_db[1] == -6.0f, "...and its sends");
	Check(back->branches[0].gain_db == -3.0f && back->branches[1].muted, "...and both channels' levels and mutes");
}

void TestEditedWhileCollapsed()
{
	// A pedal removed while flattened is simply gone; the rest still go back.
	{
		sf::NodeId split = sf::kNoNode;
		sf::ChainGraph graph = Board(split);

		sf::RememberedSplit remembered;
		std::string error;

		Check(sf::RejoinSplit(graph, split, &remembered, error), "collapse succeeds");

		const sf::NodeId amp = graph.Flatten()[2].id;
		Check(graph.Remove(amp, error), "a pedal can be removed while collapsed");

		Check(sf::RestoreSplit(graph, remembered, error), "the rest still restore");

		const sf::NodeId restored = SplitIn(graph);
		Check(Lane(graph, restored, 0) == "drive", "the removed pedal is simply gone from its channel");
		Check(Lane(graph, restored, 1) == "clean", "...and the other channel is untouched");
	}

	// A pedal added while flattened was never on a channel, so it stays on the
	// trunk rather than being guessed into one.
	{
		sf::NodeId split = sf::kNoNode;
		sf::ChainGraph graph = Board(split);

		sf::RememberedSplit remembered;
		std::string error;

		Check(sf::RejoinSplit(graph, split, &remembered, error), "collapse succeeds");

		graph.AppendPlugin(Stage("delay"));

		Check(sf::RestoreSplit(graph, remembered, error), "restoring works with a new pedal present");

		const sf::NodeId restored = SplitIn(graph);
		Check(Trunk(graph) == "comp,<split>,reverb,delay", "a pedal added while collapsed stays on the trunk");
		Check(Lane(graph, restored, 0) == "drive,amp", "...and the channels are unaffected by it");
	}

	// Nothing left of the old layout is not an error -- it is the caller's cue
	// to make a plain empty split instead.
	{
		sf::NodeId split = sf::kNoNode;
		sf::ChainGraph graph = Board(split);

		sf::RememberedSplit remembered;
		std::string error;

		Check(sf::RejoinSplit(graph, split, &remembered, error), "collapse succeeds");

		for (sf::NodeId id : {remembered.lane[0][0], remembered.lane[0][1], remembered.lane[1][0]})
		{
			graph.Remove(id, error);
		}

		Check(!sf::RestoreSplit(graph, remembered, error), "with every remembered pedal gone, there is nothing to restore");
		Check(SplitIn(graph) == sf::kNoNode, "...and no half-built split is left behind");
	}

	// A memory of nothing restores nothing.
	{
		sf::ChainGraph graph;
		graph.AppendPlugin(Stage("comp"));
		sf::RememberedSplit nothing;
		std::string error;

		Check(!sf::RestoreSplit(graph, nothing, error), "an empty memory restores nothing");
	}
}

void TestRefusals()
{
	sf::NodeId split = sf::kNoNode;
	sf::ChainGraph graph = Board(split);

	sf::RememberedSplit remembered;
	std::string error;

	const sf::NodeId comp = graph.Trunk()[0].id;

	Check(!sf::RejoinSplit(graph, comp, &remembered, error), "a plugin node is not a split to collapse");
	Check(!sf::RejoinSplit(graph, 9999, &remembered, error), "and neither is an id that isn't there");

	// Collapsing without recording is allowed, and is what a caller that has
	// no memory to keep should do.
	Check(sf::RejoinSplit(graph, split, nullptr, error), "collapsing without a memory works");
	Check(Trunk(graph) == "comp,drive,amp,clean,reverb", "...and still flattens correctly");
}

}  // namespace

int main()
{
	TestCollapse();
	TestRoundTrip();
	TestEditedWhileCollapsed();
	TestRefusals();

	if (g_failures > 0)
	{
		std::printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("\nall checks passed\n");

	return 0;
}
