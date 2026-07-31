#include "Chain.h"

#include <cstdio>

namespace sf
{

namespace
{

// Audio ports of `stage` with the given direction, in port-index order.
std::vector<uint32_t> AudioPortIndices(const PluginStage& stage, PortDirection direction)
{
	std::vector<uint32_t> indices;

	for (const PortInfo& port : stage.Ports())
	{
		if (port.type == PortType::Audio && port.direction == direction)
		{
			indices.push_back(port.index);
		}
	}

	return indices;
}

}  // namespace

bool Chain::IsPluginInstalled(LilvWorld* world, const std::string& plugin_uri)
{
	LilvNode* uri = lilv_new_uri(world, plugin_uri.c_str());

	if (!uri)
	{
		// Not even a URI, so nothing could ever match it.
		return false;
	}

	const LilvPlugin* plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri);
	lilv_node_free(uri);

	return plugin != nullptr;
}

std::unique_ptr<Chain> Chain::Create(LilvWorld* world, const std::vector<std::string>& plugin_uris,
	double sample_rate, uint32_t max_block_size, UridMap& urid_map, std::string* error)
{
	auto chain = std::unique_ptr<Chain>(new Chain());

	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

	// Reported and printed together, so a failure path can't update one and
	// forget the other. The print stays: a headless run has no other channel.
	char reason[256];

	auto fail = [&error, &reason]
	{
		std::fprintf(stderr, "%s\n", reason);

		if (error)
		{
			*error = reason;
		}

		return nullptr;
	};

	for (const std::string& uri_string : plugin_uris)
	{
		LilvNode* uri = lilv_new_uri(world, uri_string.c_str());
		const LilvPlugin* plugin = uri ? lilv_plugins_get_by_uri(plugins, uri) : nullptr;

		if (uri)
		{
			lilv_node_free(uri);
		}

		if (!plugin)
		{
			std::snprintf(reason, sizeof(reason), "plugin not installed: %s", uri_string.c_str());
			return fail();
		}

		std::unique_ptr<PluginStage> stage = PluginStage::Create(world, plugin, sample_rate, max_block_size, urid_map);

		if (!stage)
		{
			std::snprintf(reason, sizeof(reason), "could not instantiate %s", uri_string.c_str());
			return fail();
		}

		chain->stages_.push_back(std::move(stage));
	}

	if (chain->stages_.empty())
	{
		std::snprintf(reason, sizeof(reason), "a chain needs at least one stage");
		return fail();
	}

	// Wire stage N's outputs directly into stage N+1's inputs.
	for (size_t i = 0; i + 1 < chain->stages_.size(); ++i)
	{
		PluginStage& upstream = *chain->stages_[i];
		PluginStage& downstream = *chain->stages_[i + 1];

		const std::vector<uint32_t> outputs = AudioPortIndices(upstream, PortDirection::Output);
		const std::vector<uint32_t> inputs = AudioPortIndices(downstream, PortDirection::Input);

		if (outputs.size() != inputs.size())
		{
			std::snprintf(reason, sizeof(reason),
				"stage %zu has %zu output(s) but stage %zu wants %zu input(s), and channel adaptation isn't "
				"implemented yet",
				i, outputs.size(), i + 1, inputs.size());
			return fail();
		}

		for (size_t ch = 0; ch < outputs.size(); ++ch)
		{
			chain->link_buffers_.emplace_back(max_block_size, 0.0f);
			float* buffer = chain->link_buffers_.back().data();

			upstream.ConnectAudioPort(outputs[ch], buffer);
			downstream.ConnectAudioPort(inputs[ch], buffer);
		}
	}

	// Boundary ports: first stage's audio inputs, then last stage's audio
	// outputs, renumbered into a flat 0..k-1 index space for this Chain.
	PluginStage& first = *chain->stages_.front();
	PluginStage& last = *chain->stages_.back();

	for (const PortInfo& port : first.Ports())
	{
		if (port.type != PortType::Audio || port.direction != PortDirection::Input)
		{
			continue;
		}

		PortInfo boundary_port = port;
		boundary_port.index = static_cast<uint32_t>(chain->boundary_ports_.size());
		chain->boundary_ports_.push_back(boundary_port);
		chain->boundary_bindings_.push_back({0, port.index});
	}

	for (const PortInfo& port : last.Ports())
	{
		if (port.type != PortType::Audio || port.direction != PortDirection::Output)
		{
			continue;
		}

		PortInfo boundary_port = port;
		boundary_port.index = static_cast<uint32_t>(chain->boundary_ports_.size());
		chain->boundary_ports_.push_back(boundary_port);
		chain->boundary_bindings_.push_back({static_cast<uint32_t>(chain->stages_.size() - 1), port.index});
	}

	return chain;
}

void Chain::ConnectAudioPort(uint32_t index, float* buffer)
{
	const BoundaryBinding& binding = boundary_bindings_[index];
	stages_[binding.stage_index]->ConnectAudioPort(binding.port_index, buffer);
}

void Chain::Settle()
{
	for (std::unique_ptr<PluginStage>& stage : stages_)
	{
		stage->SettleWorker();
	}
}

void Chain::PrepareAtomPorts()
{
	for (std::unique_ptr<PluginStage>& stage : stages_)
	{
		stage->PrepareAtomPorts();
	}
}

void Chain::Run(uint32_t sample_count)
{
	for (std::unique_ptr<PluginStage>& stage : stages_)
	{
		stage->Run(sample_count);
	}
}

}  // namespace sf
