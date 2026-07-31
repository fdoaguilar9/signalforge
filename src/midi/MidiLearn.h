#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MidiEvent.h"
#include "MidiMap.h"

namespace sf
{

// "Press the thing you want to use for this." Arms on a target (an action
// plus its parameters), then turns the next deliberate MIDI message into a
// binding for it.
//
// The hard part is not listening -- it's deciding which message the user
// meant. Two things get in the way:
//
//   - A footswitch sends a press *and* a release. Learning from the release
//     would bind a value of 0, and the resulting binding would then fire on
//     the wrong edge.
//   - Controllers chatter. An expression pedal with a bad pot streams CC
//     messages continuously whether or not anyone touches it, and it will
//     always win a race against a human foot.
//
// So MidiLearn watches traffic continuously (Observe), and when armed it
// rejects any controller that was already streaming before the user was
// asked to press something.
class MidiLearn
{
public:
	// How much traffic from one controller, in the window before arming,
	// counts as "this thing is chattering on its own".
	static constexpr uint64_t kNoiseWindowMs = 3000;
	static constexpr size_t kNoiseCount = 4;

	enum class Result
	{
		Ignored,  // not a message this target can use (a release, a wrong type)
		Noisy,    // a controller that was already streaming before we armed
		Bound     // `out` is now a complete binding
	};

	// Feed every incoming event here, armed or not -- the noise history is
	// what makes arming reliable, and it has to predate the arm.
	void Observe(const MidiEvent& event, uint64_t now_ms);

	// `target` carries the action and its parameters (preset, stage, port,
	// range); its matcher fields are ignored and filled in by learning.
	void Arm(const MidiBinding& target, uint64_t now_ms);

	bool Armed() const { return armed_; }
	void Cancel() { armed_ = false; }

	// The target being learned, for prompts. Only meaningful while armed.
	const MidiBinding& Target() const { return target_; }

	// Offers one event to the armed learn. On Bound, `out` holds the finished
	// binding and the learn disarms itself.
	Result Offer(const MidiEvent& event, uint64_t now_ms, MidiBinding& out);

private:
	struct Traffic
	{
		MidiEventType type;
		int channel;
		int number;
		size_t count;
		uint64_t last_ms;
	};

	std::vector<Traffic> traffic_;

	bool armed_ = false;
	MidiBinding target_;

	// Which controllers were already streaming at the moment of arming.
	//
	// Decided *at* Arm() rather than looked up later, because the caller
	// Observe()s an event before offering it -- so by the time Offer() runs,
	// the offending controller's own last-seen time has already moved past
	// the arming instant, and any "how recently did this chatter" comparison
	// would be against a moving target.
	struct Source
	{
		MidiEventType type;
		int channel;
		int number;
	};

	std::vector<Source> chattering_;

	bool WasChattering(const MidiEvent& event) const;
};

// Parses a "learn ..." console command into the target binding, e.g.
//   learn next_preset
//   learn load_preset 02-rhythm
//   learn toggle_bypass 1
//   learn set_control 0 input_level -20 20
//
// Returns false with a reason in `error`. The matcher fields of `out` are
// left at their defaults; learning fills them in.
bool ParseLearnCommand(const std::string& arguments, MidiBinding& out, std::string& error);

}  // namespace sf
