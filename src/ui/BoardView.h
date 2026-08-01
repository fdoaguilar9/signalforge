#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sf
{

// One entry in the preset list. Deliberately not PresetLibrary::Entry: the
// view needs two strings, not a parsed preset apiece, and keeping the whole
// library out of the drawing code means a redraw can never touch preset data
// that a switch is in the middle of replacing.
struct BoardPreset
{
	std::string name;  // the preset's own "name" field
	std::string stem;  // its filename, which is what the console commands take

	// The MIDI message that selects this preset, in words, or empty when
	// nothing is bound. Shown so the board can be read at a glance rather than
	// by opening the mapping file.
	std::string binding;
};

// One control port of one stage, with everything needed to draw the right
// widget for it. The hints matter: without them an on/off switch becomes a
// 0..1 float slider and a frequency control's useful range is squeezed into
// the first millimetre of a linear one.
struct BoardControl
{
	std::string symbol;  // what a preset file and a MIDI binding call it
	std::string name;    // what the plugin calls it, for the label

	float value = 0.0f;
	float minimum = 0.0f;
	float maximum = 1.0f;

	bool toggled = false;
	bool integer = false;
	bool logarithmic = false;

	// False for output control ports -- meters and readouts. Shown, because
	// they say what the plugin is doing, but never editable.
	bool writable = true;

	// The MIDI message that drives this control, in words, or empty.
	std::string binding;
};

// A file-valued parameter of a stage: which model or impulse response is
// loaded, and what the plugin will accept in its place.
struct BoardPathProperty
{
	std::string uri;
	std::string label;
	std::string value;  // absolute path, empty when nothing is loaded

	// Lowercase extensions without the dot. Empty means the plugin didn't
	// say, which means show everything.
	std::vector<std::string> file_types;
};

// One stage of the running chain, as the view needs to show it.
//
// A snapshot rather than the live PluginStage for the same reason the preset
// list is flattened: it keeps the drawing code out of the audio objects, and
// it means the whole view can be exercised by a test with no plugins installed
// and no engine running.
struct BoardStage
{
	std::string name;  // the plugin's declared name
	std::string uri;   // the full URI, for the tooltip
	bool bypassed = false;

	std::vector<BoardControl> controls;
	std::vector<BoardPathProperty> paths;

	// The MIDI message that toggles this stage's bypass, in words, or empty.
	std::string bypass_binding;
};

// One row of the file picker. The listing is done by the caller, so the view
// never touches the filesystem -- which is what lets the whole picker be
// driven by a test with no files to find.
struct BoardFileEntry
{
	std::string name;
	bool is_directory = false;
};

// "No preset selected" / "nothing requested". Not an index into anything.
inline constexpr size_t kNoPreset = static_cast<size_t>(-1);

// The level meter's scale. The floor is where the bar bottoms out: below this
// a guitar signal is inaudible anyway, and spending scale on it would squash
// the part that matters into a sliver. The other two are where the bar
// changes colour -- -6 dBFS is the usual "getting hot" mark, and -1 is close
// enough to full scale that the next transient may not fit.
inline constexpr float kMeterFloorDb = -60.0f;
inline constexpr float kMeterWarnDb = -6.0f;
inline constexpr float kMeterHotDb = -1.0f;

// Where a level sits on that scale, always within 0..1.
//
// Pure and inline so the mapping can be asserted directly rather than only
// looked at: the interesting inputs are the ones that aren't ordinary
// numbers. Silence arrives as -INFINITY (log10 of zero), a clipping signal
// exceeds 0 dBFS, and a divide-by-zero upstream would hand this a NaN. Left
// unguarded, each becomes a fraction outside 0..1 -- which a drawing library
// may quietly saturate today and not tomorrow.
inline float MeterFraction(float db)
{
	if (!std::isfinite(db))
	{
		return 0.0f;
	}

	const float clamped = std::min(0.0f, std::max(kMeterFloorDb, db));

	return (clamped - kMeterFloorDb) / -kMeterFloorDb;
}

// One installed plugin, as the browser needs to show it. Built once from
// lilv; scanning the world is far too slow to do while drawing.
struct BoardPlugin
{
	std::string name;
	std::string uri;

	// Shown so a player can tell a mono pedal from a stereo one before
	// inserting it and finding out the chain won't build.
	unsigned audio_inputs = 0;
	unsigned audio_outputs = 0;
};

// The bits of view state that have to survive between frames: what the user
// has typed, and what is open. Owned by the caller, because BoardViewState is
// rebuilt from scratch every frame and this must not be.
struct BoardViewUi
{
	// A fixed buffer rather than std::string: Dear ImGui's text input wants
	// one, and vendoring its std::string helper for a search box that will
	// never hold a sentence isn't a trade worth making.
	char plugin_search[128] = {};

	// The open file picker: which stage and which of its file parameters, and
	// where in the filesystem it is looking. The directory is owned here
	// because it has to survive between frames; the caller reads it to decide
	// what to list.
	bool picker_open = false;
	size_t picker_stage = 0;
	std::string picker_property;
	std::string browse_directory;

	// Which stages have their controls showing, by index. Grown by the view as
	// needed. Indices shift when stages are reordered, so this is cosmetic
	// state that may follow the wrong row after a move -- a fair trade against
	// tracking identity for something the user can re-click.
	std::vector<bool> stage_expanded;
};

// Where one interactive widget ended up, recorded only when the view is asked
// to. Named ("preset:0", "bypass:1", "up:0") so a test can say what it means
// to click rather than where.
struct WidgetProbe
{
	std::string id;
	float x = 0.0f;
	float y = 0.0f;
	bool enabled = true;
};

// What a "learn" button was pressed for. The view deliberately doesn't know
// about MidiBinding or MidiLearn -- it names the thing to be bound, and the
// caller translates that into the MIDI layer's terms.
enum class LearnTarget
{
	None,
	Cancel,
	LoadPreset,
	NextPreset,
	PrevPreset,
	ToggleBypass,
	SetControl
};

struct LearnRequest
{
	LearnTarget target = LearnTarget::None;

	std::string preset;  // LoadPreset: the filename stem
	size_t stage = 0;    // ToggleBypass, SetControl
	std::string symbol;  // SetControl

	// SetControl: the port's real range, so a pedal sweeps the whole control
	// rather than the 0..1 a binding would otherwise default to.
	float minimum = 0.0f;
	float maximum = 1.0f;
};

// What the user asked to do to one stage of the running chain.
//
// Bypass is a different kind of thing from the other three and it shows here:
// it changes a flag on a running plugin, while moving or removing changes the
// shape of the chain and costs a rebuild. They share this enum because they
// share a row in the UI, not because they cost the same.
enum class StageEdit
{
	None,
	ToggleBypass,
	MoveUp,
	MoveDown,
	Remove
};

// Everything the board view reads, gathered once per frame by the caller.
//
// The view takes a snapshot rather than the program's live objects because the
// two are owned by different things: `chain` is replaced wholesale on a preset
// switch, and the rest is engine state that only the caller knows how to ask
// for. Passing a struct keeps the drawing code from reaching into either.
struct BoardViewState
{
	std::string preset_name;
	std::string preset_path;

	double sample_rate = 0.0;
	uint32_t block_size = 0;

	// Round-trip latency in milliseconds, as JACK reports it. The number a
	// player actually cares about -- it's the delay between the string and
	// the speaker, and it is what a buffer-size change is traded against.
	double latency_ms = 0.0;

	// Peak levels since the last frame, in dBFS, with 0 dBFS being full
	// scale. -INFINITY for silence.
	//
	// dB rather than raw magnitude because the useful part of a meter is all
	// squeezed into the top of a linear scale: the difference between a good
	// level and a clipping one is 0.5 to 1.0 linear, but a legible -6 to 0
	// in dB.
	float input_db = -INFINITY;
	float output_db = -INFINITY;

	// Whether the output has hit full scale recently enough to still be worth
	// shouting about. The caller decides how long "recently" is -- the engine
	// only counts clipped blocks, and how long a warning should stay up is a
	// UI judgement, not an audio one.
	bool output_clipping = false;

	uint64_t midi_drops = 0;
	size_t midi_bindings = 0;

	// The switchable library, in stomp order, and where in it we are.
	// Borrowed: the caller owns the vector and it outlives the call.
	const std::vector<BoardPreset>* presets = nullptr;

	// kNoPreset when the running preset didn't come from the library (it was
	// given as a path), in which case nothing in the list is highlighted --
	// better than highlighting something that isn't playing.
	size_t current_preset = kNoPreset;

	// The running chain, in signal order. Borrowed; rebuilt by the caller each
	// frame so bypass state is never stale.
	const std::vector<BoardStage>* stages = nullptr;

	// Set by the view when the user clicks a preset. The caller performs the
	// switch and is free to refuse; the list highlights `current_preset`, so a
	// failed switch corrects itself on the next frame rather than leaving the
	// selection lying about what's playing.
	size_t requested_preset = kNoPreset;

	// Whether the running chain differs from the preset file on disk.
	bool unsaved_changes = false;

	// One line of feedback from the last action -- what was saved, or why a
	// reorder was refused. The caller owns the text and how long it lives.
	std::string status;

	// A condition rather than an event: the running board is not what the
	// preset file says, because some of it couldn't be built (SF-31). Drawn at
	// the top and it stays there, unlike `status`, which is about the last
	// thing the user did. Empty when the board is intact.
	std::string warning;

	// Why saving is refused right now, or empty when it's allowed. Non-empty
	// disables the button and explains itself on hover -- a "Save" that
	// silently writes a file missing half your board is worse than one you
	// can't press.
	std::string save_blocked_reason;

	// What the user asked to do to a stage this frame, and which one. At most
	// one edit per frame: each costs either a rebuild or a visible change, and
	// batching them would make a failure impossible to attribute.
	StageEdit stage_edit = StageEdit::None;
	size_t stage_edit_index = kNoPreset;

	bool save_requested = false;

	// What the caller found in `ui.browse_directory`. Borrowed.
	const std::vector<BoardFileEntry>* file_entries = nullptr;

	// The picker wants to move: a subdirectory name, or "..". Resolved by the
	// caller, which is the side that has a filesystem.
	std::string browse_request;

	// A file the user chose, for one stage's file parameter. Absolute.
	size_t path_stage = kNoPreset;
	std::string path_property;
	std::string path_file;

	// Bindings for the two library-wide actions, in words, or empty.
	std::string next_binding;
	std::string prev_binding;

	// Whether learn is armed, and what for -- so the window can say "press
	// something" rather than leaving the user guessing why nothing happened.
	bool learning = false;
	std::string learning_what;

	// A learn button pressed this frame.
	LearnRequest learn_request;

	// A control the user moved this frame: which stage, which port, and the
	// value it now reads. Applied to the running plugin, not to a file --
	// there is no rebuild and no crossfade, so it is audible immediately.
	size_t control_stage = kNoPreset;
	std::string control_symbol;
	float control_value = 0.0f;

	// Every installed plugin that can sit in a chain, for the browser.
	// Borrowed; built once by the caller.
	const std::vector<BoardPlugin>* plugins = nullptr;

	// How many installed plugins were left out because they have no audio
	// path, so the browser can account for them rather than appear incomplete.
	size_t plugins_hidden = 0;

	// Index into `plugins` of one the user chose to append to the chain.
	size_t requested_plugin = kNoPreset;

	// Set by the view when the user asks to stop.
	bool quit_requested = false;

	// SF-37: set when "Audio Settings..." is picked from the menu bar. The
	// menu bar is the first entry in what will grow into a general app menu;
	// this is its only item so far.
	bool audio_settings_requested = false;

	// Testing aid, off by default. When set, the view records where each
	// interactive widget landed and whether it was clickable, so the self-test
	// can drive real widgets by name instead of hard-coding coordinates that
	// any layout change would silently invalidate.
	bool record_widgets = false;
	std::vector<WidgetProbe> widgets;
};

// Draws one frame. Must be called between AppWindow::BeginFrame and EndFrame.
void DrawBoard(BoardViewState& state, BoardViewUi& ui);

}  // namespace sf
