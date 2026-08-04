#include "Metronome.h"

#include <algorithm>
#include <cmath>

namespace sf
{

namespace
{

// The range the design's tempo control offers. Clamped rather than refused:
// this is fed by a slider and by a MIDI binding, and neither can produce
// anything a person meant by it.
constexpr float kMinBpm = 40.0f;
constexpr float kMaxBpm = 240.0f;

constexpr double kPi = 3.14159265358979323846;

}  // namespace

void Metronome::SetSampleRate(double sample_rate)
{
	sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
	Recompute();
}

void Metronome::SetTempo(float bpm)
{
	bpm_.store(std::min(std::max(bpm, kMinBpm), kMaxBpm), std::memory_order_relaxed);
	Recompute();
}

void Metronome::SetBeatsPerBar(unsigned beats)
{
	beats_per_bar_.store(std::max(1u, beats), std::memory_order_relaxed);
}

void Metronome::SetLevel(float level)
{
	level_.store(std::min(std::max(level, 0.0f), 1.0f), std::memory_order_relaxed);
}

void Metronome::SetRunning(bool running)
{
	// Starting rewinds; stopping does not. A metronome that resumed mid-bar
	// after a stop would be counting from somewhere nobody chose.
	if (running && !running_.load(std::memory_order_relaxed))
	{
		Rewind();
	}

	running_.store(running, std::memory_order_relaxed);
}

void Metronome::Rewind()
{
	// Asked for rather than done: `position_` and `counted_` belong to the
	// audio thread, and writing them from here would be writing them while it
	// reads them.
	rewind_.store(true, std::memory_order_relaxed);
}

void Metronome::Recompute()
{
	const double bpm = static_cast<double>(bpm_.load(std::memory_order_relaxed));

	// Rounded, not truncated: at 48 kHz and 120 BPM this is exactly 24000, and
	// a tempo whose period is not a whole number of samples should land on the
	// nearer sample rather than always the earlier one.
	const double period = sample_rate_ * 60.0 / bpm;

	period_samples_.store(static_cast<uint64_t>(period + 0.5), std::memory_order_relaxed);
	beat_samples_.store(
		static_cast<uint32_t>(sample_rate_ * static_cast<double>(kMetronomeBeatSeconds)), std::memory_order_relaxed);
}

uint64_t Metronome::SamplesToNextBeat() const
{
	const uint64_t period = period_samples_.load(std::memory_order_relaxed);

	return period == 0 ? 0 : period - (position_ % period);
}

void Metronome::Mix(float* left, float* right, uint32_t frames)
{
	if (!running_.load(std::memory_order_relaxed))
	{
		return;
	}

	const uint64_t period = period_samples_.load(std::memory_order_relaxed);

	if (period == 0 || !left)
	{
		return;
	}

	if (rewind_.exchange(false, std::memory_order_relaxed))
	{
		position_ = 0;
		counted_ = 0;
		phase_ = 0.0;
		beat_.store(0, std::memory_order_relaxed);
	}

	const uint32_t beat_length = beat_samples_.load(std::memory_order_relaxed);
	const float level = level_.load(std::memory_order_relaxed);
	const unsigned per_bar = beats_per_bar_.load(std::memory_order_relaxed);

	for (uint32_t i = 0; i < frames; ++i)
	{
		// The beat boundary, checked per sample. A metronome that only looked
		// at block boundaries would quantise every click to the buffer size --
		// at 256 samples that is 5 ms of jitter, which is audible as sloppiness
		// against anything being played to it.
		if (position_ >= period)
		{
			position_ -= period;
			counted_ = per_bar == 0 ? 0 : (counted_ + 1) % per_bar;
			phase_ = 0.0;

			beat_.store(counted_, std::memory_order_relaxed);
		}

		if (position_ < beat_length && level > 0.0f)
		{
			const double frequency = counted_ == 0 ? kMetronomeDownbeatHz : kMetronomeBeatHz;

			// Linear decay across the burst, so it reads as a tick rather than
			// as a tone that stops.
			const float envelope = 1.0f - static_cast<float>(position_) / static_cast<float>(beat_length);
			const float sample = level * envelope * static_cast<float>(std::sin(phase_)) * 0.5f;

			left[i] += sample;

			if (right)
			{
				right[i] += sample;
			}

			phase_ += 2.0 * kPi * frequency / sample_rate_;

			if (phase_ > 2.0 * kPi)
			{
				phase_ -= 2.0 * kPi;
			}
		}

		++position_;
	}
}

}  // namespace sf
