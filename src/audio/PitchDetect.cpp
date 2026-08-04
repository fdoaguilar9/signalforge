#include "PitchDetect.h"

#include <cmath>

namespace sf
{

namespace
{

// Below this the window is not periodic enough to name a note: a chord, a
// palm mute dying away, or the room. Chosen to reject an open chord while
// still holding a note that has rung for a couple of seconds.
constexpr float kClarityFloor = 0.75f;

// A parabola through the difference function's minimum and its two
// neighbours, so the period is not quantised to whole samples.
//
// **This is what makes the reading usable rather than merely correct.** At
// 48 kHz, A4 is 109.09 samples per period; the nearest whole lags are 109 and
// 110, which are A4 +1.4 cents and -14.4 cents. Rounding to a sample would
// put a perfectly tuned string 14 cents out, and the display would be
// reporting its own arithmetic rather than the guitar.
double RefinePeriod(const std::vector<float>& difference, size_t lag)
{
	if (lag == 0 || lag + 1 >= difference.size())
	{
		return static_cast<double>(lag);
	}

	const double left = difference[lag - 1];
	const double centre = difference[lag];
	const double right = difference[lag + 1];

	const double denominator = 2.0 * (2.0 * centre - left - right);

	if (std::fabs(denominator) < 1e-12)
	{
		return static_cast<double>(lag);
	}

	return static_cast<double>(lag) + (right - left) / denominator;
}

}  // namespace

PitchReading DetectPitch(const float* samples, uint32_t count, double sample_rate)
{
	PitchReading out;

	if (!samples || sample_rate <= 0.0)
	{
		return out;
	}

	const uint32_t min_lag = static_cast<uint32_t>(sample_rate / kPitchMaxHz);
	const uint32_t max_lag = static_cast<uint32_t>(sample_rate / kPitchMinHz);

	// Two periods of the lowest note, or there is nothing to correlate the
	// second half of the window against.
	if (max_lag == 0 || count < max_lag * 2)
	{
		return out;
	}

	const uint32_t window = count - max_lag;

	// YIN's squared-difference function: how unlike itself the signal is at
	// each lag. A minimum is a period.
	std::vector<float> difference(max_lag + 1, 0.0f);

	for (uint32_t lag = min_lag; lag <= max_lag; ++lag)
	{
		double sum = 0.0;

		for (uint32_t i = 0; i < window; ++i)
		{
			const double delta = static_cast<double>(samples[i]) - samples[i + lag];
			sum += delta * delta;
		}

		difference[lag] = static_cast<float>(sum);
	}

	// The cumulative mean normalisation, which is the whole reason this is YIN
	// and not plain autocorrelation. Dividing each lag by the running mean of
	// everything below it makes the *first* deep minimum win rather than the
	// deepest -- and the deepest is very often the octave below, because a
	// periodic signal is just as unlike itself at twice its period.
	std::vector<float> normalised(max_lag + 1, 1.0f);

	double running = 0.0;

	for (uint32_t lag = min_lag; lag <= max_lag; ++lag)
	{
		running += difference[lag];

		normalised[lag] = running > 0.0
			? static_cast<float>(difference[lag] * static_cast<double>(lag - min_lag + 1) / running)
			: 1.0f;
	}

	// The first lag that dips below the threshold and then starts climbing
	// again -- "first", not "smallest", for the octave reason above.
	uint32_t best = 0;

	for (uint32_t lag = min_lag; lag <= max_lag; ++lag)
	{
		if (normalised[lag] >= 1.0f - kClarityFloor)
		{
			continue;
		}

		while (lag + 1 <= max_lag && normalised[lag + 1] < normalised[lag])
		{
			++lag;
		}

		best = lag;
		break;
	}

	if (best == 0)
	{
		return out;
	}

	const double period = RefinePeriod(difference, best);

	if (period <= 0.0)
	{
		return out;
	}

	out.clarity = 1.0f - normalised[best];
	out.frequency_hz = static_cast<float>(sample_rate / period);

	// A refined period can land just outside the searched range; a frequency
	// the caller was told would not appear is worse than none.
	if (out.frequency_hz < kPitchMinHz || out.frequency_hz > kPitchMaxHz)
	{
		return PitchReading();
	}

	return out;
}

NoteReading DescribeNote(float frequency_hz)
{
	NoteReading out;

	if (!(frequency_hz > 0.0f))
	{
		return out;
	}

	// Sharps rather than flats throughout. Guitarists say "F sharp" for the
	// second fret of the top string, and a display that alternated between
	// spellings by key would need to know the key.
	static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

	// Semitones from A4 = 440 Hz, fractional.
	const double semitones = 12.0 * std::log2(static_cast<double>(frequency_hz) / 440.0);

	const double rounded = std::round(semitones);

	out.cents = static_cast<float>((semitones - rounded) * 100.0);

	// A4 is MIDI 69, and MIDI note 0 is C-1 -- so the octave is the note
	// number divided by twelve, less one.
	const int midi = 69 + static_cast<int>(rounded);
	int index = midi % 12;

	if (index < 0)
	{
		index += 12;
	}

	out.name = kNames[index];

	// Floor division, which is not what integer division does for negatives:
	// MIDI 11 is B-1, and 11/12 == 0 would call it B0.
	out.octave = (midi - index) / 12 - 1;

	return out;
}

}  // namespace sf
