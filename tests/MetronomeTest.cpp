// The metronome's timing (SF-82).
//
// The claim is exact and arithmetical -- at 120 BPM and 48 kHz a click lands
// every 24000 frames -- so it is checked rather than tapped along to. What
// cannot be checked here is whether it *feels* steady, but a click that lands
// on the wrong sample will never feel steady, and this is the half that can be
// pinned down.

#include "../src/audio/Metronome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

constexpr double kRate = 48000.0;

// Where each beat starts, rendered a block at a time.
//
// A beat is a *burst*, not a plateau: it is a decaying sine, so it crosses
// zero every half cycle and "the first loud sample after a quiet one" finds
// dozens of them per click. What separates one beat from the next is a gap
// far longer than the burst itself, so that is what this looks for.
std::vector<uint64_t> BeatStarts(sf::Metronome& metronome, uint64_t frames, uint32_t block)
{
	std::vector<uint64_t> starts;
	std::vector<float> left(block, 0.0f);
	std::vector<float> right(block, 0.0f);

	// Comfortably longer than one burst and shorter than the fastest tempo's
	// gap: 240 BPM at 48 kHz is 12000 frames between beats, and a burst is
	// 1440.
	const uint64_t separation = static_cast<uint64_t>(kRate * sf::kMetronomeBeatSeconds) * 2;

	uint64_t position = 0;
	uint64_t last_loud = 0;
	bool seen = false;

	while (position < frames)
	{
		const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(block, frames - position));

		std::fill(left.begin(), left.begin() + take, 0.0f);
		std::fill(right.begin(), right.begin() + take, 0.0f);

		metronome.Mix(left.data(), right.data(), take);

		for (uint32_t i = 0; i < take; ++i)
		{
			if (std::fabs(left[i]) <= 1e-6f)
			{
				continue;
			}

			const uint64_t at = position + i;

			if (!seen || at - last_loud > separation)
			{
				starts.push_back(at);
			}

			last_loud = at;
			seen = true;
		}

		position += take;
	}

	return starts;
}

void TestTiming()
{
	sf::Metronome metronome;
	metronome.SetSampleRate(kRate);
	metronome.SetTempo(120.0f);
	metronome.SetBeatsPerBar(4);
	metronome.SetLevel(1.0f);
	metronome.SetRunning(true);

	// **The acceptance, exactly.** 120 BPM at 48 kHz is one beat every 24000
	// frames, and every gap has to be that -- not on average, not within a
	// millisecond. A metronome that drifts by one sample a beat is four
	// samples out after a bar and a hundred after a chorus.
	const std::vector<uint64_t> starts = BeatStarts(metronome, 48000 * 5, 256);

	Check(starts.size() >= 9, "five seconds at 120 BPM is at least nine clicks");

	bool spaced = true;

	for (size_t i = 1; i < starts.size(); ++i)
	{
		if (starts[i] - starts[i - 1] != 24000)
		{
			std::printf("FAIL  beat %zu came %llu frames after the last, expected 24000\n", i,
				static_cast<unsigned long long>(starts[i] - starts[i - 1]));
			spaced = false;
			++g_failures;
			break;
		}
	}

	if (spaced)
	{
		std::printf("ok    every beat is exactly 24000 frames after the one before\n");
	}

	// The block size must not move the beat. A metronome that only checked
	// block boundaries would quantise every click to the buffer, which at 256
	// samples is 5 ms of jitter -- audible as sloppiness against anything
	// being played to it.
	for (uint32_t block : {13u, 64u, 512u, 1024u})
	{
		sf::Metronome other;
		other.SetSampleRate(kRate);
		other.SetTempo(120.0f);
		other.SetLevel(1.0f);
		other.SetRunning(true);

		const std::vector<uint64_t> at_block = BeatStarts(other, 48000 * 2, block);

		char what[80];
		std::snprintf(what, sizeof(what), "a block size of %u puts the beats in the same places", block);

		bool same = !at_block.empty();

		for (size_t i = 0; i < at_block.size() && i < starts.size(); ++i)
		{
			same = same && at_block[i] == starts[i];
		}

		Check(same, what);
	}

	// Other tempos, from the two ends of the design's range.
	struct TempoCase
	{
		float bpm;
		uint64_t expected;
	};

	const TempoCase tempos[] = {{60.0f, 48000}, {40.0f, 72000}, {240.0f, 12000}, {90.0f, 32000}};

	for (const TempoCase& one : tempos)
	{
		sf::Metronome other;
		other.SetSampleRate(kRate);
		other.SetTempo(one.bpm);
		other.SetLevel(1.0f);
		other.SetRunning(true);

		const std::vector<uint64_t> at_tempo = BeatStarts(other, one.expected * 3, 256);

		char what[80];
		std::snprintf(what, sizeof(what), "%.0f BPM is a beat every %llu frames", static_cast<double>(one.bpm),
			static_cast<unsigned long long>(one.expected));

		Check(at_tempo.size() >= 2 && at_tempo[1] - at_tempo[0] == one.expected, what);
	}

	// Out of range is clamped, not obeyed: this is fed by a slider and by a
	// MIDI binding, and neither can produce a number a person meant.
	{
		sf::Metronome other;
		other.SetTempo(10000.0f);
		Check(other.Tempo() <= 240.0f, "an absurd tempo is clamped to the top of the range");

		other.SetTempo(-5.0f);
		Check(other.Tempo() >= 40.0f, "...and a negative one to the bottom");
	}
}

void TestBarsAndLevels()
{
	// The downbeat is a different pitch from the others, which is how a
	// metronome says where the bar starts. Checked as "not the same samples"
	// rather than by measuring a frequency: what matters is that they differ.
	sf::Metronome metronome;
	metronome.SetSampleRate(kRate);
	metronome.SetTempo(120.0f);
	metronome.SetBeatsPerBar(4);
	metronome.SetLevel(1.0f);
	metronome.SetRunning(true);

	std::vector<float> rendered(24000 * 5, 0.0f);
	metronome.Mix(rendered.data(), nullptr, static_cast<uint32_t>(rendered.size()));

	const uint32_t burst = static_cast<uint32_t>(kRate * sf::kMetronomeBeatSeconds);

	bool differs = false;

	for (uint32_t i = 0; i < burst; ++i)
	{
		if (std::fabs(rendered[i] - rendered[24000 + i]) > 1e-6f)
		{
			differs = true;
			break;
		}
	}

	Check(differs, "the downbeat does not sound like the beats after it");

	// ...and the beat after four is a downbeat again.
	bool repeats = true;

	for (uint32_t i = 0; i < burst; ++i)
	{
		repeats = repeats && std::fabs(rendered[i] - rendered[24000 * 4 + i]) < 1e-6f;
	}

	Check(repeats, "and the first beat of the next bar sounds like the first of this one");

	// Which beat is showing, for the row of dots.
	Check(metronome.CurrentBeat() < 4, "the reported beat is inside the bar");

	// Stopped is silent, and costs nothing.
	{
		sf::Metronome stopped;
		stopped.SetSampleRate(kRate);
		stopped.SetLevel(1.0f);

		std::vector<float> buffer(1024, 0.0f);
		stopped.Mix(buffer.data(), nullptr, 1024);

		bool silent = true;

		for (float sample : buffer)
		{
			silent = silent && sample == 0.0f;
		}

		Check(silent, "a stopped metronome adds nothing");
	}

	// A level of zero is silence even while running -- the control goes to 0%
	// and has to mean it.
	{
		sf::Metronome quiet;
		quiet.SetSampleRate(kRate);
		quiet.SetTempo(120.0f);
		quiet.SetLevel(0.0f);
		quiet.SetRunning(true);

		std::vector<float> buffer(48000, 0.0f);
		quiet.Mix(buffer.data(), nullptr, static_cast<uint32_t>(buffer.size()));

		bool silent = true;

		for (float sample : buffer)
		{
			silent = silent && sample == 0.0f;
		}

		Check(silent, "a level of zero is silence");
	}

	// **The click is added, not written.** It mixes into the board's output,
	// so what comes out has to be the board's signal *plus* the click --
	// which is not the same as "still loud": a click is a sine and half of it
	// is negative, so adding one to a positive buffer legitimately pulls
	// samples down. The claim is the sum, so the sum is what is checked.
	{
		sf::Metronome alone;
		alone.SetSampleRate(kRate);
		alone.SetTempo(120.0f);
		alone.SetLevel(1.0f);
		alone.SetRunning(true);

		std::vector<float> click(1024, 0.0f);
		alone.Mix(click.data(), nullptr, 1024);

		sf::Metronome over;
		over.SetSampleRate(kRate);
		over.SetTempo(120.0f);
		over.SetLevel(1.0f);
		over.SetRunning(true);

		std::vector<float> buffer(1024, 0.25f);
		over.Mix(buffer.data(), nullptr, 1024);

		bool summed = true;

		for (size_t i = 0; i < buffer.size(); ++i)
		{
			summed = summed && std::fabs(buffer[i] - (0.25f + click[i])) < 1e-6f;
		}

		Check(summed, "the click is mixed into the output, not written over it");
	}
}

}  // namespace

int main()
{
	TestTiming();
	TestBarsAndLevels();

	if (g_failures > 0)
	{
		std::printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("\nall checks passed\n");

	return 0;
}
