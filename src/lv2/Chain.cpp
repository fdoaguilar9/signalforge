#include "Chain.h"

#include <algorithm>
#include <cstdio>

namespace sf
{

// A merge gain crosses from the control thread to the audio thread as a plain
// atomic load per block, which is only lock-free-and-therefore-RT-safe if the
// platform says so. It does everywhere this runs; say it out loud so a port to
// somewhere it doesn't fails to compile instead of taking a lock in Run().
static_assert(std::atomic<float>::is_always_lock_free, "merge gains are read from the audio thread");

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
	double sample_rate, uint32_t max_block_size, UridMap& urid_map, std::string* error, unsigned board_inputs)
{
	return Create(world, LinearSpec(plugin_uris), sample_rate, max_block_size, urid_map, error, board_inputs);
}

std::unique_ptr<Chain> Chain::Create(LilvWorld* world, const ChainSpec& spec, double sample_rate,
	uint32_t max_block_size, UridMap& urid_map, std::string* error, unsigned board_inputs)
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

	// Instantiated in signal order, so a stage index means the same thing here
	// as it does in the preset the spec came from.
	for (const std::string& uri_string : FlattenSpec(spec))
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

	// What the planner needs to know about each stage, asked of the plugins
	// themselves rather than assumed.
	std::vector<StageIo> stage_io;
	stage_io.reserve(chain->stages_.size());

	for (const std::unique_ptr<PluginStage>& stage : chain->stages_)
	{
		StageIo io;
		io.inputs = static_cast<unsigned>(AudioPortIndices(*stage, PortDirection::Input).size());
		io.outputs = static_cast<unsigned>(AudioPortIndices(*stage, PortDirection::Output).size());
		io.latency = stage->LatencySamples();

		stage_io.push_back(io);
	}

	// Left to itself the board takes as many inputs as the first pedal in the
	// signal path wants -- one for a guitar, two for a stereo effect at the
	// head of the chain. Everything downstream is adapted rather than required
	// to agree. An explicit 1 or 2 overrides that, so the board's width is a
	// decision rather than a consequence of what happens to be first (SF-52).
	const unsigned inputs =
		board_inputs == kAutoBoardInputs ? std::max(1u, stage_io.front().inputs) : board_inputs;

	std::string plan_error;

	if (!PlanChain(spec, stage_io, inputs, chain->plan_, plan_error))
	{
		std::snprintf(reason, sizeof(reason), "%s", plan_error.c_str());
		return fail();
	}

	// The buffer table. Every bus gets storage; the ones at the board's edge
	// have theirs pointed away from on the first cycle and never used, which
	// costs a block of silence each and saves the audio thread a branch.
	chain->bus_storage_.assign(chain->plan_.bus_count, std::vector<float>(max_block_size, 0.0f));
	chain->bus_ptr_.resize(chain->plan_.bus_count);
	chain->bus_bindings_.resize(chain->plan_.bus_count);

	for (uint32_t bus = 0; bus < chain->plan_.bus_count; ++bus)
	{
		chain->bus_ptr_[bus] = chain->bus_storage_[bus].data();
	}

	chain->delay_lines_.resize(chain->plan_.delay_lengths.size());

	for (size_t i = 0; i < chain->plan_.delay_lengths.size(); ++i)
	{
		chain->delay_lines_[i].Reset(chain->plan_.delay_lengths[i]);
	}

	// The plan carries crossover frequencies rather than coefficients, because
	// designing a biquad needs a sample rate and the planner deliberately has
	// none. This is where the two meet.
	chain->filters_.resize(chain->plan_.filters.size());

	for (size_t i = 0; i < chain->plan_.filters.size(); ++i)
	{
		chain->filters_[i].Reset(sample_rate, chain->plan_.filters[i]);
	}

	// Merge gains start where the plan put them, and the audio thread starts
	// already at that value rather than sweeping up to it from zero -- a
	// rebuilt chain must come back at the level it went away at.
	{
		const size_t count = chain->plan_.merge_gain.size();

		chain->merge_target_ = std::make_unique<std::atomic<float>[]>(count);
		chain->merge_current_.assign(count, 0.0f);
		chain->merge_from_.assign(count, 0.0f);

		for (size_t i = 0; i < count; ++i)
		{
			chain->merge_target_[i].store(chain->plan_.merge_gain[i], std::memory_order_relaxed);
			chain->merge_current_[i] = chain->plan_.merge_gain[i];
			chain->merge_from_[i] = chain->plan_.merge_gain[i];
		}
	}

	// Wire every stage's audio ports to the buses the plan put them on, and
	// remember the wiring so a bus that moves can take its ports with it.
	for (size_t s = 0; s < chain->stages_.size(); ++s)
	{
		PluginStage& stage = *chain->stages_[s];

		const std::vector<uint32_t> inputs = AudioPortIndices(stage, PortDirection::Input);
		const std::vector<uint32_t> outputs = AudioPortIndices(stage, PortDirection::Output);

		const std::vector<uint32_t>& input_buses = chain->plan_.stage_input_buses[s];
		const std::vector<uint32_t>& output_buses = chain->plan_.stage_output_buses[s];

		for (size_t ch = 0; ch < inputs.size(); ++ch)
		{
			const uint32_t bus = input_buses[ch];
			chain->bus_bindings_[bus].push_back({static_cast<uint32_t>(s), inputs[ch]});
			stage.ConnectAudioPort(inputs[ch], chain->bus_ptr_[bus]);
		}

		for (size_t ch = 0; ch < outputs.size(); ++ch)
		{
			const uint32_t bus = output_buses[ch];
			chain->bus_bindings_[bus].push_back({static_cast<uint32_t>(s), outputs[ch]});
			stage.ConnectAudioPort(outputs[ch], chain->bus_ptr_[bus]);
		}
	}

	// Boundary ports: the board's own inputs, then its own outputs, numbered
	// 0..k-1. Synthesized rather than borrowed from a stage's port list --
	// with adapters in the way, the board's edge may not be any one plugin's
	// port any more.
	auto add_boundary = [&chain](uint32_t bus, PortDirection direction, unsigned channel)
	{
		PortInfo port;
		port.index = static_cast<uint32_t>(chain->boundary_ports_.size());
		port.type = PortType::Audio;
		port.direction = direction;
		port.symbol = (direction == PortDirection::Input ? "in_" : "out_") + std::to_string(channel + 1);
		port.name = port.symbol;

		chain->boundary_ports_.push_back(port);
		chain->boundary_bus_.push_back(bus);
	};

	for (size_t ch = 0; ch < chain->plan_.input_buses.size(); ++ch)
	{
		add_boundary(chain->plan_.input_buses[ch], PortDirection::Input, static_cast<unsigned>(ch));
	}

	for (size_t ch = 0; ch < chain->plan_.output_buses.size(); ++ch)
	{
		add_boundary(chain->plan_.output_buses[ch], PortDirection::Output, static_cast<unsigned>(ch));
	}

	return chain;
}

void Chain::ConnectAudioPort(uint32_t index, float* buffer)
{
	const uint32_t bus = boundary_bus_[index];

	bus_ptr_[bus] = buffer;

	// Any plugin port sitting on this bus has to follow it. There are at most
	// a handful, they're precomputed, and nothing here allocates -- this runs
	// on the audio thread, once per boundary port per cycle.
	for (const PortBinding& binding : bus_bindings_[bus])
	{
		stages_[binding.stage]->ConnectAudioPort(binding.port, buffer);
	}
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

void Chain::SetMergeGain(uint32_t merge, unsigned lane, float gain)
{
	const size_t index = 2 * static_cast<size_t>(merge) + lane;

	if (lane > 1 || index >= merge_current_.size())
	{
		return;
	}

	merge_target_[index].store(gain, std::memory_order_relaxed);
}

float Chain::MergeGain(uint32_t merge, unsigned lane) const
{
	const size_t index = 2 * static_cast<size_t>(merge) + lane;

	if (lane > 1 || index >= merge_current_.size())
	{
		return 0.0f;
	}

	return merge_target_[index].load(std::memory_order_relaxed);
}

void Chain::Run(uint32_t sample_count)
{
	// One read of each merge gain per block, before any op looks at one. Every
	// Mix op of a given merge then sweeps between the same two numbers, and
	// the value can't shift halfway down the op list and leave two channels of
	// the same merge at different gains.
	for (size_t i = 0; i < merge_current_.size(); ++i)
	{
		merge_from_[i] = merge_current_[i];
		merge_current_[i] = merge_target_[i].load(std::memory_order_relaxed);
	}

	for (const ChainOp& op : plan_.ops)
	{
		switch (op.kind)
		{
			case OpKind::Run:
				stages_[op.stage]->Run(sample_count);
				break;

			case OpKind::Copy:
				OpCopy(bus_ptr_[op.dst], bus_ptr_[op.src0], sample_count);
				break;

			case OpKind::Downmix:
				OpDownmix(bus_ptr_[op.dst], bus_ptr_[op.src0], bus_ptr_[op.src1], sample_count);
				break;

			case OpKind::Delay:
				delay_lines_[op.delay_line].Process(bus_ptr_[op.dst], bus_ptr_[op.src0], sample_count);
				break;

			case OpKind::Gain:
				OpGain(bus_ptr_[op.dst], bus_ptr_[op.src0], op.gain0, sample_count);
				break;

			case OpKind::Filter:
				filters_[op.filter].Process(bus_ptr_[op.dst], bus_ptr_[op.src0], sample_count);
				break;

			case OpKind::Mix:
			{
				const size_t lane0 = 2 * static_cast<size_t>(op.merge);

				OpMixRamp(bus_ptr_[op.dst], bus_ptr_[op.src0], bus_ptr_[op.src1], merge_from_[lane0],
					merge_current_[lane0], merge_from_[lane0 + 1], merge_current_[lane0 + 1], sample_count);
				break;
			}
		}
	}
}

}  // namespace sf
