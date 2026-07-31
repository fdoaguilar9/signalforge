#pragma once

#include <cstdint>
#include <vector>

#include "PortInfo.h"

namespace sf
{

// Common interface an engine drives once per audio block, implemented by
// both a single PluginStage and a multi-stage Chain -- the engine doesn't
// need to know which it's talking to. Implementations are driven by
// JackEngine (live audio) and OfflineEngine (headless testing) alike.
class AudioHost
{
public:
	virtual ~AudioHost() = default;

	// Ports at this host's boundary (a Chain's inner, stage-to-stage
	// connections aren't exposed here) -- callers interested in wiring audio
	// I/O should filter for PortType::Audio; other port types may also be
	// present (e.g. PluginStage reports all of its ports here).
	virtual const std::vector<PortInfo>& Ports() const = 0;

	// `index` is a boundary audio port's index as reported by Ports().
	virtual void ConnectAudioPort(uint32_t index, float* buffer) = 0;

	virtual void PrepareAtomPorts() = 0;
	virtual void Run(uint32_t sample_count) = 0;
};

}  // namespace sf
