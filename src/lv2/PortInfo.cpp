#include "PortInfo.h"

#include <lv2/core/lv2.h>

namespace sf
{

const char* ToString(PortType type)
{
	switch (type)
	{
		case PortType::Audio: return "Audio";
		case PortType::Control: return "Control";
		case PortType::Atom: return "Atom";
		case PortType::Cv: return "CV";
		case PortType::Unknown: return "Unknown";
	}

	return "Unknown";
}

const char* ToString(PortDirection direction)
{
	return direction == PortDirection::Input ? "Input" : "Output";
}

namespace
{

// Small RAII wrapper so we don't leak LilvNode class lookups on early return.
class ClassNodes
{
public:
	explicit ClassNodes(LilvWorld* world)
		: audio_(lilv_new_uri(world, LILV_URI_AUDIO_PORT))
		, control_(lilv_new_uri(world, LILV_URI_CONTROL_PORT))
		, atom_(lilv_new_uri(world, LILV_URI_ATOM_PORT))
		, cv_(lilv_new_uri(world, LILV_URI_CV_PORT))
		, input_(lilv_new_uri(world, LILV_URI_INPUT_PORT))
		, output_(lilv_new_uri(world, LILV_URI_OUTPUT_PORT))
		, toggled_(lilv_new_uri(world, LV2_CORE__toggled))
		, integer_(lilv_new_uri(world, LV2_CORE__integer))
		, enumeration_(lilv_new_uri(world, LV2_CORE__enumeration))
		, logarithmic_(lilv_new_uri(world, "http://lv2plug.in/ns/ext/port-props#logarithmic"))
	{
	}

	~ClassNodes()
	{
		lilv_node_free(audio_);
		lilv_node_free(control_);
		lilv_node_free(atom_);
		lilv_node_free(cv_);
		lilv_node_free(input_);
		lilv_node_free(output_);
		lilv_node_free(toggled_);
		lilv_node_free(integer_);
		lilv_node_free(enumeration_);
		lilv_node_free(logarithmic_);
	}

	ClassNodes(const ClassNodes&) = delete;
	ClassNodes& operator=(const ClassNodes&) = delete;

	LilvNode* audio_;
	LilvNode* control_;
	LilvNode* atom_;
	LilvNode* cv_;
	LilvNode* input_;
	LilvNode* output_;

	// Port properties rather than port classes, but they are looked up the
	// same way and want the same cleanup.
	LilvNode* toggled_;
	LilvNode* integer_;
	LilvNode* enumeration_;
	LilvNode* logarithmic_;
};

}  // namespace

std::vector<PortInfo> ClassifyPorts(LilvWorld* world, const LilvPlugin* plugin)
{
	std::vector<PortInfo> result;

	ClassNodes classes(world);

	const uint32_t port_count = lilv_plugin_get_num_ports(plugin);

	// lilv_plugin_get_port_ranges_float wants arrays sized to port_count,
	// one call classifies every control port's default/min/max at once.
	std::vector<float> defaults(port_count, 0.0f);
	std::vector<float> minimums(port_count, 0.0f);
	std::vector<float> maximums(port_count, 0.0f);

	lilv_plugin_get_port_ranges_float(plugin, minimums.data(), maximums.data(), defaults.data());

	for (uint32_t i = 0; i < port_count; ++i)
	{
		const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);

		PortInfo info;
		info.index = i;

		const LilvNode* symbol = lilv_port_get_symbol(plugin, port);
		info.symbol = symbol ? lilv_node_as_string(symbol) : "";
		// lilv_port_get_symbol returns a node owned by the port; do not free it.

		LilvNode* name = lilv_port_get_name(plugin, port);
		info.name = name ? lilv_node_as_string(name) : "";
		lilv_node_free(name);

		if (lilv_port_is_a(plugin, port, classes.audio_))
		{
			info.type = PortType::Audio;
		}
		else if (lilv_port_is_a(plugin, port, classes.control_))
		{
			info.type = PortType::Control;
			info.default_value = defaults[i];
			info.min_value = minimums[i];
			info.max_value = maximums[i];

			info.toggled = lilv_port_has_property(plugin, port, classes.toggled_);
			info.integer = lilv_port_has_property(plugin, port, classes.integer_);
			info.enumeration = lilv_port_has_property(plugin, port, classes.enumeration_);
			info.logarithmic = lilv_port_has_property(plugin, port, classes.logarithmic_);
		}
		else if (lilv_port_is_a(plugin, port, classes.atom_))
		{
			info.type = PortType::Atom;
		}
		else if (lilv_port_is_a(plugin, port, classes.cv_))
		{
			info.type = PortType::Cv;
		}
		else
		{
			info.type = PortType::Unknown;
		}

		info.direction = lilv_port_is_a(plugin, port, classes.input_) ? PortDirection::Input : PortDirection::Output;

		result.push_back(info);
	}

	return result;
}

}  // namespace sf
