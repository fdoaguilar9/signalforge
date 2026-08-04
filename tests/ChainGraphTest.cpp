// The chain graph: node identity, signal order, structural edits, and both
// JSON forms a preset can be written in. Pure data -- no plugins, no audio,
// no filesystem -- so it runs anywhere (SF-45, SF-46).

#include "../src/preset/ChainGraph.h"

#include "../src/preset/Preset.h"

#include <cstdio>
#include <string>

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

sf::ChainGraph Linear(std::initializer_list<const char*> uris)
{
	std::vector<sf::PresetStage> stages;

	for (const char* uri : uris)
	{
		stages.push_back(Stage(uri));
	}

	return sf::ChainGraph::FromStages(stages);
}

// The uris in Flatten() order, joined -- the compact way to assert a signal
// order, since the order is the whole claim.
std::string FlatOrder(const sf::ChainGraph& graph)
{
	std::string text;

	for (const sf::FlatEntry& entry : graph.Flatten())
	{
		if (!text.empty())
		{
			text += ",";
		}

		text += graph.Find(entry.id)->stage.uri;
	}

	return text;
}

sf::ChainGraph GraphFrom(const std::string& text, const char* description)
{
	sf::ChainGraph graph;
	std::string error;

	if (!sf::ChainGraph::FromJson(nlohmann::json::parse(text), graph, error))
	{
		std::printf("FAIL  %s: unexpected load failure: %s\n", description, error.c_str());
		++g_failures;
	}

	return graph;
}

void ExpectGraphRejected(const std::string& text, const char* description)
{
	sf::ChainGraph graph;
	std::string error;

	if (sf::ChainGraph::FromJson(nlohmann::json::parse(text), graph, error))
	{
		std::printf("FAIL  %-52s expected rejection, loaded %zu plugin(s)\n", description, graph.PluginCount());
		++g_failures;
		return;
	}

	std::printf("ok    %-52s rejected: %s\n", description, error.c_str());
}

void ExpectPresetRejected(const std::string& text, const char* description)
{
	sf::Preset preset;
	std::string error;

	if (sf::Preset::FromJson(nlohmann::json::parse(text), preset, error))
	{
		std::printf("FAIL  %-52s expected rejection, loaded it\n", description);
		++g_failures;
		return;
	}

	std::printf("ok    %-52s rejected: %s\n", description, error.c_str());
}

// A trunk of a,d with a split between them: branch 0 runs b, branch 1 runs c.
sf::ChainGraph Split()
{
	sf::ChainGraph graph;
	graph.AppendPlugin(Stage("a"));

	sf::NodeId split = sf::kNoNode;
	std::string error;
	graph.InsertSplit({sf::kNoNode, 0, 1}, split, error);

	sf::NodeId ignored = sf::kNoNode;
	graph.InsertPlugin({split, 0, 0}, Stage("b"), ignored, error);
	graph.InsertPlugin({split, 1, 0}, Stage("c"), ignored, error);

	graph.AppendPlugin(Stage("d"));

	return graph;
}

// Which merge a split is (SF-72). The number has to be the one the planner
// gives the same split -- it is how a lane's level reaches the audio thread
// without a rebuild -- and both sides agree to count pre-order: a split
// before either of its own branches.
void TestMergeIndex()
{
	sf::ChainGraph linear = Linear({"a", "b"});
	Check(linear.MergeIndexOf(1) == sf::kNoMerge, "a plugin node is not a merge");
	Check(linear.MergeIndexOf(sf::kNoNode) == sf::kNoMerge, "and neither is nothing");

	sf::ChainGraph graph = Split();

	sf::NodeId outer = sf::kNoNode;

	for (const sf::GraphNode& node : graph.Trunk())
	{
		if (node.kind == sf::NodeKind::Split)
		{
			outer = node.id;
		}
	}

	Check(outer != sf::kNoNode, "the split is findable on the trunk");
	Check(graph.MergeIndexOf(outer) == 0, "the only split is merge 0");
	Check(graph.MergeIndexOf(9999) == sf::kNoMerge, "an id that isn't there is no merge");

	// A second split *inside* the first one's branch 0. Counting post-order --
	// the order the merges actually sum in -- would number this one 0 and the
	// outer one 1, which is the mistake this asserts against.
	sf::NodeId inner = sf::kNoNode;
	std::string error;

	Check(graph.InsertSplit({outer, 0, 0}, inner, error), "a split can go inside a branch");
	Check(graph.MergeIndexOf(outer) == 0, "the outer split stays merge 0");
	Check(graph.MergeIndexOf(inner) == 1, "a split inside branch 0 is merge 1");

	// And one after it on the trunk, which is met last of the three.
	sf::NodeId trailing = sf::kNoNode;
	Check(graph.InsertSplit({sf::kNoNode, 0, graph.Trunk().size()}, trailing, error),
		"a split can go at the end of the trunk");
	Check(graph.MergeIndexOf(trailing) == 2, "a later split on the trunk is numbered after a nested one");
}

void TestIdentity()
{
	sf::ChainGraph graph = Linear({"a", "b", "c"});

	Check(graph.PluginCount() == 3, "FromStages keeps every stage");
	Check(graph.IsLinear(), "a graph with no splits is linear");
	Check(graph.HasDefaultIds(), "FromStages hands out positional ids");
	Check(graph.NodeAtFlatIndex(0) == 1 && graph.NodeAtFlatIndex(2) == 3, "ids run 1..N in signal order");
	Check(graph.NodeAtFlatIndex(3) == sf::kNoNode, "a flat index past the end names no node");
	Check(graph.FlatIndexOf(2) == 1, "index and id translate both ways");
	Check(graph.FlatIndexOf(99) == sf::kNoFlatIndex, "an id that isn't there has no index");
	Check(graph.PluginAt(1) != nullptr && graph.PluginAt(1)->uri == "b", "PluginAt reads by signal position");

	// The point of ids: an insert ahead of a node moves its index but not its
	// identity, which is what a selection and a control panel have to survive.
	const sf::NodeId c = graph.NodeAtFlatIndex(2);
	sf::NodeId inserted = sf::kNoNode;
	std::string error;

	Check(graph.InsertPlugin({sf::kNoNode, 0, 0}, Stage("z"), inserted, error), "insert at the head succeeds");
	Check(graph.FlatIndexOf(c) == 3, "an insert ahead of a node shifts its index");
	Check(graph.Find(c)->stage.uri == "c", "...but not its identity");
	Check(!graph.HasDefaultIds(), "an insert makes the ids stop being positional");

	// Ids are never reused, so a stale reference stays dangling rather than
	// quietly coming to mean a different pedal.
	const sf::NodeId next = graph.AppendPlugin(Stage("y"));
	Check(graph.Remove(next, error), "remove by id succeeds");
	Check(graph.AppendPlugin(Stage("x")) != next, "a removed id is not handed out again");
	Check(!graph.Remove(next, error), "removing a node that isn't there fails");
}

void TestSignalOrder()
{
	sf::ChainGraph graph = Split();

	Check(!graph.IsLinear(), "a graph with a split is not linear");
	Check(graph.PluginCount() == 4, "a split node is not itself a plugin");

	// Both branches run before the trunk resumes -- that is what makes this a
	// signal order rather than just a traversal.
	Check(FlatOrder(graph) == "a,b,c,d", "flat order is trunk, branch 0, branch 1, trunk");

	const std::vector<sf::FlatEntry> flat = graph.Flatten();
	Check(flat[0].depth == 0 && flat[1].depth == 1 && flat[3].depth == 0, "depth counts how deep in splits a node is");
	Check(flat[1].position.branch == 0 && flat[2].position.branch == 1, "a flat entry says which lane it came from");

	// An empty branch is a straight wire, not an error: it is how the dry side
	// of a parallel blend is spelled.
	sf::ChainGraph dry;
	sf::NodeId split = sf::kNoNode;
	std::string error;
	Check(dry.InsertSplit({sf::kNoNode, 0, 0}, split, error), "a split can be inserted into an empty trunk");
	Check(dry.Validate(error), "a split with two empty branches is valid");
	Check(dry.PluginCount() == 0, "two dry wires hold no plugins");

	sf::NodeId ignored = sf::kNoNode;
	Check(dry.InsertPlugin({split, 1, 0}, Stage("comp"), ignored, error), "a pedal goes into one lane");
	Check(FlatOrder(dry) == "comp", "the other lane stays a wire");
}

void TestEdits()
{
	sf::ChainGraph graph = Linear({"a", "b", "c"});
	std::string error;

	Check(graph.Move(graph.NodeAtFlatIndex(2), -1, error), "a stage moves earlier");
	Check(FlatOrder(graph) == "a,c,b", "...and the order follows");
	Check(!graph.Move(graph.NodeAtFlatIndex(0), -1, error), "the first stage refuses to move earlier");
	Check(!graph.Move(graph.NodeAtFlatIndex(2), 1, error), "the last stage refuses to move later");
	Check(!graph.Move(graph.NodeAtFlatIndex(0), 2, error), "a move is one place at a time");
	Check(FlatOrder(graph) == "a,c,b", "a refused move changes nothing");

	// A move inside a branch stays inside that branch. Crossing lanes is a
	// different gesture (SF-55) and must not fall out of "move up".
	sf::ChainGraph split = Split();
	const sf::NodeId b = split.NodeAtFlatIndex(1);
	Check(!split.Move(b, -1, error), "the first node of a lane does not move out of it");
	Check(!split.Move(b, 1, error), "the last node of a lane does not move into the next one");
	Check(FlatOrder(split) == "a,b,c,d", "a lane's contents stay put");

	// Removing a split takes both lanes with it.
	sf::ChainGraph removal = Split();
	// "b" is in a lane, so the split is whatever its flat entry says it's in.
	Check(removal.Remove(removal.Flatten()[1].position.split, error), "a split can be removed");
	Check(FlatOrder(removal) == "a,d", "removing a split removes everything in both lanes");

	// Positions that name nothing are refused rather than clamped.
	sf::NodeId ignored = sf::kNoNode;
	Check(!graph.InsertPlugin({999, 0, 0}, Stage("x"), ignored, error), "inserting into a split that isn't there fails");
	Check(!graph.InsertPlugin({sf::kNoNode, 0, 99}, Stage("x"), ignored, error), "inserting past the end fails");
}

void TestValidation()
{
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}]}])", "a split with one branch");
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}, {"nodes": []}]}])", "a split with three");
	ExpectGraphRejected(R"([{"id": 1, "uri": "a"}, {"id": 1, "uri": "b"}])", "two nodes sharing an id");
	ExpectGraphRejected(R"([{"id": 0, "uri": "a"}])", "an id of zero");
	ExpectGraphRejected(R"([{"bypassed": true}])", "a plugin node with no uri");
	ExpectGraphRejected(R"([{"uri": "a", "controls": {"gain": "loud"}}])", "a control that isn't a number");
	ExpectGraphRejected(R"([{"branches": [{"nodes": [{"uri": "a", "state": 7}]}, {"nodes": []}]}])",
		"a bad node inside a lane");
	ExpectGraphRejected(R"({"uri": "a"})", "a graph that isn't an array");
}

void TestJsonRoundTrip()
{
	// The nested form, with everything that can vary set to something other
	// than its default.
	const std::string text = R"([
		{"id": 1, "uri": "a", "bypassed": true, "controls": {"gain": 0.25}},
		{"id": 2, "branches": [
			{"gain_db": -3.5, "muted": false, "nodes": [{"id": 3, "uri": "b", "state": "blob"}]},
			{"gain_db": 0.0, "muted": true, "nodes": []}
		]},
		{"id": 4, "uri": "c"}
	])";

	const sf::ChainGraph graph = GraphFrom(text, "nested graph");
	Check(graph.PluginCount() == 3, "the nested form loads every plugin node");
	Check(FlatOrder(graph) == "a,b,c", "...in signal order");

	sf::ChainGraph reparsed = GraphFrom(graph.ToJson().dump(), "re-parsed nested graph");
	Check(reparsed == graph, "a graph round-trips through its own JSON");

	const sf::GraphNode* split = graph.Find(2);
	Check(split != nullptr && split->kind == sf::NodeKind::Split, "the split node is a split");
	Check(split->branches[0].gain_db == -3.5f, "branch gain survives");
	Check(split->branches[1].muted, "branch mute survives");
	Check(split->branches[1].nodes.empty(), "an empty lane stays empty");
	Check(graph.Find(3)->stage.state == "blob", "a state blob inside a lane survives");
	Check(graph.Find(1)->stage.bypassed, "bypass survives");

	// Ids may be left out of a hand-written graph; they get filled in rather
	// than being an error.
	const sf::ChainGraph unnumbered = GraphFrom(R"([{"uri": "a"}, {"uri": "b"}])", "graph without ids");
	std::string error;
	Check(unnumbered.Validate(error), "a hand-written graph without ids gets them assigned");
}

// The splitter object (SF-75): how a split divides the signal, in the file.
//
// The compatibility rule is the point of most of this. An absent splitter is
// full range with unity sends, and a splitter that says exactly that is
// written back out as absent -- so every split preset written before SF-75
// round-trips byte-identical, the same promise SF-46 made about ids.
void TestSplitter()
{
	{
		const sf::ChainGraph before = GraphFrom(R"([
			{"id": 1, "branches": [{"nodes": []}, {"nodes": []}]}
		])",
			"a split with no splitter");

		const sf::GraphNode* split = before.Find(1);
		Check(split != nullptr && split->splitter.mode == sf::SplitterMode::Full, "an absent splitter is full range");
		Check(split->splitter.send_db[0] == 0.0f && split->splitter.send_db[1] == 0.0f, "...with unity sends");

		Check(!before.ToJson()[0].contains("splitter"), "...and is written back out as absent, not as a default");

		// The same graph with the defaults spelled out is the same graph, and
		// saving it drops them again: a file that says nothing and a file that
		// says "full range, unity" are one board.
		const sf::ChainGraph spelled = GraphFrom(R"([
			{"id": 1, "branches": [{"nodes": []}, {"nodes": []}],
			 "splitter": {"mode": "full", "send_a": 0.0, "send_b": 0.0, "freq": 800.0,
						  "src_a": "left", "src_b": "right"}}
		])",
			"a split with the defaults written out");

		Check(spelled == before, "a splitter spelled out in full equals an absent one");
		Check(!spelled.ToJson()[0].contains("splitter"), "...and saving it writes nothing");
	}

	// Everything that can be chosen, chosen, and back again.
	{
		const sf::ChainGraph graph = GraphFrom(R"([
			{"id": 1, "branches": [{"nodes": []}, {"nodes": []}],
			 "splitter": {"mode": "crossover", "freq": 250.5, "send_a": -6.0, "send_b": 3.0,
						  "src_a": "sum", "src_b": "left"}}
		])",
			"a split with every splitter field set");

		const sf::GraphNode* split = graph.Find(1);

		Check(split->splitter.mode == sf::SplitterMode::Crossover, "the mode survives");
		Check(split->splitter.frequency == 250.5f, "the crossover frequency survives");
		Check(split->splitter.send_db[0] == -6.0f && split->splitter.send_db[1] == 3.0f, "both sends survive");
		Check(split->splitter.source[0] == sf::SplitterSource::Sum, "src_a survives");
		Check(split->splitter.source[1] == sf::SplitterSource::Left, "src_b survives");

		const sf::ChainGraph reparsed = GraphFrom(graph.ToJson().dump(), "a re-parsed splitter");
		Check(reparsed == graph, "a splitter round-trips through its own JSON");

		// Only what differs is written. A preset that picked a crossover
		// frequency must not also acquire two stereo sources nobody chose.
		const nlohmann::json only_freq = GraphFrom(R"([
			{"id": 1, "branches": [{"nodes": []}, {"nodes": []}], "splitter": {"freq": 120.0}}
		])",
			"a split that only set a frequency")
									 .ToJson();

		Check(only_freq[0]["splitter"].size() == 1, "only the fields that differ from the default are written");
		Check(only_freq[0]["splitter"].contains("freq"), "...and it is the one that was set");
	}

	// A splitter is part of what a split *is*: two boards that sound the same
	// today but would diverge the moment a mode changed are not equal.
	{
		sf::ChainGraph left = Split();
		sf::ChainGraph right = Split();

		Check(left == right, "two identical splits are equal");

		left.Find(2)->splitter.frequency = 1200.0f;
		Check(left != right, "a crossover frequency is part of the board even in full-range mode");

		left = Split();
		left.Find(2)->splitter.send_db[1] = -3.0f;
		Check(left != right, "so is a send");
	}

	// Values that mean nothing are refused rather than read as a default. A
	// typo in a mode name is the difference between two amps and one, and a
	// board that silently ran full range would sound wrong for a reason
	// nothing reported.
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}], "splitter": {"mode": "crosover"}}])",
		"a misspelt splitter mode");
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}], "splitter": {"src_a": "middle"}}])",
		"a stereo source that isn't a side");
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}], "splitter": {"freq": 0}}])",
		"a crossover at 0 Hz");
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}], "splitter": {"freq": -100}}])",
		"a crossover below 0 Hz");
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}], "splitter": {"mode": 2}}])",
		"a splitter mode that isn't a string");
	ExpectGraphRejected(R"([{"branches": [{"nodes": []}, {"nodes": []}], "splitter": "crossover"}])",
		"a splitter that isn't an object");
}

void TestPresetForm()
{
	// A linear preset writes the flat form, with no ids, exactly as it did
	// before the graph existed.
	sf::Preset preset;
	preset.name = "Clean";
	preset.graph = Linear({"a", "b"});

	const nlohmann::json json = preset.ToJson();
	Check(json.contains("chain") && !json.contains("graph"), "a linear preset saves as \"chain\"");
	Check(!json["chain"][0].contains("id"), "...with no ids, so an untouched file doesn't churn");

	sf::Preset reparsed;
	std::string error;
	Check(sf::Preset::FromJson(json, reparsed, error), "the flat form re-parses");
	Check(reparsed == preset, "a linear preset round-trips");

	// Once an edit shuffles the ids, they start being written -- otherwise the
	// node a binding or a selection names would silently become another one on
	// the next load.
	sf::NodeId inserted = sf::kNoNode;
	preset.graph.InsertPlugin({sf::kNoNode, 0, 0}, Stage("z"), inserted, error);

	const nlohmann::json edited_json = preset.ToJson();
	Check(edited_json["chain"][0].contains("id"), "an edited linear preset writes its ids");

	sf::Preset edited_reparsed;
	Check(sf::Preset::FromJson(edited_json, edited_reparsed, error), "...and re-parses");
	Check(edited_reparsed.graph.FlatIndexOf(inserted) == 0, "...with the same ids it was saved with");

	// A split preset writes the nested form.
	sf::Preset parallel;
	parallel.name = "Parallel";
	parallel.graph = Split();

	const nlohmann::json parallel_json = parallel.ToJson();
	Check(parallel_json.contains("graph") && !parallel_json.contains("chain"), "a split preset saves as \"graph\"");

	sf::Preset parallel_reparsed;
	Check(sf::Preset::FromJson(parallel_json, parallel_reparsed, error), "the nested form re-parses");
	Check(parallel_reparsed == parallel, "a split preset round-trips");

	// The built-in gate (SF-85). Per preset, and absent from every preset
	// written before it existed -- so the compatibility rule is the whole of
	// what matters here.
	{
		sf::Preset plain;
		std::string error;

		Check(sf::Preset::FromJson(nlohmann::json::parse(R"({"name": "x", "chain": [{"uri": "a"}]})"), plain, error),
			"a preset with no gate loads");
		Check(!plain.gate.enabled, "an absent gate is a disabled one");
		Check(plain.gate.threshold_db == -50.0f && plain.gate.release_ms == 200.0f, "...with the defaults");

		// **The byte-identity claim, stated explicitly.** Comparing Preset
		// objects would pass whether or not the key was written, so the
		// absence itself is what is asserted.
		Check(!plain.ToJson().contains("gate"), "...and is written back out as absent, not as a default");

		sf::Preset spelled;
		Check(sf::Preset::FromJson(
				  nlohmann::json::parse(
					  R"({"name": "x", "chain": [{"uri": "a"}], "gate": {"enabled": false, "threshold_db": -50.0, "release_ms": 200.0}})"),
				  spelled, error),
			"a preset with the gate defaults spelled out loads");

		Check(spelled == plain, "a gate spelled out at its defaults equals an absent one");
		Check(!spelled.ToJson().contains("gate"), "...and saving it writes nothing");
	}

	{
		sf::Preset gated;
		std::string error;

		Check(sf::Preset::FromJson(
				  nlohmann::json::parse(
					  R"({"name": "x", "chain": [{"uri": "a"}], "gate": {"enabled": true, "threshold_db": -38.5, "release_ms": 90.0}})"),
				  gated, error),
			"a preset with a gate loads");

		Check(gated.gate.enabled && gated.gate.threshold_db == -38.5f && gated.gate.release_ms == 90.0f,
			"every gate field survives");

		sf::Preset reparsed;
		Check(sf::Preset::FromJson(gated.ToJson(), reparsed, error), "a gate re-parses");
		Check(reparsed == gated, "a gate round-trips through its own JSON");

		// The gate is part of what a preset *is*, or an edit to it would not
		// register as a change and would never be saved.
		sf::Preset without = gated;
		without.gate.threshold_db = -40.0f;
		Check(without != gated, "two presets differing only in gate threshold are not equal");
	}

	// Numbers out of range are clamped -- a threshold has silly values rather
	// than wrong ones -- but a value of the wrong *type* is still refused, with
	// the field named.
	{
		sf::Preset clamped;
		std::string error;

		Check(sf::Preset::FromJson(
				  nlohmann::json::parse(
					  R"({"name": "x", "chain": [{"uri": "a"}], "gate": {"threshold_db": -200.0, "release_ms": 99999.0}})"),
				  clamped, error),
			"an out-of-range gate loads rather than taking the preset away");

		Check(clamped.gate.threshold_db == sf::kGateMinThresholdDb, "...with the threshold clamped");
		Check(clamped.gate.release_ms == sf::kGateMaxReleaseMs, "...and the release clamped");
	}

	// The remembered channel layout (SF-86). Saved so that collapse, save,
	// reload, expand still puts the pedals back.
	{
		sf::Preset plain;
		std::string error;

		Check(sf::Preset::FromJson(nlohmann::json::parse(R"({"name": "x", "chain": [{"uri": "a"}]})"), plain, error),
			"a preset with no remembered layout loads");
		Check(!plain.collapsed_split.present, "...and remembers nothing");
		Check(!plain.ToJson().contains("collapsed_split"), "...and writes nothing back out");

		sf::Preset remembered;
		Check(sf::Preset::FromJson(
				  nlohmann::json::parse(
					  R"({"name": "x", "chain": [{"uri": "a"}], "collapsed_split": {"at": 1,
						 "splitter": {"mode": "crossover", "freq": 250.0},
						 "branches": [{"gain_db": -3.0, "muted": false, "nodes": [2, 3]},
									  {"gain_db": 0.0, "muted": true, "nodes": [4]}]}})"),
				  remembered, error),
			"a preset with a remembered layout loads");

		Check(remembered.collapsed_split.present, "the layout is remembered");
		Check(remembered.collapsed_split.trunk_index == 1, "...with where the split sat");
		Check(remembered.collapsed_split.lane[0] == std::vector<sf::NodeId>{2, 3}, "...which pedals were on A");
		Check(remembered.collapsed_split.lane[1] == std::vector<sf::NodeId>{4}, "...and which on B");
		Check(remembered.collapsed_split.splitter.mode == sf::SplitterMode::Crossover &&
				remembered.collapsed_split.splitter.frequency == 250.0f,
			"...and the splitter it had");
		Check(remembered.collapsed_split.gain_db[0] == -3.0f && remembered.collapsed_split.muted[1],
			"...and both channels' levels");

		sf::Preset reparsed;
		Check(sf::Preset::FromJson(remembered.ToJson(), reparsed, error), "a remembered layout re-parses");
		Check(reparsed == remembered, "a remembered layout round-trips through its own JSON");
	}

	// **Malformed is ignored, not refused** -- unlike everything else here.
	// This is a convenience the board left itself, not something the player
	// wrote, and losing a preset over it would be a poor trade for losing a
	// layout that can be rebuilt by hand in seconds.
	{
		sf::Preset odd;
		std::string error;

		Check(sf::Preset::FromJson(
				  nlohmann::json::parse(R"({"name": "x", "chain": [{"uri": "a"}], "collapsed_split": "nonsense"})"),
				  odd, error),
			"a malformed remembered layout does not take the preset with it");
		Check(!odd.collapsed_split.present, "...and is simply forgotten");

		sf::Preset empty;
		Check(sf::Preset::FromJson(
				  nlohmann::json::parse(
					  R"({"name": "x", "chain": [{"uri": "a"}], "collapsed_split": {"branches": [{"nodes": []}, {"nodes": []}]}})"),
				  empty, error),
			"a remembered layout naming no pedals loads");
		Check(!empty.collapsed_split.present, "...and counts as remembering nothing");
	}

	ExpectPresetRejected(R"({"name": "x", "chain": [{"uri": "a"}], "gate": 5})", "a gate that isn't an object");
	ExpectPresetRejected(
		R"({"name": "x", "chain": [{"uri": "a"}], "gate": {"enabled": 1}})", "a gate enabled by a number");
	ExpectPresetRejected(
		R"({"name": "x", "chain": [{"uri": "a"}], "gate": {"threshold_db": "loud"}})", "a threshold that isn't a number");

	ExpectPresetRejected(R"({"name": "x", "chain": [], "graph": []})", "a preset with both chain and graph");
	ExpectPresetRejected(R"({"name": "x"})", "a preset with neither");
	ExpectPresetRejected(R"({"name": "x", "chain": [{"branches": [{"nodes": []}, {"nodes": []}]}]})",
		"a split hiding in the flat form");
}

}  // namespace

int main()
{
	TestIdentity();
	TestMergeIndex();
	TestSignalOrder();
	TestEdits();
	TestValidation();
	TestJsonRoundTrip();
	TestSplitter();
	TestPresetForm();

	if (g_failures > 0)
	{
		std::printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("\nall checks passed\n");

	return 0;
}
