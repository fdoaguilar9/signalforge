#include "UiSelfTest.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "imgui.h"

#include "AppWindow.h"
#include "AudioSettingsView.h"
#include "BoardView.h"

namespace sf
{

namespace
{

// Dear ImGui needs a frame to lay out before item positions mean anything, so
// every click is aimed using what the previous frame reported.
constexpr int kSettleFrames = 2;

// What one click is expected to produce. The check reads the view state after
// the frame in which the mouse was released.
struct Step
{
	const char* widget;
	const char* what;
	bool (*check)(const BoardViewState&);
};

bool ExpectPreset0(const BoardViewState& view)
{
	return view.requested_preset == 0;
}

bool ExpectBypass1(const BoardViewState& view)
{
	return view.stage_edit == StageEdit::ToggleBypass && view.stage_edit_index == 1;
}

bool ExpectMoveDown0(const BoardViewState& view)
{
	return view.stage_edit == StageEdit::MoveDown && view.stage_edit_index == 0;
}

bool ExpectMoveUp2(const BoardViewState& view)
{
	return view.stage_edit == StageEdit::MoveUp && view.stage_edit_index == 2;
}

bool ExpectRemove2(const BoardViewState& view)
{
	return view.stage_edit == StageEdit::Remove && view.stage_edit_index == 2;
}

// The learn buttons. Each says what it asked to bind, and -- just as
// important -- that it did nothing else: the preset and bypass learn buttons
// sit inside rows that are themselves clickable, so a learn press that also
// switched the preset or flipped bypass would be worse than no button at all.
bool ExpectLearnNext(const BoardViewState& view)
{
	return view.learn_request.target == LearnTarget::NextPreset;
}

bool ExpectLearnPrev(const BoardViewState& view)
{
	return view.learn_request.target == LearnTarget::PrevPreset;
}

bool ExpectLearnPreset2(const BoardViewState& view)
{
	return view.learn_request.target == LearnTarget::LoadPreset && view.learn_request.preset == "03-lead" &&
		view.requested_preset == kNoPreset;
}

bool ExpectLearnBypass1(const BoardViewState& view)
{
	return view.learn_request.target == LearnTarget::ToggleBypass && view.learn_request.stage == 1 &&
		view.stage_edit == StageEdit::None;
}

// The first stage cannot move earlier. Clicking it must do nothing at all --
// not "nothing visible", nothing.
bool ExpectNothing(const BoardViewState& view)
{
	return view.stage_edit == StageEdit::None && view.requested_preset == kNoPreset && !view.save_requested;
}

// The library-wide and per-preset ones, all reachable without opening a stage.
const Step kLearnSteps[] = {
	{"learn-next:0", "arming learn for next_preset", ExpectLearnNext},
	{"learn-prev:0", "arming learn for prev_preset", ExpectLearnPrev},
	{"learn-preset:2", "arming learn for the third preset", ExpectLearnPreset2},
};

const Step kSteps[] = {
	{"preset:0", "clicking the first preset", ExpectPreset0},
	{"bypass:1", "toggling bypass on stage 1", ExpectBypass1},
	{"down:0", "moving stage 0 later", ExpectMoveDown0},
	{"up:2", "moving stage 2 earlier", ExpectMoveUp2},
	{"remove:2", "removing stage 2", ExpectRemove2},
	{"up:0", "clicking the disabled 'move earlier' on stage 0", ExpectNothing},
};

const WidgetProbe* Find(const std::vector<WidgetProbe>& widgets, const char* id)
{
	for (const WidgetProbe& probe : widgets)
	{
		if (probe.id == id)
		{
			return &probe;
		}
	}

	return nullptr;
}

// How many probes have ids beginning with `prefix` -- "how many plugin rows
// did the browser draw", without caring which.
size_t CountWithPrefix(const std::vector<WidgetProbe>& widgets, const char* prefix)
{
	const size_t length = std::strlen(prefix);
	size_t found = 0;

	for (const WidgetProbe& probe : widgets)
	{
		if (probe.id.compare(0, length, prefix) == 0)
		{
			++found;
		}
	}

	return found;
}

}  // namespace

int RunUiSelfTest(int frames)
{
	AppWindow window;
	std::string error;

	// Tall enough that everything the test clicks is on screen at once. The real
	// window scrolls; a test that had to scroll first would be testing the
	// scrollbar rather than the thing under it.
	if (!window.Open("Signalforge (test)", 700, 1100, error))
	{
		std::fprintf(stderr, "FAIL: %s\n", error.c_str());
		return 1;
	}

	// The first preset is already bound, so the row that shows an existing
	// binding is drawn as well as the rows that show none.
	const std::vector<BoardPreset> presets = {
		{"Clean", "01-clean", "PC 3"},
		{"Rhythm", "02-rhythm", ""},
		{"Lead", "03-lead", ""},
	};

	// The middle stage carries one of each kind of control, because the whole
	// point of SF-28 is that the widget follows what the port says it is.
	const std::vector<BoardControl> drive_controls = {
		// symbol, name, value, min, max, toggled, integer, logarithmic, writable
		{"gain", "Gain", -18.0f, -18.0f, 18.0f, false, false, false, true},
		{"enable", "Enable", 0.0f, 0.0f, 1.0f, true, true, false, true},
		{"steps", "Steps", 1.0f, 1.0f, 8.0f, false, true, false, true},
		{"freq", "Frequency", 20.0f, 5.0f, 1250.0f, false, false, true, true},
		{"peak", "Peak", -6.0f, -120.0f, 0.0f, false, false, false, false},
	};

	std::vector<BoardStage> stages = {{"Amp", "urn:test:amp", false, {}}, {"Drive", "urn:test:drive", true, {}},
		{"Reverb", "urn:test:reverb", false, {}}};

	stages[1].controls = drive_controls;

	// A file-valued parameter, the kind neural-amp-modeler-lv2 has.
	stages[1].paths = {{"urn:test:model", "Neural Model", "/models/amps/current.nam", {"nam", "wav"}}};

	// Already-bound bypass and control, so the "currently CC 80" half of the
	// learn affordance is drawn too, not just the button.
	stages[1].bypass_binding = "CC 80";
	stages[1].controls[0].binding = "CC 7 ch2";

	// A listing the picker will be handed. None of this exists on disk, which
	// is the point: the view never touches a filesystem.
	const std::vector<BoardFileEntry> listing = {
		{"captures", true},
		{"Fender Twin.nam", false},
		{"Marshall JCM.nam", false},
	};

	const std::vector<BoardPlugin> plugins = {{"Neural Amp Modeler", "urn:test:nam", 1, 1},
		{"x42-eq Parametric Equalizer Mono", "urn:test:eq", 1, 1}, {"Hall Reverb", "urn:test:hall", 2, 2}};

	// Start on the second preset, so clicking the first is a real change
	// rather than a click that would look successful either way.
	const size_t starting_preset = 1;

	BoardViewUi ui;
	ImGuiIO& io = ImGui::GetIO();

	int drawn = 0;
	bool closed_early = false;

	// Whether the caller has a learn armed. Owned out here because the view is
	// told this once per frame and the learn phase has to change it between
	// frames the same way an arriving MIDI message would.
	bool learning = false;

	// A board that isn't what its preset says it is: SF-31's missing-plugin
	// condition, which the window has to show and which blocks saving.
	bool degraded = false;

	// What the level meters are fed. Hot but not clipping by default, so the
	// coloured branches are exercised by every ordinary frame.
	float meter_input_db = -4.0f;
	float meter_output_db = -0.5f;

	// Draws one frame, optionally with the mouse at `target` and the button in
	// state `press`. Returns what the view reported, probes and all.
	auto draw = [&](const WidgetProbe* target, bool press, const std::vector<BoardStage>* chain_stages)
	{
		BoardViewState view;

		if (target)
		{
			io.AddMousePosEvent(target->x, target->y);
			io.AddMouseButtonEvent(0, press);
		}

		if (!window.BeginFrame())
		{
			closed_early = true;
			return view;
		}

		view.preset_name = presets[starting_preset].name;
		view.preset_path = "(self-test)";
		view.sample_rate = 48000.0;
		view.block_size = 128;
		view.latency_ms = 5.3;

		// Levels the meter has to survive, varied by the caller so every
		// branch is drawn: a normal hot signal by default, and silence
		// (-INFINITY) and over-full-scale at the end. Both extremes reach
		// the progress bar as a computed fraction, which is exactly where a
		// non-finite value would otherwise become a crash or a wild bar.
		view.input_db = meter_input_db;
		view.output_db = meter_output_db;
		view.output_clipping = degraded;
		view.midi_bindings = 3;
		view.current_preset = starting_preset;
		view.presets = &presets;
		view.stages = chain_stages;
		view.plugins = &plugins;
		view.plugins_hidden = 7;
		view.file_entries = &listing;
		view.learning = learning;
		view.learning_what = learning ? "next_preset" : "";
		view.warning = degraded ? "1 stage(s) skipped -- plugin not installed: urn:test:gone" : "";
		view.save_blocked_reason = degraded ? "Saving would write this preset without its missing stage(s)." : "";

		// So the save button is drawn enabled and can be probed; whether it
		// was clicked is checked by ExpectNothing on the last step.
		view.unsaved_changes = true;
		view.record_widgets = true;

		DrawBoard(view, ui);
		window.EndFrame();
		++drawn;

		return view;
	};

	// A click is a press and a release; the widget fires on the release, the
	// way a real button does. Returns the view from the release frame.
	auto click = [&](const std::vector<WidgetProbe>& widgets, const char* id, const std::vector<BoardStage>* chain)
	{
		const WidgetProbe* target = Find(widgets, id);

		if (!target)
		{
			std::fprintf(stderr, "FAIL: the view never drew a widget called '%s'.\n", id);
			return BoardViewState();
		}

		// The probe is a copy: the press frame redraws and invalidates the
		// vector the caller handed in.
		const WidgetProbe aim = *target;

		// Hover first, then press. A real mouse is always somewhere before it
		// is clicked, and Dear ImGui's overlap rule depends on it: a widget
		// drawn on top of a full-width row (the learn button on a preset row)
		// is only usable once the previous frame saw it hovered.
		draw(&aim, false, chain);
		draw(&aim, true, chain);

		return draw(&aim, false, chain);
	};

	// Sliders are not buttons: a slider takes its new value on mouse-*down*,
	// while a button fires on release. So this returns the press frame, and
	// still releases afterwards so nothing is left held.
	auto press = [&](const std::vector<WidgetProbe>& from, const char* id, const std::vector<BoardStage>* chain)
	{
		const WidgetProbe* target = Find(from, id);

		if (!target)
		{
			std::fprintf(stderr, "FAIL: the view never drew a widget called '%s'.\n", id);
			return BoardViewState();
		}

		const WidgetProbe aim = *target;

		draw(&aim, false, chain);

		BoardViewState pressed = draw(&aim, true, chain);
		draw(&aim, false, chain);

		return pressed;
	};

	std::vector<WidgetProbe> widgets;

	for (int i = 0; i < kSettleFrames; ++i)
	{
		widgets = draw(nullptr, false, &stages).widgets;
	}

	// Phase 1: the stage list and preset list.
	for (const Step& step : kSteps)
	{
		const BoardViewState view = click(widgets, step.widget, &stages);

		if (closed_early)
		{
			std::fprintf(stderr, "FAIL: window closed during '%s'.\n", step.what);
			return 1;
		}

		if (view.widgets.empty())
		{
			return 1;  // click() already said which widget was missing
		}

		if (!step.check(view))
		{
			std::fprintf(stderr, "FAIL: %s did not do what it should (edit=%d index=%zu preset=%zu).\n", step.what,
				static_cast<int>(view.stage_edit), view.stage_edit_index, view.requested_preset);
			return 1;
		}

		widgets = view.widgets;
	}

	// The disabled states the view is responsible for: nothing above the first
	// stage, and nothing below the last.
	if (const WidgetProbe* probe = Find(widgets, "up:0"); !probe || probe->enabled)
	{
		std::fprintf(stderr, "FAIL: 'move earlier' on the first stage should be disabled.\n");
		return 1;
	}

	if (const WidgetProbe* probe = Find(widgets, "down:2"); !probe || probe->enabled)
	{
		std::fprintf(stderr, "FAIL: 'move later' on the last stage should be disabled.\n");
		return 1;
	}

	// Phase 2: the plugin browser.
	{
		// Nothing from the browser should exist before it is opened -- a modal
		// that draws its contents while closed would let a stray click through.
		if (CountWithPrefix(widgets, "plugin:") != 0)
		{
			std::fprintf(stderr, "FAIL: the plugin browser drew rows while closed.\n");
			return 1;
		}

		// Typed, not poked into the buffer: the search box takes keyboard
		// focus when the modal opens, so Dear ImGui owns what's in it until
		// the user types. Writing the buffer directly would be overwritten and
		// would test nothing.
		auto type = [&io](const char* text) { io.AddInputCharactersUTF8(text); };

		// Opens the browser and returns the frame in which its rows exist.
		auto open_browser = [&](const std::vector<WidgetProbe>& from)
		{
			const BoardViewState opened = click(from, "add-stage:0", &stages);

			if (closed_early || opened.widgets.empty())
			{
				return BoardViewState();
			}

			return draw(nullptr, false, &stages);
		};

		BoardViewState view = open_browser(widgets);

		if (view.widgets.empty())
		{
			std::fprintf(stderr, "FAIL: could not open the plugin browser.\n");
			return 1;
		}

		if (CountWithPrefix(view.widgets, "plugin:") != plugins.size())
		{
			std::fprintf(stderr, "FAIL: the browser drew %zu of %zu plugin(s).\n",
				CountWithPrefix(view.widgets, "plugin:"), plugins.size());
			return 1;
		}

		// Searching by name.
		type("reverb");
		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "plugin:") != 1 || !Find(view.widgets, "plugin:2"))
		{
			std::fprintf(stderr, "FAIL: searching 'reverb' left %zu plugin(s), expected only 'Hall Reverb'.\n",
				CountWithPrefix(view.widgets, "plugin:"));
			return 1;
		}

		// Cancel closes it and leaves the chain alone.
		view = click(view.widgets, "browser-cancel:0", &stages);
		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "plugin:") != 0)
		{
			std::fprintf(stderr, "FAIL: Cancel did not close the browser.\n");
			return 1;
		}

		if (view.requested_plugin != kNoPreset)
		{
			std::fprintf(stderr, "FAIL: Cancel asked to add a plugin.\n");
			return 1;
		}

		// Searching by URI, which is how preset files name a plugin. Reopening
		// clears the box, which is also what makes this a fresh search.
		view = open_browser(view.widgets);
		type("urn:test:eq");
		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "plugin:") != 1 || !Find(view.widgets, "plugin:1"))
		{
			std::fprintf(stderr, "FAIL: searching by URI left %zu plugin(s), expected only the eq.\n",
				CountWithPrefix(view.widgets, "plugin:"));
			return 1;
		}

		// Case-insensitively, too.
		view = click(view.widgets, "browser-cancel:0", &stages);
		view = draw(nullptr, false, &stages);
		view = open_browser(view.widgets);
		type("HALL");
		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "plugin:") != 1 || !Find(view.widgets, "plugin:2"))
		{
			std::fprintf(stderr, "FAIL: 'HALL' should have matched 'Hall Reverb'.\n");
			return 1;
		}

		view = click(view.widgets, "plugin:2", &stages);

		if (closed_early || view.widgets.empty())
		{
			std::fprintf(stderr, "FAIL: could not click a plugin in the browser.\n");
			return 1;
		}

		// Index into the whole catalogue, not into what the search left
		// visible. Getting this wrong would add a different plugin than the
		// one that was clicked -- and only ever when a search was active.
		if (view.requested_plugin != 2)
		{
			std::fprintf(stderr, "FAIL: choosing 'Hall Reverb' requested index %zu, expected 2.\n",
				view.requested_plugin);
			return 1;
		}

		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "plugin:") != 0)
		{
			std::fprintf(stderr, "FAIL: the browser stayed open after choosing a plugin.\n");
			return 1;
		}
	}

	// Phase 3: the auto-generated control panel.
	{
		// Collapsed by default: a board of four pedals with thirty controls
		// each would otherwise open as a wall of sliders.
		if (CountWithPrefix(widgets, "control:") != 0)
		{
			std::fprintf(stderr, "FAIL: stage controls were drawn while the stage was collapsed.\n");
			return 1;
		}

		BoardViewState view = click(widgets, "expand:1", &stages);

		if (closed_early || view.widgets.empty())
		{
			std::fprintf(stderr, "FAIL: could not expand a stage.\n");
			return 1;
		}

		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "control:") != drive_controls.size())
		{
			std::fprintf(stderr, "FAIL: expanding drew %zu of %zu control(s).\n",
				CountWithPrefix(view.widgets, "control:"), drive_controls.size());
			return 1;
		}

		// Only the expanded stage's controls, not every stage's.
		if (CountWithPrefix(view.widgets, "control:0:") != 0 || CountWithPrefix(view.widgets, "control:2:") != 0)
		{
			std::fprintf(stderr, "FAIL: expanding one stage drew another stage's controls.\n");
			return 1;
		}

		// A meter is shown but cannot be dragged.
		const WidgetProbe* meter = Find(view.widgets, "control:1:peak");

		if (!meter || meter->enabled)
		{
			std::fprintf(stderr, "FAIL: an output control port should be shown but not editable.\n");
			return 1;
		}

		// A toggle goes to its maximum, not to some fraction of the way along
		// a range -- the bug you get from drawing every control as a slider.
		view = click(view.widgets, "control:1:enable", &stages);

		if (view.control_stage != 1 || view.control_symbol != "enable" || view.control_value != 1.0f)
		{
			std::fprintf(stderr, "FAIL: the toggle reported stage %zu '%s' = %g, expected 1 'enable' = 1.\n",
				view.control_stage, view.control_symbol.c_str(), view.control_value);
			return 1;
		}

		// Clicking the middle of a slider moves it; the exact value depends on
		// pixel rounding, so what's asserted is that it left the end it was
		// parked at and stayed inside its range.
		view = draw(nullptr, false, &stages);
		view = press(view.widgets, "control:1:gain", &stages);

		if (view.control_stage != 1 || view.control_symbol != "gain")
		{
			std::fprintf(stderr, "FAIL: the slider reported stage %zu '%s', expected 1 'gain'.\n", view.control_stage,
				view.control_symbol.c_str());
			return 1;
		}

		if (!(view.control_value > -17.0f && view.control_value < 18.0f))
		{
			std::fprintf(stderr, "FAIL: clicking the middle of the gain slider gave %g, expected well inside -18..18.\n",
				view.control_value);
			return 1;
		}

		// The meter refuses the same treatment.
		view = draw(nullptr, false, &stages);
		view = press(view.widgets, "control:1:peak", &stages);

		if (view.control_stage != kNoPreset)
		{
			std::fprintf(stderr, "FAIL: clicking a meter changed stage %zu '%s'.\n", view.control_stage,
				view.control_symbol.c_str());
			return 1;
		}

		// And collapsing puts them away again.
		view = draw(nullptr, false, &stages);
		view = click(view.widgets, "expand:1", &stages);
		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "control:") != 0)
		{
			std::fprintf(stderr, "FAIL: collapsing a stage left %zu control(s) drawn.\n",
				CountWithPrefix(view.widgets, "control:"));
			return 1;
		}

		widgets = view.widgets;
	}

	// Phase 4: the file picker.
	{
		BoardViewState view = click(widgets, "expand:1", &stages);
		view = draw(nullptr, false, &stages);

		// The button shows the loaded file, not the whole path, and not the
		// property URI.
		if (!Find(view.widgets, "path:100"))
		{
			std::fprintf(stderr, "FAIL: the stage's file parameter was not drawn.\n");
			return 1;
		}

		if (CountWithPrefix(view.widgets, "file:") != 0)
		{
			std::fprintf(stderr, "FAIL: the file picker drew rows while closed.\n");
			return 1;
		}

		// Opening asks to browse where the current file lives, rather than
		// wherever the picker was left last time.
		view = click(view.widgets, "path:100", &stages);

		if (view.browse_request != "/models/amps")
		{
			std::fprintf(stderr, "FAIL: opening the picker asked for '%s', expected the loaded file's directory.\n",
				view.browse_request.c_str());
			return 1;
		}

		view = draw(nullptr, false, &stages);

		// Every entry, plus the "up" row that is always offered.
		if (CountWithPrefix(view.widgets, "file:") != listing.size() + 1)
		{
			std::fprintf(stderr, "FAIL: the picker drew %zu row(s), expected %zu.\n",
				CountWithPrefix(view.widgets, "file:"), listing.size() + 1);
			return 1;
		}

		// A directory is navigation, not a choice: clicking one must not load
		// it as a model.
		view = click(view.widgets, "file:captures", &stages);

		if (view.browse_request != "captures" || view.path_stage != kNoPreset)
		{
			std::fprintf(stderr, "FAIL: clicking a directory asked for '%s' and set stage %zu; expected navigation.\n",
				view.browse_request.c_str(), view.path_stage);
			return 1;
		}

		view = draw(nullptr, false, &stages);
		view = click(view.widgets, "file:..", &stages);

		if (view.browse_request != "..")
		{
			std::fprintf(stderr, "FAIL: the up row asked for '%s'.\n", view.browse_request.c_str());
			return 1;
		}

		// Choosing a file reports it as an absolute path, against the
		// directory the picker is showing.
		view = draw(nullptr, false, &stages);
		view = click(view.widgets, "file:Marshall JCM.nam", &stages);

		if (view.path_stage != 1 || view.path_property != "urn:test:model")
		{
			std::fprintf(stderr, "FAIL: choosing a file reported stage %zu property '%s', expected 1 'urn:test:model'.\n",
				view.path_stage, view.path_property.c_str());
			return 1;
		}

		if (view.path_file.empty() || view.path_file[0] != '/' ||
			view.path_file.find("Marshall JCM.nam") == std::string::npos)
		{
			std::fprintf(stderr, "FAIL: chose '%s', expected an absolute path ending in the file name.\n",
				view.path_file.c_str());
			return 1;
		}

		// And the picker closes behind it.
		view = draw(nullptr, false, &stages);

		if (CountWithPrefix(view.widgets, "file:") != 0)
		{
			std::fprintf(stderr, "FAIL: the picker stayed open after a file was chosen.\n");
			return 1;
		}

		// Put the stage away again so the checks below see a tidy list.
		view = click(view.widgets, "expand:1", &stages);
		widgets = draw(nullptr, false, &stages).widgets;
	}

	// Phase 5: MIDI-learn, which is what SF-30 adds -- a learn button next to
	// everything bindable, so a mapping can be made without the console.
	{
		// Nothing is armed, so there is nothing to cancel. A cancel button
		// drawn while idle would be a button that cancels someone else's learn.
		if (Find(widgets, "learn-cancel:0"))
		{
			std::fprintf(stderr, "FAIL: the cancel button was drawn while nothing was being learned.\n");
			return 1;
		}

		for (const Step& step : kLearnSteps)
		{
			const BoardViewState view = click(widgets, step.widget, &stages);

			if (closed_early)
			{
				std::fprintf(stderr, "FAIL: window closed during '%s'.\n", step.what);
				return 1;
			}

			if (view.widgets.empty())
			{
				return 1;  // click() already said which widget was missing
			}

			if (!step.check(view))
			{
				std::fprintf(stderr,
					"FAIL: %s did not do what it should (target=%d preset='%s' stage=%zu, edit=%d, requested=%zu).\n",
					step.what, static_cast<int>(view.learn_request.target), view.learn_request.preset.c_str(),
					view.learn_request.stage, static_cast<int>(view.stage_edit), view.requested_preset);
				return 1;
			}

			widgets = view.widgets;
		}

		// Bypass and the controls belong to a stage, so they only appear once
		// it is open -- a collapsed board would otherwise be a row of learn
		// buttons with nothing to say which pedal they belong to.
		if (Find(widgets, "learn-bypass:1"))
		{
			std::fprintf(stderr, "FAIL: a collapsed stage drew its bypass learn button.\n");
			return 1;
		}

		BoardViewState view = click(widgets, "expand:1", &stages);
		view = draw(nullptr, false, &stages);
		view = click(view.widgets, "learn-bypass:1", &stages);

		if (closed_early || view.widgets.empty())
		{
			return 1;
		}

		if (!ExpectLearnBypass1(view))
		{
			std::fprintf(stderr, "FAIL: the bypass learn button asked for target=%d stage=%zu, and edit=%d.\n",
				static_cast<int>(view.learn_request.target), view.learn_request.stage,
				static_cast<int>(view.stage_edit));
			return 1;
		}

		// A control's learn button has to carry the port's *real* range with
		// it: bound over the default 0..1, an expression pedal would move a
		// -18..18 dB control across the first thirty-sixth of its travel.
		view = draw(nullptr, false, &stages);
		view = click(view.widgets, "learn-control:1:gain", &stages);

		if (view.learn_request.target != LearnTarget::SetControl || view.learn_request.stage != 1 ||
			view.learn_request.symbol != "gain")
		{
			std::fprintf(stderr, "FAIL: the control learn button asked for target=%d stage=%zu '%s'.\n",
				static_cast<int>(view.learn_request.target), view.learn_request.stage,
				view.learn_request.symbol.c_str());
			return 1;
		}

		if (view.learn_request.minimum != -18.0f || view.learn_request.maximum != 18.0f)
		{
			std::fprintf(stderr, "FAIL: the control learn button asked for range %g..%g, expected the port's -18..18.\n",
				view.learn_request.minimum, view.learn_request.maximum);
			return 1;
		}

		// Nor may it move the control it sits beside.
		if (view.control_stage != kNoPreset)
		{
			std::fprintf(stderr, "FAIL: pressing learn also changed stage %zu '%s'.\n", view.control_stage,
				view.control_symbol.c_str());
			return 1;
		}

		// Now with a learn armed. MidiLearn binds one thing at a time, so every
		// other learn button has to be out of reach until this one resolves --
		// otherwise the switch you are about to press lands on a target you
		// changed your mind about.
		learning = true;
		view = draw(nullptr, false, &stages);
		view = draw(nullptr, false, &stages);

		size_t offered = 0;

		for (const WidgetProbe& probe : view.widgets)
		{
			if (probe.id.compare(0, 6, "learn-") != 0 || probe.id == "learn-cancel:0")
			{
				continue;
			}

			++offered;

			if (probe.enabled)
			{
				std::fprintf(stderr, "FAIL: '%s' was still clickable while a learn was armed.\n", probe.id.c_str());
				return 1;
			}
		}

		// A count, so "all of them were disabled" can't be satisfied by drawing
		// none of them at all: next and prev, one per preset, and -- stage 1
		// being open -- its bypass and each of its writable controls. The meter
		// has nothing to bind.
		const size_t expected = 2 + presets.size() + 1 + (drive_controls.size() - 1);

		if (offered != expected)
		{
			std::fprintf(stderr, "FAIL: %zu learn button(s) were drawn while armed, expected %zu.\n", offered, expected);
			return 1;
		}

		const WidgetProbe* cancel = Find(view.widgets, "learn-cancel:0");

		if (!cancel || !cancel->enabled)
		{
			std::fprintf(stderr, "FAIL: no usable cancel button while a learn was armed.\n");
			return 1;
		}

		// Disabled means disabled: clicking one must not re-arm behind the
		// armed learn's back.
		view = click(view.widgets, "learn-next:0", &stages);

		if (view.learn_request.target != LearnTarget::None)
		{
			std::fprintf(stderr, "FAIL: a disabled learn button still asked to bind (target=%d).\n",
				static_cast<int>(view.learn_request.target));
			return 1;
		}

		view = click(view.widgets, "learn-cancel:0", &stages);

		if (view.learn_request.target != LearnTarget::Cancel)
		{
			std::fprintf(stderr, "FAIL: the cancel button reported target=%d, expected Cancel.\n",
				static_cast<int>(view.learn_request.target));
			return 1;
		}

		// The caller disarms; the view follows on the next frame.
		learning = false;
		view = draw(nullptr, false, &stages);
		view = click(view.widgets, "expand:1", &stages);
		widgets = draw(nullptr, false, &stages).widgets;
	}

	// Phase 6: a degraded board (SF-31). The window has to say the preset lost
	// a stage, and saving has to be out of reach while it has -- a capture of
	// a chain that is missing a pedal would write the pedal out of the file,
	// which is the only place it still exists.
	{
		if (const WidgetProbe* probe = Find(widgets, "save:0"); !probe || !probe->enabled)
		{
			std::fprintf(stderr, "FAIL: saving should be available on an intact board with unsaved changes.\n");
			return 1;
		}

		degraded = true;
		BoardViewState view = draw(nullptr, false, &stages);
		view = draw(nullptr, false, &stages);

		if (const WidgetProbe* probe = Find(view.widgets, "save:0"); !probe || probe->enabled)
		{
			std::fprintf(stderr, "FAIL: saving was still offered while stages were missing.\n");
			return 1;
		}

		// Disabled means disabled: the button must refuse the click, not just
		// look grey.
		view = click(view.widgets, "save:0", &stages);

		if (view.save_requested)
		{
			std::fprintf(stderr, "FAIL: a disabled save button still asked to save.\n");
			return 1;
		}

		degraded = false;
		widgets = draw(nullptr, false, &stages).widgets;

		if (const WidgetProbe* probe = Find(widgets, "save:0"); !probe || !probe->enabled)
		{
			std::fprintf(stderr, "FAIL: saving stayed blocked after the board was whole again.\n");
			return 1;
		}
	}

	// One stage left: removing it would leave the engine with no ports.
	{
		const std::vector<BoardStage> only_one = {{"Amp", "urn:test:amp", false, {}}};
		const BoardViewState view = draw(nullptr, false, &only_one);

		if (const WidgetProbe* probe = Find(view.widgets, "remove:0"); !probe || probe->enabled)
		{
			std::fprintf(stderr, "FAIL: removing the only stage should be disabled.\n");
			return 1;
		}
	}

	// And with no chain at all: the layout a board started from a preset path
	// rather than a directory draws, which no normal session shows.
	draw(nullptr, false, nullptr);

	// The meter's scale mapping, asserted rather than merely drawn. Drawing
	// alone proves very little here: Dear ImGui saturates a progress bar's
	// fraction internally, so a mapping that returns 12.0 or NaN still
	// *looks* fine while being wrong, and would stop looking fine the moment
	// anything else consumed it.
	{
		struct MeterCase
		{
			float db;
			float expected;
			const char* what;
		};

		const MeterCase cases[] = {
			{-INFINITY, 0.0f, "silence sits at the bottom"},
			{std::numeric_limits<float>::quiet_NaN(), 0.0f, "NaN sits at the bottom rather than anywhere"},
			{-120.0f, 0.0f, "below the floor is the floor"},
			{-60.0f, 0.0f, "the floor itself"},
			{-30.0f, 0.5f, "halfway up"},
			{0.0f, 1.0f, "full scale fills the bar"},
			{6.0f, 1.0f, "past full scale still just fills it"},
			{INFINITY, 0.0f, "positive infinity is not a level either"},
		};

		for (const MeterCase& test : cases)
		{
			const float got = MeterFraction(test.db);

			if (!(got >= 0.0f && got <= 1.0f) || std::fabs(got - test.expected) > 0.001f)
			{
				std::fprintf(stderr, "FAIL: meter fraction for %s: got %f, expected %f.\n", test.what,
					static_cast<double>(got), static_cast<double>(test.expected));
				return 1;
			}
		}
	}

	// And the same extremes actually drawn, since the bar, its overlay text
	// and its colour choice all read the level too.
	{
		meter_input_db = -INFINITY;
		meter_output_db = -INFINITY;
		draw(nullptr, false, nullptr);

		meter_input_db = 6.0f;   // well past full scale
		meter_output_db = 2.5f;
		degraded = false;
		draw(nullptr, false, nullptr);

		// NaN is not a level any correct code produces, but it is what a
		// divide-by-zero upstream would hand this, and a meter is not worth
		// crashing the board over.
		meter_input_db = std::numeric_limits<float>::quiet_NaN();
		meter_output_db = std::numeric_limits<float>::quiet_NaN();
		draw(nullptr, false, nullptr);

		meter_input_db = -4.0f;
		meter_output_db = -0.5f;
	}

	// SF-38/39: the Audio Settings dialog. Its port list groups by device and
	// offers two granularities per group, which is real layout logic that
	// nothing else exercises. Drawn against a synthetic graph shaped like the
	// awkward real one: a multi-channel interface (so a device row *and* its
	// individual ports both appear), a single-port device (so the no-device-
	// row branch is taken), and a saved selection naming hardware that isn't
	// in the list at all (the "not connected" preservation path).
	{
		const std::vector<AudioSettingsJackPort> jack_ports = {
			{"Scarlett:capture_AUX0", "Scarlett", "capture_AUX0", true},
			{"Scarlett:capture_AUX1", "Scarlett", "capture_AUX1", true},
			{"Razer:capture_MONO", "Razer", "capture_MONO", true},
			{"Scarlett:playback_FL", "Scarlett", "playback_FL", false},
			{"Scarlett:playback_FR", "Scarlett", "playback_FR", false},
		};

		const std::vector<AudioSettingsDevice> devices = {{"hw:0", "Scarlett Solo (hw:0)", "Scarlett"}};

		// Persistent across frames, the way the real dialog's state is: a
		// combo stays open between frames, which is the whole point here.
		AudioSettingsViewState audio;
		audio.server_running = true;
		audio.jack_ports = &jack_ports;
		audio.devices = &devices;
		audio.input_filter = "Scarlett:capture_AUX1";  // an exact channel
		audio.output_filter = "Scarlett";              // a whole device
		audio.record_widgets = true;

		auto draw_audio = [&](const WidgetProbe* aim, bool pressed)
		{
			ImGuiIO& io = ImGui::GetIO();

			if (aim)
			{
				io.AddMousePosEvent(aim->x, aim->y);
				io.AddMouseButtonEvent(0, pressed);
			}

			if (!window.BeginFrame())
			{
				closed_early = true;
				return;
			}

			audio.widgets.clear();
			audio.apply_requested = false;
			audio.cancel_requested = false;

			DrawAudioSettings(audio);
			window.EndFrame();
			++drawn;
		};

		auto click_audio = [&](const char* id)
		{
			const WidgetProbe* target = Find(audio.widgets, id);

			if (!target)
			{
				std::fprintf(stderr, "FAIL: the audio dialog never drew a widget called '%s'.\n", id);
				return false;
			}

			const WidgetProbe aim = *target;

			draw_audio(&aim, false);
			draw_audio(&aim, true);
			draw_audio(&aim, false);

			return true;
		};

		draw_audio(nullptr, false);
		draw_audio(nullptr, false);

		// Nothing was clicked, so nothing may have changed.
		if (audio.input_filter != "Scarlett:capture_AUX1" || audio.output_filter != "Scarlett")
		{
			std::fprintf(stderr, "FAIL: drawing the audio dialog altered the selection.\n");
			return 1;
		}

		if (audio.apply_requested || audio.cancel_requested)
		{
			std::fprintf(stderr, "FAIL: the audio dialog reported a click nobody made.\n");
			return 1;
		}

		// Open the input dropdown. Only then do its rows exist at all -- which
		// is exactly why this has to be a real click and not just a redraw.
		if (!closed_early && !click_audio("audio-input:combo"))
		{
			return 1;
		}

		// A popup is positioned by the frame after it opens, so the row
		// coordinates reported on the opening frame are not yet where the
		// rows will be. Aiming at those hits empty space *outside* the
		// popup, which dismisses it -- so settle first, then aim.
		for (int i = 0; i < kSettleFrames && !closed_early; ++i)
		{
			draw_audio(nullptr, false);
		}

		if (!closed_early)
		{
			// A capture-only list: the Scarlett's two inputs, the Razer's one,
			// and a device row for the Scarlett because it has more than one.
			// The playback ports must not be here -- offering an output as an
			// input is offering a setting that cannot work.
			if (!Find(audio.widgets, "audio-input:port:Scarlett:capture_AUX0") ||
				!Find(audio.widgets, "audio-input:port:Scarlett:capture_AUX1") ||
				!Find(audio.widgets, "audio-input:port:Razer:capture_MONO") ||
				!Find(audio.widgets, "audio-input:device:Scarlett"))
			{
				std::fprintf(stderr, "FAIL: the input dropdown did not list every capture port and device.\n");
				return 1;
			}

			if (Find(audio.widgets, "audio-input:port:Scarlett:playback_FL"))
			{
				std::fprintf(stderr, "FAIL: the input dropdown offered a playback port.\n");
				return 1;
			}

			// The single-port Razer gets no device row: it would mean exactly
			// the same thing as its one port row.
			if (Find(audio.widgets, "audio-input:device:Razer"))
			{
				std::fprintf(stderr, "FAIL: a one-port device was given a redundant 'all ports' row.\n");
				return 1;
			}

			// Pick the *other* channel of the same interface -- the thing a
			// mono board needs and a device-only list could never express.
			if (!click_audio("audio-input:port:Scarlett:capture_AUX0"))
			{
				return 1;
			}

			if (audio.input_filter != "Scarlett:capture_AUX0")
			{
				std::fprintf(stderr, "FAIL: choosing a channel set the filter to '%s'.\n", audio.input_filter.c_str());
				return 1;
			}

			// Choosing an input must not disturb the output: they are separate
			// settings, and conflating them is the bug this pair replaced.
			if (audio.output_filter != "Scarlett")
			{
				std::fprintf(stderr, "FAIL: choosing an input changed the output to '%s'.\n",
					audio.output_filter.c_str());
				return 1;
			}
		}

		// A saved port whose hardware isn't present must survive being drawn
		// *and* being looked at -- the row is inside the open dropdown, so
		// this only means anything with the combo open.
		if (!closed_early)
		{
			audio.input_filter = "SomeInterfaceThatIsUnplugged:capture_1";

			draw_audio(nullptr, false);

			if (!click_audio("audio-input:combo"))
			{
				return 1;
			}

			for (int i = 0; i < kSettleFrames && !closed_early; ++i)
			{
				draw_audio(nullptr, false);
			}

			if (!Find(audio.widgets, "audio-input:unavailable"))
			{
				std::fprintf(stderr, "FAIL: an unplugged device's saved port was not offered back to the user.\n");
				return 1;
			}

			if (audio.input_filter != "SomeInterfaceThatIsUnplugged:capture_1")
			{
				std::fprintf(stderr, "FAIL: opening the dropdown dropped an unplugged device's saved port ('%s').\n",
					audio.input_filter.c_str());
				return 1;
			}

			// Close it again without choosing anything.
			if (!click_audio("audio-input:combo"))
			{
				return 1;
			}

			if (audio.input_filter != "SomeInterfaceThatIsUnplugged:capture_1")
			{
				std::fprintf(stderr, "FAIL: closing the dropdown dropped an unplugged device's saved port.\n");
				return 1;
			}
		}
	}

	if (closed_early)
	{
		std::fprintf(stderr, "FAIL: window closed before the checks finished.\n");
		return 1;
	}

	window.Close();

	std::printf("UI: PASS (drew %d frame(s), %zu stage interaction(s), %zu learn target(s), plugin browser, controls, "
		"file picker, audio settings, disabled states)\n",
		drawn, std::size(kSteps), std::size(kLearnSteps) + 1);

	(void)frames;

	return 0;
}

}  // namespace sf
