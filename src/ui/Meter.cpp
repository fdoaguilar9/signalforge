#include "Meter.h"

#include <cstdio>

#include "imgui.h"

#include "Fonts.h"
#include "Theme.h"
#include "Widgets.h"

namespace sf
{

namespace
{

// The well's own metrics, from the design.
constexpr float kSegmentGap = 2.0f;
constexpr float kWellPadding = 2.0f;
constexpr float kSegmentRounding = 1.0f;

// How much room the label on the left and the reading on the right take. Fixed
// rather than measured so that two meters stacked on top of each other have
// their wells starting and ending at the same x -- a scale row underneath is
// only readable if the thing it describes is in one place.
constexpr float kLabelWidth = 44.0f;
constexpr float kReadoutWidth = 60.0f;

const ImVec4& ToneColour(MeterTone tone)
{
	switch (tone)
	{
		case MeterTone::Comfortable:
			return theme::kMeterComfortable;

		case MeterTone::Warn:
			return theme::kMeterWarn;

		case MeterTone::Hot:
			return theme::kMeterHot;

		case MeterTone::Clip:
			return theme::kMeterClip;

		case MeterTone::Off:
			break;
	}

	// An unlit segment is the well showing through, one step up from the
	// background so the bar's full length stays readable when it is quiet.
	return theme::kGray800;
}

// Draws the sunken well and its segments. Returns the well's rectangle, so the
// caller can put a peak marker inside it.
void DrawWell(const ImVec2& low, const ImVec2& high, int lit_count, const ImVec4* colours)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();

	draw->AddRectFilled(low, high, ImGui::GetColorU32(theme::kBgInset), theme::kRadiusControl);
	draw->AddRect(low, high, ImGui::GetColorU32(theme::kBorderHairline), theme::kRadiusControl);

	const float inner_x = low.x + kWellPadding;
	const float inner_width = (high.x - low.x) - kWellPadding * 2.0f;

	const float step = inner_width / static_cast<float>(kMeterSegments);
	const float width = step - kSegmentGap;

	for (int i = 0; i < lit_count; ++i)
	{
		const float x = inner_x + step * static_cast<float>(i);

		draw->AddRectFilled(ImVec2(x, low.y + kWellPadding), ImVec2(x + width, high.y - kWellPadding),
			ImGui::GetColorU32(colours[i]), kSegmentRounding);
	}
}

}  // namespace

void DrawMeter(const char* label, float db, bool clipping, MeterState& peak, float height)
{
	peak.peak_db = DecayPeak(peak.peak_db, db, ImGui::GetIO().DeltaTime);

	const ImVec2 start = ImGui::GetCursorScreenPos();

	// Captured before anything is drawn. Every position below is measured from
	// `start.x` and this width, because the cursor moves as the row is drawn
	// and asking again mid-row gives an answer relative to wherever it has got
	// to -- which is how the reading ends up off the end of the panel.
	const float available = ImGui::GetContentRegionAvail().x;
	const float row = std::max(height, ImGui::GetTextLineHeight());

	// The label, vertically centred against the well rather than the row, so a
	// 20px meter in Live doesn't leave its label floating.
	ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (row - ImGui::GetTextLineHeight()) * 0.5f));
	ImGui::TextColored(theme::kTextSecondary, "%s", label);

	const ImVec2 low(start.x + kLabelWidth, start.y + (row - height) * 0.5f);
	const ImVec2 high(start.x + available - kReadoutWidth - theme::kSpace2, low.y + height);

	ImVec4 colours[kMeterSegments];
	int lit = 0;

	for (int i = 0; i < kMeterSegments; ++i)
	{
		const MeterTone tone = SegmentTone(i, kMeterSegments, db, clipping);

		if (tone == MeterTone::Off)
		{
			break;
		}

		colours[lit++] = ToneColour(tone);
	}

	DrawWell(low, high, lit, colours);

	// The peak marker, inside the well and on top of the segments. Grey rather
	// than accent-coloured: it has to stay legible on top of every segment
	// colour, and it matters most where the segments are brightest.
	if (std::isfinite(peak.peak_db) && peak.peak_db > kMeterFloorDb)
	{
		const float inner_x = low.x + kWellPadding;
		const float inner_width = (high.x - low.x) - kWellPadding * 2.0f;
		const float at = inner_x + MeterFraction(peak.peak_db) * inner_width;

		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at - 1.0f, low.y + kWellPadding),
			ImVec2(at + 1.0f, high.y - kWellPadding), ImGui::GetColorU32(theme::kPeakMarker));
	}

	// The reading. Mono and right-aligned so the number stays in one place as
	// it changes width -- a figure that shuffles sideways is one you have to
	// stop and read.
	char reading[32];

	if (std::isfinite(db))
	{
		std::snprintf(reading, sizeof(reading), "%.1f dB", db);
	}
	else
	{
		std::snprintf(reading, sizeof(reading), "--");
	}

	{
		ScopedFont mono(Face::Mono, text::kXs);

		const float width = ImGui::CalcTextSize(reading).x;

		ImGui::SetCursorScreenPos(
			ImVec2(start.x + available - width, start.y + (row - ImGui::GetTextLineHeight()) * 0.5f));

		ImGui::TextColored(clipping ? theme::kMeterClip : theme::kTextPrimary, "%s", reading);
	}

	ImGui::SetCursorScreenPos(start);
	ImGui::Dummy(ImVec2(available, row));

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Peak since the last frame. 0 dB is full scale -- red means the signal is being clipped.");
	}
}

void DrawMeterScale(float label_width, float readout_width)
{
	static const char* kMarks[] = {"-60", "-30", "-18", "-12", "-6", "0"};

	const ImVec2 start = ImGui::GetCursorScreenPos();
	const float full = ImGui::GetContentRegionAvail().x;
	const float span = full - label_width - readout_width - theme::kSpace2;

	ScopedFont mono(Face::Mono, text::k2xs);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 colour = ImGui::GetColorU32(theme::kTextDisabled);

	const int count = static_cast<int>(sizeof(kMarks) / sizeof(kMarks[0]));

	for (int i = 0; i < count; ++i)
	{
		// Spread across the well rather than placed at their true dB
		// positions. The design draws them evenly, and it is right to: these
		// are a legend for reading the bar, not a ruler against it, and -60 to
		// -30 being half the width would make the useful end unreadable.
		const float fraction = static_cast<float>(i) / static_cast<float>(count - 1);
		const float width = ImGui::CalcTextSize(kMarks[i]).x;

		draw->AddText(ImVec2(start.x + label_width + fraction * (span - width), start.y), colour, kMarks[i]);
	}

	ImGui::Dummy(ImVec2(full, ImGui::GetTextLineHeight()));
}

void DrawLoadGauge(const char* label, float fraction, const char* tooltip)
{
	const float clamped = std::isfinite(fraction) ? std::min(1.0f, std::max(0.0f, fraction)) : 0.0f;

	const ImVec2 start = ImGui::GetCursorScreenPos();
	const float available = ImGui::GetContentRegionAvail().x;
	const float height = 14.0f;
	const float row = std::max(height, ImGui::GetTextLineHeight());

	ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (row - ImGui::GetTextLineHeight()) * 0.5f));
	ImGui::TextColored(theme::kTextSecondary, "%s", label);

	const ImVec2 low(start.x + kLabelWidth, start.y + (row - height) * 0.5f);
	const ImVec2 high(start.x + available - kReadoutWidth - theme::kSpace2, low.y + height);

	// Green until it is worth worrying about, then the meter's own warn and hot
	// -- the same three colours meaning the same three things, since running out
	// of block time and running out of headroom are both "this is about to
	// break up".
	const ImVec4& colour = clamped > 0.80f ? theme::kMeterHot
		: clamped > 0.60f                  ? theme::kMeterWarn
										   : theme::kMeterComfortable;

	const int lit = static_cast<int>(clamped * static_cast<float>(kMeterSegments) + 0.5f);

	ImVec4 colours[kMeterSegments];

	for (int i = 0; i < lit && i < kMeterSegments; ++i)
	{
		colours[i] = colour;
	}

	DrawWell(low, high, std::min(lit, kMeterSegments), colours);

	char reading[16];
	std::snprintf(reading, sizeof(reading), "%.0f %%", static_cast<double>(clamped * 100.0f));

	{
		ScopedFont mono(Face::Mono, text::kXs);

		const float width = ImGui::CalcTextSize(reading).x;

		ImGui::SetCursorScreenPos(
			ImVec2(start.x + available - width, start.y + (row - ImGui::GetTextLineHeight()) * 0.5f));

		ImGui::TextColored(theme::kTextPrimary, "%s", reading);
	}

	ImGui::SetCursorScreenPos(start);
	ImGui::Dummy(ImVec2(available, row));

	if (tooltip && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%s", tooltip);
	}
}

}  // namespace sf
