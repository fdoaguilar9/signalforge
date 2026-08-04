// Pitch detection for the tuner (SF-81).
//
// The whole point of testing this rather than eyeballing it against a guitar
// is that the claim is exact: fed a 440 Hz sine it must read A4 at 0 cents,
// and a 20-cent detune must read back as 20 cents. A tuner is an instrument
// for measuring, and one that is itself a few cents out is worse than none --
// it will be believed.

#include "../src/audio/PitchDetect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

void CheckClose(double actual, double expected, double tolerance, const char* description)
{
	if (std::fabs(actual - expected) > tolerance)
	{
		std::printf("FAIL  %-52s expected %g +/- %g, got %g\n", description, expected, tolerance, actual);
		++g_failures;
		return;
	}

	std::printf("ok    %-52s %g\n", description, actual);
}

constexpr double kRate = 48000.0;

// A steady sine, which is the easy case and the one every other case is
// measured against.
std::vector<float> Sine(double frequency, uint32_t count, float amplitude = 0.5f)
{
	std::vector<float> out(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		out[i] = amplitude * static_cast<float>(std::sin(2.0 * M_PI * frequency * i / kRate));
	}

	return out;
}

// A plucked string is not a sine: it is a fundamental with a stack of
// harmonics over it, and on a guitar the second and third are often *louder*
// than the first. This is the shape that breaks naive autocorrelation.
std::vector<float> Plucked(double fundamental, uint32_t count)
{
	std::vector<float> out(count, 0.0f);

	// Deliberately weighted so the fundamental is not the loudest partial.
	static const double kPartials[] = {0.4, 1.0, 0.8, 0.5, 0.3, 0.2};

	for (uint32_t i = 0; i < count; ++i)
	{
		double sample = 0.0;

		for (size_t h = 0; h < sizeof(kPartials) / sizeof(kPartials[0]); ++h)
		{
			const double frequency = fundamental * static_cast<double>(h + 1);

			if (frequency >= kRate * 0.5)
			{
				break;
			}

			sample += kPartials[h] * std::sin(2.0 * M_PI * frequency * i / kRate);
		}

		out[i] = static_cast<float>(sample * 0.15);
	}

	return out;
}

// The frequency `cents` away from `reference`.
double Detune(double reference, double cents)
{
	return reference * std::pow(2.0, cents / 1200.0);
}

void TestNoteNaming()
{
	// The reference itself, and the two edges of the octave numbering.
	sf::NoteReading a4 = sf::DescribeNote(440.0f);
	Check(std::strcmp(a4.name, "A") == 0 && a4.octave == 4, "440 Hz is A4");
	CheckClose(a4.cents, 0.0, 0.001, "...at 0 cents");

	sf::NoteReading c4 = sf::DescribeNote(261.6256f);
	Check(std::strcmp(c4.name, "C") == 0 && c4.octave == 4, "middle C is C4, not B3 or C3");

	// The bottom of a guitar and the bottom of a bass, which are the notes
	// this has to get right and the ones an octave error is invisible on.
	sf::NoteReading e2 = sf::DescribeNote(82.4069f);
	Check(std::strcmp(e2.name, "E") == 0 && e2.octave == 2, "the guitar's low E is E2");

	sf::NoteReading e1 = sf::DescribeNote(41.2034f);
	Check(std::strcmp(e1.name, "E") == 0 && e1.octave == 1, "the bass's low E is E1");

	// Sharps, not flats, and the semitone above A.
	sf::NoteReading a_sharp = sf::DescribeNote(466.1638f);
	Check(std::strcmp(a_sharp.name, "A#") == 0 && a_sharp.octave == 4, "the semitone above A4 is A#4");

	// Cents are signed and never leave +/-50: past that it is the next note,
	// and a tuner reading "+70 cents" would be naming the wrong string.
	CheckClose(sf::DescribeNote(static_cast<float>(Detune(440.0, 20.0))).cents, 20.0, 0.01, "20 cents sharp reads +20");
	CheckClose(
		sf::DescribeNote(static_cast<float>(Detune(440.0, -20.0))).cents, -20.0, 0.01, "20 cents flat reads -20");

	sf::NoteReading edge = sf::DescribeNote(static_cast<float>(Detune(440.0, 60.0)));
	Check(std::strcmp(edge.name, "A#") == 0, "60 cents above A is named as A#, not A");
	Check(edge.cents > -50.0f && edge.cents < 50.0f, "...and reported within +/-50 cents of it");

	Check(sf::DescribeNote(0.0f).name[0] == '\0', "no pitch names no note");
}

void TestDetection()
{
	// The acceptance, exactly: a synthetic 440 Hz sine reads A4 at 0 cents.
	{
		const std::vector<float> samples = Sine(440.0, sf::kPitchWindowSamples);
		const sf::PitchReading pitch = sf::DetectPitch(samples.data(), sf::kPitchWindowSamples, kRate);

		CheckClose(pitch.frequency_hz, 440.0, 0.5, "a 440 Hz sine reads 440 Hz");

		const sf::NoteReading note = sf::DescribeNote(pitch.frequency_hz);
		Check(std::strcmp(note.name, "A") == 0 && note.octave == 4, "...named A4");
		CheckClose(note.cents, 0.0, 1.0, "...at 0 cents");
	}

	// ...and a detune reads back within a cent or two, in both directions.
	// Sub-sample interpolation is what buys this: at 48 kHz the whole-sample
	// lags either side of A4 are +1.4 and -14.4 cents, so a detector that
	// rounded to a sample could not express "20 cents flat" at all.
	for (double cents : {-20.0, -7.0, 7.0, 20.0})
	{
		const std::vector<float> samples = Sine(Detune(440.0, cents), sf::kPitchWindowSamples);
		const sf::PitchReading pitch = sf::DetectPitch(samples.data(), sf::kPitchWindowSamples, kRate);

		char what[64];
		std::snprintf(what, sizeof(what), "%+.0f cents from A4 reads back", cents);

		CheckClose(sf::DescribeNote(pitch.frequency_hz).cents, cents, 2.0, what);
	}

	// Every open string on a guitar, as a plucked tone with the fundamental
	// quieter than its own second harmonic. **This is the octave trap**: plain
	// autocorrelation reports the strongest periodicity, which for this shape
	// is very often half the frequency -- and the string is in tune either
	// way, so the display would be the only thing lying.
	struct OpenString
	{
		double frequency;
		const char* name;
		int octave;
	};

	const OpenString strings[] = {
		{82.4069, "E", 2},
		{110.0000, "A", 2},
		{146.8324, "D", 3},
		{195.9977, "G", 3},
		{246.9417, "B", 3},
		{329.6276, "E", 4},
	};

	for (const OpenString& string : strings)
	{
		const std::vector<float> samples = Plucked(string.frequency, sf::kPitchWindowSamples);
		const sf::PitchReading pitch = sf::DetectPitch(samples.data(), sf::kPitchWindowSamples, kRate);
		const sf::NoteReading note = sf::DescribeNote(pitch.frequency_hz);

		char what[80];
		std::snprintf(what, sizeof(what), "a plucked %s%d reads %s%d", string.name, string.octave, note.name,
			note.octave);

		Check(std::strcmp(note.name, string.name) == 0 && note.octave == string.octave, what);
	}

	// A bass low E, the lowest note the window is sized for.
	{
		const std::vector<float> samples = Plucked(41.2034, sf::kPitchWindowSamples);
		const sf::PitchReading pitch = sf::DetectPitch(samples.data(), sf::kPitchWindowSamples, kRate);
		const sf::NoteReading note = sf::DescribeNote(pitch.frequency_hz);

		Check(std::strcmp(note.name, "E") == 0 && note.octave == 1, "a plucked bass E1 reads E1");
	}
}

void TestRefusals()
{
	// Silence is not a note. A tuner that named one would be naming noise.
	{
		const std::vector<float> quiet(sf::kPitchWindowSamples, 0.0f);
		Check(sf::DetectPitch(quiet.data(), sf::kPitchWindowSamples, kRate).frequency_hz == 0.0f,
			"silence has no pitch");
	}

	// Neither is a chord: two notes a fifth apart are not periodic at either
	// of them, and picking one would be picking arbitrarily.
	{
		std::vector<float> chord(sf::kPitchWindowSamples);

		for (uint32_t i = 0; i < sf::kPitchWindowSamples; ++i)
		{
			const double t = static_cast<double>(i) / kRate;
			chord[i] = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 220.0 * t) +
				0.3 * std::sin(2.0 * M_PI * 329.6276 * t) + 0.3 * std::sin(2.0 * M_PI * 415.3047 * t));
		}

		const sf::PitchReading pitch = sf::DetectPitch(chord.data(), sf::kPitchWindowSamples, kRate);
		Check(pitch.frequency_hz == 0.0f || pitch.clarity < 0.9f, "a chord is not confidently one note");
	}

	// Too short to see two periods of the lowest searched note.
	{
		const std::vector<float> samples = Sine(440.0, 256);
		Check(sf::DetectPitch(samples.data(), 256, kRate).frequency_hz == 0.0f, "a window too short reports nothing");
	}

	Check(sf::DetectPitch(nullptr, 0, kRate).frequency_hz == 0.0f, "no samples report nothing");

	{
		const std::vector<float> samples = Sine(440.0, sf::kPitchWindowSamples);
		Check(sf::DetectPitch(samples.data(), sf::kPitchWindowSamples, 0.0).frequency_hz == 0.0f,
			"no sample rate reports nothing");
	}
}

}  // namespace

int main()
{
	TestNoteNaming();
	TestDetection();
	TestRefusals();

	if (g_failures > 0)
	{
		std::printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("\nall checks passed\n");

	return 0;
}
