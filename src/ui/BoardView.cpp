#include "BoardView.h"

#include <cctype>
#include <cstdio>

#include "imgui.h"

namespace sf
{

namespace
{

// Enough for a board's worth of presets without pushing the engine and chain
// sections off the bottom of a small screen.
constexpr float kPresetListHeight = 160.0f;

// One level meter: a bar that runs from silence to full scale, coloured by
// how close it is, with the figure alongside.
//
// Colour rather than only a number because the number is unreadable at a
// glance while playing -- the point of a meter is to be understood in the
// corner of the eye, mid-song, without reading anything.
void DrawLevelMeter(const char* label, float db, bool clipping)
{
	const bool silent = !std::isfinite(db);
	const float fraction = MeterFraction(db);

	ImVec4 colour(0.25f, 0.70f, 0.35f, 1.0f);  // comfortable

	if (clipping || db >= 0.0f)
	{
		colour = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);  // clipping
	}
	else if (db >= kMeterHotDb)
	{
		colour = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);  // about to
	}
	else if (db >= kMeterWarnDb)
	{
		colour = ImVec4(0.90f, 0.80f, 0.20f, 1.0f);  // getting hot
	}

	char overlay[32];

	if (silent)
	{
		std::snprintf(overlay, sizeof(overlay), "--");
	}
	else
	{
		std::snprintf(overlay, sizeof(overlay), "%.1f dB", db);
	}

	ImGui::TextUnformatted(label);
	ImGui::SameLine(90.0f);

	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
	ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
	ImGui::PopStyleColor();

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Peak since the last frame. 0 dB is full scale -- red means the signal is being clipped.");
	}
}

// The browser holds every plugin on the machine, so it wants most of the
// window rather than a corner of it.
constexpr float kBrowserWidth = 560.0f;
constexpr float kBrowserHeight = 420.0f;

// Records the widget just drawn. Call immediately after the widget, while it
// is still "the last item" as far as Dear ImGui is concerned.
void RecordWidgetNamed(BoardViewState& state, const std::string& id, bool enabled)
{
	if (!state.record_widgets)
	{
		return;
	}

	const ImVec2 low = ImGui::GetItemRectMin();
	const ImVec2 high = ImGui::GetItemRectMax();

	state.widgets.push_back({id, (low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, enabled});
}

void RecordWidget(BoardViewState& state, const char* kind, size_t index, bool enabled)
{
	if (!state.record_widgets)
	{
		return;
	}

	RecordWidgetNamed(state, std::string(kind) + ":" + std::to_string(index), enabled);
}

// Case-insensitive substring match, the only kind of search a plugin browser
// needs: nobody remembers whether it was "x42-eq" or "X42-EQ".
bool MatchesSearch(const BoardPlugin& plugin, const std::string& needle)
{
	if (needle.empty())
	{
		return true;
	}

	auto contains = [&needle](const std::string& haystack)
	{
		if (needle.size() > haystack.size())
		{
			return false;
		}

		const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };

		for (size_t start = 0; start + needle.size() <= haystack.size(); ++start)
		{
			size_t i = 0;

			while (i < needle.size() && lower(haystack[start + i]) == lower(needle[i]))
			{
				++i;
			}

			if (i == needle.size())
			{
				return true;
			}
		}

		return false;
	};

	// The URI too: it's how the docs and preset files name a plugin, so it's
	// what someone will paste in.
	return contains(plugin.name) || contains(plugin.uri);
}

// The "learn" affordance that sits next to anything bindable: a button, and
// the message currently bound to it if there is one.
//
// Disabled while a learn is already armed, because MidiLearn binds one thing
// at a time and a second press would silently replace what you were about to
// press a switch for.
bool DrawLearnButton(BoardViewState& state, const std::string& probe_id, const std::string& binding)
{
	// Dear ImGui identifies a widget by its label, and every one of these is
	// labelled "learn". Most sit inside a per-preset or per-stage ID scope
	// that keeps them apart, but the two library-wide ones (next and prev) sit
	// side by side at window level and would otherwise be the *same* widget --
	// two buttons, one of which does nothing when pressed. The probe id is
	// already unique per button, so it makes a natural scope.
	ImGui::PushID(probe_id.c_str());

	ImGui::BeginDisabled(state.learning);
	const bool pressed = ImGui::SmallButton("learn");
	ImGui::EndDisabled();

	ImGui::PopID();

	RecordWidgetNamed(state, probe_id, !state.learning);

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip(binding.empty() ? "Bind a MIDI message to this" : "Rebind -- currently %s",
			binding.c_str());
	}

	if (!binding.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", binding.c_str());
	}

	return pressed;
}

// "/home/x/Downloads/Some Pack/Capture 16.nam" -> "Capture 16.nam". The full
// path goes in a tooltip; a row that shows it in full pushes everything else
// off the window.
std::string FileName(const std::string& path)
{
	const size_t cut = path.find_last_of('/');

	return cut == std::string::npos ? path : path.substr(cut + 1);
}

// The file-valued parameters of one stage: which model or IR is loaded, and a
// button to change it.
void DrawStagePaths(BoardViewState& state, BoardViewUi& ui, size_t stage_index, const BoardStage& stage)
{
	for (size_t p = 0; p < stage.paths.size(); ++p)
	{
		const BoardPathProperty& path = stage.paths[p];

		ImGui::PushID(static_cast<int>(p));
		ImGui::TextUnformatted(path.label.c_str());
		ImGui::SameLine();

		if (ImGui::Button(path.value.empty() ? "(none) -- choose..." : FileName(path.value).c_str()))
		{
			ui.picker_open = true;
			ui.picker_stage = stage_index;
			ui.picker_property = path.uri;

			// Open where the current file is, so replacing one model with its
			// neighbour doesn't mean navigating back from the home directory.
			if (!path.value.empty())
			{
				const size_t cut = path.value.find_last_of('/');
				state.browse_request = cut == std::string::npos ? std::string() : path.value.substr(0, cut);
			}
		}

		RecordWidget(state, "path", stage_index * 100 + p, true);

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", path.value.empty() ? path.uri.c_str() : path.value.c_str());
		}

		ImGui::PopID();
	}
}

// The control panel for one stage: a widget per control port, chosen by what
// the plugin says the port is rather than by its range alone.
void DrawStageControls(BoardViewState& state, size_t stage_index, const BoardStage& stage)
{
	if (stage.controls.empty())
	{
		ImGui::TextDisabled("This plugin has no controls.");
		return;
	}

	for (const BoardControl& control : stage.controls)
	{
		ImGui::PushID(control.symbol.c_str());

		const char* label = control.name.empty() ? control.symbol.c_str() : control.name.c_str();

		if (!control.writable)
		{
			// A meter or readout. Shown because it says what the plugin is
			// doing, but there is nothing to drag.
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine();
			ImGui::TextDisabled("%.3f", control.value);

			// Recorded as disabled rather than not recorded, so a test can
			// tell "shown but not editable" from "not shown at all".
			RecordWidgetNamed(state, "control:" + std::to_string(stage_index) + ":" + control.symbol, false);

			ImGui::PopID();
			continue;
		}

		bool changed = false;
		float value = control.value;

		if (control.toggled)
		{
			// An on/off switch. Anything above the midpoint is on, which is
			// also how the MIDI mapping reads a switch.
			bool on = value > (control.minimum + control.maximum) * 0.5f;

			if (ImGui::Checkbox(label, &on))
			{
				value = on ? control.maximum : control.minimum;
				changed = true;
			}
		}
		else if (control.integer)
		{
			int whole = static_cast<int>(value);

			if (ImGui::SliderInt(label, &whole, static_cast<int>(control.minimum), static_cast<int>(control.maximum)))
			{
				value = static_cast<float>(whole);
				changed = true;
			}
		}
		else
		{
			// Logarithmic where the plugin says so: a frequency control from
			// 5Hz to 1250Hz spends most of a linear slider above 600Hz, which
			// is the half nobody is looking for.
			const ImGuiSliderFlags flags = control.logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;

			if (ImGui::SliderFloat(label, &value, control.minimum, control.maximum, "%.3f", flags))
			{
				changed = true;
			}
		}

		RecordWidgetNamed(state, "control:" + std::to_string(stage_index) + ":" + control.symbol, true);

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s  [%g .. %g]", control.symbol.c_str(), control.minimum, control.maximum);
		}

		if (changed)
		{
			state.control_stage = stage_index;
			state.control_symbol = control.symbol;
			state.control_value = value;
		}

		ImGui::SameLine();

		if (DrawLearnButton(state, "learn-control:" + std::to_string(stage_index) + ":" + control.symbol,
				control.binding))
		{
			state.learn_request.target = LearnTarget::SetControl;
			state.learn_request.stage = stage_index;
			state.learn_request.symbol = control.symbol;

			// The port's real range travels with the request, so an expression
			// pedal sweeps the whole control rather than the 0..1 a binding
			// would otherwise default to.
			state.learn_request.minimum = control.minimum;
			state.learn_request.maximum = control.maximum;
		}

		ImGui::PopID();
	}
}

void DrawEngineRow(const char* label, const char* value)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(label);
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(value);
}

// The file picker. Deliberately knows nothing about the filesystem: it draws
// the listing it is given and reports where the user wants to look next, which
// is what lets a test drive it with a directory that doesn't exist.
void DrawFilePicker(BoardViewState& state, BoardViewUi& ui)
{
	// Opened here rather than where the button was clicked: Dear ImGui matches
	// popups by ID, and the button lives inside a per-stage ID scope that
	// BeginPopupModal at this level would never match.
	if (ui.picker_open && !ImGui::IsPopupOpen("Choose a file"))
	{
		ImGui::OpenPopup("Choose a file");
	}

	ImGui::SetNextWindowSize(ImVec2(kBrowserWidth, kBrowserHeight), ImGuiCond_Appearing);

	if (!ImGui::BeginPopupModal("Choose a file", nullptr, ImGuiWindowFlags_NoSavedSettings))
	{
		return;
	}

	// The path first, and wrapped: knowing where you are is most of using a
	// file picker.
	ImGui::TextWrapped("%s", ui.browse_directory.empty() ? "/" : ui.browse_directory.c_str());
	ImGui::Separator();

	// The filter for this property, so the listing can say what it hid.
	const std::vector<std::string>* accepted = nullptr;

	if (state.stages && ui.picker_stage < state.stages->size())
	{
		for (const BoardPathProperty& path : (*state.stages)[ui.picker_stage].paths)
		{
			if (path.uri == ui.picker_property)
			{
				accepted = &path.file_types;
				break;
			}
		}
	}

	if (ImGui::BeginChild("file-list", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f),
			ImGuiChildFlags_Borders))
	{
		// Always offered, even at the filesystem root where it goes nowhere:
		// a picker whose "up" disappears leaves you wondering where it went.
		if (ImGui::Selectable("../"))
		{
			state.browse_request = "..";
		}

		RecordWidgetNamed(state, "file:..", true);

		const size_t count = state.file_entries ? state.file_entries->size() : 0;

		for (size_t i = 0; i < count; ++i)
		{
			const BoardFileEntry& entry = (*state.file_entries)[i];

			ImGui::PushID(static_cast<int>(i));

			if (entry.is_directory)
			{
				if (ImGui::Selectable((entry.name + "/").c_str()))
				{
					state.browse_request = entry.name;
				}
			}
			else if (ImGui::Selectable(entry.name.c_str()))
			{
				state.path_stage = ui.picker_stage;
				state.path_property = ui.picker_property;

				// Absolute, because that is what the plugin gets told and what
				// a preset records. Composed here rather than by the caller so
				// there is one place that decides what "this row" means.
				state.path_file = ui.browse_directory + "/" + entry.name;

				ui.picker_open = false;
				ImGui::CloseCurrentPopup();
			}

			RecordWidgetNamed(state, "file:" + entry.name, true);

			ImGui::PopID();
		}

		if (count == 0)
		{
			ImGui::TextDisabled(accepted && !accepted->empty() ? "Nothing here this plugin can load."
															  : "This directory is empty.");
		}
	}

	ImGui::EndChild();

	if (accepted && !accepted->empty())
	{
		ImGui::TextDisabled("Showing:");

		for (size_t i = 0; i < accepted->size(); ++i)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s.%s", i > 0 ? "" : "", (*accepted)[i].c_str());
		}
	}

	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		ui.picker_open = false;
		ImGui::CloseCurrentPopup();
	}

	RecordWidget(state, "picker-cancel", 0, true);

	ImGui::EndPopup();
}

// The modal itself. Split out because it is the one part of this view with a
// life of its own -- it stays open across frames and owns what's typed in it.
void DrawPluginBrowser(BoardViewState& state, BoardViewUi& ui)
{
	ImGui::SetNextWindowSize(ImVec2(kBrowserWidth, kBrowserHeight), ImGuiCond_Appearing);

	if (!ImGui::BeginPopupModal("Add stage", nullptr, ImGuiWindowFlags_NoSavedSettings))
	{
		return;
	}

	// Focused on open, so you can start typing straight away rather than
	// having to click the box first.
	if (ImGui::IsWindowAppearing())
	{
		ImGui::SetKeyboardFocusHere();
	}

	ImGui::InputTextWithHint("##search", "Search plugins...", ui.plugin_search, sizeof(ui.plugin_search));
	RecordWidget(state, "plugin-search", 0, true);

	const std::string needle = ui.plugin_search;
	const size_t count = state.plugins ? state.plugins->size() : 0;
	size_t shown = 0;

	if (ImGui::BeginChild(
			"plugin-list", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f), ImGuiChildFlags_Borders))
	{
		for (size_t i = 0; i < count; ++i)
		{
			const BoardPlugin& plugin = (*state.plugins)[i];

			if (!MatchesSearch(plugin, needle))
			{
				continue;
			}

			ImGui::PushID(static_cast<int>(i));

			if (ImGui::Selectable(plugin.name.c_str()))
			{
				state.requested_plugin = i;
				ImGui::CloseCurrentPopup();
			}

			// Keyed by position in the full catalogue, not by position in the
			// filtered list, so a probe means the same plugin whatever has
			// been typed in the search box.
			RecordWidget(state, "plugin", i, true);

			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s\n%u in / %u out", plugin.uri.c_str(), plugin.audio_inputs, plugin.audio_outputs);
			}

			ImGui::SameLine();
			ImGui::TextDisabled("%u in / %u out", plugin.audio_inputs, plugin.audio_outputs);

			ImGui::PopID();

			++shown;
		}
	}

	ImGui::EndChild();

	if (shown == 0)
	{
		ImGui::TextDisabled(count == 0 ? "No plugins found." : "Nothing matches that.");
	}
	else
	{
		ImGui::Text("%zu of %zu", shown, count);

		// Said plainly rather than left as a discrepancy: a browser that
		// quietly shows fewer plugins than are installed looks broken.
		if (state.plugins_hidden > 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%zu more installed have no audio path)", state.plugins_hidden);
		}
	}

	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		ImGui::CloseCurrentPopup();
	}

	RecordWidget(state, "browser-cancel", 0, true);

	ImGui::EndPopup();
}

}  // namespace

void DrawBoard(BoardViewState& state, BoardViewUi& ui)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);

	// One window filling the OS window. Signalforge is a pedalboard, not a
	// desktop -- there is nothing to arrange, and floating panels would only
	// give the player something to lose.
	//
	// Spelled out rather than using NoDecoration, which also implies
	// NoScrollbar: expand a couple of stages with thirty controls each and the
	// content runs past the bottom of the window, and without a scrollbar
	// there is no way to reach it.
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;

	if (ImGui::Begin("Signalforge", nullptr, flags))
	{
		// SF-37: the first entry in what will grow into a general app menu.
		// A menu bar rather than a button alongside the board content, so
		// future entries have one obvious home instead of each needing their
		// own spot found for them.
		if (ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("Settings"))
			{
				if (ImGui::MenuItem("Audio Settings..."))
				{
					state.audio_settings_requested = true;
				}

				RecordWidgetNamed(state, "menu:audio-settings", true);

				ImGui::EndMenu();
			}

			ImGui::EndMenuBar();
		}

		ImGui::TextUnformatted(state.preset_name.empty() ? "(no preset)" : state.preset_name.c_str());

		if (!state.preset_path.empty() && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", state.preset_path.c_str());
		}

		ImGui::Separator();

		// A board that isn't what its preset says it is has to say so where it
		// can't be missed, and keep saying it: this is a condition, not a
		// message about the last click.
		if (!state.warning.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
			ImGui::TextWrapped("%s", state.warning.c_str());
			ImGui::PopStyleColor();
			ImGui::Separator();
		}

		// While armed, this is the only thing that matters on screen, so it
		// goes at the top rather than in the status line at the bottom.
		if (state.learning)
		{
			ImGui::Spacing();
			ImGui::TextWrapped("Press a switch or move a pedal to bind %s.", state.learning_what.c_str());
			ImGui::SameLine();

			if (ImGui::SmallButton("cancel"))
			{
				state.learn_request.target = LearnTarget::Cancel;
			}

			RecordWidget(state, "learn-cancel", 0, true);
			ImGui::Separator();
		}

		ImGui::Spacing();

		if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const size_t count = state.presets ? state.presets->size() : 0;

			// Stepping through the board is the binding most players want on a
			// footswitch, and it belongs to the library rather than to any one
			// preset -- so it lives above the list, not in it.
			ImGui::TextDisabled("Step:");
			ImGui::SameLine();

			if (DrawLearnButton(state, "learn-next:0", state.next_binding))
			{
				state.learn_request.target = LearnTarget::NextPreset;
			}

			ImGui::SameLine();
			ImGui::TextDisabled("next");
			ImGui::SameLine();

			if (DrawLearnButton(state, "learn-prev:0", state.prev_binding))
			{
				state.learn_request.target = LearnTarget::PrevPreset;
			}

			ImGui::SameLine();
			ImGui::TextDisabled("prev");

			if (count == 0)
			{
				ImGui::TextDisabled("No preset library.");
			}

			// A fixed height so the sections below it don't move as the
			// library grows -- a control that wanders around the window is a
			// control you can't hit without looking.
			if (count > 0 && ImGui::BeginChild("preset-list", ImVec2(0.0f, kPresetListHeight), ImGuiChildFlags_Borders))
			{
				for (size_t i = 0; i < count; ++i)
				{
					const BoardPreset& entry = (*state.presets)[i];

					ImGui::PushID(static_cast<int>(i));

					// Selection follows what's actually playing, not what was
					// clicked. See requested_preset in the header.
					//
					// AllowOverlap because the row is full-width and the learn
					// button sits on top of its right-hand end: without it the
					// row claims the hover first and pressing "learn" would
					// load the preset instead of arming a binding for it.
					if (ImGui::Selectable(entry.name.c_str(), i == state.current_preset,
							ImGuiSelectableFlags_AllowOverlap))
					{
						state.requested_preset = i;
					}

					RecordWidget(state, "preset", i, true);

					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("%s", entry.stem.c_str());
					}

					ImGui::SameLine();

					if (DrawLearnButton(state, "learn-preset:" + std::to_string(i), entry.binding))
					{
						state.learn_request.target = LearnTarget::LoadPreset;
						state.learn_request.preset = entry.stem;
					}

					ImGui::PopID();
				}
			}

			if (count > 0)
			{
				ImGui::EndChild();
			}
		}

		ImGui::Spacing();

		if (ImGui::CollapsingHeader("Engine", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("engine", 2, ImGuiTableFlags_SizingFixedFit))
			{
				char buffer[64];

				std::snprintf(buffer, sizeof(buffer), "%.0f Hz", state.sample_rate);
				DrawEngineRow("Sample rate", buffer);

				std::snprintf(buffer, sizeof(buffer), "%u frames", state.block_size);
				DrawEngineRow("Block size", buffer);

				// What a buffer-size change is actually traded against, so
				// the two are worth seeing together.
				std::snprintf(buffer, sizeof(buffer), "%.1f ms round trip", state.latency_ms);
				DrawEngineRow("Latency", buffer);

				std::snprintf(buffer, sizeof(buffer), "%zu", state.midi_bindings);
				DrawEngineRow("MIDI bindings", buffer);

				if (state.midi_drops > 0)
				{
					std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(state.midi_drops));
					DrawEngineRow("MIDI events dropped", buffer);
				}

				ImGui::EndTable();
			}

			ImGui::Spacing();

			// Below the table rather than in it: these change every frame,
			// and a bar that needs to be wide is a bad fit for a two-column
			// key/value layout built for short static strings.
			DrawLevelMeter("Input", state.input_db, false);
			DrawLevelMeter("Output", state.output_db, state.output_clipping);

			if (state.output_clipping)
			{
				ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
					"Output is clipping -- lower the level in the chain or at the interface.");
			}
		}

		ImGui::Spacing();

		if (ImGui::CollapsingHeader("Chain", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const size_t stage_count = state.stages ? state.stages->size() : 0;

			if (stage_count == 0)
			{
				ImGui::TextDisabled("No stages.");
			}

			for (size_t i = 0; i < stage_count; ++i)
			{
				const BoardStage& stage = (*state.stages)[i];

				ImGui::PushID(static_cast<int>(i));

				if (ui.stage_expanded.size() <= i)
				{
					ui.stage_expanded.resize(i + 1, false);
				}

				// Collapsed by default. A board with four pedals of thirty
				// controls each would otherwise open as a wall of sliders.
				if (ImGui::ArrowButton("##expand", ui.stage_expanded[i] ? ImGuiDir_Down : ImGuiDir_Right))
				{
					ui.stage_expanded[i] = !ui.stage_expanded[i];
				}

				RecordWidget(state, "expand", i, true);

				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s this stage's controls", ui.stage_expanded[i] ? "Hide" : "Show");
				}

				ImGui::SameLine();

				// Bypass second, and as a checkbox, because it is the one
				// control here a player reaches for mid-song. It takes effect
				// on the next audio block -- no rebuild, no crossfade.
				bool bypassed = stage.bypassed;

				if (ImGui::Checkbox("##bypass", &bypassed))
				{
					state.stage_edit = StageEdit::ToggleBypass;
					state.stage_edit_index = i;
				}

				RecordWidget(state, "bypass", i, true);

				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Bypass this stage");
				}

				ImGui::SameLine();

				// Grey the name rather than tagging it: a bypassed stage is
				// still in the chain, and reads better as "switched off" than
				// as a footnote.
				if (stage.bypassed)
				{
					ImGui::TextDisabled("%zu. %s", i, stage.name.c_str());
				}
				else
				{
					ImGui::Text("%zu. %s", i, stage.name.c_str());
				}

				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", stage.uri.c_str());
				}

				// Right-aligned, so the buttons form a column that doesn't
				// move as plugin names change length.
				const float button_width = ImGui::GetFrameHeight();
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - (button_width * 3.0f + spacing * 2.0f));

				ImGui::BeginDisabled(i == 0);

				if (ImGui::Button("^", ImVec2(button_width, 0.0f)))
				{
					state.stage_edit = StageEdit::MoveUp;
					state.stage_edit_index = i;
				}

				RecordWidget(state, "up", i, i != 0);
				ImGui::EndDisabled();

				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Move earlier in the chain");
				}

				ImGui::SameLine();
				ImGui::BeginDisabled(i + 1 >= stage_count);

				if (ImGui::Button("v", ImVec2(button_width, 0.0f)))
				{
					state.stage_edit = StageEdit::MoveDown;
					state.stage_edit_index = i;
				}

				RecordWidget(state, "down", i, i + 1 < stage_count);
				ImGui::EndDisabled();

				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Move later in the chain");
				}

				ImGui::SameLine();

				// The last stage can't go: an empty chain has no ports for the
				// engine to connect, and silently keeping one would be worse
				// than saying so.
				const bool last_one = stage_count == 1;
				ImGui::BeginDisabled(last_one);

				if (ImGui::Button("x", ImVec2(button_width, 0.0f)))
				{
					state.stage_edit = StageEdit::Remove;
					state.stage_edit_index = i;
				}

				RecordWidget(state, "remove", i, !last_one);
				ImGui::EndDisabled();

				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(last_one ? "A chain needs at least one stage" : "Remove this stage");
				}

				// On its own line: the row above is already three buttons wide,
				// and a footswitch binding is worth reading rather than
				// squeezing.
				if (ui.stage_expanded[i])
				{
					ImGui::Indent();
					ImGui::TextDisabled("Bypass");
					ImGui::SameLine();

					if (DrawLearnButton(state, "learn-bypass:" + std::to_string(i), stage.bypass_binding))
					{
						state.learn_request.target = LearnTarget::ToggleBypass;
						state.learn_request.stage = i;
					}

					ImGui::Unindent();
				}

				if (ui.stage_expanded[i])
				{
					ImGui::Indent();

					// Files first: which model is loaded matters more than
					// where a knob is, and it's what someone opens a stage to
					// change.
					DrawStagePaths(state, ui, i, stage);
					DrawStageControls(state, i, stage);

					ImGui::Unindent();
					ImGui::Spacing();
				}

				ImGui::PopID();
			}

			ImGui::Spacing();

			// Appends to the end. The stage list already has move up/down, so
			// "add then position it" is one story rather than two ways to say
			// where a plugin goes.
			if (ImGui::Button("Add stage"))
			{
				ui.plugin_search[0] = '\0';
				ImGui::OpenPopup("Add stage");
			}

			RecordWidget(state, "add-stage", 0, true);

			DrawPluginBrowser(state, ui);

			// Drawn at the same level as the plugin browser: a modal has to be
			// pumped from the same place it was opened, every frame, or Dear
			// ImGui closes it.
			DrawFilePicker(state, ui);
		}

		ImGui::Spacing();
		ImGui::Separator();

		// Closing the window is the ordinary way out; this is here so the
		// shutdown path has a second, deliberate trigger to test against.
		if (ImGui::Button("Quit"))
		{
			state.quit_requested = true;
		}

		ImGui::SameLine();

		const bool save_blocked = !state.save_blocked_reason.empty();
		ImGui::BeginDisabled(!state.unsaved_changes || save_blocked);

		if (ImGui::Button("Save preset"))
		{
			state.save_requested = true;
		}

		RecordWidget(state, "save", 0, state.unsaved_changes && !save_blocked);
		ImGui::EndDisabled();

		// AllowWhenDisabled, because the whole point of the tooltip is to
		// explain a button that can't be pressed.
		if (save_blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("%s", state.save_blocked_reason.c_str());
		}

		if (state.unsaved_changes)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("unsaved changes");
		}

		if (!state.status.empty())
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", state.status.c_str());
		}
	}

	ImGui::End();
}

}  // namespace sf
