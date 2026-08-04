#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Meter.h"
#include "Widgets.h"

namespace sf
{

using BoardNodeId = uint32_t;

inline constexpr BoardNodeId kNoBoardNode = 0;

// "This node has no stage in the running chain" -- a plugin the preset names
// but that isn't installed, so there is nothing to show controls for.
inline constexpr size_t kNoStageIndex = static_cast<size_t>(-1);

struct BoardGraphNode;

using BoardGraphNodes = std::vector<BoardGraphNode>;

struct BoardGraphBranch
{
	BoardGraphNodes nodes;

	float gain_db = 0.0f;
	bool muted = false;
};

// The splitter's settings, in the view's own terms. The names match the file's
// so that a reader moving between the two isn't translating (SF-75).
enum class BoardSplitterMode
{
	Full,
	Crossover,
	Stereo
};

enum class BoardSplitterSource
{
	Left,
	Right,
	Sum
};

struct BoardSplitter
{
	BoardSplitterMode mode = BoardSplitterMode::Full;

	// Into each lane, before its stages. Grouped with the rest of what the
	// splitter does rather than sitting on the branch beside `gain_db`,
	// exactly as the file groups it: a send is what the splitter *sends*, and
	// the return on the branch is what the mixer takes back.
	float send_db[2] = {0.0f, 0.0f};

	float frequency = 800.0f;
	BoardSplitterSource source[2] = {BoardSplitterSource::Left, BoardSplitterSource::Right};
};

// The view's own copy of the board's shape, built by the caller each frame
// from the preset's graph. Deliberately not `ChainGraph` itself: the drawing
// code has no business holding state blobs and control maps, and a snapshot
// means a redraw can never touch preset data a switch is replacing.
struct BoardGraphNode
{
	BoardNodeId id = kNoBoardNode;

	bool split = false;

	// Plugin nodes: where to find it in BoardViewState::stages, or
	// kNoStageIndex when it isn't in the running chain.
	size_t stage = kNoStageIndex;

	// Split nodes: exactly two, lane 0 drawn above the trunk line.
	std::vector<BoardGraphBranch> branches;

	// Split nodes only.
	BoardSplitter splitter;
};

// The board's noise gate, as the card needs it (SF-85).
//
// The settings come from the *preset*, not from the engine -- the preset is
// where they live, and reading them back from the engine would be reading a
// copy. `gain` is the exception and is the one thing that genuinely comes from
// the engine: it is a meter, not a setting.
struct BoardGate
{
	bool enabled = false;
	float threshold_db = -50.0f;
	float release_ms = 200.0f;

	// What the gate is currently doing, 0..1. Shown because a threshold set
	// without seeing when it bites is set by listening for the exact thing the
	// gate exists to remove.
	float gain = 1.0f;
};

// What the tuner has to show (SF-81). Already resolved into a note and a
// distance from it: which note a frequency is, is arithmetic, and arithmetic
// in the drawing code is arithmetic nothing can test.
struct BoardTuner
{
	// Empty when there is nothing to name. The display holds its last reading
	// rather than blanking between notes -- a tuner that flickered every time
	// a string decayed would be unusable at exactly the moment it is used.
	std::string note;

	float frequency_hz = 0.0f;
	float cents = 0.0f;

	// True once anything has been heard at all, so the modal can say "play a
	// string" rather than showing a confident A4 at 0 cents that is really
	// just an uninitialised struct.
	bool heard = false;
};

// What the metronome is doing (SF-82). A reading, not the thing itself: the
// click runs on the audio thread and the view never touches it.
struct BoardMetronome
{
	bool running = false;
	float tempo = 120.0f;
	unsigned beats_per_bar = 4;
	float level = 0.5f;

	// Which beat of the bar is sounding, for the row of dots.
	unsigned beat = 0;

	// What starts and stops it, in words, or empty.
	std::string binding;
	std::string tempo_binding;
};

// One row of the MIDI mappings table (SF-78), already in words. The view does
// no formatting of its own here: what a binding *means* is the MIDI layer's
// vocabulary, and a second translation living in the drawing code is a second
// thing to drift.
struct BoardMidiBinding
{
	std::string message;  // "CC 80 ch2" -- what arrives
	std::string action;   // "set_control" -- the action's own name
	std::string target;   // "stage 1 'gain'", or a preset name, or empty
	std::string range;    // "0..1", or empty where the action takes no value
};

// Where an insertion lands. Mirrors the engine's GraphPosition, in the view's
// own terms: `split == kNoBoardNode` means the trunk.
struct BoardSlot
{
	BoardNodeId split = kNoBoardNode;
	unsigned branch = 0;
	size_t index = 0;

	bool operator==(const BoardSlot& other) const
	{
		return split == other.split && branch == other.branch && index == other.index;
	}
};


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

	// Whether the plugin ships an editor of its own (SF-79). Decides whether
	// the card offers `Open plugin UI` -- a button that opened nothing would
	// be worse than no button, and most plugins have no editor to mention.
	bool has_custom_ui = false;
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

	// Which screen is showing (SF-77). A UI fact and nothing more: the board
	// and the live screen are two views of one running chain, and switching
	// between them changes what is drawn and nothing about what is playing.
	bool live_screen = false;

	// Whether the MIDI mappings modal is showing (SF-78). Purely a UI fact --
	// the table is read-only and derived, so opening it changes nothing about
	// the board and nothing about the preset.
	bool midi_map_open = false;

	// Whether the gate card is expanded (SF-85). UI only -- collapsing a card
	// is not a preset edit.
	//
	// **Closed to start with, and so are the splitter and the mixer.** Three
	// open cards push the pedals -- which are what the board is actually about
	// -- a long way down the page, and on a split board the chain area opens
	// with more settings than signal path visible. Each card's header says its
	// current setting when it is shut (`off`, `crossover at 800 Hz`,
	// `A 0.0 dB / B 0.0 dB`), so what closing costs is the ability to *change*
	// them at a glance, not the ability to read them.
	bool gate_open = false;

	// Whether the metronome modal is showing (SF-82). UI only -- unlike the
	// tuner, opening this changes nothing about the board; the click is
	// started by its own button, not by the modal appearing.
	bool metronome_open = false;

	// Whether the tuner modal is showing (SF-81). Unlike the other modals this
	// one is *not* purely a UI fact -- the board is muted while it is open --
	// so the caller watches it and mutes accordingly.
	bool tuner_open = false;

	// The plugin-UI modal (SF-79) and which stage it is showing. kNoPreset
	// when it is closed, which is also what makes "closed" and "showing stage
	// 0" different states rather than one ambiguous zero.
	size_t plugin_ui_stage = kNoPreset;

	// The slider currently being dragged, and what it has been dragged to
	// (SF-75).
	//
	// A slider that reports on release has to remember this itself. Dear ImGui
	// writes the dragged value into the caller's variable while the item is
	// *active*, and on the frame it is released it writes nothing -- so a
	// control that read its value back on deactivation would read the value it
	// started from and report that the player had changed nothing.
	//
	// It doubles as what the slider *shows* mid-drag, which is the behaviour
	// these controls want anyway: the number follows the cursor, and the sound
	// catches up when it is let go.
	std::string dragging;
	float dragging_value = 0.0f;

	// Which cards have their controls showing, **by node id** (SF-70).
	//
	// Not by index, which is what this was when there was one expander per row
	// and its own comment admitted followed the wrong row after a move. With
	// every card expandable at once rather than one selected stage, that stops
	// being cosmetic: expand three pedals, move the first, and two of the three
	// would close while an unrelated one opened.
	std::vector<BoardNodeId> expanded_nodes;

	bool IsExpanded(BoardNodeId node) const
	{
		return std::find(expanded_nodes.begin(), expanded_nodes.end(), node) != expanded_nodes.end();
	}

	void SetExpanded(BoardNodeId node, bool open)
	{
		const auto found = std::find(expanded_nodes.begin(), expanded_nodes.end(), node);

		if (open && found == expanded_nodes.end())
		{
			expanded_nodes.push_back(node);
		}
		else if (!open && found != expanded_nodes.end())
		{
			expanded_nodes.erase(found);
		}
	}

	// Whether the splitter's and the mixer's controls are showing. UI-only:
	// collapsing a card does not dirty the preset.
	//
	// **Closed to start with**, like the gate. See the note there.
	bool splitter_open = false;
	bool mixer_open = false;

	// Where a plugin picked in the browser will be inserted. Set when a "+"
	// on the board is pressed, so the browser inserts *there* rather than
	// always appending.
	BoardSlot pending_slot;
	bool pending_slot_valid = false;

	// Set by the graph when a "+" is pressed. The browser popup is opened by
	// the code that draws it, which is at window level -- Dear ImGui wants a
	// popup opened in the same ID scope it lives in.
	bool open_browser = false;

	// The peak-hold markers. Owned here rather than computed per frame
	// because a peak hold *is* a memory: the engine reports what the last
	// block did, and how long that is worth still showing is a decision the
	// view makes.
	MeterState input_meter;
	MeterState output_meter;
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
	SetControl,

	// The board-wide trims (SF-66/SF-68). No stage and no symbol: there is one
	// of each, and they are not in the chain.
	SetInputGain,
	SetOutputGain,

	// Stepping banks (SF-67). The design draws only a "next bank" button, so
	// that is the only one with a home on screen -- prev_bank exists in the
	// mapping file for anyone with a switch to spare.
	NextBank,

	// A lane's level into the merge (SF-72). `symbol` is "a" or "b" -- the
	// lane, not a split, because a binding outlives the preset it was made on
	// and a node id would mean a different node in the next one.
	SetMixerLevel,

	// The metronome (SF-82). Neither takes a stage or a symbol: there is one
	// metronome and it is not in the chain.
	SetTempo,
	ToggleMetronome
};

struct LearnRequest
{
	LearnTarget target = LearnTarget::None;

	std::string preset;  // LoadPreset: the filename stem
	size_t stage = 0;    // ToggleBypass, SetControl
	std::string symbol;  // SetControl: the port; SetMixerLevel: "a" or "b"

	// SetControl: the port's real range, so a pedal sweeps the whole control
	// rather than the 0..1 a binding would otherwise default to.
	float minimum = 0.0f;
	float maximum = 1.0f;
};

// What the user asked to do to the *shape* of the board (SF-55).
//
// Separate from StageEdit, which addresses a stage by its position in the
// running chain. These address a node by identity, or a gap by where it is,
// because that is what survives the edit: inserting a pedal renumbers every
// position after it, and a request that named a position would then mean
// something else by the time it was applied.
enum class GraphEdit
{
	None,
	InsertPlugin,  // at `graph_edit_slot`, the plugin chosen in the browser
	InsertSplit,   // at `graph_edit_slot`
	Remove,        // `graph_edit_node`
	MoveEarlier,   // `graph_edit_node`, within its own lane
	MoveLater,
	Rejoin,        // `graph_edit_node`, a split: lane 0 then lane 1 into the trunk
	ToggleMute,    // `graph_edit_node` split, lane `graph_edit_branch`
	SetLaneGain,   // ...and `graph_edit_gain_db`

	// The whole splitter of `graph_edit_node`, replaced by
	// `graph_edit_splitter` (SF-75). One edit rather than one per field
	// because every one of them is planned into the chain and so costs the
	// same rebuild -- and because a mode and the setting it makes meaningful
	// are often chosen in the same gesture.
	SetSplitter
};

// What the user asked to do to one stage of the running chain.
//
// Bypass is a different kind of thing from the other three and it shows here:
// it changes a flag on a running plugin, while moving or removing changes the
// shape of the chain and costs a rebuild. They share this enum because they
// share a row in the UI, not because they cost the same.
// Now only the one thing: bypass changes a flag on a running plugin and is
// audible on the next block. Moving and removing change the *shape* of the
// board, cost a rebuild, and address nodes by identity rather than position
// -- they went to GraphEdit above when the chain stopped being a list.
enum class StageEdit
{
	None,
	ToggleBypass
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

	// The bank the running preset belongs to, for the header's breadcrumb.
	// Empty when there is no library at all, in which case the breadcrumb is
	// just the preset -- a `›` with nothing on its left would be furniture
	// pretending to be information.
	std::string bank_name;

	// Every bank, in stomp order, and which one is showing (SF-67). Borrowed;
	// the caller owns the vector and it outlives the call.
	const std::vector<std::string>* banks = nullptr;
	size_t current_bank = 0;

	// Set by the view when another bank is picked. kNoPreset means "not this
	// frame". Changing bank loads that bank's first preset, so the caller
	// treats this as a preset switch and may refuse it.
	size_t requested_bank = kNoPreset;

	// The message bound to "next bank", in words, or empty.
	std::string bank_binding;

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

	// The noise gate (SF-85): its settings from the running preset, and its
	// current gain from the engine.
	BoardGate gate;

	// Set by the card. NaN means "not this frame", the same sentinel the trims
	// use -- and for the same reason, since every value in range is one a
	// player might legitimately ask for.
	float gate_threshold_request = NAN;
	float gate_release_request = NAN;
	bool gate_toggle_requested = false;

	// What the tuner is hearing (SF-81), or a zero frequency for "nothing
	// convincing". Read by the caller from the engine's input tap and named by
	// the caller too: the view draws a needle, it does not know what a note
	// is.
	BoardTuner tuner;

	// Every binding the engine would resolve against, in the words the
	// mappings table shows them (SF-78). Borrowed; the caller rebuilds it when
	// the map changes, not per frame.
	//
	// **Derived from the live map, not stored.** A binding made through learn
	// has to appear here without a reload, and the only way to be sure of that
	// is for this to have no independent existence -- there is one list of
	// bindings and this is a rendering of it.
	const std::vector<BoardMidiBinding>* midi_map = nullptr;

	// The MIDI input actually connected, in whatever words JACK calls it
	// (SF-69). Empty means nothing is, which is a fact worth showing rather
	// than a blank -- a footswitch that does nothing is the most common thing
	// to be confused by, and this is the row that answers it.
	std::string midi_controller;

	// Share of each audio block's time budget spent running the chain, 0..1
	// (SF-65). The number that says *why* a board is about to start xrunning,
	// which nothing showed before.
	float dsp_load = 0.0f;

	// The board-wide trims in dB (SF-66), and their bindings. Settings rather
	// than preset state -- see Settings::input_gain_db for why.
	float input_gain_db = 0.0f;
	float output_gain_db = 0.0f;

	std::string input_gain_binding;
	std::string output_gain_binding;

	// What is bound to each lane's level on the mixer card (SF-72), lane A
	// then lane B. Board-wide rather than per split, for the same reason the
	// binding itself is: the board draws one split at a time, and an
	// expression pedal assigned to "channel B" should still mean that after a
	// preset change.
	std::string mixer_binding[2];

	// Set by the view when a trim slider moves. NaN means "not this frame",
	// since 0 dB is a perfectly ordinary thing to ask for and could not be
	// used as the sentinel.
	float input_gain_request = NAN;
	float output_gain_request = NAN;

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

	// The board's shape, for the graph view. Borrowed, and rebuilt each frame
	// like `stages`. A plugin node's `stage` indexes into `stages` above; the
	// two are separate because one is what is *playing* and the other is how
	// it is wired, and a missing plugin has the second without the first.
	//
	// Null or empty falls back to nothing drawn -- the view never invents a
	// shape it wasn't given.
	const BoardGraphNodes* graph = nullptr;

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

	// A shape change asked for this frame. At most one, for the same reason
	// `stage_edit` is at most one: each costs a rebuild, and batching them
	// would make a failure impossible to attribute.
	GraphEdit graph_edit = GraphEdit::None;
	BoardNodeId graph_edit_node = kNoBoardNode;
	BoardSlot graph_edit_slot;
	unsigned graph_edit_branch = 0;
	float graph_edit_gain_db = 0.0f;
	BoardSplitter graph_edit_splitter;

	// How many channels the board is actually taking in. The splitter card
	// needs it to say whether stereo mode has anything to divide yet -- with
	// one input channel the mode still selects and the board still plays, it
	// just means what full range means until a stereo input is chosen in Audio
	// settings (SF-76). Saying so beats greying out a control for a reason the
	// player has to guess at.
	unsigned board_inputs = 1;

	// Where the board's audio comes from and goes, in words, for the plugs at
	// the ends of the wire. Empty reads as "every physical port".
	std::string input_routing;
	std::string output_routing;

	// Set when a plug is clicked: the routing panel should open (SF-56).
	bool input_routing_requested = false;
	bool output_routing_requested = false;

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

	// Set when the header's `Metronome` button is pressed (SF-82). Opened by
	// the caller, like the tuner, because what it drives is engine state.
	bool metronome_requested = false;

	// What the metronome is doing, and what the modal asks it to do. The
	// caller owns the click itself -- it runs on the audio thread -- so these
	// are a reading out and a request back, the same shape as everything else
	// here.
	BoardMetronome metronome;

	// Set when the metronome's Start/Stop is pressed, or a tap lands.
	bool metronome_toggle_requested = false;
	bool metronome_tap_requested = false;

	// NaN for "not this frame", the same sentinel the trims use: 0 is a
	// perfectly ordinary level to ask for.
	float metronome_tempo_request = NAN;
	float metronome_level_request = NAN;
	unsigned metronome_beats_request = 0;

	// Set when the header's `Tuner` button is pressed (SF-81). The caller
	// opens it, because opening it *mutes the board* -- that is engine state,
	// not a UI fact, and the view has no business reaching for it.
	bool tuner_requested = false;

	// Set when the header's `MIDI map` button is pressed (SF-78). The modal
	// itself is drawn by the view and its open/closed state lives in
	// BoardViewUi -- it reads a list and changes nothing, so there is nothing
	// for the caller to do but hand it the list.
	bool midi_map_requested = false;

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
