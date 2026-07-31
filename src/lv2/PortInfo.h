#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lilv/lilv.h>

namespace sf
{

enum class PortType
{
	Audio,
	Control,
	Atom,
	Cv,
	Unknown
};

enum class PortDirection
{
	Input,
	Output
};

struct PortInfo
{
	uint32_t index = 0;
	std::string symbol;
	std::string name;
	PortType type = PortType::Unknown;
	PortDirection direction = PortDirection::Input;

	// Only meaningful for PortType::Control.
	float default_value = 0.0f;
	float min_value = 0.0f;
	float max_value = 0.0f;

	// lv2:portProperty hints. Without these a control panel gets it wrong in
	// ways that matter: an on/off switch drawn as a 0..1 float slider, a
	// whole-number setting that can be dragged to 2.7, or a gain control whose
	// useful range is squeezed into the first millimetre of a linear slider.
	bool toggled = false;      // lv2:toggled
	bool integer = false;      // lv2:integer
	bool enumeration = false;  // lv2:enumeration
	bool logarithmic = false;  // pprops:logarithmic
};

const char* ToString(PortType type);
const char* ToString(PortDirection direction);

// Classifies every port of `plugin` (audio/control/atom/cv x input/output),
// pulling control port ranges where applicable. `world` must be the same
// LilvWorld that `plugin` was obtained from.
std::vector<PortInfo> ClassifyPorts(LilvWorld* world, const LilvPlugin* plugin);

}  // namespace sf
