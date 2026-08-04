#include "ChainPreset.h"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace sf
{

Preset CaptureChainPreset(const Chain& chain, UridMap& urid_map, const std::string& name)
{
	Preset preset;
	preset.name = name;

	for (size_t i = 0; i < chain.StageCount(); ++i)
	{
		const PluginStage* stage = chain.Stage(i);

		PresetStage preset_stage;
		preset_stage.uri = stage->Uri();
		preset_stage.bypassed = stage->IsBypassed();
		preset_stage.controls = stage->CaptureControls();
		preset_stage.state = stage->SaveState(urid_map);

		// Linear, because a running Chain is: the split/merge shape lives in
		// the preset the chain was built from, and carrying it back through a
		// capture is SF-49's job, once there is a chain that can hold one.
		preset.graph.AppendPlugin(preset_stage);
	}

	return preset;
}

Preset CaptureChainPresetInto(const Preset& source, const Chain& chain, UridMap& urid_map,
	const std::vector<size_t>& skipped_indices)
{
	Preset out = source;

	const std::vector<FlatEntry> flat = out.graph.Flatten();

	size_t stage_index = 0;

	for (size_t i = 0; i < flat.size(); ++i)
	{
		const bool skipped =
			std::find(skipped_indices.begin(), skipped_indices.end(), i) != skipped_indices.end();

		if (skipped)
		{
			// Nothing was built for this one, so there is nothing to read
			// back. Leaving the file's values is the only honest option --
			// the alternative is writing out a stage's worth of defaults over
			// settings the player still wants when the plugin comes back.
			continue;
		}

		if (stage_index >= chain.StageCount())
		{
			break;
		}

		const PluginStage* stage = chain.Stage(stage_index++);

		PresetStage* node = out.graph.PluginAt(i);

		if (node == nullptr)
		{
			break;
		}

		node->bypassed = stage->IsBypassed();
		node->controls = stage->CaptureControls();
		node->state = stage->SaveState(urid_map);
	}

	return out;
}

std::unique_ptr<Chain> BuildChainFromPreset(LilvWorld* world, const Preset& preset, double sample_rate,
	uint32_t max_block_size, UridMap& urid_map, std::string& error, ChainBuildReport* report, unsigned board_inputs)
{
	if (report)
	{
		*report = ChainBuildReport();
	}

	const std::vector<FlatEntry> flat = preset.graph.Flatten();

	if (flat.empty())
	{
		error = "preset '" + preset.name + "' has an empty chain";
		return nullptr;
	}

	// Which stages of the file made it into the chain, in chain order. Kept
	// because a skipped stage shifts everything after it: chain stage 1 may be
	// the file's stage 2, and restoring state by the wrong index would load one
	// pedal's settings into another.
	std::vector<size_t> kept;
	size_t position = 0;

	// The graph, translated into the shape lv2/ speaks. This is the only place
	// the two models meet -- the hosting layer never learns what a preset is.
	// The walk order is ChainGraph::Flatten's, so `position` counts the same
	// stages in the same order the chain will.
	std::function<SpecList(const NodeList&)> translate = [&](const NodeList& nodes)
	{
		SpecList out;

		for (const GraphNode& node : nodes)
		{
			if (node.kind == NodeKind::Split)
			{
				SpecNode split;
				split.branches.resize(2);

				// The splitter, in the terms lv2/ speaks. Two enums that
				// mean the same thing in two layers is the price of lv2/ not
				// knowing what a preset is; translating them here is what
				// keeps that true (SF-48, SF-75).
				switch (node.splitter.mode)
				{
					case SplitterMode::Full:
						split.splitter.mode = SplitMode::Full;
						break;

					case SplitterMode::Crossover:
						split.splitter.mode = SplitMode::Crossover;
						break;

					case SplitterMode::Stereo:
						split.splitter.mode = SplitMode::Stereo;
						break;
				}

				split.splitter.frequency = node.splitter.frequency;

				// Whether a lane was authored empty, which is a dry wire and
				// perfectly ordinary -- as opposed to emptied by dropping,
				// which is the case below.
				bool emptied = false;

				for (unsigned b = 0; b < 2; ++b)
				{
					// Both lanes are walked in order, so the branch's own
					// nodes are numbered before the trunk resumes.
					split.branches[b].nodes = translate(node.branches[b].nodes);
					split.branches[b].gain_db = node.branches[b].gain_db;
					split.branches[b].muted = node.branches[b].muted;

					emptied = emptied || (split.branches[b].nodes.empty() && !node.branches[b].nodes.empty());

					// The send sits on the splitter in the file and on the
					// branch in the plan: the file groups it with the other
					// half of "what the splitter does", the planner wants it
					// where the lane it feeds is.
					split.branches[b].send_db = node.splitter.send_db[b];

					switch (node.splitter.source[b])
					{
						case SplitterSource::Left:
							split.splitter.source[b] = SplitSource::Left;
							break;

						case SplitterSource::Right:
							split.splitter.source[b] = SplitSource::Right;
							break;

						case SplitterSource::Sum:
							split.splitter.source[b] = SplitSource::Sum;
							break;
					}
				}

				// A lane that lost everything it had takes the split with it
				// (SF-58). Left standing, it would be a dry copy summed
				// against the other lane -- two correlated signals at unity,
				// which is +6 dB and a tone nobody saved. Collapsing to a
				// series is what the board would have been without the pedal
				// that is missing, which is the nearest honest thing.
				//
				// An *authored* empty lane is untouched: that is a dry
				// parallel path, and the +6 dB is the point of it.
				if (emptied)
				{
					for (unsigned b = 0; b < 2; ++b)
					{
						for (SpecNode& inner : split.branches[b].nodes)
						{
							out.push_back(std::move(inner));
						}
					}

					if (report)
					{
						++report->collapsed_splits;
					}

					std::fprintf(stderr,
						"Warning: a split in preset '%s' lost a whole channel to a missing plugin -- "
						"collapsing it to a series.\n",
						preset.name.c_str());

					continue;
				}

				out.push_back(std::move(split));
				continue;
			}

			const size_t index = position++;

			// Missing plugins are the one failure a player can't fix mid-song,
			// so they cost that stage rather than the whole preset. Anything
			// else -- a plugin that won't instantiate, a channel count this
			// host can't adapt -- is still Chain::Create's to refuse.
			if (!Chain::IsPluginInstalled(world, node.stage.uri))
			{
				std::fprintf(stderr, "Warning: skipping stage %zu of preset '%s' -- plugin not installed: %s\n", index,
					preset.name.c_str(), node.stage.uri.c_str());

				if (report)
				{
					report->skipped_indices.push_back(index);
					report->skipped_uris.push_back(node.stage.uri);
				}

				continue;
			}

			kept.push_back(index);

			SpecNode plugin;
			plugin.uri = node.stage.uri;
			out.push_back(std::move(plugin));
		}

		return out;
	};

	const ChainSpec spec = translate(preset.graph.Trunk());

	if (kept.empty())
	{
		error = "preset '" + preset.name + "' needs " + std::to_string(flat.size()) +
			" plugin(s) and none of them are installed";
		return nullptr;
	}

	std::string reason;
	std::unique_ptr<Chain> chain = Chain::Create(world, spec, sample_rate, max_block_size, urid_map, &reason, board_inputs);

	if (!chain)
	{
		error = reason.empty() ? "could not build the chain for preset '" + preset.name + "'" : reason;
		return nullptr;
	}

	for (size_t i = 0; i < kept.size(); ++i)
	{
		const PresetStage& preset_stage = *preset.graph.PluginAt(kept[i]);
		PluginStage* stage = chain->Stage(i);

		if (!stage->RestoreState(preset_stage.state, urid_map))
		{
			// The file's number, not the chain's: it's the one the user can
			// find in the preset they'd have to edit.
			error = "could not restore state for stage " + std::to_string(kept[i]) + " (" + preset_stage.uri + ")";
			return nullptr;
		}

		// A stage saved bypassed comes back bypassed, so a preset captures the
		// whole board state and not just the plugins that happened to be on.
		stage->SetBypassed(preset_stage.bypassed);

		// After state restore, so the preset's own controls win.
		for (const auto& [symbol, value] : preset_stage.controls)
		{
			stage->SetControl(symbol, value);
		}
	}

	chain->Settle();

	return chain;
}

}  // namespace sf
