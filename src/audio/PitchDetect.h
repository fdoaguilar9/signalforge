#pragma once

#include <cstdint>
#include <vector>

namespace sf
{

// Monophonic pitch detection for the tuner (SF-81).
//
// **Control thread only.** This is far too much work to do inline in a process
// callback -- it walks the whole window once per candidate lag -- so the audio
// thread's job is to hand over a copy of what it just heard and this runs on
// whatever thread is drawing.
//
// The method is autocorrelation with the cumulative-mean normalisation from
// YIN, which is the part that matters: plain autocorrelation picks the octave
// below about as often as the note, because a signal correlates with itself
// just as well at twice the period. A guitar tuner that reads E2 as E1 is
// worse than no tuner, since the string is *in tune* either way and the
// display is what's lying.

// A window long enough to see several cycles of the lowest note a guitar
// makes. Low E on a 4-string bass is 41.2 Hz -- 1165 samples at 48 kHz -- and
// the detector needs at least two periods plus the lag it is testing, so the
// window is the practical floor rather than a round number.
inline constexpr uint32_t kPitchWindowSamples = 4096;

// The range worth searching. Below 40 Hz is nothing a guitar or bass plays;
// above 1400 Hz is past the 22nd fret of the top string, and searching further
// only adds candidate lags for noise to win at.
inline constexpr float kPitchMinHz = 40.0f;
inline constexpr float kPitchMaxHz = 1400.0f;

struct PitchReading
{
	// Zero when nothing convincing was found -- silence, a chord, or a noise
	// floor. **Not an error**: a tuner between notes is the ordinary case, and
	// the display holds what it last had rather than flickering to a wrong
	// answer.
	float frequency_hz = 0.0f;

	// How periodic the window was, 0..1. The reading is only reported at all
	// above a threshold; this is exposed so a caller can show confidence, and
	// so a test can say *why* something was rejected.
	float clarity = 0.0f;
};

// Reads the pitch of `samples`. Anything shorter than a couple of periods of
// the lowest searched note reports nothing rather than guessing.
PitchReading DetectPitch(const float* samples, uint32_t count, double sample_rate);

// The note a frequency is, and how far from it.
struct NoteReading
{
	// "A", "C#", ... and the octave in scientific pitch notation, so A440 is
	// A4. Empty when there is no pitch to name.
	const char* name = "";
	int octave = 0;

	// How far off, in cents, negative flat. Always within +/-50: past that it
	// is the next note, and a tuner that read "+70 cents" would be naming the
	// wrong string.
	float cents = 0.0f;
};

// A440 equal temperament. Fixed rather than configurable: a tuner that could
// be set to A432 would be a tuner that could be *wrong* about which A it meant,
// and nothing else here needs the reference to move.
NoteReading DescribeNote(float frequency_hz);

}  // namespace sf
