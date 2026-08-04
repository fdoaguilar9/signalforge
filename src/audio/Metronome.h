#pragma once

#include <atomic>
#include <cstdint>

namespace sf
{

// The click (SF-82).
//
// **Beats are scheduled from a sample counter on the audio thread.** No timer,
// no wall clock, no allocation: the click has to land on the same sample every
// time or it is not a metronome, and anything that consults a clock will drift
// against the audio it is supposed to be counting.
//
// The click is mixed into the output *after* the chain and after the output
// trim, so the board's gain staging cannot change how loud it is and it can
// never feed back into the tuner.

// What a beat sounds like: a short burst of sine with a fast decay. Not a
// sample file -- one fewer thing to ship, to load, and to fail to find -- and
// not a click of noise, which is hard to place in time by ear at low levels.
inline constexpr float kMetronomeBeatSeconds = 0.03f;

// The downbeat is a fifth above the others, which is how every metronome
// distinguishes them and needs no explaining to anyone who has used one.
inline constexpr float kMetronomeBeatHz = 880.0f;
inline constexpr float kMetronomeDownbeatHz = 1320.0f;

class Metronome
{
public:
	// Control thread. Safe while the audio thread is running: everything the
	// audio thread reads is an atomic, and none of these allocate.
	void SetSampleRate(double sample_rate);
	void SetTempo(float bpm);
	void SetBeatsPerBar(unsigned beats);
	void SetLevel(float level);      // 0..1
	void SetRunning(bool running);

	float Tempo() const { return bpm_.load(std::memory_order_relaxed); }
	unsigned BeatsPerBar() const { return beats_per_bar_.load(std::memory_order_relaxed); }
	float Level() const { return level_.load(std::memory_order_relaxed); }
	bool Running() const { return running_.load(std::memory_order_relaxed); }

	// Which beat of the bar is sounding, 0-based, for the row of dots. Reads a
	// value the audio thread publishes; never blocks and never waits.
	unsigned CurrentBeat() const { return beat_.load(std::memory_order_relaxed); }

	// How many samples until the next beat starts. Exposed for tests -- "the
	// click lands every 24000 frames at 120 BPM" is a claim about *this*, and
	// checking it through a rendered buffer would be checking the oscillator
	// at the same time.
	uint64_t SamplesToNextBeat() const;

	// Adds `frames` of click into `left` and `right`, which already hold the
	// board's output. Silent and nearly free when stopped.
	//
	// **Audio thread.** Allocates nothing, locks nothing, and calls nothing
	// that could.
	void Mix(float* left, float* right, uint32_t frames);

	// Puts the count back to the start of a bar without stopping. What Start
	// does, and what a tempo change does not -- moving the tempo mid-bar
	// should change the spacing, not restart the count.
	void Rewind();

private:
	// Recomputed on the control thread whenever tempo or rate changes, and
	// read as one number by the audio thread. Not derived per block, which
	// would divide in the process callback for no reason.
	std::atomic<uint64_t> period_samples_{0};
	std::atomic<uint32_t> beat_samples_{0};

	std::atomic<float> bpm_{120.0f};
	std::atomic<unsigned> beats_per_bar_{4};
	std::atomic<float> level_{0.5f};
	std::atomic<bool> running_{false};
	std::atomic<bool> rewind_{false};

	std::atomic<unsigned> beat_{0};

	double sample_rate_ = 48000.0;

	// Audio thread only.
	uint64_t position_ = 0;   // samples since the current beat started
	unsigned counted_ = 0;    // which beat of the bar that is
	double phase_ = 0.0;

	void Recompute();
};

}  // namespace sf
