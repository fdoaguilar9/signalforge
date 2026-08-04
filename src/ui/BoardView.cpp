#include "BoardView.h"

#include "Fonts.h"
#include "ChainView.h"
#include "StageCard.h"
#include "Theme.h"

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

// --- the window's horizontal bands (SF-62) ---
//
// The window is three bands: a header, the body, and a status bar, with the
// learn strip and the missing-plugin warning appearing between the first two
// when they have something to say. Each band is full width, paints its own
// background, and is separated from its neighbour by a 1px hairline.
//
// Bands are child windows rather than drawn rectangles because each needs its
// own clipping and its own cursor -- the header lays out left to right and the
// body scrolls, and neither should be able to spill into the other.

enum class BandEdge
{
	None,
	Top,
	Bottom
};

// A 1px rule, snapped to the pixel grid.
//
// Not ImDrawList::AddLine, which anti-aliases: a horizontal line whose
// coordinate isn't exactly on a pixel centre gets spread across two rows, and
// since the spread depends on the coordinate, **the same rule comes out 1px
// thick at one end and 2px at the other**. This whole design is hairlines, and
// a hairline that can't decide how thick it is reads as a rendering fault.
// A filled rect on whole pixels cannot do that.
void HairlineH(ImDrawList* draw, float x0, float x1, float y, ImU32 colour)
{
	draw->AddRectFilled(ImVec2(std::floor(x0), std::floor(y)), ImVec2(std::floor(x1), std::floor(y) + 1.0f), colour);
}

void HairlineV(ImDrawList* draw, float y0, float y1, float x, ImU32 colour)
{
	draw->AddRectFilled(ImVec2(std::floor(x), std::floor(y0)), ImVec2(std::floor(x) + 1.0f, std::floor(y1)), colour);
}

// A band's own padding, since the ordinary window padding is turned off for
// the frame: bands butt against each other, and 16px of window padding at the
// top of the window would put the header 16px down from the title bar that
// isn't there.
bool BeginBand(const char* id, float height, const ImVec4& background, const ImVec2& padding)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);

	ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// The bands themselves stack flush; what is *inside* one is spaced
	// normally. Without this the header's items would sit against each other.
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kSpace2, theme::kSpace2));

	return true;
}

void EndBand(BandEdge edge)
{
	ImGui::PopStyleVar();  // the item spacing pushed inside the band
	ImGui::EndChild();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor();

	if (edge == BandEdge::None)
	{
		return;
	}

	// Drawn on the parent's list after the child is closed, so it lands *on*
	// the boundary rather than one pixel inside whichever band drew it.
	const ImVec2 low = ImGui::GetItemRectMin();
	const ImVec2 high = ImGui::GetItemRectMax();
	const float y = edge == BandEdge::Top ? low.y : high.y - 1.0f;

	HairlineH(ImGui::GetWindowDrawList(), low.x, high.x, y, ImGui::GetColorU32(theme::kBorderHairline));
}

// Puts the cursor where a single row of `height` sits centred in the band it
// is in. Every band here is one row tall.
void CentreRow(float band_height, float row_height)
{
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (band_height - row_height) * 0.5f);
}

// A 1px vertical rule, the header's one piece of furniture.
void VerticalDivider(float height)
{
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float top = at.y + (ImGui::GetFrameHeight() - height) * 0.5f;

	HairlineV(ImGui::GetWindowDrawList(), top, top + height, at.x, ImGui::GetColorU32(theme::kBorderHairline));

	ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
}

// The design's lightest button weight: no fill until it is pointed at. Local
// to this file for now -- the shared widget set (SF-63) is where the three
// weights come to live together, and writing that here would be writing it
// twice.
bool GhostButton(const char* label)
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kBgSurfaceRaised);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kGray700);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

	const bool pressed = ImGui::Button(label);

	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);

	return pressed;
}

// What a button of this label costs in width, for laying a row out from the
// right-hand end rather than by trial and error.
float ButtonWidth(const char* label)
{
	return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// The browser holds every plugin on the machine, so it wants most of the
// window rather than a corner of it.
constexpr float kBrowserWidth = 560.0f;
constexpr float kBrowserHeight = 420.0f;

// Tall enough to show a working board's worth of bindings without the modal
// growing past the window on a small screen. Past that it scrolls, which is
// why the header row is frozen.
constexpr float kMidiMapTableHeight = 360.0f;

// The design's 200px well: enough to read as "a thing goes here" rather than
// as a gap someone forgot to fill.
constexpr float kPluginUiWellHeight = 200.0f;

// The tuner's measurements from the design (SF-81). "In tune" is +/-5 cents:
// tighter than a guitar holds between chords, and looser than the detector's
// own error, which is a fraction of a cent.
constexpr float kTunerWidth = 440.0f;
constexpr float kTunerTrack = 40.0f;
constexpr float kTunerNeedle = 3.0f;
constexpr float kTunerInTuneCents = 5.0f;

// The metronome's, likewise (SF-82).
constexpr float kMetronomeWidth = 460.0f;
constexpr float kMetronomeDot = 14.0f;

// The live screen's measurements, all from the design (SF-77). They are larger
// than anything on the board screen for one reason: this is the screen that is
// read from two metres away with a guitar on.
constexpr float kLivePadX = 48.0f;
constexpr float kLivePadY = 32.0f;
constexpr float kLiveGap = 28.0f;
constexpr float kLiveTile = 104.0f;
constexpr float kLiveStomp = 64.0f;
constexpr float kLiveMeter = 20.0f;

// The probe sink for this frame, in the form the shared widgets take.
ProbeRecorder Probes(BoardViewState& state)
{
	return ProbeRecorder{state.record_widgets ? &state.widgets : nullptr};
}

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

	state.widgets.push_back({id, (low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, ImGui::GetItemID(), enabled});
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

// Which stage of the running chain a node is, searching the shape rather than
// assuming the two are in step: a node whose plugin isn't installed has no
// stage, and every node after it is offset by one.
size_t StageOfNode(const BoardGraphNodes& nodes, BoardNodeId id)
{
	for (const BoardGraphNode& node : nodes)
	{
		if (!node.split)
		{
			if (node.id == id)
			{
				return node.stage == kNoStageIndex ? kNoPreset : node.stage;
			}

			continue;
		}

		for (const BoardGraphBranch& branch : node.branches)
		{
			const size_t found = StageOfNode(branch.nodes, id);

			if (found != kNoPreset)
			{
				return found;
			}
		}
	}

	return kNoPreset;
}

// The first node that is actually playing, for a selection that has to land
// somewhere.
BoardNodeId FirstNodeWithStage(const BoardGraphNodes& nodes)
{
	for (const BoardGraphNode& node : nodes)
	{
		if (!node.split)
		{
			if (node.stage != kNoStageIndex)
			{
				return node.id;
			}

			continue;
		}

		for (const BoardGraphBranch& branch : node.branches)
		{
			const BoardNodeId found = FirstNodeWithStage(branch.nodes);

			if (found != kNoBoardNode)
			{
				return found;
			}
		}
	}

	return kNoBoardNode;
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

// One of the two board-wide trims: a slider and a learn button (SF-66).
//
// -24..+24 dB from the design. Not a plugin control, so it takes none of
// DrawStageControls' hint machinery -- there is exactly one shape this can be.
void DrawGainSlider(BoardViewState& state, const char* label, float value, const std::string& binding,
	float& request, LearnTarget learn)
{
	const std::string probe = std::string("gain:") + (learn == LearnTarget::SetInputGain ? "input" : "output");

	ImGui::PushID(probe.c_str());

	float edited = value;

	// Room for the learn button and its binding text on the same row.
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);

	if (ImGui::SliderFloat(label, &edited, -24.0f, 24.0f, "%+.1f dB"))
	{
		request = edited;
	}

	RecordWidgetNamed(state, probe, true);

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%s", learn == LearnTarget::SetInputGain
				? "Trim before the first stage. Sets how hard the board is driven, not how loud it is."
				: "Trim after the last stage. Matches the board to whatever it is plugged into.");
	}

	ImGui::SameLine();

	if (widgets::LearnButton(ProbeRecorder{state.record_widgets ? &state.widgets : nullptr},
			std::string("learn-") + probe, binding, state.learning))
	{
		state.learn_request.target = learn;
		state.learn_request.minimum = -24.0f;
		state.learn_request.maximum = 24.0f;
	}

	ImGui::PopID();
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
// Every binding the engine would resolve against, as a table (SF-78).
//
// Read-only and **derived**: the caller renders the live `MidiMap` into rows
// each time it changes, and this draws them. There is no stored copy to fall
// behind, which is what makes a binding just made through learn appear here
// without anything having to remember to refresh.
void DrawMidiMap(BoardViewState& state, BoardViewUi& ui)
{
	if (ui.midi_map_open && !ImGui::IsPopupOpen("MIDI mappings"))
	{
		ImGui::OpenPopup("MIDI mappings");
	}

	if (!widgets::BeginModal("MIDI mappings", 680.0f))
	{
		return;
	}

	static const std::vector<BoardMidiBinding> kNone;
	const std::vector<BoardMidiBinding>& bindings = state.midi_map ? *state.midi_map : kNone;

	if (bindings.empty())
	{
		ImGui::TextDisabled("No bindings yet -- press learn next to anything bindable.");
	}
	else if (ImGui::BeginTable("midi-map", 4,
				 ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH,
				 ImVec2(0.0f, kMidiMapTableHeight)))
	{
		// 110 / 130 / 1fr / 90 from the design: the message and the action are
		// fixed-width because they are short and want to line up, and the
		// target takes what is left because it is the one that can be long.
		ImGui::TableSetupColumn("MESSAGE", ImGuiTableColumnFlags_WidthFixed, 110.0f);
		ImGui::TableSetupColumn("ACTION", ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn("TARGET", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("RANGE", ImGuiTableColumnFlags_WidthFixed, 90.0f);

		// Sticky: the listing scrolls and a header that scrolled with it would
		// leave four unlabelled columns.
		ImGui::TableSetupScrollFreeze(0, 1);

		{
			ScopedFont label(Face::Mono, text::k2xs);
			ImGui::TableHeadersRow();
		}

		for (size_t row = 0; row < bindings.size(); ++row)
		{
			const BoardMidiBinding& binding = bindings[row];

			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			{
				ScopedFont mono(Face::Mono, text::kXs);
				ImGui::TextColored(theme::kAccent, "%s", binding.message.c_str());
			}

			ImGui::TableNextColumn();
			{
				ScopedFont mono(Face::Mono, text::kXs);
				ImGui::TextColored(theme::kTextSecondary, "%s", binding.action.c_str());
			}

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(binding.target.c_str());

			ImGui::TableNextColumn();
			{
				ScopedFont mono(Face::Mono, text::kXs);

				// `--` rather than a blank cell where the action takes no
				// value: an empty cell reads as a missing number, and "this
				// action has no range" is a different thing from that.
				ImGui::TextDisabled("%s", binding.range.empty() ? "--" : binding.range.c_str());
			}

			// Recorded as disabled -- there is nothing to click on a
			// read-only row -- but recorded, so a test can count the rows and
			// hold the table to the same number the sidebar shows.
			RecordWidget(state, "midi-map:row", row, false);
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();

	{
		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("%zu binding%s", bindings.size(), bindings.size() == 1 ? "" : "s");
	}

	ImGui::SameLine();

	const char* kClose = "Close";
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ButtonWidth(kClose));

	if (widgets::SecondaryButton(kClose) || widgets::ModalDismissed())
	{
		ui.midi_map_open = false;
		ImGui::CloseCurrentPopup();
	}

	RecordWidgetNamed(state, "midi-map:close", true);

	widgets::EndModal();
}

// The metronome (SF-82). A row of dots, a tempo, and the two buttons a foot or
// a hand reaches for.
//
// Nothing here counts anything: beats are scheduled from a sample counter on
// the audio thread, and this reads which one is sounding. A UI that counted
// its own beats would drift against the click within a bar.
void DrawMetronome(BoardViewState& state, BoardViewUi& ui)
{
	if (ui.metronome_open && !ImGui::IsPopupOpen("Metronome"))
	{
		ImGui::OpenPopup("Metronome");
	}

	if (!widgets::BeginModal("Metronome", kMetronomeWidth))
	{
		return;
	}

	// The dots, and the tempo beside them. One dot per beat of the bar, the
	// sounding one amber -- which is the whole of what a metronome display is
	// for: knowing where in the bar you are without counting.
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();

		const unsigned beats = std::max(1u, state.metronome.beats_per_bar);

		for (unsigned i = 0; i < beats; ++i)
		{
			const float x = origin.x + kMetronomeDot * 0.5f + static_cast<float>(i) * (kMetronomeDot + theme::kSpace2);
			const bool on = state.metronome.running && i == state.metronome.beat;

			draw->AddCircleFilled(ImVec2(x, origin.y + kMetronomeDot * 0.5f), kMetronomeDot * 0.5f,
				ImGui::GetColorU32(on ? theme::kAccent : theme::kBgSurfaceRaised));
		}

		ImGui::Dummy(ImVec2(static_cast<float>(beats) * (kMetronomeDot + theme::kSpace2), kMetronomeDot));
		ImGui::SameLine(0.0f, theme::kSpace4);

		ScopedFont big(Face::Mono, text::kLg);
		ImGui::Text("%.0f BPM", static_cast<double>(state.metronome.tempo));
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	// Tempo, with a learn button: this is the control most worth putting on an
	// expression pedal.
	{
		float tempo = state.metronome.tempo;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 140.0f);

		if (ImGui::SliderFloat("Tempo", &tempo, 40.0f, 240.0f, "%.0f BPM"))
		{
			state.metronome_tempo_request = tempo;
		}

		RecordWidgetNamed(state, "metronome:tempo", true);
		ImGui::SameLine();

		if (widgets::LearnButton(Probes(state), "learn-metronome:tempo", state.metronome.tempo_binding, state.learning))
		{
			state.learn_request.target = LearnTarget::SetTempo;
			state.learn_request.minimum = 40.0f;
			state.learn_request.maximum = 240.0f;
		}
	}

	{
		static const char* const kSignatures[] = {"4/4", "3/4", "6/8", "2/4"};
		static const unsigned kBeats[] = {4, 3, 6, 2};

		int selected = 0;

		for (int i = 0; i < 4; ++i)
		{
			if (kBeats[i] == state.metronome.beats_per_bar)
			{
				selected = i;
			}
		}

		ImGui::SetNextItemWidth(-1.0f);

		if (ImGui::BeginCombo("##time-signature", kSignatures[selected]))
		{
			for (int i = 0; i < 4; ++i)
			{
				if (ImGui::Selectable(kSignatures[i], i == selected) && i != selected)
				{
					state.metronome_beats_request = kBeats[i];
				}

				RecordWidgetNamed(state, std::string("metronome:signature:") + kSignatures[i], true);
			}

			ImGui::EndCombo();
		}

		RecordWidgetNamed(state, "metronome:signature", true);
	}

	{
		float level = state.metronome.level * 100.0f;

		ImGui::SetNextItemWidth(-1.0f);

		if (ImGui::SliderFloat("##level", &level, 0.0f, 100.0f, "Level  %.0f%%"))
		{
			state.metronome_level_request = level / 100.0f;
		}

		RecordWidgetNamed(state, "metronome:level", true);
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	if (widgets::PrimaryButton(state.metronome.running ? "Stop" : "Start"))
	{
		state.metronome_toggle_requested = true;
	}

	RecordWidgetNamed(state, "metronome:toggle", true);
	ImGui::SameLine();

	if (widgets::SecondaryButton("Tap tempo"))
	{
		state.metronome_tap_requested = true;
	}

	RecordWidgetNamed(state, "metronome:tap", true);
	ImGui::SameLine();

	if (widgets::LearnButton(Probes(state), "learn-metronome:toggle", state.metronome.binding, state.learning))
	{
		state.learn_request.target = LearnTarget::ToggleMetronome;
	}

	ImGui::SameLine();

	const char* kClose = "Close";
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ButtonWidth(kClose));

	if (widgets::SecondaryButton(kClose) || widgets::ModalDismissed())
	{
		ui.metronome_open = false;
		ImGui::CloseCurrentPopup();
	}

	RecordWidgetNamed(state, "metronome:close", true);

	widgets::EndModal();
}

// The tuner (SF-81). A note, a needle, and a number.
//
// **The board is muted while this is open**, and the mute is the output trim
// from SF-66 rather than a second way of silencing the board -- one thing that
// can take the sound away is one thing to check when it does not come back.
// The caller owns that; what this owns is saying so, in the words the design
// gives, at the top where it cannot be missed.
//
// Nothing here computes anything. Which note a frequency is, is arithmetic,
// and arithmetic in the drawing code is arithmetic nothing can test -- the
// caller hands over a note and a distance from it, both already decided by
// something with a test around it.
void DrawTuner(BoardViewState& state, BoardViewUi& ui)
{
	if (ui.tuner_open && !ImGui::IsPopupOpen("Tuner"))
	{
		ImGui::OpenPopup("Tuner");
	}

	if (!widgets::BeginModal("Tuner", kTunerWidth))
	{
		return;
	}

	{
		ScopedFont label(Face::Mono, text::k2xs);
		widgets::SectionLabel("OUTPUT IS MUTED WHILE THE TUNER IS OPEN", theme::kStatusWarning);
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace3));

	const bool in_tune = state.tuner.heard && std::fabs(state.tuner.cents) <= kTunerInTuneCents;
	const ImVec4 verdict = in_tune ? theme::kStatusOk : theme::kStatusWarning;

	// The note, centred and enormous: this is read from wherever the guitar
	// is, which is not in front of the screen.
	{
		ScopedFont huge(Face::Ui, text::kTunerNote);

		const char* note = state.tuner.heard ? state.tuner.note.c_str() : "--";
		const float width = ImGui::CalcTextSize(note).x;

		ImGui::SetCursorPosX((ImGui::GetContentRegionMax().x - width) * 0.5f);
		ImGui::TextColored(state.tuner.heard ? verdict : theme::kTextDisabled, "%s", note);
	}

	{
		ScopedFont mono(Face::Mono, text::kSm);

		char hz[32];
		std::snprintf(hz, sizeof(hz), "%.1f Hz", static_cast<double>(state.tuner.frequency_hz));

		const char* text = state.tuner.heard ? hz : "play a string";
		const float width = ImGui::CalcTextSize(text).x;

		ImGui::SetCursorPosX((ImGui::GetContentRegionMax().x - width) * 0.5f);
		ImGui::TextDisabled("%s", text);
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace3));

	// The needle. Drawn rather than assembled from widgets: it is a position
	// on a track, and there is nothing here to click.
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		const float centre_x = origin.x + width * 0.5f;

		draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + kTunerTrack),
			ImGui::GetColorU32(theme::kBgInset), theme::kRadiusCard);

		// The centre, and +/-25 cents either side of it, so the scale is read
		// off the track rather than only off the number below it.
		for (int cents = -50; cents <= 50; cents += 25)
		{
			const float x = centre_x + width * 0.5f * static_cast<float>(cents) / 50.0f;
			const bool middle = cents == 0;

			widgets::HairlineV(draw, origin.y + (middle ? 0.0f : kTunerTrack * 0.3f), origin.y + kTunerTrack, x,
				ImGui::GetColorU32(middle ? theme::kTextSecondary : theme::kBorderHairline));
		}

		if (state.tuner.heard)
		{
			// Clamped to the track: a reading past +/-50 cents is the next
			// note, and a needle off the end would be pointing at nothing.
			const float cents = std::min(std::max(state.tuner.cents, -50.0f), 50.0f);
			const float x = centre_x + width * 0.5f * cents / 50.0f;

			draw->AddRectFilled(ImVec2(x - kTunerNeedle * 0.5f, origin.y),
				ImVec2(x + kTunerNeedle * 0.5f, origin.y + kTunerTrack), ImGui::GetColorU32(verdict));
		}

		ImGui::Dummy(ImVec2(width, kTunerTrack));
	}

	{
		ScopedFont mono(Face::Mono, text::k2xs);

		// The scale, laid out on the same halves the ticks were.
		const float width = ImGui::GetContentRegionAvail().x;
		static const char* const kMarks[] = {"-50", "-25", "0", "+25", "+50"};

		const ImVec2 origin = ImGui::GetCursorScreenPos();

		for (int i = 0; i < 5; ++i)
		{
			const float x = origin.x + width * static_cast<float>(i) / 4.0f;
			const float text_width = ImGui::CalcTextSize(kMarks[i]).x;

			ImGui::SetCursorScreenPos(ImVec2(std::min(std::max(x - text_width * 0.5f, origin.x),
										   origin.x + width - text_width),
				origin.y));

			ImGui::TextDisabled("%s", kMarks[i]);
		}
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	{
		ScopedFont mono(Face::Mono, text::kSm);

		char cents[32];
		std::snprintf(cents, sizeof(cents), "%+.0f cents", static_cast<double>(state.tuner.cents));

		const char* text = state.tuner.heard ? cents : "";
		const float width = ImGui::CalcTextSize(text).x;

		ImGui::SetCursorPosX((ImGui::GetContentRegionMax().x - width) * 0.5f);
		ImGui::TextColored(verdict, "%s", text);
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	const char* kClose = "Close";
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ButtonWidth(kClose));

	if (widgets::SecondaryButton(kClose) || widgets::ModalDismissed())
	{
		ui.tuner_open = false;
		ImGui::CloseCurrentPopup();
	}

	RecordWidgetNamed(state, "tuner:close", true);

	widgets::EndModal();
}

// The plugin-UI modal (SF-79): an honest placeholder where the plugin's own
// editor would go, over the generic controls that are the actual fallback.
//
// **The well is not a promise.** Embedding a plugin's editor through suil is a
// v1 non-goal (PLAN.md §2) and stays one. What this modal is for is the player
// who has seen the plugin's own screenshots and wants to know why the knobs
// here look different -- saying so beats leaving them to conclude the plugin is
// broken. The controls under it are the same rows the card draws, and they have
// to keep working whether or not the well ever becomes real.
void DrawPluginUi(BoardViewState& state, BoardViewUi& ui)
{
	if (ui.plugin_ui_stage != kNoPreset && !ImGui::IsPopupOpen("Plugin UI"))
	{
		ImGui::OpenPopup("Plugin UI");
	}

	if (!widgets::BeginModal("Plugin UI", 640.0f))
	{
		return;
	}

	// A stage can go while its modal is open -- a preset switch replaces the
	// whole chain -- so the modal has to survive being about nothing.
	static const BoardStage kGone;

	const bool built = state.stages && ui.plugin_ui_stage < state.stages->size();
	const BoardStage& stage = built ? (*state.stages)[ui.plugin_ui_stage] : kGone;

	ImGui::TextUnformatted(built ? stage.name.c_str() : "(no stage)");

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	// The well: 200px of inset surface saying what would be here.
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgInset);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusCard);

		ImGui::BeginChild("plugin-ui-well", ImVec2(0.0f, kPluginUiWellHeight), ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		const float centre = ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetTextLineHeight();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, centre));

		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("The plugin's own editor (suil embed) renders here.");
		ImGui::PopTextWrapPos();

		{
			ScopedFont mono(Face::Mono, text::kXs);
			ImGui::TextDisabled("%s", built ? stage.uri.c_str() : "");
		}

		ImGui::EndChild();

		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace3));
	widgets::SectionLabel("GENERIC CONTROLS");
	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

	if (built)
	{
		// The same rows the card draws, from the same data, reported through
		// the same fields -- so a knob moved here reaches the running plugin by
		// the path that already works rather than a second one beside it.
		DrawStagePaths(state, ui, ui.plugin_ui_stage, stage, "plugin-ui:");
		DrawStageControls(state, ui.plugin_ui_stage, stage, "plugin-ui:");
	}
	else
	{
		ImGui::TextDisabled("That stage is no longer in the chain.");
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	const char* kClose = "Close";
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ButtonWidth(kClose));

	if (widgets::SecondaryButton(kClose) || widgets::ModalDismissed())
	{
		ui.plugin_ui_stage = kNoPreset;
		ImGui::CloseCurrentPopup();
	}

	RecordWidgetNamed(state, "plugin-ui:close", true);

	widgets::EndModal();
}

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

	// Where this is going. Said out loud because "add" used to mean one thing
	// -- the end of the chain -- and now means wherever the "+" that was
	// pressed was.
	if (ui.pending_slot_valid)
	{
		if (ui.pending_slot.split == kNoBoardNode)
		{
			ImGui::TextDisabled("Inserting at position %zu of the main line", ui.pending_slot.index);
		}
		else
		{
			ImGui::TextDisabled("Inserting into lane %u of the split, at position %zu", ui.pending_slot.branch,
				ui.pending_slot.index);
		}
	}
	else
	{
		ImGui::TextDisabled("Adding to the end of the chain");
	}

	// A split goes in the same places a pedal does, so it is offered in the
	// same place rather than as a separate mode with its own buttons on the
	// board. Both lanes start empty: the split exists first, then pedals go
	// into it.
	if (ImGui::Button("Split here instead"))
	{
		state.graph_edit = GraphEdit::InsertSplit;
		state.graph_edit_slot = ui.pending_slot;
		ImGui::CloseCurrentPopup();
	}

	RecordWidget(state, "browser-split", 0, true);

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Put a parallel split here instead of a pedal.\nBoth lanes start empty -- an empty lane is a "
						  "straight wire.");
	}

	ImGui::Separator();

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

				// Carried on the request rather than left in `ui`, so the
				// caller applies the insert to the place that was clicked
				// even if something else has since changed the selection.
				state.graph_edit = ui.pending_slot_valid ? GraphEdit::InsertPlugin : GraphEdit::None;
				state.graph_edit_slot = ui.pending_slot;

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

// --- the body's two panes ---
//
// Fixed-width sidebar on the left, the chain taking whatever is left, each
// scrolling independently. That independence is the whole reason the body is
// two children rather than two columns: expanding a stage with thirty controls
// used to push the meters off the bottom of the window, because the meters and
// the chain shared one scrolling column.

void BeginSidebar()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace4, theme::kSpace4));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kSpace2, theme::kSpace2));

	ImGui::BeginChild("sidebar", ImVec2(theme::kSidebarWidth, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
}

void EndSidebar()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(4);

	// The hairline that separates the two panes, drawn on the body's list
	// after the child closes so it isn't clipped away by whichever pane owns
	// the pixel.
	const ImVec2 low = ImGui::GetItemRectMin();
	const ImVec2 high = ImGui::GetItemRectMax();

	HairlineV(ImGui::GetWindowDrawList(), low.y, high.y, high.x - 1.0f, ImGui::GetColorU32(theme::kBorderHairline));

	// Flush against the hairline: any spacing here would show as a seam.
	ImGui::SameLine(0.0f, 0.0f);
}

void BeginChainArea()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace6, theme::kSpace6));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kSpace2, theme::kSpace2));

	ImGui::BeginChild("chain", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
}

void EndChainArea()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(4);
}

// The top band: who this is, what is playing, and the ways out of the board.
//
// The breadcrumb is the only place the running preset is named now that the
// board has a sidebar full of presets that aren't playing -- so it carries the
// file path as its tooltip, which used to be the job of the line at the top of
// the window.
void DrawHeader(BoardViewState& state, BoardViewUi& ui)
{
	BeginBand("header", theme::kHeaderHeight, theme::kBgSurface, ImVec2(theme::kSpace4, 0.0f));

	CentreRow(theme::kHeaderHeight, ImGui::GetFrameHeight());

	{
		ScopedFont wordmark(Face::UiMedium, text::kMd);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Signalforge");
	}

	ImGui::SameLine(0.0f, theme::kSpace4);
	VerticalDivider(20.0f);

	if (!state.bank_name.empty())
	{
		ImGui::SameLine(0.0f, theme::kSpace4);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(theme::kTextSecondary, "%s", state.bank_name.c_str());

		ImGui::SameLine(0.0f, theme::kSpace2);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("›");
		ImGui::SameLine(0.0f, theme::kSpace2);
	}
	else
	{
		ImGui::SameLine(0.0f, theme::kSpace4);
	}

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(state.preset_name.empty() ? "(no preset)" : state.preset_name.c_str());

	if (!state.preset_path.empty() && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%s", state.preset_path.c_str());
	}

	// Board | Live (SF-77). Not one of the buttons to its right, and drawn
	// apart from them: those open something *over* the window, this chooses
	// what the window is.
	ImGui::SameLine(0.0f, theme::kSpace4);

	{
		static const char* const kScreens[] = {"Board", "Live"};

		const int pressed =
			widgets::SegmentedControl("screen", kScreens, 2, ui.live_screen ? 1 : 0, Probes(state), "screen");

		if (pressed >= 0)
		{
			ui.live_screen = pressed == 1;
		}
	}

	// Laid out from the right-hand end rather than after a spacer, so adding a
	// button here is a change to one number instead of a rebalancing act.
	// Right to left in the order they are written, so the code reads the way
	// the row does. The design's other two -- Tuner and Metronome -- arrive
	// with the screens they open (SF-81, SF-82) rather than sitting here doing
	// nothing in the meantime.
	struct HeaderButton
	{
		const char* label;
		const char* probe;
		bool* request;
	};

	const HeaderButton buttons[] = {
		{"Audio settings", "menu:audio-settings", &state.audio_settings_requested},
		{"MIDI map", "menu:midi-map", &state.midi_map_requested},
		{"Tuner", "menu:tuner", &state.tuner_requested},
		{"Metronome", "menu:metronome", &state.metronome_requested},
	};

	float right = ImGui::GetContentRegionMax().x;

	for (const HeaderButton& button : buttons)
	{
		right -= ButtonWidth(button.label);

		ImGui::SameLine();
		ImGui::SetCursorPosX(right);

		if (GhostButton(button.label))
		{
			*button.request = true;
		}

		RecordWidgetNamed(state, button.probe, true);

		right -= theme::kSpace2;
	}

	EndBand(BandEdge::Bottom);
}

// While a learn is armed this is the only thing on screen that matters, so it
// gets a band of its own directly under the header rather than a line in the
// status bar at the bottom.
void DrawLearnStrip(BoardViewState& state)
{
	if (!state.learning)
	{
		return;
	}

	const float height = ImGui::GetFrameHeight() + 10.0f * 2.0f;

	BeginBand("learn-strip", height, theme::kBgSurface, ImVec2(theme::kSpace4, 10.0f));

	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(theme::kStatusWarning, "Press a switch or move a pedal to bind %s.",
		state.learning_what.c_str());

	ImGui::SameLine(0.0f, theme::kSpace3);

	if (GhostButton("cancel"))
	{
		state.learn_request.target = LearnTarget::Cancel;
	}

	RecordWidget(state, "learn-cancel", 0, true);

	EndBand(BandEdge::Bottom);
}

// The board is not what its preset says it is (SF-31). A condition rather than
// an event, so unlike the status line it stays up until the condition does.
//
// Not a full-width band: the design draws it as an inset bordered box, which
// reads as "something is wrong with this board" rather than as another piece
// of window furniture.
void DrawWarningBanner(BoardViewState& state)
{
	if (state.warning.empty())
	{
		return;
	}

	const float side = theme::kSpace4;
	const float wrap = ImGui::GetContentRegionAvail().x - side * 2.0f - theme::kSpace3 * 2.0f;
	const float text_height = ImGui::CalcTextSize(state.warning.c_str(), nullptr, false, wrap).y;

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + side);

	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgSurface);
	ImGui::PushStyleColor(ImGuiCol_Border, theme::kStatusWarning);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace3, theme::kSpace2));

	ImGui::BeginChild("warning", ImVec2(ImGui::GetContentRegionAvail().x - side, text_height + theme::kSpace2 * 2.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ImGui::PushStyleColor(ImGuiCol_Text, theme::kStatusWarning);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(state.warning.c_str());
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();

	ImGui::EndChild();

	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
}

// The bottom band: whether there is anything to save, and what happened last.
void DrawStatusBar(BoardViewState& state)
{
	const float height = ImGui::GetFrameHeight() + theme::kSpace2 * 2.0f;

	BeginBand("status-bar", height, theme::kBgSurface, ImVec2(theme::kSpace4, theme::kSpace2));

	const bool save_blocked = !state.save_blocked_reason.empty();
	ImGui::BeginDisabled(!state.unsaved_changes || save_blocked);

	// The one primary-weight button in the program. Amber fill, dark text --
	// which is why the colours are pushed here rather than being the default
	// button treatment.
	ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccentActive);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccentOn);

	if (ImGui::Button("Save preset"))
	{
		state.save_requested = true;
	}

	ImGui::PopStyleColor(4);

	RecordWidget(state, "save", 0, state.unsaved_changes && !save_blocked);
	ImGui::EndDisabled();

	// AllowWhenDisabled, because the whole point of the tooltip is to explain a
	// button that can't be pressed.
	if (save_blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("%s", state.save_blocked_reason.c_str());
	}

	if (state.unsaved_changes)
	{
		ImGui::SameLine(0.0f, theme::kSpace3);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("unsaved changes");
	}

	if (!state.status.empty())
	{
		// Right-aligned and single-line: this says what the last action did,
		// and a status that wrapped to two lines would change the height of
		// the band it lives in.
		const float width = ImGui::CalcTextSize(state.status.c_str()).x;

		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - width);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(theme::kTextSecondary, "%s", state.status.c_str());
	}

	// **Quit is a key now, not a button** (SF-73). The design's status bar is
	// Save, the dirty caption and the status line, and closing the window is
	// how a player actually leaves. It stays reachable because the shutdown
	// path deserves a deliberate trigger that isn't the window manager --
	// Ctrl-Q, which is where every other program keeps it.
	//
	// Not recorded as a widget: there is nothing on screen to aim at, and a
	// probe for something invisible would be a lie to whatever clicked it.
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q))
	{
		state.quit_requested = true;
	}

	EndBand(BandEdge::Top);
}

}  // namespace

// The live screen (SF-77): the same running chain, drawn to be read from two
// metres away with a guitar on.
//
// **Nothing here edits anything.** It switches presets and it shows levels;
// every control that changes the board lives on the board screen. That is the
// whole argument for the type sizes -- a screen you can only read at arm's
// length would be a worse board screen rather than a live one.
void DrawLiveScreen(BoardViewState& state, BoardViewUi& ui)
{
	static const std::vector<BoardPreset> kNoPresets;
	const std::vector<BoardPreset>& presets = state.presets ? *state.presets : kNoPresets;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kLivePadX, kLivePadY));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kSpace3, kLiveGap));

	ImGui::BeginChild("live", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);

	if (!state.bank_name.empty())
	{
		ScopedFont label(Face::Mono, text::k2xs);
		widgets::SectionLabel(state.bank_name.c_str(), theme::kTextSecondary);
	}

	{
		ScopedFont huge(Face::Ui, text::kLivePreset);
		ImGui::TextUnformatted(state.preset_name.empty() ? "(no preset)" : state.preset_name.c_str());
	}

	// What the next stomp lands on. The single most useful thing to know
	// mid-song, and the reason the preset list below is worth glancing at
	// rather than reading.
	{
		const size_t next = presets.empty() ? kNoPreset : (state.current_preset + 1) % presets.size();

		ScopedFont mono(Face::Mono, text::kSm);
		ImGui::TextDisabled("Next: %s", next == kNoPreset ? "--" : presets[next].name.c_str());
	}

	ImGui::Dummy(ImVec2(0.0f, 0.0f));

	// Three columns of 104px tiles. A grid rather than a list because the
	// point is to find one by *position* -- the third one along, second row --
	// which is how a pedalboard is used and not how a list is read.
	if (!presets.empty() && ImGui::BeginTable("live-presets", 3, ImGuiTableFlags_SizingStretchSame))
	{
		for (size_t i = 0; i < presets.size(); ++i)
		{
			ImGui::TableNextColumn();

			const bool active = i == state.current_preset;

			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, active ? theme::kBgSurfaceRaised : theme::kBgSurface);
			ImGui::PushStyleColor(ImGuiCol_Border, active ? theme::kAccent : theme::kBorderHairline);
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusCard);
			ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, active ? 2.0f : 1.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace3, theme::kSpace3));

			ImGui::BeginChild("tile", ImVec2(0.0f, kLiveTile),
				ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

			// The whole tile is the target -- at two metres the tile *is* the
			// button. An invisible button covering it rather than the child
			// itself: a child window is not an item, it captures the mouse on
			// its own behalf, and `IsItemClicked` on one is silently never
			// true. Drawn first and then written over, which is the ordinary
			// way to make an area of ImGui clickable.
			const ImVec2 origin = ImGui::GetCursorPos();
			const bool pressed = ImGui::InvisibleButton("hit", ImGui::GetContentRegionAvail());

			RecordWidget(state, "live-preset", i, true);

			if (pressed)
			{
				state.requested_preset = i;
			}

			ImGui::SetCursorPos(origin);

			{
				ScopedFont name(Face::Ui, text::kLg);
				ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::kAccent : theme::kTextPrimary);
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(presets[i].name.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}

			// The binding, so the player can see which switch fires which
			// tile without opening the mappings table. `unbound` rather than
			// blank: a tile with nothing under it is a tile you have to walk
			// over to reach, which is worth knowing before the song starts.
			{
				ScopedFont mono(Face::Mono, text::k2xs);
				ImGui::SetCursorPosY(ImGui::GetContentRegionMax().y - ImGui::GetTextLineHeight());
				ImGui::TextDisabled("%s", presets[i].binding.empty() ? "unbound" : presets[i].binding.c_str());
			}

			ImGui::EndChild();

			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor(2);
			ImGui::PopID();
		}

		ImGui::EndTable();
	}
	else if (presets.empty())
	{
		ImGui::TextDisabled("No presets in this bank.");
	}

	// Full-width and 64px tall: these are the two things a foot finds.
	{
		const float half = (ImGui::GetContentRegionAvail().x - theme::kSpace3) * 0.5f;

		// Both report through `requested_preset`, the same field a tile uses
		// and the same one the sidebar's list uses. Stepping is a preset
		// switch; giving it a second channel to the caller would be a second
		// thing that could disagree about which preset is playing.
		const size_t count = presets.size();

		ImGui::BeginDisabled(count < 2);

		if (widgets::SecondaryButtonSized("Prev", ImVec2(half, kLiveStomp)) && count > 0)
		{
			state.requested_preset = (state.current_preset + count - 1) % count;
		}

		RecordWidgetNamed(state, "live:prev", count >= 2);
		ImGui::SameLine(0.0f, theme::kSpace3);

		if (widgets::SecondaryButtonSized("Next", ImVec2(half, kLiveStomp)) && count > 0)
		{
			state.requested_preset = (state.current_preset + 1) % count;
		}

		RecordWidgetNamed(state, "live:next", count >= 2);

		ImGui::EndDisabled();
	}

	DrawMeter("Input", state.input_db, false, ui.input_meter, kLiveMeter);
	DrawMeter("Output", state.output_db, state.output_clipping, ui.output_meter, kLiveMeter);
	DrawMeterScale(44.0f, 60.0f);

	ImGui::EndChild();

	ImGui::PopStyleVar(2);
}

void DrawBoard(BoardViewState& state, BoardViewUi& ui)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);

	// One window filling the OS window. Signalforge is a pedalboard, not a
	// desktop -- there is nothing to arrange, and floating panels would only
	// give the player something to lose.
	//
	// It scrolls nowhere itself: the two panes inside the body do, separately,
	// which is the point of the layout. Expanding a stage used to push the
	// meters off the bottom of the window because everything shared one
	// scrolling column.
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse;

	// The frame owns its own padding. Each band insets its own contents, and
	// window padding here would push the header down from a title bar that
	// isn't there and leave a gap beside the sidebar's hairline.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

	const bool open = ImGui::Begin("Signalforge", nullptr, flags);

	ImGui::PopStyleVar(3);

	if (open)
	{
		// Bands stack flush against each other; each one spaces its own
		// contents normally.
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

		DrawHeader(state, ui);
		DrawLearnStrip(state);
		DrawWarningBanner(state);

		// The body: sidebar and chain side by side, each scrolling on its own,
		// filling everything the status bar doesn't want.
		const float status_height = ImGui::GetFrameHeight() + theme::kSpace2 * 2.0f;

		ImGui::BeginChild("body", ImVec2(0.0f, -status_height), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		// Live replaces the body and nothing else: the header, the learn strip
		// and the status bar are the same on both screens, because what they
		// say is true on both.
		if (ui.live_screen)
		{
			DrawLiveScreen(state, ui);

			ImGui::EndChild();  // body

			DrawStatusBar(state);

			ImGui::PopStyleVar();  // the band stacking's item spacing
			ImGui::End();

			return;
		}

		BeginSidebar();

		// --- BANK (SF-67, SF-69) ---
		{
			widgets::SectionLabel("BANK");
			ImGui::SameLine();

			const float learn_width = 60.0f;
			const float caption = ImGui::CalcTextSize("next bank").x;

			ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - caption - learn_width - theme::kSpace2);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("next bank");
			ImGui::SameLine(0.0f, theme::kSpace2);

			if (widgets::LearnButton(Probes(state), "learn-bank:0", state.bank_binding, state.learning))
			{
				state.learn_request.target = LearnTarget::NextBank;
			}

			// The select sits on its own line: it has a 260px minimum and
			// would push everything above it off the sidebar if it shared one.
			const size_t banks = state.banks ? state.banks->size() : 0;
			const char* current = banks > 0 && state.current_bank < banks
				? (*state.banks)[state.current_bank].c_str()
				: "(no library)";

			ImGui::SetNextItemWidth(-1.0f);
			ImGui::BeginDisabled(banks < 2);

			if (ImGui::BeginCombo("##bank", current))
			{
				for (size_t i = 0; i < banks; ++i)
				{
					if (ImGui::Selectable((*state.banks)[i].c_str(), i == state.current_bank))
					{
						state.requested_bank = i;
					}

					Probes(state).Record("bank:" + std::to_string(i), true);
				}

				ImGui::EndCombo();
			}

			ImGui::EndDisabled();
			Probes(state).Record("bank-select", banks >= 2);

			if (banks < 2 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("A bank is a folder of presets. Make one next to the others to switch between them.");
			}
		}

		ImGui::Dummy(ImVec2(0.0f, theme::kSpace3));

		// --- PRESETS ---
		{
			const size_t count = state.presets ? state.presets->size() : 0;

			widgets::SectionLabel("PRESETS");
			ImGui::SameLine(0.0f, theme::kSpace2);

			{
				ScopedFont mono(Face::Mono, text::k2xs);

				if (state.current_preset != kNoPreset && count > 0)
				{
					ImGui::TextDisabled("%zu of %zu", state.current_preset + 1, count);
				}
				else
				{
					ImGui::TextDisabled("%zu", count);
				}
			}

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
					// Name over filename, so the row says both what the sound
					// is called and which file to go and edit -- the stem used
					// to be a tooltip, which is no use when you are looking
					// for a file rather than at a row.
					const std::string sublabel = entry.stem + ".json";

					if (widgets::ListRow(entry.name.c_str(), sublabel.c_str(), i == state.current_preset))
					{
						state.requested_preset = i;
					}

					RecordWidget(state, "preset", i, true);

					// Placed from the right-hand edge rather than with a bare
					// SameLine: the row is a full-width selectable, and
					// SameLine after one of those puts the next item back at
					// the left, on top of the name.
					const float learn_width = 52.0f + (entry.binding.empty() ? 0.0f : 60.0f);

					ImGui::SameLine();
					ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - learn_width);

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

		ImGui::Dummy(ImVec2(0.0f, theme::kSpace3));

		// --- ENGINE ---
		{
			widgets::SectionLabel("ENGINE");

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

				// Above the bindings would read as "what is bound"; below it
				// reads as "and this is what would send it", which is the
				// order the question is actually asked in.
				DrawEngineRow("Controller", state.midi_controller.empty() ? "none" : state.midi_controller.c_str());

				if (state.midi_drops > 0)
				{
					std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(state.midi_drops));
					DrawEngineRow("MIDI events dropped", buffer);
				}

				ImGui::EndTable();
			}

			ImGui::Spacing();

			// The DSP gauge closes the ENGINE group rather than opening
			// LEVELS: it is a fact about the engine, and it is the number that
			// explains the other facts above it when a board starts xrunning.
			DrawLoadGauge("DSP", state.dsp_load, "Share of each audio block's time budget spent running the chain");
		}

		ImGui::Dummy(ImVec2(0.0f, theme::kSpace3));

		// --- LEVELS ---
		{
			widgets::SectionLabel("LEVELS");
			ImGui::Spacing();

			DrawMeter("Input", state.input_db, false, ui.input_meter, 14.0f);
			DrawMeter("Output", state.output_db, state.output_clipping, ui.output_meter, 14.0f);
			DrawMeterScale(44.0f, 60.0f);

			ImGui::Spacing();

			// The trims, under the meters they are set against (SF-66). Both
			// report continuously rather than on mouse release: the gain is an
			// atomic the audio thread ramps to, so there is no rebuild to pay
			// for and hearing the change while dragging is the whole point.
			DrawGainSlider(state, "Input gain", state.input_gain_db, state.input_gain_binding,
				state.input_gain_request, LearnTarget::SetInputGain);

			DrawGainSlider(state, "Output gain", state.output_gain_db, state.output_gain_binding,
				state.output_gain_request, LearnTarget::SetOutputGain);

			if (state.output_clipping)
			{
				ImGui::TextColored(theme::kMeterClip,
					"Output is clipping -- lower the level in the chain or at the interface.");
			}
		}

		EndSidebar();

		// The chain gets everything the sidebar didn't. No collapsing header
		// around it any more: this pane *is* the chain, and a board you can
		// fold away is a board you can lose.
		BeginChainArea();

		{
			// The board itself (SF-71): stage cards from INPUT to OUTPUT, in two
			// lanes where the signal splits. This replaced the wired canvas
			// rather than joining it -- two ways to edit one chain is how they
			// drift apart, which is the same reason the canvas replaced the list
			// before it (SF-57).
			DrawChain(state, ui);

			ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

			// The graph asks for the browser rather than opening it: a popup
			// has to be opened in the same ID scope it is drawn in, and the
			// board is drawn inside a child window.
			if (ui.open_browser)
			{
				ui.open_browser = false;
				ImGui::OpenPopup("Add stage");
			}

			DrawPluginBrowser(state, ui);

			// Drawn at the same level as the plugin browser: a modal has to be
			// pumped from the same place it was opened, every frame, or Dear
			// ImGui closes it.
			DrawFilePicker(state, ui);

			// Same reason again: pumped every frame from where it was opened.
			DrawMidiMap(state, ui);
			DrawPluginUi(state, ui);
			DrawTuner(state, ui);
			DrawMetronome(state, ui);
		}

		EndChainArea();

		ImGui::EndChild();  // body

		DrawStatusBar(state);

		ImGui::PopStyleVar();  // the band stacking's item spacing
	}

	ImGui::End();
}

}  // namespace sf
