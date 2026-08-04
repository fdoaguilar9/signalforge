#pragma once

#include <string>

#include "imgui.h"

namespace sf
{

// The design's typefaces and type scale (SF-61).
//
// Two faces do the whole program. Space Grotesk carries the interface; IBM
// Plex Mono carries **every numeric or technical readout** -- a dB figure, a
// stage code, a MIDI message, a file path, a section label. That split is not
// decoration: a proportional font makes a column of numbers ragged, and a
// meter reading that changes width as it changes value is one a player has to
// stop and read rather than glance at.
//
// Both are substitutions chosen by the design rather than anything the program
// had before, and both are SIL OFL, vendored under `resources/fonts/` with
// their licences.
//
// **The design asks for weight 600 and Space Grotesk has no such static
// instance** -- upstream ships Light, Regular, Medium and Bold, plus a
// variable font that ImGui's stb_truetype backend cannot instance. Weight 600
// is therefore Medium (500) here. Bold (700) is the other candidate and is
// too heavy at 84px, which is the size that decides it.

// The scale, in unscaled pixels, exactly as `tokens/typography.css` states it.
// A size that isn't on this list is a size somebody guessed at.
namespace text
{

constexpr float k2xs = 11.0f;  // section labels, sublabels, captions
constexpr float kXs = 12.0f;   // stage codes, small readouts
constexpr float kSm = 13.0f;   // body -- the default
constexpr float kBase = 14.0f;
constexpr float kMd = 16.0f;  // the wordmark
constexpr float kLg = 20.0f;  // Live preset tiles, the metronome's BPM
constexpr float kXl = 26.0f;
constexpr float k2xl = 34.0f;
constexpr float k3xl = 46.0f;

// The two display sizes, which are on no scale because there is one use of
// each: the tuner's note, and the Live screen's preset name read from across
// a stage.
constexpr float kTunerNote = 72.0f;
constexpr float kLivePreset = 84.0f;

}  // namespace text

enum class Face
{
	Ui,        // Space Grotesk Regular
	UiMedium,  // Space Grotesk Medium -- the design's weight 600
	Mono       // IBM Plex Mono Regular
};

namespace fonts
{

// Loads the three faces into the atlas. Must be called after the ImGui
// context exists and before the first frame.
//
// Returns false when a file was missing, in which case **the program keeps
// going on ImGui's bundled bitmap font** and `warning` says which file. A
// board with no `resources/` -- a Pi image built from the binary alone -- has
// to boot and make noise; ugly type is not a reason to refuse to open a
// window.
bool Load(std::string& warning);

// The face, or null when Load fell back. Null is what ImGui's PushFont takes
// to mean "keep the current font", so a fallback build draws in the bundled
// font at the right *size* rather than not drawing at all.
ImFont* Get(Face face);

void Push(Face face, float size_px);
void Pop();

// Where the fonts were found, for the "could not find them" message to be
// able to say where it looked. Empty when nothing was found.
const std::string& ResourceDirectory();

}  // namespace fonts

// Scoped push/pop. Every readout in this design is a font change, and a
// hand-balanced Push/Pop across an early `continue` is a state leak that
// shows up three widgets later as everything being the wrong size.
struct ScopedFont
{
	ScopedFont(Face face, float size_px) { fonts::Push(face, size_px); }
	~ScopedFont() { fonts::Pop(); }

	ScopedFont(const ScopedFont&) = delete;
	ScopedFont& operator=(const ScopedFont&) = delete;
};

}  // namespace sf
