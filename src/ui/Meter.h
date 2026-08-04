#pragma once

#include <algorithm>
#include <cmath>

namespace sf
{

// The level meters, and the arithmetic behind them (SF-64).
//
// Everything above the drawing function is pure and has no Dear ImGui in it,
// which is what lets the interesting cases be asserted directly rather than
// only looked at. The interesting inputs are the ones that aren't ordinary
// numbers: silence arrives as -INFINITY (log10 of zero), a clipping signal
// exceeds 0 dBFS, and a divide-by-zero upstream would hand this a NaN.

// 28 segments, from the design. Enough that the bar reads as a level rather
// than as a row of blocks, few enough that each one is still a block.
inline constexpr int kMeterSegments = 28;

// The scale's floor. Below this a guitar signal is inaudible anyway, and
// spending scale on it would squash the part that matters into a sliver.
inline constexpr float kMeterFloorDb = -60.0f;

// Where the bar changes colour, **by the position of each segment rather than
// by the level as a whole**.
//
// This is the one place the design and the code it was drawn from actually
// disagree. The old meter coloured the whole bar by the current level, at -6
// and -1 dBFS; a segmented meter colours each segment by where that segment
// sits, so a hot signal is green at the bottom and orange at the top the way
// a mixing desk is. A meter that changes colour all at once is exactly the
// thing this one replaces, so the constants changed meaning rather than
// surviving unchanged.
inline constexpr float kMeterComfortableDb = -12.0f;  // below: comfortable
inline constexpr float kMeterWarnDb = -6.0f;          // -12 .. -6: warn, above: hot
inline constexpr float kMeterClipDb = -3.0f;          // while clipping, above: clip

// The peak-hold marker's fall. The design states it as 1.2 dB per 450 ms
// frame, which is a rate rather than a per-frame step -- the prototype
// happened to tick at 450 ms and this program draws at 60 Hz, so the figure
// only means the same thing if it is expressed in time.
inline constexpr float kPeakFallDbPerSecond = 1.2f / 0.450f;

enum class MeterTone
{
	// Above the current level: drawn as the well's own background, so the bar
	// reads as a bar rather than as 28 lit squares of varying brightness.
	Off,

	Comfortable,
	Warn,
	Hot,
	Clip
};

// Where a level sits on the scale, always within 0..1.
//
// Left unguarded, each of the non-ordinary inputs above becomes a fraction
// outside 0..1 -- which a drawing library may quietly saturate today and not
// tomorrow.
inline float MeterFraction(float db)
{
	if (!std::isfinite(db))
	{
		return 0.0f;
	}

	const float clamped = std::min(0.0f, std::max(kMeterFloorDb, db));

	return (clamped - kMeterFloorDb) / -kMeterFloorDb;
}

// The dB level at the middle of one segment. The middle rather than either
// edge so that a segment is the colour of what it mostly represents; the
// boundaries (-12, -6, -3) do not fall on segment edges at any segment count
// worth using, so some rule has to be chosen and stated.
inline float SegmentCentreDb(int index, int count)
{
	if (count <= 0)
	{
		return kMeterFloorDb;
	}

	const float fraction = (static_cast<float>(index) + 0.5f) / static_cast<float>(count);

	return kMeterFloorDb + fraction * -kMeterFloorDb;
}

// What colour one segment is, given where the level currently is.
inline MeterTone SegmentTone(int index, int count, float level_db, bool clipping)
{
	const float lit = MeterFraction(level_db) * static_cast<float>(count);

	if (static_cast<float>(index) >= lit)
	{
		return MeterTone::Off;
	}

	const float db = SegmentCentreDb(index, count);

	// Clipping repaints the top of the scale rather than the whole bar: the
	// signal that clipped is still mostly at a sane level, and reddening all
	// of it would hide where it actually is.
	if (clipping && db >= kMeterClipDb)
	{
		return MeterTone::Clip;
	}

	if (db >= kMeterWarnDb)
	{
		return MeterTone::Hot;
	}

	if (db >= kMeterComfortableDb)
	{
		return MeterTone::Warn;
	}

	return MeterTone::Comfortable;
}

// Where the peak marker is after `seconds` have passed.
//
// Rises instantly and falls slowly, which is the whole point of a peak hold:
// the transient that clipped lasted one block and would otherwise be gone
// before anyone looked up.
inline float DecayPeak(float held_db, float level_db, float seconds)
{
	const float held = std::isfinite(held_db) ? held_db : kMeterFloorDb;
	const float live = std::isfinite(level_db) ? level_db : kMeterFloorDb;

	const float fallen = held - kPeakFallDbPerSecond * std::max(0.0f, seconds);

	return std::max(live, std::max(kMeterFloorDb, fallen));
}

// --- drawing ---

// State the view owns between frames, because a peak hold is a memory and the
// engine reports only what the last block did.
struct MeterState
{
	float peak_db = -INFINITY;
};

// One meter: the label, the segmented well, the peak marker, and the reading.
//
// `height` is 14 on the board and 20 in Live -- the only difference between
// the two, which is why there is one function and not two.
void DrawMeter(const char* label, float db, bool clipping, MeterState& peak, float height);

// The shared `-60 -30 -18 -12 -6 0` row that sits under a stack of meters.
// `label_width` and `readout_width` are what the meters above it used, so the
// numbers line up with the well rather than with the row.
void DrawMeterScale(float label_width, float readout_width);

// The same segmented well, carrying a percentage rather than a level: the DSP
// gauge (SF-65). Shares the drawing because it is the same object -- a
// different scale in the same chassis.
void DrawLoadGauge(const char* label, float fraction, const char* tooltip);

}  // namespace sf
