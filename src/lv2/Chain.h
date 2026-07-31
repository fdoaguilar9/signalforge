#pragma once

#include <memory>
#include <string>
#include <vector>

#include <lilv/lilv.h>

#include "AudioHost.h"
#include "PluginStage.h"
#include "UridMap.h"

namespace sf
{

// Hosts several LV2 plugins in a fixed sequential chain: stage N's audio
// output buffers are stage N+1's audio input buffers (shared memory, no
// copying). Exposes only the first stage's audio inputs and the last
// stage's audio outputs as its own AudioHost boundary.
//
// Every stage must have the same audio channel count as its neighbor at
// each connection point -- internal channel-count adaptation (e.g. mono
// into a stereo-only plugin mid-chain) is intentionally out of scope, not
// just deferred; keep chains mono end-to-end. The one adaptation this host
// does apply is mono->stereo duplication at the very final JACK output,
// handled by JackEngine, not here (see SF-9).
class Chain : public AudioHost
{
public:
	// Returns nullptr on failure (bad URI, a plugin failing to instantiate, or
	// a channel-count mismatch between adjacent stages). The reason is always
	// printed to stderr, and also written to `error` when one is supplied --
	// callers with a UI need it as text, not on a terminal nobody is reading.
	static std::unique_ptr<Chain> Create(LilvWorld* world, const std::vector<std::string>& plugin_uris,
		double sample_rate, uint32_t max_block_size, UridMap& urid_map, std::string* error = nullptr);

	// Whether `plugin_uri` names something lilv can find. Asking first is what
	// lets a caller that can degrade (a preset, which would rather play
	// without one pedal than not at all -- SF-31) tell "not installed" apart
	// from every other reason Create can fail, without parsing its message.
	// Create itself stays strict: a URI typed on the command line is meant.
	static bool IsPluginInstalled(LilvWorld* world, const std::string& plugin_uri);

	const std::vector<PortInfo>& Ports() const override { return boundary_ports_; }

	size_t StageCount() const { return stages_.size(); }

	// Access to an individual stage (e.g. to call LoadPathProperty/SetControl
	// on it directly before starting the audio engine). Not owned; valid for
	// the Chain's lifetime.
	PluginStage* Stage(size_t index) const { return stages_[index].get(); }

	// Settles every stage's worker (see PluginStage::SettleWorker). Call
	// after loading models/IRs and before processing, when the result needs
	// to be deterministic rather than "whenever the load lands".
	void Settle();

	void ConnectAudioPort(uint32_t index, float* buffer) override;
	void PrepareAtomPorts() override;
	void Run(uint32_t sample_count) override;

private:
	Chain() = default;

	struct BoundaryBinding
	{
		uint32_t stage_index;
		uint32_t port_index;  // port index within that stage
	};

	std::vector<std::unique_ptr<PluginStage>> stages_;

	// Keeps the stage-to-stage connecting buffers alive; stages_ point
	// directly into these via ConnectAudioPort.
	std::vector<std::vector<float>> link_buffers_;

	// Boundary port bookkeeping: boundary_ports_[i] is synthesized PortInfo
	// (renumbered so .index == i) describing the same port as
	// boundary_bindings_[i], which says which underlying stage/port it maps
	// to for ConnectAudioPort.
	std::vector<PortInfo> boundary_ports_;
	std::vector<BoundaryBinding> boundary_bindings_;
};

}  // namespace sf
