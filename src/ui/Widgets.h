#pragma once

#include <string>
#include <vector>

#include "imgui.h"

#include "Theme.h"

namespace sf
{

// The parts every screen is built from (SF-63).
//
// Dear ImGui gives one button, one selectable and one label; the design has
// three button weights, a two-line list row, a segmented control, a rail, and
// a section label with letter-spacing. Written once here rather than pushed
// and popped at each call site, so that "what does a ghost button look like"
// has one answer and changing it is one edit.
//
// Nothing here knows about presets, stages or the engine. These are shapes.

// Where one interactive widget ended up, recorded only when a test asks for
// it. Named ("preset:0", "bypass:1", "up:0") so a test can say what it means
// to click rather than where.
struct WidgetProbe
{
	std::string id;
	float x = 0.0f;
	float y = 0.0f;

	// The id Dear ImGui knows this widget by, recorded so a test can find two
	// widgets sharing one.
	//
	// Sharing an id is not automatically a dead button -- hover and press
	// resolve per rectangle, so two plain buttons with one id both still work.
	// What it breaks is id-*keyed* state: which popup is open, a slider's
	// value while it is being dragged, a tree node's openness, keyboard focus.
	// Those fail silently and in ways no screenshot shows, and by then the
	// collision is usually old.
	//
	// So the ids are compared directly, across every screen, whether or not
	// today's widget happens to get away with it.
	unsigned int imgui_id = 0;

	bool enabled = true;

	// For a widget that is on or off -- a checkbox, a toggle -- what it is
	// currently showing. Dear ImGui doesn't keep this where a test could read
	// it back, and a widget whose polarity is inverted from the flag it drives
	// (the bypass checkbox: ticked means *active*) is exactly the kind of
	// thing that silently flips back one day. False on everything else.
	bool checked = false;
};

// The probe sink, passed down into every widget that can be clicked.
//
// Null `into` -- which is every real frame -- makes Record a no-op, so the
// recording costs nothing when nobody is listening and no widget needs to ask
// whether it is being tested.
struct ProbeRecorder
{
	std::vector<WidgetProbe>* into = nullptr;

	// Records the item just drawn. Call immediately after it, while it is
	// still "the last item" as far as Dear ImGui is concerned. `checked` is
	// only meaningful for widgets that have an on and an off.
	void Record(const std::string& id, bool enabled, bool checked = false) const;

	explicit operator bool() const { return into != nullptr; }
};

namespace widgets
{

// --- rules ---
//
// 1px, snapped to the pixel grid. Not ImDrawList::AddLine, which
// anti-aliases: a horizontal rule whose coordinate isn't on a pixel centre
// gets spread across two rows, and because the spread depends on the
// coordinate, the same rule comes out 1px thick at one end and 2px at the
// other. On a design made almost entirely of hairlines that reads as a
// rendering fault.
void HairlineH(ImDrawList* draw, float x0, float x1, float y, ImU32 colour);
void HairlineV(ImDrawList* draw, float y0, float y1, float x, ImU32 colour);

// --- text ---

// Text with letter-spacing, which Dear ImGui has no notion of and which the
// design's mono labels all carry.
//
// Drawn one character at a time, so **ASCII only** -- every label that wants
// this is an uppercase word like `SIGNAL PATH`, and a multi-byte code point
// would be split into meaningless bytes. Anything else should use ordinary
// text.
void TextTracked(const char* text, const ImVec4& colour, float tracking_px);

// The 11px mono heading over every group in the sidebar and every card on the
// board: `BANK`, `PRESETS`, `SIGNAL PATH`, `CHANNEL A`.
void SectionLabel(const char* text, const ImVec4& colour = theme::kTextDisabled);

// A section label with a hairline running out from it to both edges: the
// `INPUT` / `SPLIT` / `MERGE` / `OUTPUT` markers that show where on the signal
// path you are.
void Rail(const char* label, const ImVec4& colour = theme::kTextDisabled);

// --- buttons ---
//
// Three weights, in order of how loudly they ask to be pressed. Primary is
// amber-on-dark and there is at most one on screen; ghost has no fill until
// it is pointed at and is what the header and the `Add stage` rows use.

bool PrimaryButton(const char* label);
bool SecondaryButton(const char* label);

// The same, at a size the caller states. The live screen's Prev/Next are
// full-width and 64px tall because they are aimed at with a foot, which is not
// a size any ordinary button wants.
bool SecondaryButtonSized(const char* label, const ImVec2& size);
bool GhostButton(const char* label);

// What a button of this label costs, for laying a row out from its right-hand
// end rather than by trial and error.
float ButtonWidth(const char* label);

// --- composites ---

// A row of mutually exclusive choices sharing one border: Board | Live, and
// `1 channel` | `2 channels`. Returns the index pressed, or -1.
//
// Drawn rather than assembled from buttons because the design gives the group
// one rounded outline with square divisions inside it, and ImGui rounds every
// button's four corners or none of them.
int SegmentedControl(const char* id, const char* const* options, int count, int selected,
	const ProbeRecorder& probes, const char* probe_prefix);

enum class BannerTone
{
	// The board is not what its preset says it is (SF-31).
	Warning,

	// A saved audio device that isn't plugged in (SF-39).
	Unavailable
};

// A bordered box that states a condition and stays until the condition goes.
// Wraps, and takes as much height as the text needs.
void StatusBanner(const char* id, BannerTone tone, const char* text);

// One row of a list: a name over a smaller, dimmer second line. Returns true
// when clicked.
//
// `sublabel` may be null, in which case the row is one line tall -- the file
// picker's entries have nothing to say underneath them, and a row padded out
// to two lines for consistency would just be a longer list to scroll.
bool ListRow(const char* label, const char* sublabel, bool selected);

// The "learn" affordance beside anything bindable: a button, and the message
// currently bound to it if there is one.
//
// Disabled while `armed` -- MidiLearn binds one thing at a time, and a second
// press would silently replace what you were about to press a switch for.
bool LearnButton(const ProbeRecorder& probes, const std::string& probe_id, const std::string& binding, bool armed);

// --- modals ---
//
// The scrim, radius and border come from the theme; what these add is one
// place that decides how wide a modal is and that Escape always closes one.

bool BeginModal(const char* title, float width);
void EndModal();

// True on the frame Escape was pressed inside a modal. The caller closes,
// rather than this doing it, because closing usually means undoing something
// as well -- a picker has state to clear.
bool ModalDismissed();

}  // namespace widgets
}  // namespace sf
