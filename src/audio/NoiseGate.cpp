#include "NoiseGate.h"

#include <algorithm>
#include <cmath>

#include "Gain.h"

namespace sf
{

namespace
{

// Fixed, and expressed in time so they mean the same thing at any sample rate.
// These are the numbers to argue with if the gate ever feels wrong.

// How fast the *gain* opens, as a full-scale ramp time. The detector itself is
// instantaneous (see Process): making both slow would attenuate the front of a
// picked note twice over, once waiting for the detector to charge and again
// waiting for the gain to rise.
constexpr float kAttackMs = 1.0f;

// How long the gate stays open after the signal falls below the close
// threshold.
//
// 50 ms is more than two periods of a drop-tuned low F# (46 Hz, 22 ms), which
// is the point: a peak detector's envelope ripples between waveform peaks, and
// if it dips below the close threshold *between peaks of a sustained note* the
// gate chatters at the note's own frequency.
//
// **This and the detector decay below are deliberately redundant**, and that
// was measured rather than assumed: shortening either one alone does not
// produce chatter, because the other still bridges the gap. It takes losing
// both. Two cheap mechanisms covering one another is the right trade for a
// failure that sounds like a broken amp -- but it does mean neither constant
// can be justified by a test on its own, and the test asserts the property
// rather than either mechanism.
constexpr uint32_t kHoldMs = 50;

// The gate opens at the threshold and closes 6 dB below it. Hold alone does not
// settle a signal sitting exactly on the threshold; two thresholds do.
constexpr float kHysteresisDb = 6.0f;

// How fast the detector falls. Fast enough to notice a palm mute, slow enough
// not to follow individual cycles -- at 10 ms the envelope of a 46 Hz note
// sags about 9 dB between peaks rather than collapsing, which is the other half
// of the chatter defence described above.
constexpr float kDetectorDecayMs = 10.0f;

// A one-pole high-pass on the *detector* signal only, never on the audio.
//
// Without it, a converter with a few LSB of DC offset holds |x| permanently
// above a low threshold and the gate never closes at all -- and the symptom is
// "the gate does nothing", which gets blamed on the threshold.
constexpr float kDetectorHighpassHz = 20.0f;

// Below this the gain is flushed to exactly zero.
//
// A multiplicative decay never actually reaches zero, so without this the board
// feeds a permanent denormal trickle into every downstream plugin. Nothing here
// sets FTZ/DAZ, and denormals on the audio thread are a 10-100x cost cliff, not
// a rounding curiosity.
constexpr float kSilenceFloor = 0.0001f;  // -80 dB

float OnePoleCoefficient(float milliseconds, double sample_rate)
{
	if (milliseconds <= 0.0f || sample_rate <= 0.0)
	{
		return 0.0f;
	}

	return static_cast<float>(std::exp(-1.0 / (sample_rate * milliseconds / 1000.0)));
}

}  // namespace

void NoiseGate::Reset(double sample_rate)
{
	sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;

	Recompute();

	// Open, not closed: the alternative fades the board in every time the audio
	// settings are applied.
	envelope_ = 0.0f;
	gain_ = 1.0f;
	held_ = hold_samples_.load(std::memory_order_relaxed);
	highpass_state_[0] = 0.0f;
	highpass_state_[1] = 0.0f;

	published_gain_.store(1.0f, std::memory_order_relaxed);
	processed_frames_.store(0, std::memory_order_relaxed);
}

void NoiseGate::SetEnabled(bool enabled)
{
	enabled_.store(enabled, std::memory_order_relaxed);
}

void NoiseGate::SetThresholdDb(float db)
{
	threshold_db_.store(std::min(std::max(db, kGateMinThresholdDb), kGateMaxThresholdDb), std::memory_order_relaxed);
	Recompute();
}

void NoiseGate::SetReleaseMs(float ms)
{
	release_ms_.store(std::min(std::max(ms, kGateMinReleaseMs), kGateMaxReleaseMs), std::memory_order_relaxed);
	Recompute();
}

void NoiseGate::Set(const NoiseGateSettings& settings)
{
	SetThresholdDb(settings.threshold_db);
	SetReleaseMs(settings.release_ms);
	SetEnabled(settings.enabled);
}

NoiseGateSettings NoiseGate::Settings() const
{
	NoiseGateSettings out;
	out.enabled = enabled_.load(std::memory_order_relaxed);
	out.threshold_db = threshold_db_.load(std::memory_order_relaxed);
	out.release_ms = release_ms_.load(std::memory_order_relaxed);

	return out;
}

void NoiseGate::Recompute()
{
	const float threshold = threshold_db_.load(std::memory_order_relaxed);

	open_level_.store(DbToLinear(threshold), std::memory_order_relaxed);
	close_level_.store(DbToLinear(threshold - kHysteresisDb), std::memory_order_relaxed);

	// The release is stated as "60 dB in release_ms", so the per-sample factor
	// is the one that loses 60 dB over that many samples. Stating it that way
	// is what makes the slider a promise a test can hold it to.
	const double samples = sample_rate_ * static_cast<double>(release_ms_.load(std::memory_order_relaxed)) / 1000.0;

	release_coefficient_.store(
		samples > 0.0 ? static_cast<float>(std::pow(10.0, -60.0 / (20.0 * samples))) : 0.0f,
		std::memory_order_relaxed);

	hold_samples_.store(
		static_cast<uint32_t>(sample_rate_ * static_cast<double>(kHoldMs) / 1000.0), std::memory_order_relaxed);

	// A rate, not a time-to-full: reopening from a half-closed gain then takes
	// proportionally less time, and there is no step when a player plays into
	// the middle of a release.
	const double attack_samples = sample_rate_ * static_cast<double>(kAttackMs) / 1000.0;
	attack_step_.store(attack_samples > 0.0 ? static_cast<float>(1.0 / attack_samples) : 1.0f,
		std::memory_order_relaxed);

	detector_decay_.store(OnePoleCoefficient(kDetectorDecayMs, sample_rate_), std::memory_order_relaxed);

	// The usual one-pole high-pass coefficient for a corner at 20 Hz.
	const double omega = 2.0 * 3.14159265358979323846 * kDetectorHighpassHz / sample_rate_;
	highpass_coefficient_.store(static_cast<float>(1.0 / (1.0 + omega)), std::memory_order_relaxed);
}

void NoiseGate::Process(float* const* channels, unsigned count, uint32_t frames)
{
	if (!channels || count == 0 || frames == 0)
	{
		return;
	}

	const bool enabled = enabled_.load(std::memory_order_relaxed);

	// Switching the gate off does **not** mean "pass the input through": if it
	// is shut when a preset changes, jumping the gain from silence to unity is
	// a click, and it lands exactly where the crossfade has just gone to the
	// trouble of avoiding one. Off means "release up to unity and stay there",
	// so this only short-circuits once it has arrived.
	if (!enabled && gain_ >= 1.0f)
	{
		published_gain_.store(1.0f, std::memory_order_relaxed);
		return;
	}

	const float open_level = open_level_.load(std::memory_order_relaxed);
	const float close_level = close_level_.load(std::memory_order_relaxed);
	const float release = release_coefficient_.load(std::memory_order_relaxed);
	const uint32_t hold = hold_samples_.load(std::memory_order_relaxed);
	const float attack = attack_step_.load(std::memory_order_relaxed);
	const float decay = detector_decay_.load(std::memory_order_relaxed);
	const float highpass = highpass_coefficient_.load(std::memory_order_relaxed);

	// One bad sample would otherwise wedge the gate shut forever: every
	// comparison against NaN is false, so the envelope stays NaN, the gate
	// closes, and nothing short of a Reset reopens it.
	if (!std::isfinite(envelope_))
	{
		envelope_ = 0.0f;
	}

	if (!std::isfinite(gain_))
	{
		gain_ = 1.0f;
	}

	// **The high-pass state has to be cleared too, and forgetting it was a real
	// bug the test caught.** Clearing the envelope alone is not enough: the
	// filter's own state stays NaN, so every following sample filters to NaN,
	// `level > envelope_` is false forever, and the envelope sits at zero with
	// the gate shut. The symptom is a board that goes permanently silent after
	// one bad sample, which is a far worse failure than the one bad sample.
	for (float& state : highpass_state_)
	{
		if (!std::isfinite(state))
		{
			state = 0.0f;
		}
	}

	const unsigned detector_channels = std::min(count, 2u);

	for (uint32_t i = 0; i < frames; ++i)
	{
		// The detector, across every channel, high-passed so a DC offset
		// cannot hold the gate open. The audio itself is never filtered.
		float level = 0.0f;

		for (unsigned c = 0; c < detector_channels; ++c)
		{
			const float sample = channels[c][i];
			const float filtered = highpass * (highpass_state_[c] + sample);

			highpass_state_[c] = filtered - sample;

			level = std::max(level, std::fabs(filtered));
		}

		// Instant attack on the detector: a picked note must not have to
		// charge this *and* raise the gain before it is heard.
		envelope_ = level > envelope_ ? level : envelope_ * decay;

		if (enabled)
		{
			if (envelope_ >= open_level)
			{
				held_ = hold;
			}
			else if (envelope_ >= close_level && held_ > 0)
			{
				// Between the two thresholds the hold keeps recharging, which
				// is what makes hysteresis and hold cooperate rather than race.
				held_ = hold;
			}
			else if (held_ > 0)
			{
				--held_;
			}
		}
		else
		{
			held_ = hold;
		}

		const bool opening = !enabled || held_ > 0;

		if (opening)
		{
			gain_ = std::min(1.0f, gain_ + attack);
		}
		else
		{
			gain_ *= release;

			// Flushed rather than left to decay forever: see kSilenceFloor.
			if (gain_ < kSilenceFloor)
			{
				gain_ = 0.0f;
			}
		}

		for (unsigned c = 0; c < count; ++c)
		{
			channels[c][i] *= gain_;
		}
	}

	published_gain_.store(gain_, std::memory_order_relaxed);
	processed_frames_.fetch_add(frames, std::memory_order_relaxed);
}

}  // namespace sf
