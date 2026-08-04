#include "StageCard.h"

#include <cstdio>

#include "imgui.h"

#include "Fonts.h"
#include "Theme.h"
#include "Widgets.h"

namespace sf
{

namespace
{

// The three square buttons on a card's right-hand end.
constexpr float kIconButton = 24.0f;

// The disclosure arrow's hit area, and the room its column takes.
constexpr float kArrowWidth = 14.0f;

ProbeRecorder Probes(BoardViewState& state)
{
	return ProbeRecorder{state.record_widgets ? &state.widgets : nullptr};
}

// "/home/x/Some Pack/Capture 16.nam" -> "Capture 16.nam". The full path goes
// in a tooltip; a row that shows it in full pushes everything else off the
// card.
std::string FileName(const std::string& path)
{
	const size_t cut = path.find_last_of('/');

	return cut == std::string::npos ? path : path.substr(cut + 1);
}

// The design's ▸ / ▾, drawn rather than set as text.
//
// **Neither Space Grotesk nor IBM Plex Mono carries U+25B8 or U+25BE** -- this
// was checked, not assumed (SF-61) -- so setting them as characters would give
// a box in both faces. ImGui's own ArrowButton draws the triangle as geometry,
// which is what the design's glyph was standing in for anyway.
bool DisclosureArrow(const char* id, bool open)
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kBgSurfaceRaised);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kGray700);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

	const bool pressed = ImGui::ArrowButton(id, open ? ImGuiDir_Down : ImGuiDir_Right);

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(4);

	return pressed;
}

// A 24px square in mono: `^`, `v`, `x`.
bool IconButton(const char* label, bool enabled, const char* tooltip)
{
	ImGui::BeginDisabled(!enabled);

	ImGui::PushStyleColor(ImGuiCol_Button, theme::kBgSurfaceRaised);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kGray700);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kGray600);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);

	bool pressed = false;

	{
		ScopedFont mono(Face::Mono, text::kXs);
		pressed = ImGui::Button(label, ImVec2(kIconButton, kIconButton));
	}

	ImGui::PopStyleColor(4);
	ImGui::EndDisabled();

	// AllowWhenDisabled: the whole point of the tooltip on a greyed `x` is to
	// explain why it is greyed.
	if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("%s", tooltip);
	}

	return pressed;
}

}  // namespace

// The file-valued parameters of one stage: which model or IR is loaded, and a
// button to change it.
void DrawStagePaths(
	BoardViewState& state, BoardViewUi& ui, size_t stage_index, const BoardStage& stage, const char* probe_prefix)
{
	for (size_t p = 0; p < stage.paths.size(); ++p)
	{
		const BoardPathProperty& path = stage.paths[p];

		ImGui::PushID(static_cast<int>(p));

		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(theme::kTextSecondary, "%s", path.label.c_str());
		ImGui::SameLine(0.0f, theme::kSpace3);

		if (widgets::SecondaryButton(path.value.empty() ? "(none) -- choose..." : FileName(path.value).c_str()))
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

		Probes(state).Record(probe_prefix + std::string("path:") + std::to_string(stage_index * 100 + p), true);

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", path.value.empty() ? path.uri.c_str() : path.value.c_str());
		}

		ImGui::PopID();
	}
}

// A widget per control port, chosen by what the plugin says the port is rather
// than by its range alone.
void DrawStageControls(
	BoardViewState& state, size_t stage_index, const BoardStage& stage, const char* probe_prefix)
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
		const std::string probe = probe_prefix + std::string("control:") + std::to_string(stage_index) + ":" + control.symbol;

		if (!control.writable)
		{
			// A meter or readout. Shown because it says what the plugin is
			// doing, but there is nothing to drag.
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine();

			{
				ScopedFont mono(Face::Mono, text::kXs);
				ImGui::TextDisabled("%.3f", control.value);
			}

			// Recorded as disabled rather than not recorded, so a test can
			// tell "shown but not editable" from "not shown at all".
			Probes(state).Record(probe, false);

			ImGui::PopID();
			continue;
		}

		bool changed = false;
		float value = control.value;

		// Room for the learn button and its binding text on the same row.
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 140.0f);

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
			const ImGuiSliderFlags flags =
				control.logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;

			changed = ImGui::SliderFloat(label, &value, control.minimum, control.maximum, "%.3f", flags);
		}

		Probes(state).Record(probe, true);

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

		if (widgets::LearnButton(Probes(state), "learn-" + probe, control.binding, state.learning))
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

void DrawStageCard(BoardViewState& state, BoardViewUi& ui, BoardNodeId node, const StageCardPlace& place)
{
	static const BoardStage kMissing;

	const bool built = place.stage != kNoPreset && state.stages && place.stage < state.stages->size();
	const BoardStage& stage = built ? (*state.stages)[place.stage] : kMissing;

	const bool expanded = ui.IsExpanded(node);
	const std::string id = "stage-card:" + std::to_string(node);

	ImGui::PushID(id.c_str());

	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgSurface);
	ImGui::PushStyleColor(ImGuiCol_Border, built ? theme::kBorderHairline : theme::kMeterClip);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusCard);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace4, theme::kSpace3));

	// AutoResizeY: the card is as tall as what is in it, and what is in it
	// depends on how many controls the plugin has. A fixed height would either
	// clip a big plugin or leave a hole under a small one.
	ImGui::BeginChild(id.c_str(), ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// --- header row ---

	if (DisclosureArrow("expand", expanded))
	{
		ui.SetExpanded(node, !expanded);
	}

	Probes(state).Record("expand:" + std::to_string(node), true);
	ImGui::SameLine(0.0f, theme::kSpace2);

	// **Checked means active.** Inverted from the flag it drives, and from
	// every previous version of this checkbox, because "bypass" as a tick box
	// reads backwards: a ticked pedal is one that is doing something.
	bool active = !stage.bypassed;

	ImGui::BeginDisabled(!built);

	if (ImGui::Checkbox("##active", &active))
	{
		state.stage_edit = StageEdit::ToggleBypass;
		state.stage_edit_index = place.stage;
	}

	ImGui::EndDisabled();

	Probes(state).Record("bypass:" + std::to_string(place.stage), built, active);

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("%s", built ? "Bypass this stage" : "This stage is not running");
	}

	ImGui::SameLine(0.0f, theme::kSpace2);
	ImGui::AlignTextToFramePadding();

	const std::string name = built ? stage.name : std::string("(not installed)");

	if (stage.bypassed || !built)
	{
		ImGui::TextDisabled("%s", name.c_str());
	}
	else
	{
		ImGui::TextUnformatted(name.c_str());
	}

	if (ImGui::IsItemHovered())
	{
		// The flat index lives here rather than in the corner: the corner
		// shows where the card *is*, and for a stage in a lane the two are
		// different numbers (see StageCardPlace::code).
		if (built)
		{
			ImGui::SetTooltip("%s\nstage %zu in the running chain", stage.uri.c_str(), place.stage);
		}
		else
		{
			ImGui::SetTooltip("This preset names a plugin that isn't installed.");
		}
	}

	// The right-hand end: the code, then the three buttons. Laid out from the
	// right edge so adding a button is a change to one number.
	const float buttons = kIconButton * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;

	{
		ScopedFont mono(Face::Mono, text::kXs);

		const float code_width = ImGui::CalcTextSize(place.code.c_str()).x;

		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttons - theme::kSpace3 - code_width);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(theme::kTextSecondary, "%s", place.code.c_str());
	}

	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttons);

	if (IconButton("^", !place.first, "Move earlier in its lane"))
	{
		state.graph_edit = GraphEdit::MoveEarlier;
		state.graph_edit_node = node;
	}

	Probes(state).Record("up:" + std::to_string(node), !place.first);
	ImGui::SameLine();

	if (IconButton("v", !place.last, "Move later in its lane"))
	{
		state.graph_edit = GraphEdit::MoveLater;
		state.graph_edit_node = node;
	}

	Probes(state).Record("down:" + std::to_string(node), !place.last);
	ImGui::SameLine();

	if (IconButton("x", !place.only_stage,
			place.only_stage ? "A chain needs at least one stage" : "Take this stage off the board"))
	{
		state.graph_edit = GraphEdit::Remove;
		state.graph_edit_node = node;
	}

	Probes(state).Record("remove:" + std::to_string(node), !place.only_stage);

	// --- body ---

	if (expanded && built)
	{
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
		ImGui::Indent();

		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(theme::kTextSecondary, "Bypass");
		ImGui::SameLine(0.0f, theme::kSpace3);

		if (widgets::LearnButton(Probes(state), "learn-bypass:" + std::to_string(place.stage), stage.bypass_binding,
				state.learning))
		{
			state.learn_request.target = LearnTarget::ToggleBypass;
			state.learn_request.stage = place.stage;
		}

		// Right-aligned on the same row as Bypass, and only for plugins that
		// have an editor to talk about (SF-79). Most do not, and a button that
		// opened a modal saying "this plugin has no editor" would be clutter
		// on every card to serve the few that do.
		if (stage.has_custom_ui)
		{
			const char* kOpen = "Open plugin UI";

			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - widgets::ButtonWidth(kOpen));

			if (widgets::SecondaryButton(kOpen))
			{
				ui.plugin_ui_stage = place.stage;
			}

			Probes(state).Record("plugin-ui:" + std::to_string(place.stage), true);
		}

		// Files first: which model is loaded matters more than where a knob
		// is, and it's what someone opens a pedal to change.
		DrawStagePaths(state, ui, place.stage, stage);
		DrawStageControls(state, place.stage, stage);

		ImGui::Unindent();
	}
	else if (expanded)
	{
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
		ImGui::Indent();
		ImGui::TextDisabled("Nothing to show -- this stage isn't running.");
		ImGui::Unindent();
	}

	ImGui::EndChild();

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(2);
	ImGui::PopID();
}

}  // namespace sf
