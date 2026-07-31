// Crossfade tests (SF-33): the gain ramp a preset switch runs down. Pure
// arithmetic -- no JACK, no audio hardware, no plugins -- which is the point:
// the shape of a switch is measurable, and only the choice between the shapes
// is a matter of taste.

#include "../src/audio/Crossfade.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool condition, const char* description)
{
	if (condition)
	{
		std::printf("ok    %s\n", description);
		return;
	}

	std::printf("FAIL  %s\n", description);
	++g_failures;
}

void CheckNear(float value, float expected, float tolerance, const char* description)
{
	if (std::fabs(value - expected) <= tolerance)
	{
		std::printf("ok    %-58s %.6f\n", description, value);
		return;
	}

	std::printf("FAIL  %-58s %.6f, expected %.6f +/- %.6f\n", description, value, expected, tolerance);
	++g_failures;
}

// Runs a fade to completion in blocks, mixing a constant 1.0 against a
// constant 0.0 so the output *is* the outgoing gain, sample by sample. That is
// what makes the ramp itself inspectable rather than only its endpoints.
std::vector<float> RampOf(sf::Crossfade& fade, uint32_t block, uint32_t max_blocks = 4096)
{
	std::vector<float> ramp;

	const std::vector<float> ones(block, 1.0f);
	const std::vector<float> zeros(block, 0.0f);
	std::vector<float> out(block, 0.0f);

	for (uint32_t i = 0; i < max_blocks && !fade.Done(); ++i)
	{
		fade.MixChannel(out.data(), ones.data(), zeros.data(), block);
		fade.Advance(block);

		ramp.insert(ramp.end(), out.begin(), out.end());
	}

	return ramp;
}

void TestCurves()
{
	std::printf("\n-- the two curves --\n");

	float from = 0.0f;
	float to = 0.0f;

	sf::Crossfade::Gains(0.0f, sf::CrossfadeCurve::EqualPower, from, to);
	CheckNear(from, 1.0f, 1e-6f, "equal-power starts fully on the outgoing chain");
	CheckNear(to, 0.0f, 1e-6f, "equal-power starts with no incoming chain");

	sf::Crossfade::Gains(1.0f, sf::CrossfadeCurve::EqualPower, from, to);
	CheckNear(from, 0.0f, 1e-6f, "equal-power ends with no outgoing chain");
	CheckNear(to, 1.0f, 1e-6f, "equal-power ends fully on the incoming chain");

	// The claim equal-power makes, and the only one: constant power across the
	// whole fade. Two different sounds are closer to uncorrelated than to
	// coherent, and uncorrelated signals add in power.
	float worst_power = 0.0f;
	float worst_sum = 0.0f;

	for (int i = 0; i <= 1000; ++i)
	{
		const float position = static_cast<float>(i) / 1000.0f;
		sf::Crossfade::Gains(position, sf::CrossfadeCurve::EqualPower, from, to);

		worst_power = std::max(worst_power, std::fabs(from * from + to * to - 1.0f));
		worst_sum = std::max(worst_sum, std::fabs(from + to - 1.0f));
	}

	CheckNear(worst_power, 0.0f, 1e-5f, "equal-power holds from^2 + to^2 at 1 throughout");

	// And the price it pays, stated rather than hidden: two *identical* signals
	// crossfaded this way bulge by 3dB in the middle. That is why linear is
	// offered at all.
	CheckNear(worst_sum, 0.41421f, 1e-3f, "equal-power sums to 1.414 at the midpoint (the 3dB bulge)");

	float linear_worst_power = 0.0f;
	float linear_worst_sum = 0.0f;

	for (int i = 0; i <= 1000; ++i)
	{
		const float position = static_cast<float>(i) / 1000.0f;
		sf::Crossfade::Gains(position, sf::CrossfadeCurve::Linear, from, to);

		linear_worst_power = std::max(linear_worst_power, std::fabs(from * from + to * to - 1.0f));
		linear_worst_sum = std::max(linear_worst_sum, std::fabs(from + to - 1.0f));
	}

	CheckNear(linear_worst_sum, 0.0f, 1e-5f, "linear holds from + to at 1 throughout");
	CheckNear(linear_worst_power, 0.5f, 1e-3f, "linear loses half the power at the midpoint (the hole)");

	// Positions outside 0..1 are clamped rather than extrapolated: a gain of
	// 1.2 on the way in would be a switch that is briefly louder than either
	// chain, and a negative one would invert it.
	sf::Crossfade::Gains(-0.5f, sf::CrossfadeCurve::EqualPower, from, to);
	CheckNear(from, 1.0f, 1e-6f, "a position before the start clamps to the outgoing chain");

	sf::Crossfade::Gains(2.0f, sf::CrossfadeCurve::Linear, from, to);
	CheckNear(to, 1.0f, 1e-6f, "a position past the end clamps to the incoming chain");
}

void TestLength()
{
	std::printf("\n-- length and block boundaries --\n");

	sf::Crossfade fade;
	fade.Configure(48000.0, 12.0, sf::CrossfadeCurve::EqualPower);

	Check(fade.LengthInSamples() == 576, "12ms at 48kHz is 576 samples");
	Check(!fade.Done(), "a fresh fade has not finished");

	// The ramp has to continue *across* blocks, not restart in each one. A fade
	// that reset per block would hold the outgoing chain at full gain forever
	// and never hand over -- and with a big enough block, no test that only
	// looked at the first and last sample would notice.
	const std::vector<float> ramp = RampOf(fade, 64);

	Check(fade.Done(), "the fade completes");
	Check(ramp.size() >= 576 && ramp.size() <= 576 + 64, "it takes the expected number of samples, to the block");

	CheckNear(ramp.front(), 1.0f, 1e-6f, "the ramp starts at full outgoing gain");
	CheckNear(ramp[576 - 1], 0.0f, 1e-2f, "and has handed over by the end of the fade");

	bool monotonic = true;
	float worst_step = 0.0f;

	for (size_t i = 1; i < ramp.size(); ++i)
	{
		if (ramp[i] > ramp[i - 1] + 1e-6f)
		{
			monotonic = false;
		}

		worst_step = std::max(worst_step, std::fabs(ramp[i] - ramp[i - 1]));
	}

	Check(monotonic, "the outgoing gain only ever falls -- no restart at a block boundary");

	// The click test. At 12ms/48kHz the biggest gain change between adjacent
	// samples is about 0.0027; anything approaching a step of 0.05 would be an
	// edge you can hear.
	CheckNear(worst_step, 0.0027f, 0.002f, "no audible step between adjacent samples");

	// The same fade, mixed one block at a time in a different block size, has
	// to be the same ramp: JACK's period size is not ours to choose.
	sf::Crossfade other;
	other.Configure(48000.0, 12.0, sf::CrossfadeCurve::EqualPower);

	const std::vector<float> big_blocks = RampOf(other, 512);

	bool same = true;

	for (size_t i = 0; i < 576; ++i)
	{
		if (std::fabs(big_blocks[i] - ramp[i]) > 1e-5f)
		{
			same = false;
		}
	}

	Check(same, "block size does not change the ramp");
}

void TestConfiguration()
{
	std::printf("\n-- configuration --\n");

	// Zero means "swap at the next block boundary", which is what --no-crossfade
	// does by another route. It must not divide by zero, or hang.
	sf::Crossfade instant;
	instant.Configure(48000.0, 0.0, sf::CrossfadeCurve::EqualPower);

	Check(instant.LengthInSamples() == 0, "a zero-length fade spans no samples");

	const std::vector<float> ramp = RampOf(instant, 64);
	Check(instant.Done(), "a zero-length fade completes in one block");

	// Nonsense in, sane out: a settings file is hand-editable, and a negative
	// or NaN length must not turn every switch into silence.
	CheckNear(static_cast<float>(sf::Crossfade::ClampMilliseconds(-5.0)), 0.0f, 1e-6f, "a negative length clamps to 0");
	CheckNear(static_cast<float>(sf::Crossfade::ClampMilliseconds(std::nan(""))), 0.0f, 1e-6f, "NaN clamps to 0");
	CheckNear(static_cast<float>(sf::Crossfade::ClampMilliseconds(10000.0)),
		static_cast<float>(sf::Crossfade::kMaxMilliseconds), 1e-6f, "an absurd length clamps to the maximum");

	sf::Crossfade nan_fade;
	nan_fade.Configure(48000.0, std::nan(""), sf::CrossfadeCurve::EqualPower);

	const std::vector<float> nan_ramp = RampOf(nan_fade, 64);
	Check(nan_fade.Done() && !nan_ramp.empty() && !std::isnan(nan_ramp[0]), "a NaN length still produces real audio");

	// Restart is what a second stomp does: the same fade, from the beginning.
	sf::Crossfade reused;
	reused.Configure(48000.0, 12.0, sf::CrossfadeCurve::EqualPower);
	RampOf(reused, 64);
	Check(reused.Done(), "the first switch finished");

	reused.Restart();
	Check(!reused.Done() && reused.Position() == 0.0f, "restarting puts the fade back at the beginning");

	sf::CrossfadeCurve curve = sf::CrossfadeCurve::Linear;

	Check(sf::ParseCrossfadeCurve("equal-power", curve) && curve == sf::CrossfadeCurve::EqualPower,
		"'equal-power' parses");
	Check(sf::ParseCrossfadeCurve("equal_power", curve) && curve == sf::CrossfadeCurve::EqualPower,
		"'equal_power' parses too -- JSON keys and command lines punctuate differently");
	Check(sf::ParseCrossfadeCurve("linear", curve) && curve == sf::CrossfadeCurve::Linear, "'linear' parses");
	Check(!sf::ParseCrossfadeCurve("smooth", curve), "an unknown curve name is rejected");
	Check(std::string(sf::ToString(sf::CrossfadeCurve::EqualPower)) == "equal-power", "round-trips to its own name");
}

void TestMixing()
{
	std::printf("\n-- mixing --\n");

	sf::Crossfade fade;
	fade.Configure(48000.0, 12.0, sf::CrossfadeCurve::EqualPower);

	// Two channels of one block: both must walk the same ramp from the same
	// place, or a stereo switch smears in time between left and right.
	const std::vector<float> ones(64, 1.0f);
	const std::vector<float> zeros(64, 0.0f);

	std::vector<float> left(64, 0.0f);
	std::vector<float> right(64, 0.0f);

	fade.MixChannel(left.data(), ones.data(), zeros.data(), 64);
	fade.MixChannel(right.data(), ones.data(), zeros.data(), 64);

	bool identical = true;

	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i] != right[i])
		{
			identical = false;
		}
	}

	Check(identical, "every channel of a block walks the same ramp");

	// Mixing must not advance the fade; only Advance does. Otherwise a stereo
	// chain would fade twice as fast as a mono one.
	Check(fade.Position() == 0.0f, "mixing a channel does not move the fade on");

	fade.Advance(64);
	Check(fade.Position() > 0.0f, "advancing does");

	// The incoming side is a real mix, not a switch: halfway through, both
	// signals are present.
	sf::Crossfade half;
	half.Configure(48000.0, 12.0, sf::CrossfadeCurve::EqualPower);
	half.Advance(288);  // half of 576

	std::vector<float> out(64, 0.0f);
	const std::vector<float> twos(64, 2.0f);

	half.MixChannel(out.data(), ones.data(), twos.data(), 64);

	Check(out[0] > 1.0f && out[0] < 2.2f, "halfway through, both chains are audible");
}

}  // namespace

int main()
{
	TestCurves();
	TestLength();
	TestConfiguration();
	TestMixing();

	std::printf("\n%s\n", g_failures == 0 ? "All crossfade tests passed." : "Crossfade tests FAILED.");

	return g_failures == 0 ? 0 : 1;
}
