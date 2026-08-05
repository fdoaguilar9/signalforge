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

	// The plugin's own latency readout: a control *output* port designated
	// lv2:latency (or carrying the older lv2:reportsLatency property), whose
	// value is how many samples of delay the plugin adds. There is at most
	// one. Needed to keep the two lanes of a split time-aligned (SF-51).
	bool reports_latency = false;

	// lv2:isSideChain -- an audio input the plugin *listens* to rather than
	// processes: a compressor's key input, a vocoder's modulator.
	//
	// **A sidechain is not a channel.** Counting one would make a mono
	// compressor look like a stereo pedal, and on a stereo board the planner
	// would then hand it the right channel as its key signal: the board would
	// duck to whatever happens to be in one speaker. Worse, a stereo pedal
	// with a key input reports three audio inputs and is refused outright as
	// something this host cannot adapt. Both are the same mistake -- reading a
	// port count as a channel count.
	bool sidechain = false;
};

const char* ToString(PortType type);
const char* ToString(PortDirection direction);

// Classifies every port of `plugin` (audio/control/atom/cv x input/output),
// pulling control port ranges where applicable. `world` must be the same
// LilvWorld that `plugin` was obtained from.
std::vector<PortInfo> ClassifyPorts(LilvWorld* world, const LilvPlugin* plugin);

}  // namespace sf
