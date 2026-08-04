#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ChainSpec.h"

namespace sf
{

// Compiling a chain's shape into a flat list of block operations, and the
// operations themselves (SF-48/49/50).
//
// The planner is pure: it takes a shape plus each stage's channel counts and
// latency, and returns the ops, the buffers they need, and where the board's
// own inputs and outputs land. It never touches lilv, so what a split *means*
// -- the signal order, where an adapter goes, which lane gets delayed -- is
// assertable in a unit test with no plugins installed and no audio server
// running, which is the only way this part was ever going to be checked.
//
// The result is flat on purpose. The audio thread walks a vector and
// switches on a small enum: no recursion, no allocation, no tree.

enum class OpKind
{
	Run,      // run stage `stage`; its ports are already connected to buses
	Copy,     // dst <- src0
	Downmix,  // dst <- 0.5 * (src0 + src1)
	Delay,    // dst <- src0, delayed by delay line `delay_line`
	Gain,     // dst <- gain0 * src0
	Filter,   // dst <- filter `filter`(src0)
	Mix       // dst <- gain0 * src0 + gain1 * src1
};

// One half of a crossover: which way it passes, and where it turns over.
struct CrossoverSpec
{
	float frequency = 800.0f;
	bool highpass = false;
};

// One block operation on one channel. Bus ids index the buffer table; which
// fields mean anything depends on `kind`.
struct ChainOp
{
	OpKind kind = OpKind::Run;

	uint32_t stage = 0;
	uint32_t dst = 0;
	uint32_t src0 = 0;
	uint32_t src1 = 0;
	uint32_t delay_line = 0;
	uint32_t filter = 0;

	float gain0 = 1.0f;
	float gain1 = 1.0f;

	// Mix only: which merge these gains belong to, so a host can change them
	// on a running chain instead of replanning it (SF-72). All the channel
	// ops of one split share a merge index; `gain0`/`gain1` are that merge's
	// values at plan time, which is what a host that never touches them gets.
	uint32_t merge = 0;
};

// What the planner needs to know about one plugin, in flat stage order.
struct StageIo
{
	unsigned inputs = 1;
	unsigned outputs = 1;

	// The plugin's own reported latency, in samples. Used to keep the two
	// lanes of a split time-aligned (SF-51).
	uint32_t latency = 0;
};

struct ChainPlan
{
	std::vector<ChainOp> ops;

	// How many single-channel buffers the ops index into.
	uint32_t bus_count = 0;

	// Which buses are the board's own in and out, in channel order.
	std::vector<uint32_t> input_buses;
	std::vector<uint32_t> output_buses;

	// Length in samples of each delay line the Delay ops name.
	std::vector<uint32_t> delay_lengths;

	// What each filter the Filter ops name is. The frequency travels rather
	// than the coefficients: designing a biquad needs the sample rate, and the
	// planner deliberately has no idea what that is -- it takes channel counts
	// and latencies as plain numbers so that a split's meaning is assertable
	// with nothing running.
	std::vector<CrossoverSpec> filters;

	// How many merges the plan has -- one per split, numbered in the order the
	// planner meets them (a split's own index before either of its branches',
	// so it matches a plain walk of the shape it was planned from).
	uint32_t merge_count = 0;

	// Each merge's two lane gains, linear, `2 * merge` and `2 * merge + 1`.
	// The same numbers the Mix ops carry; a host that wants them live keeps
	// this as its starting point rather than reading them back off the ops.
	std::vector<float> merge_gain;

	// Per flat stage index: which buses its audio input and output ports are
	// connected to. The host wires the ports once from this.
	std::vector<std::vector<uint32_t>> stage_input_buses;
	std::vector<std::vector<uint32_t>> stage_output_buses;

	// End-to-end latency of the planned chain, in samples.
	uint32_t latency = 0;
};

// Returns false with a reason when the shape can't be built: a channel count
// this host doesn't adapt (anything but 1 or 2), a split without exactly two
// branches, or a stage-io list that doesn't match the spec's plugin count.
bool PlanChain(
	const ChainSpec& spec, const std::vector<StageIo>& stage_io, unsigned board_inputs, ChainPlan& out, std::string& error);

// The block operations, as free functions so they can be measured directly.
// Each writes `frames` samples of one channel.

void OpCopy(float* dst, const float* src, uint32_t frames);

// Two channels into one. Halved, not summed: a mono signal duplicated to
// stereo and then folded back must come out at the level it went in, and two
// correlated copies of the same signal sum to twice the amplitude.
void OpDownmix(float* dst, const float* src0, const float* src1, uint32_t frames);

void OpGain(float* dst, const float* src, float gain, uint32_t frames);

void OpMix(float* dst, const float* src0, const float* src1, float gain0, float gain1, uint32_t frames);

// The same mix with each gain sweeping linearly across the block, which is what
// a merge gain that can change under the audio thread needs: stepping a
// summing gain at a block boundary is a click, and a fader that clicks while
// it moves is not usable as a blend.
//
// With `from == to` on both lanes this is OpMix exactly -- same arithmetic,
// same result, bit for bit -- so a chain nobody is touching pays a multiply
// and an add per sample and sounds no different.
void OpMixRamp(float* dst, const float* src0, const float* src1, float gain0_from, float gain0_to, float gain1_from,
	float gain1_to, uint32_t frames);

// dB to a linear gain. -INFINITY is not special-cased; a muted lane is
// planned as a gain of exactly zero rather than as a very small number.
float GainFromDb(float db);

// One half of a 4th-order Linkwitz-Riley crossover: two identical 2nd-order
// Butterworth sections in series (SF-75).
//
// **4th-order LR specifically, and not a Butterworth pair.** Two Butterworth
// halves summed give +3 dB at the crossover point; two LR4 halves sum to unity
// magnitude across the whole band, which is what makes a crossover you can put
// a split behind without it colouring the sound. A crossover that changes the
// tone at 800 Hz is a crossover that will get blamed on the amps.
//
// One instance filters one channel: the state is the filter's, so two channels
// through one of these would smear each into the other.
class CrossoverFilter
{
public:
	// Designs the sections and clears the state. Control thread only.
	//
	// A frequency at or above Nyquist, or at or below zero, is clamped into
	// range rather than refused: this is fed by a number a player typed, and
	// a silent board is a worse answer than a crossover at the edge of the
	// band.
	void Reset(double sample_rate, const CrossoverSpec& spec);

	// dst may alias src.
	void Process(float* dst, const float* src, uint32_t frames);

private:
	// Transposed direct form II: two state variables per section, and the
	// arithmetic that is least unkind to a float at low frequencies.
	struct Section
	{
		float b0 = 1.0f;
		float b1 = 0.0f;
		float b2 = 0.0f;
		float a1 = 0.0f;
		float a2 = 0.0f;

		float z1 = 0.0f;
		float z2 = 0.0f;
	};

	Section sections_[2];
};

// A fixed whole-sample delay, used only to time-align the two lanes of a
// split. Not an effect: no feedback, no interpolation, no smoothing on a
// length change (there is none -- the length is fixed when the chain is
// built).
class DelayLine
{
public:
	// Allocates for `length` samples of delay. Control thread only.
	void Reset(uint32_t length);

	uint32_t Length() const { return length_; }

	// dst may not alias src.
	void Process(float* dst, const float* src, uint32_t frames);

private:
	std::vector<float> buffer_;
	uint32_t length_ = 0;
	uint32_t write_ = 0;
};

}  // namespace sf
