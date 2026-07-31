#include "PluginStage.h"

#include <cstdio>
#include <cstring>
#include <limits>

#include <cstdlib>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/worker/worker.h>

namespace sf
{

namespace
{

// Plenty for the small control-style atom sequences (e.g. patch:Set
// messages carrying a model file path) this host deals with today.
constexpr uint32_t kAtomBufferBytes = 4096;

void InitAtomSequence(uint8_t* buffer)
{
	auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(buffer);
	sequence->atom.type = 0;
	sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
	sequence->body.unit = 0;
	sequence->body.pad = 0;
}

// Identity path mapping: our host doesn't (yet) virtualize paths relative to
// a bundle/preset directory, so "abstract" and "absolute" are the same
// string. Allocated with strdup/malloc, matching what plugins fall back to
// freeing with plain free() when no state:freePath feature is provided.
char* MapPathIdentity(LV2_State_Map_Path_Handle, const char* path)
{
	return strdup(path);
}

// Context + callback for a one-shot, single-property LV2_State_Retrieve_Function:
// serves exactly one atom:Path-typed property without needing a real state
// store/preset file.
struct SinglePropertyRetrieveContext
{
	LV2_URID property;
	LV2_URID atom_path_type;
	const std::string* value;
};

const void* RetrieveSingleProperty(
	LV2_State_Handle handle, uint32_t key, size_t* size, uint32_t* type, uint32_t* flags)
{
	auto* ctx = static_cast<SinglePropertyRetrieveContext*>(handle);

	if (key != ctx->property)
	{
		return nullptr;
	}

	*size = ctx->value->size() + 1;
	*type = ctx->atom_path_type;
	*flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;

	return ctx->value->c_str();
}

// The mirror of the above: pulls one property back out of a plugin, so the UI
// can show which model or impulse response is actually loaded. Asking the
// plugin is the only reliable way -- a path that arrived inside a restored
// state blob was never seen by this program.
struct SinglePropertyStoreContext
{
	LV2_URID property;
	LV2_URID atom_path_type;
	std::string* value;
};

LV2_State_Status StoreSingleProperty(
	LV2_State_Handle handle, uint32_t key, const void* value, size_t size, uint32_t type, uint32_t flags)
{
	(void)flags;

	auto* ctx = static_cast<SinglePropertyStoreContext*>(handle);

	if (key != ctx->property || type != ctx->atom_path_type || !value || size == 0)
	{
		return LV2_STATE_SUCCESS;
	}

	// Stored as a null-terminated string, but trust `size` rather than the
	// terminator: a plugin is free to hand back a length that includes it or
	// not, and reading past the buffer to find out is not an option.
	const char* text = static_cast<const char*>(value);
	const size_t length = text[size - 1] == '\0' ? size - 1 : size;

	ctx->value->assign(text, length);

	return LV2_STATE_SUCCESS;
}

}  // namespace

std::unique_ptr<PluginStage> PluginStage::Create(
	LilvWorld* world, const LilvPlugin* plugin, double sample_rate, uint32_t max_block_size, UridMap& urid_map)
{
	auto stage = std::unique_ptr<PluginStage>(new PluginStage());

	stage->world_ = world;
	stage->plugin_ = plugin;
	stage->ports_ = ClassifyPorts(world, plugin);

	const uint32_t port_count = static_cast<uint32_t>(stage->ports_.size());
	stage->control_storage_.resize(port_count, 0.0f);
	stage->atom_storage_.resize(port_count);
	stage->audio_scratch_.resize(port_count);
	stage->audio_buffers_.assign(port_count, nullptr);

	// Cached so the bypass path in Run() does no searching per block.
	for (const PortInfo& port : stage->ports_)
	{
		if (port.type != PortType::Audio)
		{
			continue;
		}

		(port.direction == PortDirection::Input ? stage->audio_inputs_ : stage->audio_outputs_).push_back(port.index);
	}

	stage->max_block_length_value_ = static_cast<int32_t>(max_block_size);

	const LV2_URID max_block_length_key = urid_map.Map(LV2_BUF_SIZE__maxBlockLength);
	const LV2_URID int_type = urid_map.Map(LV2_ATOM__Int);

	stage->float_urid_ = urid_map.Map(LV2_ATOM__Float);

	stage->options_[0] = {LV2_OPTIONS_INSTANCE, 0, max_block_length_key, sizeof(int32_t), int_type,
		&stage->max_block_length_value_};
	stage->options_[1] = {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr};

	stage->options_feature_ = {LV2_OPTIONS__options, stage->options_};
	stage->bounded_block_length_feature_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};

	const LV2_Feature* features[] = {
		urid_map.MapFeature(),
		urid_map.UnmapFeature(),
		&stage->options_feature_,
		&stage->bounded_block_length_feature_,
		stage->worker_.ScheduleFeature(),
		nullptr,
	};

	stage->instance_ = lilv_plugin_instantiate(plugin, sample_rate, features);

	if (!stage->instance_)
	{
		std::fprintf(stderr,
			"Failed to instantiate plugin (likely missing a required host feature this host doesn't yet "
			"implement).\n");

		return nullptr;
	}

	const auto* worker_interface = static_cast<const LV2_Worker_Interface*>(
		lilv_instance_get_extension_data(stage->instance_, LV2_WORKER__interface));

	if (worker_interface)
	{
		stage->worker_.Bind(worker_interface, lilv_instance_get_handle(stage->instance_));
	}

	for (const PortInfo& port : stage->ports_)
	{
		switch (port.type)
		{
			case PortType::Control:
				stage->control_storage_[port.index] = port.default_value;
				lilv_instance_connect_port(stage->instance_, port.index, &stage->control_storage_[port.index]);
				break;

			case PortType::Atom:
				stage->atom_storage_[port.index].resize(kAtomBufferBytes);
				InitAtomSequence(stage->atom_storage_[port.index].data());
				lilv_instance_connect_port(stage->instance_, port.index, stage->atom_storage_[port.index].data());
				break;

			case PortType::Audio:
			case PortType::Cv:
				// Scratch buffer so activate()/an initial run() (before the
				// caller repoints via ConnectAudioPort) has a valid pointer.
				stage->audio_scratch_[port.index].resize(max_block_size, 0.0f);
				stage->audio_buffers_[port.index] = stage->audio_scratch_[port.index].data();
				lilv_instance_connect_port(stage->instance_, port.index, stage->audio_scratch_[port.index].data());
				break;

			case PortType::Unknown:
				lilv_instance_connect_port(stage->instance_, port.index, nullptr);
				break;
		}
	}

	lilv_instance_activate(stage->instance_);

	return stage;
}

PluginStage::~PluginStage()
{
	// Must happen before lilv_instance_free(): the worker thread calls back
	// into the plugin instance, so it has to be stopped while the instance
	// is still valid.
	worker_.Stop();

	if (instance_)
	{
		lilv_instance_deactivate(instance_);
		lilv_instance_free(instance_);
	}
}

void PluginStage::ConnectAudioPort(uint32_t index, float* buffer)
{
	audio_buffers_[index] = buffer;
	lilv_instance_connect_port(instance_, index, buffer);
}

void PluginStage::SetBypassed(bool bypassed)
{
	bypassed_.store(bypassed, std::memory_order_release);
}

bool PluginStage::IsBypassed() const
{
	return bypassed_.load(std::memory_order_acquire);
}

bool PluginStage::SetControl(const std::string& symbol, float value)
{
	for (const PortInfo& port : ports_)
	{
		if (port.type != PortType::Control || port.symbol != symbol)
		{
			continue;
		}

		float clamped = value;
		if (clamped < port.min_value)
		{
			clamped = port.min_value;
		}
		if (clamped > port.max_value)
		{
			clamped = port.max_value;
		}

		control_storage_[port.index] = clamped;

		return true;
	}

	std::fprintf(stderr, "No control port named '%s' on this plugin.\n", symbol.c_str());

	return false;
}

float PluginStage::GetControl(const std::string& symbol) const
{
	for (const PortInfo& port : ports_)
	{
		if (port.type == PortType::Control && port.symbol == symbol)
		{
			return control_storage_[port.index];
		}
	}

	return std::numeric_limits<float>::quiet_NaN();
}

void PluginStage::PrepareAtomPorts()
{
	for (const PortInfo& port : ports_)
	{
		if (port.type == PortType::Atom)
		{
			InitAtomSequence(atom_storage_[port.index].data());
		}
	}
}

void PluginStage::Run(uint32_t sample_count)
{
	// Per the worker extension's contract: deliver any responses received
	// since the last run() before calling run() again. Done even when
	// bypassed: a model may still be loading, and dropping its response
	// would leave the plugin wrong when it's switched back in.
	worker_.DrainResponses();

	if (bypassed_.load(std::memory_order_acquire))
	{
		for (size_t out = 0; out < audio_outputs_.size(); ++out)
		{
			float* output = audio_buffers_[audio_outputs_[out]];

			if (!output)
			{
				continue;
			}

			if (audio_inputs_.empty())
			{
				std::memset(output, 0, sample_count * sizeof(float));
				continue;
			}

			// Extra outputs repeat the last input, so bypassing a mono->stereo
			// plugin feeds both sides rather than leaving one silent.
			const size_t in = out < audio_inputs_.size() ? out : audio_inputs_.size() - 1;
			const float* input = audio_buffers_[audio_inputs_[in]];

			if (input && input != output)
			{
				std::memcpy(output, input, sample_count * sizeof(float));
			}
		}

		return;
	}

	lilv_instance_run(instance_, sample_count);
}

std::string PluginStage::Uri() const
{
	return lilv_node_as_uri(lilv_plugin_get_uri(plugin_));
}

std::string PluginStage::Name() const
{
	// Looked up on demand rather than cached: this is for showing a person
	// what's in the chain, so it happens at UI speed, never on the audio
	// thread.
	LilvNode* name = lilv_plugin_get_name(plugin_);

	if (!name)
	{
		return Uri();
	}

	std::string result = lilv_node_as_string(name);
	lilv_node_free(name);

	// A plugin is free to declare an empty name; the URI at least identifies
	// it, which a blank row in the chain list would not.
	return result.empty() ? Uri() : result;
}

std::vector<PathProperty> PluginStage::PathProperties() const
{
	return FindPathProperties(world_, plugin_);
}

std::string PluginStage::GetPathProperty(const std::string& property_uri, UridMap& urid_map) const
{
	const auto* state_interface =
		static_cast<const LV2_State_Interface*>(lilv_instance_get_extension_data(instance_, LV2_STATE__interface));

	// Same guard as SaveState: only ask plugins that declare state:interface,
	// because some hand one back from extension_data() without declaring it
	// and calling save() on those produces a spurious error.
	LilvNode* state_interface_uri = lilv_new_uri(world_, LV2_STATE__interface);
	const bool declares_state = lilv_plugin_has_extension_data(plugin_, state_interface_uri);
	lilv_node_free(state_interface_uri);

	if (!declares_state || !state_interface || !state_interface->save)
	{
		return std::string();
	}

	std::string result;

	SinglePropertyStoreContext ctx;
	ctx.property = urid_map.Map(property_uri.c_str());
	ctx.atom_path_type = urid_map.Map(LV2_ATOM__Path);
	ctx.value = &result;

	LV2_State_Map_Path map_path;
	map_path.handle = nullptr;
	map_path.abstract_path = &MapPathIdentity;
	map_path.absolute_path = &MapPathIdentity;

	LV2_Feature map_path_feature = {LV2_STATE__mapPath, &map_path};

	const LV2_Feature* features[] = {&map_path_feature, nullptr};

	state_interface->save(lilv_instance_get_handle(instance_), &StoreSingleProperty, &ctx,
		LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features);

	return result;
}

std::string PluginStage::SaveState(UridMap& urid_map) const
{
	// The TTL is the contract: only ask for state from plugins that actually
	// declare state:interface. Some plugins (e.g. Carla's) hand back an
	// interface from extension_data() without declaring it, and that
	// interface's save() then fails -- calling it would produce a spurious
	// error and a state blob with nothing in it.
	LilvNode* state_interface_uri = lilv_new_uri(world_, LV2_STATE__interface);
	const bool declares_state = lilv_plugin_has_extension_data(plugin_, state_interface_uri);
	lilv_node_free(state_interface_uri);

	if (!declares_state)
	{
		return std::string();
	}

	// Returning nullptr for every port tells lilv "don't snapshot port
	// values" -- see the note in the header on why controls are kept in the
	// preset JSON instead.
	auto skip_port_values = [](const char*, void*, uint32_t*, uint32_t*) -> const void* { return nullptr; };

	// All directories null: no copying or linking of referenced files, so
	// state records the model/IR by its absolute path, which is what a
	// pedalboard wants (the user's models stay where they are).
	LilvState* state = lilv_state_new_from_instance(plugin_, instance_, urid_map.MapData(),
		/*scratch_dir=*/nullptr, /*copy_dir=*/nullptr, /*link_dir=*/nullptr, /*save_dir=*/nullptr, skip_port_values,
		/*user_data=*/nullptr, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, /*features=*/nullptr);

	if (!state)
	{
		return std::string();
	}

	// Nothing actually saved (e.g. NAM with no model loaded) -- storing the
	// empty preset shell would just be noise in the preset file.
	if (lilv_state_get_num_properties(state) == 0)
	{
		lilv_state_free(state);
		return std::string();
	}

	// `uri` is mandatory; base_uri must be null or the result may not be
	// restorable via lilv_state_new_from_string().
	char* serialized = lilv_state_to_string(world_, urid_map.MapData(), urid_map.UnmapData(), state,
		"urn:signalforge:stage-state", /*base_uri=*/nullptr);

	std::string result;

	if (serialized)
	{
		result = serialized;
		free(serialized);
	}

	lilv_state_free(state);

	return result;
}

bool PluginStage::RestoreState(const std::string& serialized_state, UridMap& urid_map)
{
	if (serialized_state.empty())
	{
		return true;
	}

	LilvState* state = lilv_state_new_from_string(world_, urid_map.MapData(), serialized_state.c_str());

	if (!state)
	{
		std::fprintf(stderr, "Failed to parse saved state for '%s'.\n", Uri().c_str());
		return false;
	}

	// lilv supplies state:mapPath / state:freePath itself during restore, so
	// no extra features are needed here.
	lilv_state_restore(state, instance_, &PluginStage::SetPortValueCallback, this, /*flags=*/0, /*features=*/nullptr);

	lilv_state_free(state);

	return true;
}

void PluginStage::SetPortValueCallback(
	const char* port_symbol, void* user_data, const void* value, uint32_t size, uint32_t type)
{
	auto* self = static_cast<PluginStage*>(user_data);

	// Only float control values are meaningful for a control port.
	if (size != sizeof(float) || type != self->float_urid_)
	{
		return;
	}

	float parsed = 0.0f;
	std::memcpy(&parsed, value, sizeof(parsed));

	self->SetControl(port_symbol, parsed);
}

std::map<std::string, float> PluginStage::CaptureControls() const
{
	std::map<std::string, float> controls;

	for (const PortInfo& port : ports_)
	{
		if (port.type == PortType::Control && port.direction == PortDirection::Input)
		{
			controls[port.symbol] = control_storage_[port.index];
		}
	}

	return controls;
}

void PluginStage::SettleWorker()
{
	// Delivering a response can schedule further work (e.g. NAM responds to
	// a model load by scheduling a free of the old model), so repeat until
	// things go quiet. Bounded so a plugin that ping-pongs work forever
	// can't hang us.
	constexpr int kMaxRounds = 16;

	for (int round = 0; round < kMaxRounds; ++round)
	{
		worker_.WaitForPendingWork();
		worker_.DrainResponses();

		if (!worker_.HasPendingWork())
		{
			return;
		}
	}

	std::fprintf(stderr, "Warning: worker still busy after %d settle rounds; continuing anyway.\n", kMaxRounds);
}

bool PluginStage::LoadPathProperty(const std::string& property_uri, const std::string& file_path, UridMap& urid_map)
{
	const auto* state_interface =
		static_cast<const LV2_State_Interface*>(lilv_instance_get_extension_data(instance_, LV2_STATE__interface));

	if (!state_interface || !state_interface->restore)
	{
		std::fprintf(stderr, "Plugin has no state:interface; cannot set '%s'.\n", property_uri.c_str());
		return false;
	}

	SinglePropertyRetrieveContext ctx;
	ctx.property = urid_map.Map(property_uri.c_str());
	ctx.atom_path_type = urid_map.Map(LV2_ATOM__Path);
	ctx.value = &file_path;

	LV2_State_Map_Path map_path;
	map_path.handle = nullptr;
	map_path.abstract_path = &MapPathIdentity;
	map_path.absolute_path = &MapPathIdentity;

	LV2_Feature map_path_feature = {LV2_STATE__mapPath, &map_path};

	const LV2_Feature* features[] = {&map_path_feature, nullptr};

	const LV2_State_Status status =
		state_interface->restore(lilv_instance_get_handle(instance_), &RetrieveSingleProperty, &ctx, 0, features);

	if (status != LV2_STATE_SUCCESS)
	{
		std::fprintf(stderr, "restore() for '%s' failed with status %d.\n", property_uri.c_str(), status);
		return false;
	}

	return true;
}

}  // namespace sf
