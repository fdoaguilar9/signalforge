#include "ChainPreset.h"

#include <cstdio>

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

		preset.chain.push_back(std::move(preset_stage));
	}

	return preset;
}

std::unique_ptr<Chain> BuildChainFromPreset(LilvWorld* world, const Preset& preset, double sample_rate,
	uint32_t max_block_size, UridMap& urid_map, std::string& error, ChainBuildReport* report)
{
	if (report)
	{
		*report = ChainBuildReport();
	}

	if (preset.chain.empty())
	{
		error = "preset '" + preset.name + "' has an empty chain";
		return nullptr;
	}

	// Which stages of the file made it into the chain, in chain order. Kept
	// because a skipped stage shifts everything after it: chain stage 1 may be
	// the file's stage 2, and restoring state by the wrong index would load one
	// pedal's settings into another.
	std::vector<size_t> kept;
	std::vector<std::string> uris;
	kept.reserve(preset.chain.size());
	uris.reserve(preset.chain.size());

	for (size_t i = 0; i < preset.chain.size(); ++i)
	{
		const PresetStage& stage = preset.chain[i];

		// Missing plugins are the one failure a player can't fix mid-song, so
		// they cost that stage rather than the whole preset. Anything else --
		// a plugin that won't instantiate, a channel-count mismatch -- is
		// still Chain::Create's to refuse.
		if (!Chain::IsPluginInstalled(world, stage.uri))
		{
			std::fprintf(stderr, "Warning: skipping stage %zu of preset '%s' -- plugin not installed: %s\n", i,
				preset.name.c_str(), stage.uri.c_str());

			if (report)
			{
				report->skipped_indices.push_back(i);
				report->skipped_uris.push_back(stage.uri);
			}

			continue;
		}

		kept.push_back(i);
		uris.push_back(stage.uri);
	}

	if (uris.empty())
	{
		error = "preset '" + preset.name + "' needs " + std::to_string(preset.chain.size()) +
			" plugin(s) and none of them are installed";
		return nullptr;
	}

	std::string reason;
	std::unique_ptr<Chain> chain = Chain::Create(world, uris, sample_rate, max_block_size, urid_map, &reason);

	if (!chain)
	{
		error = reason.empty() ? "could not build the chain for preset '" + preset.name + "'" : reason;
		return nullptr;
	}

	for (size_t i = 0; i < kept.size(); ++i)
	{
		const PresetStage& preset_stage = preset.chain[kept[i]];
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
