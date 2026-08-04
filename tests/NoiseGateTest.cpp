// The built-in noise gate.
//
// A gate is a thing that is meant to be *inaudible when it is right* and
// obvious when it is wrong, which makes it exactly the wrong thing to tune by
// listening. Every claim the sliders make -- "60 dB in this many milliseconds",
// "it holds for 50 ms", "it does not chatter" -- is arithmetic, so all of it is
// checked here rather than played through.
//
// The two that matter most are the ones a gate fails at in the field: chatter
// on a low note, and a disabled gate that is not quite a no-op.

#include "../src/audio/NoiseGate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

void CheckClose(double actual, double expected, double tolerance, const char* description)
{
	if (std::fabs(actual - expected) > tolerance)
	{
		std::printf("FAIL  %-54s expected %g +/- %g, got %g\n", description, expected, tolerance, actual);
		++g_failures;
		return;
	}

	std::printf("ok    %-54s %g\n", description, actual);
}

constexpr double kRate = 48000.0;

double DbToLinear(double db)
{
	return std::pow(10.0, db / 20.0);
}

// A steady sine at a stated level, which is how every threshold claim below is
// posed: "a signal at -50 dBFS, with the threshold at -40".
std::vector<float> Sine(double db, double frequency, uint32_t frames)
{
	std::vector<float> out(frames);

	for (uint32_t i = 0; i < frames; ++i)
	{
		out[i] = static_cast<float>(DbToLinear(db) * std::sin(2.0 * M_PI * frequency * i / kRate));
	}

	return out;
}

// Runs a buffer through in blocks, in place.
void Run(sf::NoiseGate& gate, std::vector<float>& samples, uint32_t block = 256)
{
	for (uint32_t at = 0; at < samples.size(); at += block)
	{
		const uint32_t take = std::min<uint32_t>(block, static_cast<uint32_t>(samples.size()) - at);
		float* channel = samples.data() + at;

		gate.Process(&channel, 1, take);
	}
}

// Configured in place rather than returned: a NoiseGate owns atomics, so it is
// neither copyable nor movable -- which is the correct shape for something the
// audio thread reads and exactly why it cannot be handed back by value.
void Configure(sf::NoiseGate& gate, double threshold_db, double release_ms, bool enabled = true)
{
	gate.Reset(kRate);
	gate.SetThresholdDb(static_cast<float>(threshold_db));
	gate.SetReleaseMs(static_cast<float>(release_ms));
	gate.SetEnabled(enabled);
}

void TestPassAndClose()
{
	// **A gate that has never been enabled is a no-op**, bit for bit, for any
	// input. This is what lets every preset written before the gate existed
	// keep its exact sound, and it is the first thing to check.
	//
	// Bit-identity alone would not prove it: multiplying by 1.0f is also
	// bit-identical. The frame counter is what distinguishes "returned early"
	// from "did the work and happened not to change anything".
	{
		sf::NoiseGate gate;
		gate.Reset(kRate);

		std::vector<float> quiet = Sine(-70.0, 220.0, 48000);
		const std::vector<float> before = quiet;

		Run(gate, quiet);

		Check(std::memcmp(quiet.data(), before.data(), before.size() * sizeof(float)) == 0,
			"a gate that was never enabled changes nothing, bit for bit");
		Check(gate.ProcessedFrames() == 0, "...and does no per-sample work at all");
	}

	// Above the threshold, once open, is unity -- exactly, not nearly.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 200.0);

		std::vector<float> loud = Sine(-20.0, 220.0, 48000);
		const std::vector<float> before = loud;

		Run(gate, loud);

		bool identical = true;

		// Skipping the first attack ramp, which is the one place it is
		// deliberately not unity.
		for (size_t i = 480; i < loud.size(); ++i)
		{
			identical = identical && loud[i] == before[i];
		}

		Check(identical, "a signal above the threshold passes at unity, bit for bit");
		Check(gate.Gain() == 1.0f, "...with the gate reporting fully open");
	}

	// Below it, the gate reaches **exactly** zero -- not a small number. A
	// multiplicative decay never arrives on its own, and the residue would be
	// a denormal trickle into every plugin downstream.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 100.0);

		std::vector<float> quiet = Sine(-60.0, 220.0, 48000);
		Run(gate, quiet);

		bool silent = true;

		for (size_t i = 24000; i < quiet.size(); ++i)
		{
			silent = silent && quiet[i] == 0.0f;
		}

		Check(silent, "a signal below the threshold reaches exact zero, not a denormal");
		Check(gate.Gain() == 0.0f, "...and the gate reports fully closed");
	}

	// A signal that never reaches the open threshold never *opens* a shut gate.
	//
	// Started from shut deliberately. A signal at -41 dB is below the -40 open
	// threshold but above the -46 close threshold, so it would legitimately
	// hold an already-open gate open -- that is what hysteresis means, and
	// asserting this from a fresh gate (which starts open) would be asserting
	// the opposite of the intended behaviour.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 100.0);

		std::vector<float> silence(static_cast<size_t>(kRate), 0.0f);
		Run(gate, silence);

		Check(gate.Gain() == 0.0f, "the gate starts this check shut");

		std::vector<float> under = Sine(-41.0, 220.0, 48000);
		Run(gate, under);

		Check(gate.Gain() == 0.0f, "a signal a dB under the threshold never opens a shut gate");
	}
}

void TestTiming()
{
	// How long from the signal stopping to the gain being fully down. Returned
	// in samples, measured from the cut.
	const auto silence_after = [](double release_ms, double* out_minus_60)
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, release_ms);

		// Open it first.
		std::vector<float> loud = Sine(-20.0, 220.0, 24000);
		Run(gate, loud);

		std::vector<float> nothing(static_cast<size_t>(kRate * 4), 0.0f);

		// Measured from the first sample the gain actually moves, not from the
		// cut. Two other things happen in between and neither is the release:
		// the detector has to decay from the signal's level down to the close
		// threshold, and then the hold has to expire. Timing from the cut would
		// be timing all three and calling it one.
		uint64_t started_at = 0;
		uint64_t minus_60_at = 0;
		uint64_t position = 0;

		for (uint32_t at = 0; at < nothing.size(); at += 64)
		{
			const uint32_t take = std::min<uint32_t>(64, static_cast<uint32_t>(nothing.size()) - at);
			float* channel = nothing.data() + at;

			gate.Process(&channel, 1, take);
			position += take;

			if (started_at == 0 && gate.Gain() < 1.0f)
			{
				started_at = position;
			}

			if (started_at != 0 && minus_60_at == 0 && gate.Gain() <= 0.001f)
			{
				minus_60_at = position;
			}
		}

		*out_minus_60 = static_cast<double>(minus_60_at - started_at);
	};

	// **The release time is the release time.** The slider says "60 dB in this
	// many milliseconds" and that is a promise, not a feel. The tolerance is
	// the 64-sample granularity of the measurement, not slack in the claim.
	for (double release : {60.0, 200.0, 500.0})
	{
		double measured = 0.0;
		silence_after(release, &measured);

		const double expected = kRate * release / 1000.0;

		char what[80];
		std::snprintf(what, sizeof(what), "%.0f ms release reaches -60 dB when it said it would", release);

		CheckClose(measured, expected, expected * 0.05 + 128.0, what);
	}

	// The hold keeps it open first. A gate that started releasing the moment
	// the signal dipped would cut the tail off every note.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 500.0);

		std::vector<float> loud = Sine(-20.0, 220.0, 24000);
		Run(gate, loud);

		// 45 ms is inside the hold on its own, and the detector's decay from
		// -20 dB down to the close threshold buys another ~30 ms on top, so
		// this is comfortably still open.
		std::vector<float> nothing(static_cast<size_t>(kRate * 0.045), 0.0f);
		Run(gate, nothing, 64);

		Check(gate.Gain() == 1.0f, "45 ms after the signal stops the gate is still fully open");

		// ...and by 150 ms the detector has fallen, the hold has expired, and
		// the release is under way.
		std::vector<float> more(static_cast<size_t>(kRate * 0.105), 0.0f);
		Run(gate, more, 64);

		Check(gate.Gain() < 1.0f, "...and by 150 ms it has started to close");
	}

	// A pick attack is not swallowed: from shut, a step to full scale is at
	// unity within a couple of milliseconds, and it *ramps* rather than
	// jumping -- a gate that stepped would click.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 100.0);

		std::vector<float> quiet(static_cast<size_t>(kRate), 0.0f);
		Run(gate, quiet);

		Check(gate.Gain() == 0.0f, "the gate starts this check shut");

		std::vector<float> hit(96, 0.5f);
		float* channel = hit.data();
		gate.Process(&channel, 1, 24);

		Check(gate.Gain() > 0.0f && gate.Gain() < 1.0f, "half a millisecond in, the attack is still ramping");

		float* rest = hit.data() + 24;
		gate.Process(&rest, 1, 72);

		Check(gate.Gain() == 1.0f, "two milliseconds in, it is fully open");
	}

	// The timings are times, not sample counts: the same release at 96 kHz
	// takes the same number of milliseconds.
	{
		sf::NoiseGate gate;
		gate.Reset(96000.0);
		gate.SetThresholdDb(-40.0f);
		gate.SetReleaseMs(200.0f);
		gate.SetEnabled(true);

		std::vector<float> loud(48000);

		for (size_t i = 0; i < loud.size(); ++i)
		{
			loud[i] = static_cast<float>(0.1 * std::sin(2.0 * M_PI * 220.0 * static_cast<double>(i) / 96000.0));
		}

		Run(gate, loud);

		std::vector<float> nothing(static_cast<size_t>(96000.0 * 0.4), 0.0f);
		Run(gate, nothing, 64);

		// 50 ms hold + 200 ms release = 250 ms, comfortably inside 400 ms.
		Check(gate.Gain() == 0.0f, "a release stated in ms means the same at 96 kHz");
	}
}

void TestNoChatter()
{
	// **The one that matters in the field.** A peak detector's envelope ripples
	// between waveform peaks, and the lower the note the wider the ripple: low
	// E is a 12 ms period and a drop-tuned low F# is 22 ms. With too short a
	// hold the envelope dips below the close threshold *between peaks of a
	// sustained note*, and the gate chatters at the note's own frequency --
	// which sounds like the amp is broken.
	//
	// Counted as "how many times did the gain go down", which must be zero
	// while a note is sounding above the threshold.
	// **Just above the threshold, which is the only place this can happen.**
	// A note 10 dB clear of the threshold cannot ripple below the close
	// threshold however short the hold, so testing there asserts nothing --
	// and did, until an injected 5 ms hold sailed straight through it. A note
	// decaying *toward* the threshold is when a real gate chatters, so that is
	// where this sits.
	//
	// What it holds the gate to is the *property*, not either mechanism. Hold
	// and detector decay each prevent this on their own, so shortening one
	// still passes; it takes losing both, which is what was checked.
	for (double frequency : {46.0, 82.4, 110.0})
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 200.0);

		std::vector<float> note = Sine(-37.0, frequency, static_cast<uint32_t>(kRate * 2));

		float previous = 0.0f;
		int dips = 0;

		for (uint32_t at = 0; at < note.size(); at += 64)
		{
			const uint32_t take = std::min<uint32_t>(64, static_cast<uint32_t>(note.size()) - at);
			float* channel = note.data() + at;

			gate.Process(&channel, 1, take);

			if (at > 4800 && gate.Gain() < previous)
			{
				++dips;
			}

			previous = gate.Gain();
		}

		char what[80];
		std::snprintf(what, sizeof(what), "a sustained %.0f Hz note never chatters the gate", frequency);

		Check(dips == 0, what);
	}

	// Hysteresis: a note that peaks above the open threshold and settles
	// between the two stays open. Without the 6 dB gap this is the signal that
	// sits on the threshold and stutters.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 200.0);

		std::vector<float> loud = Sine(-38.0, 220.0, 12000);
		Run(gate, loud);

		std::vector<float> between = Sine(-44.0, 220.0, static_cast<uint32_t>(kRate));
		Run(gate, between);

		Check(gate.Gain() == 1.0f, "a signal between the open and close thresholds stays open");
	}
}

void TestLinkedAndBlocks()
{
	// **Linked, not per channel.** A per-channel gate collapses a stereo image
	// the moment one side drops below the threshold. Checked by scaling one
	// channel by exactly 2, which is exact in binary, so the two outputs must
	// stay in exactly that ratio.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 200.0);

		std::vector<float> left = Sine(-30.0, 220.0, 24000);
		std::vector<float> right(left.size());

		for (size_t i = 0; i < left.size(); ++i)
		{
			right[i] = left[i] * 2.0f;
		}

		for (uint32_t at = 0; at < left.size(); at += 256)
		{
			const uint32_t take = std::min<uint32_t>(256, static_cast<uint32_t>(left.size()) - at);
			float* channels[2] = {left.data() + at, right.data() + at};

			gate.Process(channels, 2, take);
		}

		bool linked = true;

		for (size_t i = 0; i < left.size(); ++i)
		{
			linked = linked && right[i] == left[i] * 2.0f;
		}

		Check(linked, "both channels get one gain, so a stereo image cannot collapse");
	}

	// A silent right channel must not gate the left one. This is the case a
	// linked gate exists for and an unlinked one gets wrong.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 200.0);

		std::vector<float> left = Sine(-20.0, 220.0, 24000);
		std::vector<float> right(left.size(), 0.0f);

		for (uint32_t at = 0; at < left.size(); at += 256)
		{
			const uint32_t take = std::min<uint32_t>(256, static_cast<uint32_t>(left.size()) - at);
			float* channels[2] = {left.data() + at, right.data() + at};

			gate.Process(channels, 2, take);
		}

		Check(gate.Gain() == 1.0f, "one loud channel keeps the gate open for both");
	}

	// **Block-size independence.** This is the claim that forbids computing
	// anything per block: the envelope, the hold and the ramp are all per
	// sample, so the buffer size the server happens to use cannot change the
	// sound. 13 especially -- a size that divides nothing.
	{
		std::vector<float> reference;

		for (uint32_t block : {13u, 64u, 256u, 1024u})
		{
			sf::NoiseGate gate;
		Configure(gate, -40.0, 150.0);

			std::vector<float> signal = Sine(-25.0, 220.0, 12000);
			std::vector<float> tail(12000, 0.0f);

			signal.insert(signal.end(), tail.begin(), tail.end());

			Run(gate, signal, block);

			if (reference.empty())
			{
				reference = signal;
				continue;
			}

			char what[80];
			std::snprintf(what, sizeof(what), "a block size of %u gives the same samples", block);

			Check(std::memcmp(signal.data(), reference.data(), reference.size() * sizeof(float)) == 0, what);
		}
	}
}

void TestSurvival()
{
	// A single bad sample must not wedge the gate shut forever. Every
	// comparison against NaN is false, so an unguarded envelope stays NaN, the
	// gate closes, and nothing short of a restart reopens it.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 100.0);

		std::vector<float> poisoned(256, 0.5f);
		poisoned[100] = std::nanf("");

		float* channel = poisoned.data();
		gate.Process(&channel, 1, 256);

		std::vector<float> clean = Sine(-20.0, 220.0, 4800);
		Run(gate, clean);

		Check(gate.Gain() == 1.0f, "a NaN in the signal does not wedge the gate shut");
	}

	// DC offset must not wedge it *open*. An interface with a few LSB of
	// offset holds |x| above a low threshold forever, and the symptom is a
	// gate that appears to do nothing.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 100.0);

		std::vector<float> offset = Sine(-60.0, 220.0, static_cast<uint32_t>(kRate * 2));

		for (float& sample : offset)
		{
			sample += 0.01f;  // about -40 dBFS of DC
		}

		Run(gate, offset);

		Check(gate.Gain() == 0.0f, "a DC offset does not hold the gate open");
	}

	// Enabling mid-note must not cut it. The gate comes up open.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 200.0, false);

		std::vector<float> playing = Sine(-20.0, 220.0, 4800);
		Run(gate, playing);

		gate.SetEnabled(true);

		std::vector<float> more = Sine(-20.0, 220.0, 480);
		const std::vector<float> before = more;

		Run(gate, more);

		Check(std::memcmp(more.data(), before.data(), before.size() * sizeof(float)) == 0,
			"enabling the gate while a note rings does not cut it");
	}

	// Disabling it while shut must ramp up, not step. A step from silence to
	// unity is a click, and it would land exactly on a preset change.
	{
		sf::NoiseGate gate;
		Configure(gate, -40.0, 100.0);

		std::vector<float> quiet(static_cast<size_t>(kRate), 0.0f);
		Run(gate, quiet);

		Check(gate.Gain() == 0.0f, "the gate starts this check shut");

		gate.SetEnabled(false);

		std::vector<float> constant(48, 1.0f);
		float* channel = constant.data();
		gate.Process(&channel, 1, 48);

		bool ramped = true;
		float previous = 0.0f;

		for (float sample : constant)
		{
			// Each step is the attack rate, one part in 48 at this rate.
			ramped = ramped && sample >= previous && (sample - previous) < 0.05f;
			previous = sample;
		}

		Check(ramped, "disabling the gate while shut ramps back to unity rather than stepping");
	}

	// Settings survive a Reset, which happens on every engine restart. Losing
	// them there would silently drop a preset's gate the first time someone
	// changed their buffer size.
	{
		sf::NoiseGate gate;
		Configure(gate, -33.0, 321.0);
		gate.Reset(96000.0);

		const sf::NoiseGateSettings settings = gate.Settings();

		Check(settings.threshold_db == -33.0f && settings.release_ms == 321.0f && settings.enabled,
			"a Reset keeps the settings and only clears the runtime state");
		Check(gate.Gain() == 1.0f, "...and comes back open rather than fading the board in");
	}

	// Out-of-range values are clamped, not obeyed: these come from a slider and
	// from a file a person may have edited by hand.
	{
		sf::NoiseGate gate;
		gate.Reset(kRate);

		gate.SetThresholdDb(40.0f);
		Check(gate.Settings().threshold_db == sf::kGateMaxThresholdDb, "an absurd threshold is clamped");

		gate.SetReleaseMs(-5.0f);
		Check(gate.Settings().release_ms == sf::kGateMinReleaseMs, "a negative release is clamped");
	}
}

}  // namespace

int main()
{
	TestPassAndClose();
	TestTiming();
	TestNoChatter();
	TestLinkedAndBlocks();
	TestSurvival();

	if (g_failures > 0)
	{
		std::printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("\nall checks passed\n");

	return 0;
}
