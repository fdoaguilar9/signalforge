#include "Widgets.h"

#include <cmath>

#include "Fonts.h"

namespace sf
{

void ProbeRecorder::Record(const std::string& id, bool enabled, bool checked) const
{
	if (!into)
	{
		return;
	}

	const ImVec2 low = ImGui::GetItemRectMin();
	const ImVec2 high = ImGui::GetItemRectMax();

	into->push_back({id, (low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, ImGui::GetItemID(), enabled, checked});
}

namespace widgets
{

namespace
{

// The design's 0.08em on an 11px label. Expressed as a fraction of the size
// rather than as a pixel figure so a label at another size tracks in
// proportion.
constexpr float kTrackingWider = 0.08f;

// The segmented control's own metrics, from the design: 6px 16px padding,
// 3px outer radius.
constexpr float kSegmentPaddingX = 16.0f;
constexpr float kSegmentPaddingY = 6.0f;

}  // namespace

void HairlineH(ImDrawList* draw, float x0, float x1, float y, ImU32 colour)
{
	draw->AddRectFilled(ImVec2(std::floor(x0), std::floor(y)), ImVec2(std::floor(x1), std::floor(y) + 1.0f), colour);
}

void HairlineV(ImDrawList* draw, float y0, float y1, float x, ImU32 colour)
{
	draw->AddRectFilled(ImVec2(std::floor(x), std::floor(y0)), ImVec2(std::floor(x) + 1.0f, std::floor(y1)), colour);
}

namespace
{

// What TextTracked will occupy. Needed by anything that has to place a tracked
// label rather than just emit one -- a rail draws its rules up to the label's
// edges, and a width that is merely close leaves a gap or an overlap.
float TrackedTextWidth(const char* text, float tracking_px)
{
	const float glyphs = ImGui::CalcTextSize(text).x;

	size_t count = 0;

	while (text[count] != '\0')
	{
		++count;
	}

	return count > 1 ? glyphs + tracking_px * static_cast<float>(count - 1) : glyphs;
}

}  // namespace

void TextTracked(const char* text, const ImVec4& colour, float tracking_px)
{
	const ImVec2 start = ImGui::GetCursorScreenPos();
	const ImU32 packed = ImGui::GetColorU32(colour);

	ImDrawList* draw = ImGui::GetWindowDrawList();

	float x = start.x;

	for (const char* c = text; *c != '\0'; ++c)
	{
		const char one[2] = {*c, '\0'};

		draw->AddText(ImVec2(x, start.y), packed, one);
		x += ImGui::CalcTextSize(one).x + tracking_px;
	}

	// Claim the space that was drawn into, so whatever comes next is laid out
	// after it rather than on top of it. The trailing gap is not part of the
	// text, hence the one subtraction.
	const float width = x > start.x ? x - start.x - tracking_px : 0.0f;

	ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
}

void SectionLabel(const char* text, const ImVec4& colour)
{
	ScopedFont label(Face::Mono, text::k2xs);
	TextTracked(text, colour, text::k2xs * kTrackingWider);
}

void Rail(const char* label, const ImVec4& colour)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 hairline = ImGui::GetColorU32(theme::kBorderHairline);

	const float full = ImGui::GetContentRegionAvail().x;
	const ImVec2 start = ImGui::GetCursorScreenPos();

	float text_width = 0.0f;
	float line_height = 0.0f;

	{
		ScopedFont measuring(Face::Mono, text::k2xs);

		text_width = TrackedTextWidth(label, text::k2xs * kTrackingWider);
		line_height = ImGui::GetTextLineHeight();
	}

	const float centre_y = start.y + line_height * 0.5f;
	const float gap = theme::kSpace3;
	const float rule = (full - text_width - gap * 2.0f) * 0.5f;

	HairlineH(draw, start.x, start.x + rule, centre_y, hairline);
	HairlineH(draw, start.x + rule + gap * 2.0f + text_width, start.x + full, centre_y, hairline);

	ImGui::SetCursorScreenPos(ImVec2(start.x + rule + gap, start.y));
	SectionLabel(label, colour);
}

namespace
{

bool ButtonWithColours(const char* label, const ImVec4& fill, const ImVec4& hovered, const ImVec4& active,
	const ImVec4& text_colour, float border, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
	ImGui::PushStyleColor(ImGuiCol_Text, text_colour);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border);

	const bool pressed = ImGui::Button(label, size);

	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);

	return pressed;
}

constexpr ImVec4 kTransparent(0.0f, 0.0f, 0.0f, 0.0f);

}  // namespace

bool PrimaryButton(const char* label)
{
	return ButtonWithColours(
		label, theme::kAccent, theme::kAccentHover, theme::kAccentActive, theme::kAccentOn, 0.0f);
}

bool SecondaryButton(const char* label)
{
	return ButtonWithColours(
		label, theme::kBgSurfaceRaised, theme::kGray700, theme::kGray600, theme::kTextPrimary, 1.0f);
}

bool SecondaryButtonSized(const char* label, const ImVec2& size)
{
	return ButtonWithColours(
		label, theme::kBgSurfaceRaised, theme::kGray700, theme::kGray600, theme::kTextPrimary, 1.0f, size);
}

bool GhostButton(const char* label)
{
	return ButtonWithColours(
		label, kTransparent, theme::kBgSurfaceRaised, theme::kGray700, theme::kTextSecondary, 0.0f);
}

float ButtonWidth(const char* label)
{
	return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

int SegmentedControl(const char* id, const char* const* options, int count, int selected,
	const ProbeRecorder& probes, const char* probe_prefix)
{
	if (count <= 0)
	{
		return -1;
	}

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 start = ImGui::GetCursorScreenPos();

	const float height = ImGui::GetTextLineHeight() + kSegmentPaddingY * 2.0f;

	// Measured first, drawn second: the group's outline has to be drawn before
	// the segments sit on it, and it can't be drawn before its width is known.
	float total = 0.0f;

	for (int i = 0; i < count; ++i)
	{
		total += ImGui::CalcTextSize(options[i]).x + kSegmentPaddingX * 2.0f;
	}

	const ImU32 border = ImGui::GetColorU32(theme::kBorderHairline);
	const ImU32 border_active = ImGui::GetColorU32(theme::kBorderStrong);

	draw->AddRectFilled(start, ImVec2(start.x + total, start.y + height), ImGui::GetColorU32(theme::kBgSurface),
		theme::kRadiusControl);

	int pressed = -1;
	float x = start.x;

	ImGui::PushID(id);

	for (int i = 0; i < count; ++i)
	{
		const float width = ImGui::CalcTextSize(options[i]).x + kSegmentPaddingX * 2.0f;
		const bool active = i == selected;

		const ImVec2 low(x, start.y);
		const ImVec2 high(x + width, start.y + height);

		if (active)
		{
			// Only the outer corners of the whole group are rounded, so the
			// first and last segments round on one side and the middle ones
			// not at all -- otherwise an active middle segment looks like a
			// separate control that has come loose.
			ImDrawFlags corners = ImDrawFlags_RoundCornersNone;

			if (i == 0)
			{
				corners = ImDrawFlags_RoundCornersLeft;
			}
			else if (i == count - 1)
			{
				corners = ImDrawFlags_RoundCornersRight;
			}

			draw->AddRectFilled(
				low, high, ImGui::GetColorU32(theme::kBgSurfaceRaised), theme::kRadiusControl, corners);
		}

		if (i > 0)
		{
			HairlineV(draw, low.y, high.y, low.x, border);
		}

		ImGui::SetCursorScreenPos(low);
		ImGui::PushID(i);

		if (ImGui::InvisibleButton("##segment", ImVec2(width, height)))
		{
			pressed = i;
		}

		ImGui::PopID();

		if (probes)
		{
			probes.Record(std::string(probe_prefix) + ":" + std::to_string(i), true);
		}

		const bool hovered = ImGui::IsItemHovered();

		const ImVec4& colour = active ? theme::kAccent : (hovered ? theme::kTextPrimary : theme::kTextSecondary);
		const ImVec2 text_size = ImGui::CalcTextSize(options[i]);

		draw->AddText(ImVec2(low.x + (width - text_size.x) * 0.5f, low.y + kSegmentPaddingY),
			ImGui::GetColorU32(colour), options[i]);

		x += width;
	}

	ImGui::PopID();

	// The shared outline last, over the fills, so the active segment doesn't
	// paint across the border it shares with its neighbour.
	draw->AddRect(start, ImVec2(start.x + total, start.y + height), selected >= 0 ? border_active : border,
		theme::kRadiusControl);

	ImGui::SetCursorScreenPos(start);
	ImGui::Dummy(ImVec2(total, height));

	return pressed;
}

void StatusBanner(const char* id, BannerTone tone, const char* text)
{
	const ImVec4& colour = tone == BannerTone::Warning ? theme::kStatusWarning : theme::kStatusUnavailable;

	const float available = ImGui::GetContentRegionAvail().x;
	const float wrap = available - theme::kSpace3 * 2.0f;
	const float height = ImGui::CalcTextSize(text, nullptr, false, wrap).y + theme::kSpace2 * 2.0f;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBgSurface);
	ImGui::PushStyleColor(ImGuiCol_Border, colour);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kSpace3, theme::kSpace2));

	ImGui::BeginChild(id, ImVec2(available, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ImGui::PushStyleColor(ImGuiCol_Text, colour);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();

	ImGui::EndChild();

	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
}

bool ListRow(const char* label, const char* sublabel, bool selected)
{
	const float line = ImGui::GetTextLineHeight();
	const float height = sublabel ? line + text::k2xs + theme::kSpace1 * 2.0f : line + theme::kSpace1 * 2.0f;

	const ImVec2 start = ImGui::GetCursorScreenPos();

	// AllowOverlap because these rows are full width and a learn button sits on
	// top of the right-hand end: without it the row claims the hover first and
	// pressing "learn" would load the preset instead of arming a binding.
	const bool clicked = ImGui::Selectable(
		"##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, height));

	ImDrawList* draw = ImGui::GetWindowDrawList();

	draw->AddText(ImVec2(start.x + theme::kSpace2, start.y + theme::kSpace1),
		ImGui::GetColorU32(selected ? theme::kAccent : theme::kTextPrimary), label);

	if (sublabel)
	{
		ScopedFont small(Face::Mono, text::k2xs);

		draw->AddText(ImVec2(start.x + theme::kSpace2, start.y + theme::kSpace1 + line),
			ImGui::GetColorU32(theme::kTextDisabled), sublabel);
	}

	return clicked;
}

bool LearnButton(const ProbeRecorder& probes, const std::string& probe_id, const std::string& binding, bool armed)
{
	// Dear ImGui identifies a widget by its label, and every one of these is
	// labelled "learn". Most sit inside a per-preset or per-stage ID scope that
	// keeps them apart, but the library-wide ones sit side by side at window
	// level and would otherwise be the *same* widget -- two buttons, one of
	// which does nothing when pressed. The probe id is already unique per
	// button, so it makes a natural scope.
	ImGui::PushID(probe_id.c_str());

	ImGui::BeginDisabled(armed);

	ImGui::PushStyleColor(ImGuiCol_Button, kTransparent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kBgSurfaceRaised);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kGray700);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);

	const bool pressed = ImGui::SmallButton("learn");

	ImGui::PopStyleColor(4);
	ImGui::EndDisabled();
	ImGui::PopID();

	probes.Record(probe_id, !armed);

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip(
			binding.empty() ? "Bind a MIDI message to this" : "Rebind -- currently %s", binding.c_str());
	}

	if (!binding.empty())
	{
		ImGui::SameLine(0.0f, theme::kSpace1);

		ScopedFont mono(Face::Mono, text::k2xs);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", binding.c_str());
	}

	return pressed;
}

bool BeginModal(const char* title, float width)
{
	// Width from the design, height from the contents: every modal here states
	// how wide it is and none of them states how tall.
	ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

	return ImGui::BeginPopupModal(title, nullptr,
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
}

void EndModal()
{
	ImGui::EndPopup();
}

bool ModalDismissed()
{
	return ImGui::IsKeyPressed(ImGuiKey_Escape);
}

}  // namespace widgets
}  // namespace sf
