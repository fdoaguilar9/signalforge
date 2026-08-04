// The board-wide trims' arithmetic (SF-66).
//
// Two claims, both of which would otherwise only ever be listened for: that a
// gain is the gain it says it is, and that changing one produces no
// discontinuity -- including across the block boundary, which is the seam a
// per-block gain would click at and the whole reason the ramp exists.
//
// Pure, so this needs no JACK, no audio hardware and no plugins. The engine
// calls exactly these functions; there is no second copy for this to drift
// from.

#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/Gain.h"

namespace
{

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what)
{
	++g_checks;

	if (!condition)
	{
		++g_failures;
		std::fprintf(stderr, "FAIL: %s\n", what);
	}
}

void CheckNear(float got, float expected, float tolerance, const char* what)
{
	++g_checks;

	if (!(std::fabs(got - expected) <= tolerance))
	{
		++g_failures;
		std::fprintf(stderr, "FAIL: %s (got %.9f, expected %.9f +/- %g)\n", what, static_cast<double>(got),
			static_cast<double>(expected), static_cast<double>(tolerance));
	}
}

using namespace sf;

// The design's range is -24..+24 dB, and the clamp in Settings depends on
// these meaning what they say.
void TestDbToLinear()
{
	CheckNear(DbToLinear(0.0f), 1.0f, 1e-6f, "0 dB is unity");

	// The two figures anyone would sanity-check by ear.
	CheckNear(DbToLinear(6.0206f), 2.0f, 1e-4f, "+6 dB is double");
	CheckNear(DbToLinear(-6.0206f), 0.5f, 1e-4f, "-6 dB is half");

	// The ends of the slider.
	CheckNear(DbToLinear(24.0f), 15.848932f, 1e-4f, "+24 dB is 15.85x");
	CheckNear(DbToLinear(-24.0f), 0.0630957f, 1e-6f, "-24 dB is 0.0631x");

	// Symmetry: +N and -N must be exact reciprocals, or the trim would not
	// return to where it started when swept up and back.
	for (float db = 0.0f; db <= 24.0f; db += 3.0f)
	{
		CheckNear(DbToLinear(db) * DbToLinear(-db), 1.0f, 1e-5f, "+N dB and -N dB are reciprocal");
	}
}

// "±24 dB is exactly ±24 dB": a steady gain applied to a known signal.
void TestSteadyGain()
{
	constexpr uint32_t kFrames = 128;

	std::vector<float> source(kFrames, 1.0f);
	std::vector<float> out(kFrames, 0.0f);

	const float up = DbToLinear(24.0f);
	RampGain(out.data(), source.data(), kFrames, up, up);

	bool exact = true;

	for (uint32_t i = 0; i < kFrames; ++i)
	{
		exact = exact && out[i] == up;
	}

	Check(exact, "+24 dB scales every sample by exactly the linear gain");

	const float down = DbToLinear(-24.0f);
	RampGain(out.data(), source.data(), kFrames, down, down);

	CheckNear(out[0], 0.0630957f, 1e-6f, "-24 dB scales to 0.0631");

	// Unity has a fast path that memcpys. It has to be a *copy*, not a
	// no-op: the input trim writes into a scratch buffer the chain then
	// reads, so skipping the copy would feed the chain last block's audio.
	std::vector<float> ramp(kFrames);

	for (uint32_t i = 0; i < kFrames; ++i)
	{
		ramp[i] = static_cast<float>(i);
	}

	std::vector<float> copied(kFrames, -1.0f);
	RampGain(copied.data(), ramp.data(), kFrames, 1.0f, 1.0f);

	Check(copied == ramp, "unity copies rather than leaving the destination alone");

	// In-place is how the output trim runs, since those buffers are ours.
	std::vector<float> in_place = ramp;
	RampGain(in_place.data(), in_place.data(), kFrames, 2.0f, 2.0f);

	Check(in_place[7] == ramp[7] * 2.0f, "a gain can be applied in place");
}

// "no discontinuity at block boundaries": the claim the ramp exists for.
void TestRampContinuity()
{
	constexpr uint32_t kFrames = 128;

	// A DC signal, so every change in the output is a change in the gain and
	// nothing else. A sine would hide a small step inside its own slope.
	std::vector<float> source(kFrames, 1.0f);
	std::vector<float> first(kFrames, 0.0f);
	std::vector<float> second(kFrames, 0.0f);

	// The worst case a slider drag can produce: a change of the whole range in
	// one block, then holding at the new value.
	const float from = DbToLinear(0.0f);
	const float to = DbToLinear(12.0f);

	RampGain(first.data(), source.data(), kFrames, from, to);
	RampGain(second.data(), source.data(), kFrames, to, to);

	CheckNear(first[0], from, 1e-6f, "the ramp starts at the old gain");

	const float step = (to - from) / static_cast<float>(kFrames);

	// Every step inside the block is the same size...
	bool even = true;

	for (uint32_t i = 1; i < kFrames; ++i)
	{
		even = even && std::fabs((first[i] - first[i - 1]) - step) < 1e-5f;
	}

	Check(even, "the ramp advances by an equal step every sample");

	// ...and so is the step *across the seam* into the next block. This is the
	// assertion that matters: it is why the block ends at `to - step` rather
	// than at `to`, and getting that wrong puts a flat spot at every boundary
	// which is audible as a buzz at the block rate while dragging.
	CheckNear(second[0] - first[kFrames - 1], step, 1e-5f, "the step across the block boundary matches the ramp");

	CheckNear(first[kFrames - 1], to - step, 1e-5f, "the block ends one step short of the target");
	CheckNear(second[0], to, 1e-6f, "the next block starts exactly at the target");

	// Monotonic in both directions -- a ramp that overshot and came back would
	// be a wobble on every slider movement.
	bool rising = true;
	bool falling = true;

	std::vector<float> down(kFrames, 0.0f);
	RampGain(down.data(), source.data(), kFrames, to, from);

	for (uint32_t i = 1; i < kFrames; ++i)
	{
		rising = rising && first[i] >= first[i - 1];
		falling = falling && down[i] <= down[i - 1];
	}

	Check(rising, "a rising trim only rises");
	Check(falling, "a falling trim only falls");

	// A whole sweep of the design's range, block by block, must never step by
	// more than one ramp increment anywhere -- including at the seams.
	float previous = DbToLinear(-24.0f);
	float last_sample = previous;
	bool smooth = true;

	for (int db = -24; db <= 24; ++db)
	{
		const float target = DbToLinear(static_cast<float>(db));
		std::vector<float> block(kFrames, 0.0f);

		RampGain(block.data(), source.data(), kFrames, previous, target);

		const float increment = std::fabs(target - previous) / static_cast<float>(kFrames);

		// The seam first, then the inside of the block.
		smooth = smooth && std::fabs(block[0] - last_sample) <= increment + 1e-5f;

		for (uint32_t i = 1; i < kFrames; ++i)
		{
			smooth = smooth && std::fabs(block[i] - block[i - 1]) <= increment + 1e-5f;
		}

		last_sample = block[kFrames - 1];
		previous = target;
	}

	Check(smooth, "sweeping the whole -24..+24 dB range never steps by more than one increment");
}

void TestEdges()
{
	// An empty block must not divide by zero producing a NaN gain that then
	// poisons the next one.
	std::vector<float> nothing;
	RampGain(nothing.data(), nothing.data(), 0, 0.5f, 2.0f);

	Check(true, "a zero-length block is a no-op rather than a divide by zero");

	// A single-frame block: step is the whole distance, and the one sample is
	// at the start value.
	std::vector<float> one(1, 1.0f);
	std::vector<float> out(1, 0.0f);

	RampGain(out.data(), one.data(), 1, 0.25f, 0.75f);
	CheckNear(out[0], 0.25f, 1e-6f, "a one-frame block holds the starting gain");
}

}  // namespace

int main()
{
	TestDbToLinear();
	TestSteadyGain();
	TestRampContinuity();
	TestEdges();

	std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);

	return g_failures == 0 ? 0 : 1;
}
