#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "../lv2/AudioHost.h"

namespace sf
{

// Drives an AudioHost with no audio backend at all: allocates its own
// buffers for the host's boundary ports, feeds a generated signal into the
// inputs, runs the host block by block, and reports what came out.
//
// Deliberately has zero JACK/ALSA involvement, so it runs on a machine with
// no audio server, no sound card, and no JACK session -- i.e. in CI.
class OfflineEngine
{
public:
	struct Result
	{
		uint64_t frames_processed = 0;

		// Samples that were NaN or +/-Inf. Any of these is a hard failure:
		// they propagate through a chain and can wedge later stages.
		uint64_t non_finite_samples = 0;

		float peak = 0.0f;
		double rms = 0.0;

		// FNV-1a over the raw bits of every output sample, in order. Lets
		// two runs be compared for bit-exactness rather than just "similar
		// level" -- which is what verifying a preset reproduces its saved
		// sound actually requires.
		uint64_t output_hash = 0;

		uint32_t input_channels = 0;
		uint32_t output_channels = 0;
	};

	explicit OfflineEngine(uint32_t block_size);

	// Processes `total_frames` frames in blocks of at most block_size. If
	// total_frames isn't a multiple of the block size, the final block is a
	// short one -- worth exercising, since plugins are allowed to be handed
	// fewer frames than the max block length they were configured with.
	//
	// `generator(frame_index, channel)` supplies the input sample for one
	// channel at that frame. Per channel rather than per frame since SF-76:
	// a board that takes two inputs and divides them between two lanes cannot
	// be checked at all if both of its channels carry the same signal.
	Result Run(AudioHost& host, uint64_t total_frames, const std::function<float(uint64_t, uint32_t)>& generator);

private:
	uint32_t block_size_;
};

// Stateless exponential sine sweep, useful as a generator for Run(): covers
// a wide frequency range so a misbehaving filter/convolution stage is more
// likely to produce something non-finite than a single steady tone would.
//
// **Channel 0 gets the sweep; every channel after it gets the same sweep 12 dB
// down.** The two sides have to be distinguishable or a stereo board proves
// nothing -- swapping which input feeds which lane would come out identical.
// A quieter copy rather than silence, so that a lane fed from the right is
// still a lane making sound: "it went quiet" and "it was routed wrong" should
// not look the same.
//
// Channel 0 is untouched by this, so every mono measurement ever taken with
// it still holds.
std::function<float(uint64_t, uint32_t)> MakeSineSweep(
	double sample_rate, double duration_seconds, double start_hz, double end_hz, float amplitude);

}  // namespace sf
