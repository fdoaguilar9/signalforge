#pragma once

#include <cstdint>
#include <string>

namespace sf
{

// How the two halves of a switch are mixed while one chain hands over to the
// next.
//
// EqualPower is the default because the two signals are *not* the same sound:
// a clean amp and a fuzz share almost nothing, so their sum is closer to
// uncorrelated than to coherent, and uncorrelated signals add in power. Gains
// that follow cos/sin keep from^2 + to^2 constant, so the loudness holds
// steady across the fade.
//
// Linear is here because it is right for the other case -- two takes of nearly
// the same sound, where the signals add in amplitude and cos/sin would bulge
// about 3dB in the middle. Both are one line apart, and which one sounds right
// is a question about the material, not about the code, so it is a setting.
enum class CrossfadeCurve
{
	EqualPower,
	Linear
};

const char* ToString(CrossfadeCurve curve);
bool ParseCrossfadeCurve(const std::string& text, CrossfadeCurve& out);

// The gain ramp of one switch, in samples.
//
// Deliberately knows nothing about JACK, plugins or chains: it is the arithmetic
// of the fade and nothing else, which is what lets it be measured offline
// rather than only heard. JackEngine owns one and drives it a block at a time.
class Crossfade
{
public:
	// Long enough not to click, short enough that a stomp still feels
	// immediate. See --tune-crossfade for how this number was arrived at, and
	// PLAN.md/TASKS.md SF-33 for the by-ear pass that confirmed it.
	static constexpr double kDefaultMilliseconds = 12.0;

	// A fade longer than this stops being a switch and starts being an
	// effect -- and both chains run for its whole length, so it is also
	// double CPU. Zero is allowed and means "swap at the block boundary".
	static constexpr double kMaxMilliseconds = 250.0;

	static double ClampMilliseconds(double milliseconds);

	// Safe to call again on an already-configured fade; it takes effect on the
	// next Restart(). Not audio-thread safe.
	void Configure(double sample_rate, double milliseconds, CrossfadeCurve curve);

	// Puts the fade back at its start. Call before publishing an incoming
	// chain, never during one.
	void Restart();

	// The gains at `position` (0 = all outgoing, 1 = all incoming). Static and
	// public because it is the whole claim this class makes, and a test should
	// be able to interrogate it directly.
	static void Gains(float position, CrossfadeCurve curve, float& from_gain, float& to_gain);

	// Mixes one channel of one block, starting from the current position and
	// *without* advancing it, so every channel of a block walks the same ramp
	// from the same place and the channels stay phase-coherent. Returns the
	// position the ramp ended on.
	float MixChannel(float* out, const float* from, const float* to, uint32_t frames) const;

	// Moves the fade on by one block, once all its channels are mixed.
	void Advance(uint32_t frames);

	bool Done() const { return position_ >= 1.0f; }

	float Position() const { return position_; }
	double Milliseconds() const { return milliseconds_; }
	CrossfadeCurve Curve() const { return curve_; }

	// How many samples the fade spans at the configured rate. Zero when the
	// fade is instant.
	uint64_t LengthInSamples() const { return length_samples_; }

private:
	CrossfadeCurve curve_ = CrossfadeCurve::EqualPower;
	double milliseconds_ = kDefaultMilliseconds;

	uint64_t length_samples_ = 0;

	float position_ = 0.0f;

	// Per-sample increment. 1.0 means "finish inside the first sample", which
	// is what a zero-length fade is.
	float step_ = 1.0f;
};

}  // namespace sf
