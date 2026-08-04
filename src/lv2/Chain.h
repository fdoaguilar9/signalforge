#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <lilv/lilv.h>

#include "AudioHost.h"
#include "ChainSchedule.h"
#include "ChainSpec.h"
#include "PluginStage.h"
#include "UridMap.h"

namespace sf
{

// Hosts several LV2 plugins in the shape a ChainSpec describes: a series of
// stages, where any point in the series may split into two parallel lanes
// that sum back together.
//
// The shape is compiled once, on the control thread, into a flat list of
// block operations over a table of single-channel buffers (see
// ChainSchedule). Run() walks that list. Nothing about the tree survives into
// the audio thread: no recursion, no allocation, no branching beyond a switch
// on a small enum.
//
// Channel counts no longer have to match between neighbours. Mono into a
// stereo pedal duplicates; stereo into a mono one folds down at -6 dB per
// side, so a signal that goes mono -> stereo -> mono comes back out at the
// level it went in. Anything wider than stereo is refused rather than
// guessed at.

// "Take as many inputs as the first pedal in the chain wants."
inline constexpr unsigned kAutoBoardInputs = 0;

class Chain : public AudioHost
{
public:
	// The plain series this host ran before Milestone 9. Returns nullptr on
	// failure (bad URI, a plugin failing to instantiate). The reason is always
	// printed to stderr, and also written to `error` when one is supplied --
	// callers with a UI need it as text, not on a terminal nobody is reading.
	static std::unique_ptr<Chain> Create(LilvWorld* world, const std::vector<std::string>& plugin_uris,
		double sample_rate, uint32_t max_block_size, UridMap& urid_map, std::string* error = nullptr,
		unsigned board_inputs = kAutoBoardInputs);

	// The general form: any shape a ChainSpec can describe.
	//
	// `board_inputs` is how many inputs the board itself takes. kAutoBoardInputs
	// means "as many as the first pedal wants", which is what this did before
	// it was a choice; 1 or 2 says so outright (SF-52).
	static std::unique_ptr<Chain> Create(LilvWorld* world, const ChainSpec& spec, double sample_rate,
		uint32_t max_block_size, UridMap& urid_map, std::string* error = nullptr,
		unsigned board_inputs = kAutoBoardInputs);

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
	//
	// Stages are indexed in signal order -- the trunk in order, and at a split
	// all of lane 0 then all of lane 1 before the trunk resumes. The same
	// order ChainGraph::Flatten reports, so a stage index means the same thing
	// to the engine, the UI, and a MIDI binding.
	PluginStage* Stage(size_t index) const { return stages_[index].get(); }

	// End-to-end latency of the chain in samples: the plugins' own reported
	// latency, plus whatever alignment delay the lanes of a split needed.
	uint32_t LatencySamples() const { return plan_.latency; }

	// How many merges this chain has -- one per split, numbered in the order a
	// pre-order walk of the shape meets them. Zero on a plain series.
	uint32_t MergeCount() const { return plan_.merge_count; }

	// Changes one lane's contribution to one merge on the *running* chain: no
	// replan, no rebuild, no crossfade (SF-72). `lane` is 0 or 1; `gain` is
	// linear, and 0 is how a muted lane is spelled.
	//
	// Control thread. The audio thread picks the new value up on its next
	// block and sweeps to it across that block, so this is safe to call as
	// fast as a mouse moves -- which is the point of it. Out-of-range
	// arguments are ignored rather than refused; the caller is a fader.
	void SetMergeGain(uint32_t merge, unsigned lane, float gain);

	// What SetMergeGain last set, or the planned value if it never was.
	float MergeGain(uint32_t merge, unsigned lane) const;

	// Settles every stage's worker (see PluginStage::SettleWorker). Call
	// after loading models/IRs and before processing, when the result needs
	// to be deterministic rather than "whenever the load lands".
	void Settle();

	void ConnectAudioPort(uint32_t index, float* buffer) override;
	void PrepareAtomPorts() override;
	void Run(uint32_t sample_count) override;

private:
	Chain() = default;

	// One plugin port that has to follow a bus wherever it points. Only the
	// buses at the board's edge ever move -- JACK hands out different buffers
	// every cycle -- but the bookkeeping is the same for all of them.
	struct PortBinding
	{
		uint32_t stage;
		uint32_t port;
	};

	std::vector<std::unique_ptr<PluginStage>> stages_;

	ChainPlan plan_;

	// The buffer table the ops index into. `bus_storage_` owns a block for
	// every bus; `bus_ptr_` is what the ops actually read, so that a bus at
	// the board's edge can be repointed at a JACK buffer for the cycle
	// without copying.
	std::vector<std::vector<float>> bus_storage_;
	std::vector<float*> bus_ptr_;

	std::vector<std::vector<PortBinding>> bus_bindings_;

	std::vector<DelayLine> delay_lines_;

	// One per Filter op: a crossover half, designed from the plan's frequency
	// and this chain's sample rate. One per *channel* as well as per half --
	// the state is the filter's, and two channels through one would smear each
	// into the other.
	std::vector<CrossoverFilter> filters_;

	// Merge gains, `2 * merge + lane`, in three parts: what the control thread
	// has asked for, what the audio thread is currently at, and where it was
	// when the block started. Every Mix op of one merge sweeps between the
	// same two numbers, so the channels of a stereo merge stay in step -- one
	// shared "current" advanced per op would give the second channel a
	// different ramp from the first.
	std::unique_ptr<std::atomic<float>[]> merge_target_;
	std::vector<float> merge_current_;
	std::vector<float> merge_from_;

	// Which bus each of our own boundary ports is.
	std::vector<uint32_t> boundary_bus_;
	std::vector<PortInfo> boundary_ports_;
};

}  // namespace sf
