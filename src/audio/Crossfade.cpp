#include "Crossfade.h"

#include <algorithm>
#include <cmath>

namespace sf
{

namespace
{

constexpr float kHalfPi = 1.57079632679489661923f;

}  // namespace

const char* ToString(CrossfadeCurve curve)
{
	switch (curve)
	{
		case CrossfadeCurve::EqualPower:
			return "equal-power";

		case CrossfadeCurve::Linear:
			return "linear";
	}

	return "equal-power";
}

bool ParseCrossfadeCurve(const std::string& text, CrossfadeCurve& out)
{
	// Both spellings of each, because "equal_power" is what a JSON key looks
	// like and "equal-power" is what a command line looks like, and being told
	// off for the wrong punctuation helps nobody.
	if (text == "equal-power" || text == "equal_power" || text == "equalpower")
	{
		out = CrossfadeCurve::EqualPower;
		return true;
	}

	if (text == "linear")
	{
		out = CrossfadeCurve::Linear;
		return true;
	}

	return false;
}

double Crossfade::ClampMilliseconds(double milliseconds)
{
	if (!(milliseconds > 0.0))
	{
		// Also catches NaN, which would otherwise turn the step into a NaN and
		// silence every switch from then on.
		return 0.0;
	}

	return std::min(milliseconds, kMaxMilliseconds);
}

void Crossfade::Configure(double sample_rate, double milliseconds, CrossfadeCurve curve)
{
	curve_ = curve;
	milliseconds_ = ClampMilliseconds(milliseconds);

	const double samples = sample_rate > 0.0 ? sample_rate * (milliseconds_ / 1000.0) : 0.0;

	length_samples_ = static_cast<uint64_t>(samples);

	// A fade of a sample or less is a swap. Saying so here means the audio
	// thread never divides by anything, and never has to special-case it.
	step_ = samples > 1.0 ? static_cast<float>(1.0 / samples) : 1.0f;

	position_ = 0.0f;
}

void Crossfade::Restart()
{
	position_ = 0.0f;
}

void Crossfade::Gains(float position, CrossfadeCurve curve, float& from_gain, float& to_gain)
{
	const float clamped = std::min(1.0f, std::max(0.0f, position));

	if (curve == CrossfadeCurve::Linear)
	{
		from_gain = 1.0f - clamped;
		to_gain = clamped;
		return;
	}

	const float angle = clamped * kHalfPi;
	from_gain = std::cos(angle);
	to_gain = std::sin(angle);
}

float Crossfade::MixChannel(float* out, const float* from, const float* to, uint32_t frames) const
{
	float position = position_;

	for (uint32_t i = 0; i < frames; ++i)
	{
		float from_gain = 0.0f;
		float to_gain = 0.0f;
		Gains(position, curve_, from_gain, to_gain);

		out[i] = from[i] * from_gain + to[i] * to_gain;

		position = std::min(1.0f, position + step_);
	}

	return position;
}

void Crossfade::Advance(uint32_t frames)
{
	// Computed rather than taken from MixChannel's return so that a fade with
	// no output channels at all still completes instead of hanging.
	position_ = std::min(1.0f, position_ + step_ * static_cast<float>(frames));
}

}  // namespace sf
