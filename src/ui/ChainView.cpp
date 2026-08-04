#include "ChainView.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "../audio/NoiseGate.h"
#include "Fonts.h"
#include "StageCard.h"
#include "Theme.h"
#include "Widgets.h"

namespace sf
{

namespace
{

// The splitter and the mixer are the same card twice: 560px, centred, with a
// header row that collapses it.
constexpr float kCardWidth = 560.0f;

// The gate's indicator dot.
constexpr float kGateDotRadius = 6.0f;

ProbeRecorder Probes(BoardViewState& state)
{
	return ProbeRecorder{state.record_widgets ? &state.widgets : nullptr};
}

bool ContainsSplit(const BoardGraphNodes& nodes)
{
	for (const BoardGraphNode& node : nodes)
	{
		if (node.split)
		{
			return true;
		}
	}

	return false;
}

// A `+`-less "Add stage" at the end of a section. Insertion is by section
// append plus `^`/`v`; the gap-by-gap `+` went with the canvas.
void AddStageButton(BoardViewState& state, BoardViewUi& ui, const BoardSlot& slot, const char* probe)
{
	// Dear ImGui identifies a widget by its label, and every one of these is
	// labelled "Add stage". A linear board draws one and never notices; a split
	// board draws four, and the two on the trunk -- pre and post -- sit at the
	// same level with **the same id**. The ones inside the lanes escape only
	// because a lane is its own child window.
	//
	// **What that actually cost, accurately:** both buttons still worked. Hover
	// and press resolve per *rectangle*, not per id -- and ImGui's "is another
	// item already hovered" guard passes precisely because the two share an id.
	// What a duplicate id really breaks is id-*keyed* state: popups, a slider's
	// value mid-drag, tree nodes, keyboard focus. These are plain buttons with
	// none of that, and keyboard nav is not even enabled here, so the whole
	// visible symptom was ImGui's debug tooltip in front of whoever hovered one
	// of the pair.
	//
	// Scoped anyway, and the check that found it kept, for the reason it will
	// eventually matter: the day one of these grows a tooltip or opens a popup,
	// the same collision stops being harmless and starts being silent. The
	// probe id is already unique per button, so it makes a natural scope -- the
	// same fix, for the same reason, as the learn buttons.
	ImGui::PushID(probe);

	if (widgets::GhostButton("Add stage"))
	{
		ui.plugin_search[0] = '\0';
		ui.pending_slot = slot;
		ui.pending_slot_valid = true;

		// Asked for rather than opened here: a popup has to be opened in the
		// same ID scope it is drawn in, and that is the window, not this child.
		ui.open_browser = true;
	}

	Probes(state).Record(probe, true);

	ImGui::PopID();
}

// The header of a collapsible card: an arrow, a mono label, and a summary that
// says the current setting when it is closed.
bool CardHeader(BoardViewState& state, const char* label, const char* summary, bool open, const char* probe)
{
	bool toggled = false;

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kBgSurfaceRaised);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kGray700);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

	toggled = ImGui::ArrowButton(probe, open ? ImGuiDir_Down : ImGuiDir_Right);

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(4);

	Probes(state).Record(probe, true);

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Show or hide these controls");
	}

	ImGui::SameLine(0.0f, theme::kSpace2);
	widgets::SectionLabel(label, theme::kTextSecondary);

	ImGui::SameLine(0.0f, theme::kSpace2);

	{
		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("%s", summary);
	}

	return toggled;
}

// The 560px centred card the splitter and the mixer both are.
void BeginCentredCard(const char* id)
{
	const float available = ImGui::GetContentRegionAvail().x;
	const float width = std::min(kCardWidth, available);

	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - width) * 0.5f);

	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgSurface);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusCard);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace4, theme::kSpace3));

	ImGui::BeginChild(id, ImVec2(width, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void EndCentredCard()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

// The signal-path card: the only control that splits and un-splits the board.
//
// Named "signal path" rather than "amp section", deliberately -- the split is
// not amp-specific, and half the reasons to reach for one (parallel
// compression, a wet/dry blend) have no amp in them at all.
void DrawSignalPath(BoardViewState& state, BoardViewUi& ui, const BoardShape& shape)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgSurface);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusCard);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace3, 10.0f));

	ImGui::BeginChild("signal-path", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ImGui::AlignTextToFramePadding();
	widgets::SectionLabel("SIGNAL PATH", theme::kTextSecondary);

	ImGui::SameLine(0.0f, theme::kSpace3);

	static const char* kChannels[] = {"1 channel", "2 channels"};

	const int selected = shape.split ? 1 : 0;
	const int pressed = widgets::SegmentedControl("channels", kChannels, 2, selected, Probes(state), "channels");

	if (pressed == 1 && !shape.split)
	{
		// A new split goes at the end of the trunk with both lanes empty: the
		// split exists first, then stages go into it.
		state.graph_edit = GraphEdit::InsertSplit;
		state.graph_edit_slot = BoardSlot{kNoBoardNode, 0, shape.trunk_size};
	}
	else if (pressed == 0 && shape.split)
	{
		// Collapsing concatenates lane A, then lane B, then whatever followed
		// the merge, into one series. No stage is lost -- a collapse that kept
		// one lane and dropped the other would silently delete pedals, and the
		// lane it dropped would be a choice the player never made.
		state.graph_edit = GraphEdit::Rejoin;
		state.graph_edit_node = shape.split_node;
	}

	ImGui::SameLine(0.0f, theme::kSpace3);

	{
		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s",
			shape.split ? "Two parallel channels, each with its own amp, merged back through the mixer."
						: "One series chain from input to output.");
		ImGui::PopTextWrapPos();
	}

	ImGui::EndChild();

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

// Asks the caller to replace the whole splitter, starting from what it is now.
// One edit per field would be four edits that each cost the same rebuild and
// could each arrive without the others.
void RequestSplitter(BoardViewState& state, const BoardShape& shape, const BoardSplitter& wanted)
{
	state.graph_edit = GraphEdit::SetSplitter;
	state.graph_edit_node = shape.split_node;
	state.graph_edit_splitter = wanted;
}

// A slider whose value is planned into the chain, so it reports once, on
// release, rather than sixty times a second while a mouse moves (SF-56).
//
// Returns true on the frame it was let go, with `out` carrying where it was
// dragged to. The mid-drag value lives in `ui` rather than in the caller's
// local: Dear ImGui writes into the caller's variable while the item is active
// and writes nothing on the frame it is released, so reading the local back on
// deactivation gives the value the drag *started* from.
bool DeferredSlider(BoardViewState& state, BoardViewUi& ui, const char* probe, const char* label, float value,
	float minimum, float maximum, const char* format, ImGuiSliderFlags flags, float& out)
{
	const bool dragging = ui.dragging == probe;
	float shown = dragging ? ui.dragging_value : value;

	ImGui::SliderFloat(label, &shown, minimum, maximum, format, flags);
	Probes(state).Record(probe, true);

	if (ImGui::IsItemActive())
	{
		ui.dragging = probe;
		ui.dragging_value = shown;
	}

	if (!ImGui::IsItemDeactivatedAfterEdit())
	{
		return false;
	}

	// A click that never moved leaves nothing remembered, in which case what
	// the widget itself reported this frame is the whole of the edit.
	out = dragging ? ui.dragging_value : shown;

	if (dragging)
	{
		ui.dragging.clear();
	}

	return out != value;
}

// What the card says about itself when it is closed.
void SplitterSummary(const BoardShape& shape, char* out, size_t size)
{
	static const char* const kSides[] = {"Left", "Right", "Sum of both"};

	switch (shape.splitter.mode)
	{
		case BoardSplitterMode::Crossover:
			std::snprintf(out, size, "crossover at %.0f Hz", static_cast<double>(shape.splitter.frequency));
			break;

		case BoardSplitterMode::Stereo:
			std::snprintf(out, size, "%s input / %s input", kSides[static_cast<int>(shape.splitter.source[0])],
				kSides[static_cast<int>(shape.splitter.source[1])]);
			break;

		case BoardSplitterMode::Full:
		default:
			std::snprintf(out, size, "full range into both channels");
			break;
	}
}

// The noise gate (SF-85): what the board does to its input before anything
// else touches it.
//
// Drawn above the `INPUT` rail because that is where it happens -- after the
// board-wide trim, before the first pedal. Gating *before* any distortion is
// the whole reason it works: a high-gain amp model turns a noise floor into a
// roar, and no gate after one can put that back.
//
// Both sliders report continuously rather than on release. Unlike the splitter
// beside it, nothing here is planned into the chain -- they are atomics the
// audio thread reads -- so there is no rebuild to pay for, and hearing the
// threshold while dragging it is the entire point of having it.
void DrawGate(BoardViewState& state, BoardViewUi& ui)
{
	BeginCentredCard("gate-card");

	char summary[96];

	if (state.gate.enabled)
	{
		// **What it is doing, not only what it is set to.** The card is shut by
		// default, and the indicator lives in its body -- so without this the
		// live reading a player needs while setting a threshold is exactly the
		// thing hidden by the state the board opens in. One word in the header
		// keeps it visible for the cost of nothing.
		const char* doing = state.gate.gain > 0.99f ? "open" : (state.gate.gain < 0.01f ? "cutting" : "closing");

		std::snprintf(summary, sizeof(summary), "%.0f dB, %.0f ms release -- %s",
			static_cast<double>(state.gate.threshold_db), static_cast<double>(state.gate.release_ms), doing);
	}
	else
	{
		std::snprintf(summary, sizeof(summary), "off");
	}

	if (CardHeader(state, "GATE", ui.gate_open ? "what the board cuts before it hears anything" : summary,
			ui.gate_open, "gate-toggle"))
	{
		ui.gate_open = !ui.gate_open;
	}

	if (!ui.gate_open)
	{
		EndCentredCard();
		return;
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

	bool enabled = state.gate.enabled;

	if (ImGui::Checkbox("Gate the input", &enabled))
	{
		state.gate_toggle_requested = true;
	}

	Probes(state).Record("gate:enabled", true);

	// The indicator, beside the switch: a dot that dims with the gate's gain.
	// Without it the threshold is set by listening for the noise it removes,
	// and the input meter deliberately reads *before* the gate, so the meter
	// cannot answer the question either.
	{
		ImGui::SameLine(0.0f, theme::kSpace4);

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float radius = kGateDotRadius;
		const float centre = at.y + ImGui::GetFrameHeight() * 0.5f;

		// Amber when the signal is through, dark when it is cut, and every
		// shade between while it releases -- the release is the part worth
		// seeing, and a binary lamp would hide exactly that.
		const float open = state.gate.enabled ? std::min(std::max(state.gate.gain, 0.0f), 1.0f) : 1.0f;

		ImVec4 colour = theme::kBgSurfaceRaised;
		colour.x += (theme::kAccent.x - colour.x) * open;
		colour.y += (theme::kAccent.y - colour.y) * open;
		colour.z += (theme::kAccent.z - colour.z) * open;

		draw->AddCircleFilled(ImVec2(at.x + radius, centre), radius, ImGui::GetColorU32(colour));

		ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetFrameHeight()));
		ImGui::SameLine(0.0f, theme::kSpace2);

		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", !state.gate.enabled ? "" : (open > 0.99f ? "open" : (open < 0.01f ? "closed" : "closing")));
	}

	ImGui::BeginDisabled(!state.gate.enabled);

	{
		float threshold = state.gate.threshold_db;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);

		if (ImGui::SliderFloat("Threshold", &threshold, kGateMinThresholdDb, kGateMaxThresholdDb, "%.0f dB"))
		{
			state.gate_threshold_request = threshold;
		}

		Probes(state).Record("gate:threshold", state.gate.enabled);

		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Anything quieter than this is cut.\nMeasured after the input trim, so it is the same "
							  "scale the input meter reads -- and moving that trim moves this with it.");
		}
	}

	{
		float release = state.gate.release_ms;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);

		// Logarithmic: the useful settings crowd the bottom of a 20..2000
		// range, and a linear slider would spend most of its travel above
		// half a second.
		if (ImGui::SliderFloat("Release", &release, kGateMinReleaseMs, kGateMaxReleaseMs, "%.0f ms",
				ImGuiSliderFlags_Logarithmic))
		{
			state.gate_release_request = release;
		}

		Probes(state).Record("gate:release", state.gate.enabled);

		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("How long the gate takes to close once you stop playing.\nShort is tight; long "
							  "leaves a note's tail alone.");
		}
	}

	ImGui::EndDisabled();

	{
		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("cut before the first pedal, which is the only place it can catch amp noise");
	}

	EndCentredCard();
}

// The splitter: how the signal divides into A and B (SF-75).
//
// Everything here is planned into the chain, so everything here costs a
// rebuild. The two selects report the moment they change -- one click, one
// rebuild -- while the sliders report on mouse release, the same rule the lane
// gains followed before they became live (SF-56). A crossover frequency
// dragged live would replan the board sixty times a second, and unlike a merge
// gain there is no atomic that could avoid it: the filter coefficients are
// built when the chain is.
void DrawSplitter(BoardViewState& state, BoardViewUi& ui, const BoardShape& shape)
{
	BeginCentredCard("splitter-card");

	char summary[96];
	SplitterSummary(shape, summary, sizeof(summary));

	if (CardHeader(state, "SPLITTER", ui.splitter_open ? "how the signal divides into A and B" : summary,
			ui.splitter_open, "splitter-toggle"))
	{
		ui.splitter_open = !ui.splitter_open;
	}

	if (!ui.splitter_open)
	{
		EndCentredCard();
		return;
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

	// Full width: the design's Select has a 260px minimum and two of them
	// side by side overflow a 560px card.
	ImGui::SetNextItemWidth(-1.0f);

	static const char* const kModes[] = {"Full range (copy)", "Frequency crossover", "Stereo input (L / R)"};

	const int mode = static_cast<int>(shape.splitter.mode);

	if (ImGui::BeginCombo("##splitter-mode", kModes[mode]))
	{
		static const char* const kModeProbes[] = {"full", "crossover", "stereo"};

		for (int i = 0; i < 3; ++i)
		{
			if (ImGui::Selectable(kModes[i], i == mode) && i != mode)
			{
				BoardSplitter wanted = shape.splitter;
				wanted.mode = static_cast<BoardSplitterMode>(i);

				RequestSplitter(state, shape, wanted);
			}

			// The rows exist only while the dropdown is open, which is why a
			// test has to click it open first rather than aim straight at one.
			Probes(state).Record(std::string("splitter-mode:") + kModeProbes[i], true);
		}

		ImGui::EndCombo();
	}

	Probes(state).Record("splitter-mode", true);

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

	if (shape.splitter.mode == BoardSplitterMode::Crossover)
	{
		// Logarithmic, because a crossover between an amp and a bass rig
		// spends its whole useful life below 1 kHz and a linear 20..20000
		// slider gives that a twentieth of its travel.
		ImGui::SetNextItemWidth(-1.0f);

		float frequency = 0.0f;

		if (DeferredSlider(state, ui, "splitter-freq", "##crossover-freq", shape.splitter.frequency, 20.0f, 20000.0f,
				"%.0f Hz", ImGuiSliderFlags_Logarithmic, frequency))
		{
			BoardSplitter wanted = shape.splitter;
			wanted.frequency = frequency;

			RequestSplitter(state, shape, wanted);
		}

		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("Below this goes to channel A, above it to channel B.");
	}
	else if (shape.splitter.mode == BoardSplitterMode::Stereo)
	{
		static const char* const kSources[] = {"Left input", "Right input", "Sum of both"};

		for (unsigned lane = 0; lane < 2; ++lane)
		{
			ImGui::PushID(static_cast<int>(lane));
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(theme::kTextSecondary, "Channel %s", lane == 0 ? "A" : "B");
			ImGui::SameLine(0.0f, theme::kSpace3);

			const int source = static_cast<int>(shape.splitter.source[lane]);

			ImGui::SetNextItemWidth(-1.0f);

			if (ImGui::BeginCombo("##src", kSources[source]))
			{
				static const char* const kSourceProbes[] = {"left", "right", "sum"};

				for (int i = 0; i < 3; ++i)
				{
					if (ImGui::Selectable(kSources[i], i == source) && i != source)
					{
						BoardSplitter wanted = shape.splitter;
						wanted.source[lane] = static_cast<BoardSplitterSource>(i);

						RequestSplitter(state, shape, wanted);
					}

					Probes(state).Record(
						std::string("splitter-src:") + (lane == 0 ? "a:" : "b:") + kSourceProbes[i], true);
				}

				ImGui::EndCombo();
			}

			Probes(state).Record(std::string("splitter-src:") + (lane == 0 ? "a" : "b"), true);
			ImGui::PopID();
		}

		ScopedFont caption(Face::Ui, text::k2xs);

		// The caveat rather than a disabled control (SF-76). The mode selects
		// and the board plays either way; what changes is whether there are
		// two sides to take. A greyed-out select would be a refusal the player
		// has to guess the reason for.
		if (state.board_inputs < 2)
		{
			ImGui::TextDisabled("The board is taking one input, so both channels get the same signal. "
								"Choose a stereo input in Audio settings to divide it.");
		}
		else
		{
			ImGui::TextDisabled("Each channel takes one side of the stereo input.");
		}
	}
	else
	{
		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("The same signal goes into both channels.");
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));

	// The sends. Drawn in every mode because they apply in every mode: how
	// hard each channel is driven is a separate question from how the signal
	// was divided, and it is the one that decides how a lane's amp breaks up.
	for (unsigned lane = 0; lane < 2; ++lane)
	{
		ImGui::PushID(static_cast<int>(100 + lane));

		const std::string probe = std::string("send:") + (lane == 0 ? "a" : "b");

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);

		float send = 0.0f;

		if (DeferredSlider(state, ui, probe.c_str(), lane == 0 ? "Send A" : "Send B", shape.splitter.send_db[lane],
				-24.0f, 24.0f, "%+.1f dB", ImGuiSliderFlags_None, send))
		{
			BoardSplitter wanted = shape.splitter;
			wanted.send_db[lane] = send;

			RequestSplitter(state, shape, wanted);
		}

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("How hard channel %s is driven, before its stages.\nThe mixer's level is the other "
							  "end -- how much comes back after them.",
				lane == 0 ? "A" : "B");
		}

		ImGui::PopID();
	}

	{
		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("into each channel, before its stages");
	}

	EndCentredCard();
}

// The merge: how much of each channel comes back in.
void DrawMixer(BoardViewState& state, BoardViewUi& ui, const BoardShape& shape)
{
	BeginCentredCard("mixer-card");

	char summary[96];

	if (shape.muted[0] || shape.muted[1])
	{
		std::snprintf(summary, sizeof(summary), "A %s / B %s", shape.muted[0] ? "muted" : "on",
			shape.muted[1] ? "muted" : "on");
	}
	else
	{
		std::snprintf(summary, sizeof(summary), "A %.1f dB / B %.1f dB", static_cast<double>(shape.gain_db[0]),
			static_cast<double>(shape.gain_db[1]));
	}

	if (CardHeader(state, "MIXER", ui.mixer_open ? "channel levels into the merge" : summary, ui.mixer_open,
			"mixer-toggle"))
	{
		ui.mixer_open = !ui.mixer_open;
	}

	if (ui.mixer_open)
	{
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

		for (unsigned lane = 0; lane < 2; ++lane)
		{
			ImGui::PushID(static_cast<int>(lane));

			const char* label = lane == 0 ? "Level A" : "Level B";
			float gain = shape.gain_db[lane];

			// A muted lane's slider is disabled rather than hidden: the level
			// it will come back at is still worth seeing, and a control that
			// vanishes reads as a lane that was deleted.
			ImGui::BeginDisabled(shape.muted[lane]);

			// Room on the row for the label, the learn button and its binding
			// text, and the mute beside it.
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 280.0f);

			// Reported continuously, not on mouse release: a merge gain goes
			// straight to the running chain (SF-72), so there is no rebuild to
			// pay for and hearing the blend while dragging is the point of
			// having a blend.
			if (ImGui::SliderFloat(label, &gain, -60.0f, 6.0f, "%+.1f dB"))
			{
				state.graph_edit = GraphEdit::SetLaneGain;
				state.graph_edit_node = shape.split_node;
				state.graph_edit_branch = lane;
				state.graph_edit_gain_db = gain;
			}

			ImGui::EndDisabled();
			Probes(state).Record(std::string("mix:") + (lane == 0 ? "a" : "b"), !shape.muted[lane]);

			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("How much of channel %s comes back in.\nTwo channels at 0 dB carrying the same "
								  "signal sum to +6 dB -- this is where that is fixed.",
					lane == 0 ? "A" : "B");
			}

			ImGui::SameLine();

			const char* const lane_name = lane == 0 ? "a" : "b";

			// The lane the pedal moves, not the split it is currently on: a
			// binding is kept in the mapping file and outlives this preset.
			if (widgets::LearnButton(
					Probes(state), std::string("learn-mix:") + lane_name, state.mixer_binding[lane], state.learning))
			{
				state.learn_request.target = LearnTarget::SetMixerLevel;
				state.learn_request.symbol = lane_name;
				state.learn_request.minimum = -60.0f;
				state.learn_request.maximum = 6.0f;
			}

			ImGui::SameLine();

			if (widgets::GhostButton(shape.muted[lane] ? "muted" : "mute"))
			{
				state.graph_edit = GraphEdit::ToggleMute;
				state.graph_edit_node = shape.split_node;
				state.graph_edit_branch = lane;
			}

			Probes(state).Record(std::string("mute:") + lane_name, true);

			ImGui::PopID();
		}

		ScopedFont caption(Face::Ui, text::k2xs);
		ImGui::TextDisabled("channel levels into the merge");
	}

	EndCentredCard();
}

// One run of stage cards, with the Add stage that ends it.
void DrawSection(BoardViewState& state, BoardViewUi& ui, const std::vector<const BoardGraphNode*>& nodes,
	const char* code_prefix, BoardNodeId split, unsigned branch, size_t base_index, bool only_stage,
	const char* add_probe)
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		const BoardGraphNode& node = *nodes[i];

		StageCardPlace place;
		place.code = std::string(code_prefix) + std::to_string(i);
		place.first = i == 0;
		place.last = i + 1 == nodes.size();
		place.only_stage = only_stage;
		place.stage = node.stage == kNoStageIndex ? kNoPreset : node.stage;

		DrawStageCard(state, ui, node.id, place);
	}

	AddStageButton(state, ui, BoardSlot{split, branch, base_index + nodes.size()}, add_probe);
}

}  // namespace

size_t BoardShape::Stages() const
{
	return pre.size() + lane[0].size() + lane[1].size() + post.size();
}

BoardShape ReadShape(const BoardGraphNodes& trunk)
{
	BoardShape shape;
	shape.trunk_size = trunk.size();

	for (size_t i = 0; i < trunk.size(); ++i)
	{
		const BoardGraphNode& node = trunk[i];

		if (!node.split)
		{
			(shape.split ? shape.post : shape.pre).push_back(&node);
			continue;
		}

		if (shape.split)
		{
			shape.drawable = false;
			shape.problem =
				"This preset has more than one split. The board draws one, so it is showing this preset "
				"read-only -- edit it as a file, or collapse it to a single channel elsewhere.";
			return shape;
		}

		if (node.branches.size() != 2)
		{
			shape.drawable = false;
			shape.problem = "This preset has a split with something other than two channels in it.";
			return shape;
		}

		for (unsigned lane = 0; lane < 2; ++lane)
		{
			if (ContainsSplit(node.branches[lane].nodes))
			{
				shape.drawable = false;
				shape.problem =
					"This preset has a split inside a channel. The board draws one level, so it is showing "
					"this preset read-only -- edit it as a file.";
				return shape;
			}

			for (const BoardGraphNode& inner : node.branches[lane].nodes)
			{
				shape.lane[lane].push_back(&inner);
			}

			shape.gain_db[lane] = node.branches[lane].gain_db;
			shape.muted[lane] = node.branches[lane].muted;
		}

		shape.split = true;
		shape.split_node = node.id;
		shape.split_index = i;
		shape.splitter = node.splitter;
	}

	return shape;
}

void DrawChain(BoardViewState& state, BoardViewUi& ui)
{
	static const BoardGraphNodes kNoGraph;

	const BoardGraphNodes& graph = state.graph ? *state.graph : kNoGraph;
	const BoardShape shape = ReadShape(graph);

	// A shape the board cannot draw says so and stops, rather than drawing the
	// part it understands. Half a board shown silently is worse than a board
	// that admits it needs the file -- and this is reachable by hand-editing a
	// preset, so it is not a hypothetical.
	if (!shape.drawable)
	{
		widgets::StatusBanner("chain-undrawable", widgets::BannerTone::Warning, shape.problem.c_str());

		// Recorded as a widget so a test can assert the banner is *there*,
		// not merely that the board it replaces is absent. Disabled, because
		// that is exactly what it is: there is nothing here to click.
		Probes(state).Record("chain-undrawable", false);
		return;
	}

	DrawSignalPath(state, ui, shape);

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
	DrawGate(state, ui);

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
	widgets::Rail("INPUT");
	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

	// The last stage on the board can't go: an empty chain has no ports for
	// the engine to connect. Computed once for the whole board rather than per
	// section, since it is a property of the board.
	const bool only_stage = shape.Stages() <= 1;

	DrawSection(state, ui, shape.pre, "", kNoBoardNode, 0, 0, only_stage, "add-stage:pre");

	if (shape.split)
	{
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
		widgets::Rail("SPLIT", theme::kStatusWarning);
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

		DrawSplitter(state, ui, shape);

		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

		// Two lanes side by side. A table rather than hand-computed columns so
		// the gap and the widths follow the pane as it resizes.
		if (ImGui::BeginTable("channels", 2, ImGuiTableFlags_SizingStretchSame))
		{
			for (unsigned lane = 0; lane < 2; ++lane)
			{
				ImGui::TableNextColumn();
				ImGui::PushID(static_cast<int>(lane));

				ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgInset);
				ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusCard);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace3, theme::kSpace3));

				ImGui::BeginChild("lane", ImVec2(0.0f, 0.0f),
					ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
					ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

				widgets::SectionLabel(lane == 0 ? "CHANNEL A" : "CHANNEL B", theme::kTextSecondary);
				ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

				DrawSection(state, ui, shape.lane[lane], lane == 0 ? "A" : "B", shape.split_node, lane, 0, only_stage,
					lane == 0 ? "add-stage:a" : "add-stage:b");

				ImGui::EndChild();

				ImGui::PopStyleVar(2);
				ImGui::PopStyleColor();
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
		widgets::Rail("MERGE", theme::kStatusWarning);
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

		DrawMixer(state, ui, shape);

		ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));

		// Post-merge stages continue the trunk's numbering, so the code on a
		// card after the merge is the position it actually holds.
		DrawSection(state, ui, shape.post, "", kNoBoardNode, 0, shape.split_index + 1, only_stage, "add-stage:post");
	}

	ImGui::Dummy(ImVec2(0.0f, theme::kSpace1));
	widgets::Rail("OUTPUT");

	if (shape.Stages() == 0)
	{
		ImGui::Dummy(ImVec2(0.0f, theme::kSpace2));
		ImGui::TextDisabled("No stages. Add one to start building a board.");
	}
}

}  // namespace sf
