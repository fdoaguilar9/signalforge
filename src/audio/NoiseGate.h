#pragma once

#include <atomic>
#include <cstdint>

namespace sf

{

// The board's built-in noise gate.
//
// It sits at the input, after the board-wide trim and before the first stage,
// which is where a gate belongs: gating *before* any distortion is what makes
// one effective, since a high-gain amp model turns a noise floor into a roar
// and no gate after it can put that back.
//
// **The settings are per preset**, not board-wide: a high-gain lead needs to
// gate hard and a clean tone should not gate at all, and one threshold cannot
// serve both. The engine is only a sink for whichever preset's numbers are
// current -- see the note on ownership in JackEngine.
//
// JACK-free and preset-free on purpose, like Gain.h and Metronome: the claims
// worth making about a gate are arithmetic ("it reaches silence in the release
// time it promised", "it does not chatter on a low note") and they are far
// better asserted than listened for. There is no second copy of this in
// JackEngine.cpp for a test to drift from.
//
// **What this is not.** No lookahead, so the first millisecond below the
// threshold always gets through -- the alternative is latency on the whole
// board, which is a worse trade for a guitar rig. No soft knee, no depth
// control, no unlinking the channels. And it cannot do what a four-cable-method
// gate does, which is sit *after* a high-gain preamp; that is a stage, not
// this.

// What a preset stores. Defaults are deliberately "changes nothing": a preset
// written before the gate existed parses into exactly this and must sound
// identical, so `enabled` starts false.
struct NoiseGateSettings
{
	bool enabled = false;

	// Where the gate opens, in dBFS, measured *after* the input trim -- the
	// same domain the input meter reads in, so the number you set and the
	// number you watch cannot disagree. The corollary is worth knowing: moving
	// the global input trim moves every preset's gate with it.
	float threshold_db = -50.0f;

	// How long the gain takes to fall 60 dB once the gate closes.
	float release_ms = 200.0f;

	bool operator==(const NoiseGateSettings& other) const
	{
		return enabled == other.enabled && threshold_db == other.threshold_db && release_ms == other.release_ms;
	}

	bool operator!=(const NoiseGateSettings& other) const { return !(*this == other); }
};

// The ranges the UI offers and the file is clamped into. -70 dB is below any
// real interface's noise floor and -20 would gate a quiet chord, which brackets
// every useful setting; 20 ms is tight enough for palm-muted chugs and 2 s long
// enough to leave a natural decay alone.
inline constexpr float kGateMinThresholdDb = -70.0f;
inline constexpr float kGateMaxThresholdDb = -20.0f;
inline constexpr float kGateMinReleaseMs = 20.0f;
inline constexpr float kGateMaxReleaseMs = 2000.0f;

class NoiseGate
{
public:
	// Control thread. Recomputes coefficients and clears runtime state; it
	// does **not** clear the settings, so restarting the engine at a new sample
	// rate keeps the player's numbers.
	//
	// Leaves the gate *open*. Starting closed would fade the board in every
	// time the audio settings were applied.
	void Reset(double sample_rate);

	// Control thread. Values outside the ranges above are clamped rather than
	// refused: these are fed by a slider and by a file a person may have
	// edited, and neither can produce a number nobody meant.
	void SetEnabled(bool enabled);
	void SetThresholdDb(float db);
	void SetReleaseMs(float ms);

	void Set(const NoiseGateSettings& settings);
	NoiseGateSettings Settings() const;

	// The gain the gate is currently applying, 0..1, for the indicator. A
	// meter, not a setting -- this is the one thing the UI reads back *from*
	// the engine rather than from the preset.
	float Gain() const { return published_gain_.load(std::memory_order_relaxed); }

	// How many frames have actually been through the per-sample loop. Exists
	// so a test can tell "returned early" from "multiplied by 1.0f": both are
	// bit-identical, and only one of them is the claim that a disabled gate
	// costs nothing.
	uint64_t ProcessedFrames() const { return processed_frames_.load(std::memory_order_relaxed); }

	// Applies the gate in place across `count` channels of `frames` samples.
	//
	// **One linked gain for every channel.** A per-channel gate collapses a
	// stereo image the moment one side drops below the threshold, which is a
	// worse artefact than the noise it removed.
	//
	// Audio thread: no allocation, no locks, and every parameter read once at
	// the top of the block.
	void Process(float* const* channels, unsigned count, uint32_t frames);

private:
	void Recompute();

	// Raw settings, written by the control thread. Stored raw and converted in
	// Recompute so the audio thread never calls pow() -- the same division of
	// labour as SetInputGainDb.
	std::atomic<bool> enabled_{false};
	std::atomic<float> threshold_db_{-50.0f};
	std::atomic<float> release_ms_{200.0f};

	// Derived, and read by the audio thread. These are separate atomics, so a
	// block can briefly see one updated and another not -- which costs at most
	// one block of a slightly wrong threshold and is not worth a lock on the
	// audio thread. The same property Metronome's period/beat pair has.
	std::atomic<float> open_level_{0.0f};
	std::atomic<float> close_level_{0.0f};
	std::atomic<float> release_coefficient_{0.0f};
	std::atomic<uint32_t> hold_samples_{0};
	std::atomic<float> attack_step_{1.0f};
	std::atomic<float> detector_decay_{0.0f};
	std::atomic<float> highpass_coefficient_{0.0f};

	std::atomic<float> published_gain_{1.0f};
	std::atomic<uint64_t> processed_frames_{0};

	double sample_rate_ = 48000.0;

	// Audio thread only.
	float envelope_ = 0.0f;
	float gain_ = 1.0f;
	uint32_t held_ = 0;
	float highpass_state_[2] = {0.0f, 0.0f};
};

}  // namespace sf
