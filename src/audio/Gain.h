#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sf
{

// The board-wide trims' arithmetic (SF-66).
//
// Header-only and free of JACK, so the two claims worth making about a gain
// -- that it is the gain it says it is, and that changing it makes no
// discontinuity -- can be asserted directly rather than listened for. The
// engine calls exactly these functions; there is no second copy in
// JackEngine.cpp for the test to drift from.

// dB to a linear multiplier. 0 dB is unity, +6 is roughly double, -6 roughly
// half.
inline float DbToLinear(float db)
{
	return std::pow(10.0f, db / 20.0f);
}

// Copies `source` to `destination` scaled by a gain that walks linearly from
// `from` to `to` across the block. The two may be the same buffer.
//
// The ramp is what makes a slider drag silent. A trim applied as a single
// multiplier per block steps the waveform at every block boundary the value
// changes on, and at 128 frames that is a click 375 times a second while the
// mouse is moving. Linear rather than equal-power because this is one signal
// being scaled, not two being crossfaded.
//
// **The last sample of a block is at `to - step`, not at `to`.** That is what
// makes the ramp continue smoothly into the next block, which starts at `to`:
// the increment across the seam is the same `step` as the increments inside
// it. Ending the block exactly at `to` would double up that value and put a
// small flat spot at every boundary.
//
// Audio thread: no allocation, and no per-sample branching beyond the
// arithmetic.
inline void RampGain(float* destination, const float* source, uint32_t frames, float from, float to)
{
	if (frames == 0)
	{
		return;
	}

	if (from == to)
	{
		// The overwhelmingly common case -- nobody is touching the slider --
		// and worth keeping off the per-sample interpolation.
		if (to == 1.0f)
		{
			if (destination != source)
			{
				std::memcpy(destination, source, frames * sizeof(float));
			}

			return;
		}

		for (uint32_t i = 0; i < frames; ++i)
		{
			destination[i] = source[i] * to;
		}

		return;
	}

	const float step = (to - from) / static_cast<float>(frames);
	float gain = from;

	for (uint32_t i = 0; i < frames; ++i)
	{
		destination[i] = source[i] * gain;
		gain += step;
	}
}

}  // namespace sf
