// The meter's arithmetic, with no window (SF-64).
//
// Everything here is a claim the drawing depends on and that looking at the
// screen would not settle: which segment is which colour at a boundary, what
// silence and a clipped signal do to the fraction, and how fast the peak
// marker falls. The drawing itself is checked by `ui_smoke`; this is checked
// by arithmetic.

#include <cmath>
#include <cstdio>

#include "ui/Meter.h"

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
		std::fprintf(stderr, "FAIL: %s (got %f, expected %f +/- %f)\n", what, static_cast<double>(got),
			static_cast<double>(expected), static_cast<double>(tolerance));
	}
}

using namespace sf;

// The scale, including every input that isn't an ordinary number. Each of
// these reaches the drawing as a segment count, and an unguarded one becomes
// an index off the end of a fixed array.
void TestFraction()
{
	CheckNear(MeterFraction(0.0f), 1.0f, 0.0001f, "0 dBFS is the top of the scale");
	CheckNear(MeterFraction(kMeterFloorDb), 0.0f, 0.0001f, "the floor is the bottom");
	CheckNear(MeterFraction(-30.0f), 0.5f, 0.0001f, "halfway down the scale is halfway along");

	Check(MeterFraction(-INFINITY) == 0.0f, "silence is the bottom, not a negative fraction");
	Check(MeterFraction(NAN) == 0.0f, "a NaN level is the bottom rather than an unbounded one");
	Check(MeterFraction(-200.0f) == 0.0f, "below the floor clamps to the floor");

	// A signal *above* full scale is the case that matters most: it is exactly
	// when the meter is being looked at, and an unclamped fraction would index
	// past the last segment.
	Check(MeterFraction(6.0f) == 1.0f, "above full scale clamps to the top");
}

// Colour by segment position, which is the thing that changed from the old
// whole-bar meter and the thing most likely to be quietly reverted.
void TestSegmentTones()
{
	// A level at the very top lights everything, so each segment's tone is its
	// own position's tone and nothing is Off.
	const float loud = 0.0f;

	Check(SegmentTone(0, kMeterSegments, loud, false) == MeterTone::Comfortable,
		"the bottom segment is comfortable even when the signal is hot");
	Check(SegmentTone(kMeterSegments - 1, kMeterSegments, loud, false) == MeterTone::Hot,
		"the top segment is hot even though the same bar starts green");

	// The boundaries. Found by walking the scale rather than hard-coding
	// indices, so the assertion survives a change to the segment count.
	for (int i = 0; i < kMeterSegments; ++i)
	{
		const float db = SegmentCentreDb(i, kMeterSegments);
		const MeterTone tone = SegmentTone(i, kMeterSegments, loud, false);

		if (db < kMeterComfortableDb)
		{
			Check(tone == MeterTone::Comfortable, "below -12 dB is comfortable");
		}
		else if (db < kMeterWarnDb)
		{
			Check(tone == MeterTone::Warn, "-12 to -6 dB is warn");
		}
		else
		{
			Check(tone == MeterTone::Hot, "above -6 dB is hot");
		}
	}

	// Clipping repaints only the top of the scale. A bar that went entirely red
	// would hide where the signal actually is, which is the thing being fixed.
	for (int i = 0; i < kMeterSegments; ++i)
	{
		const float db = SegmentCentreDb(i, kMeterSegments);
		const MeterTone tone = SegmentTone(i, kMeterSegments, loud, true);

		if (db >= kMeterClipDb)
		{
			Check(tone == MeterTone::Clip, "while clipping, above -3 dB is red");
		}
		else
		{
			Check(tone != MeterTone::Clip, "while clipping, the bottom of the bar keeps its own colour");
		}
	}
}

// Which segments are lit at all.
void TestSegmentsLit()
{
	Check(SegmentTone(0, kMeterSegments, -INFINITY, false) == MeterTone::Off, "silence lights nothing");

	Check(SegmentTone(kMeterSegments - 1, kMeterSegments, -30.0f, false) == MeterTone::Off,
		"a half-scale signal does not light the top segment");
	Check(SegmentTone(0, kMeterSegments, -30.0f, false) != MeterTone::Off,
		"a half-scale signal does light the bottom one");

	// The lit count follows the fraction: at halfway, half the segments.
	int lit = 0;

	for (int i = 0; i < kMeterSegments; ++i)
	{
		if (SegmentTone(i, kMeterSegments, -30.0f, false) != MeterTone::Off)
		{
			++lit;
		}
	}

	Check(lit == kMeterSegments / 2, "half a scale is half the segments");
}

// The peak hold: instant up, slow down, and it stops at the floor rather than
// falling forever into a coordinate nothing can draw.
void TestPeakDecay()
{
	CheckNear(DecayPeak(-40.0f, -6.0f, 0.016f), -6.0f, 0.0001f, "the peak jumps straight to a louder level");

	// One second of falling at the design's rate: 1.2 dB per 450 ms.
	const float after_one_second = DecayPeak(-6.0f, -INFINITY, 1.0f);
	CheckNear(after_one_second, -6.0f - kPeakFallDbPerSecond, 0.0001f, "the peak falls at 1.2 dB per 450 ms");

	// The same fall taken in 60 Hz steps has to land in the same place, or the
	// marker's speed would depend on the frame rate -- which is exactly what
	// expressing the design's per-frame figure as a rate is meant to prevent.
	float stepped = -6.0f;

	for (int i = 0; i < 60; ++i)
	{
		stepped = DecayPeak(stepped, -INFINITY, 1.0f / 60.0f);
	}

	CheckNear(stepped, after_one_second, 0.01f, "falling in small steps matches falling in one big one");

	Check(DecayPeak(kMeterFloorDb, -INFINITY, 100.0f) == kMeterFloorDb, "the peak stops at the floor");
	Check(std::isfinite(DecayPeak(-INFINITY, -INFINITY, 0.016f)), "a peak that started at silence stays a number");
	Check(DecayPeak(-6.0f, -INFINITY, -1.0f) <= -6.0f, "a negative time step does not raise the marker");
}

}  // namespace

int main()
{
	TestFraction();
	TestSegmentTones();
	TestSegmentsLit();
	TestPeakDecay();

	std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);

	return g_failures == 0 ? 0 : 1;
}
