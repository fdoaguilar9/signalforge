#pragma once

#include "imgui.h"

namespace sf
{

// The design system's tokens, as ImGui colours and metrics (SF-60).
//
// `design/design_handoff_pedalboard_ui/_ds/*/tokens/` is the authority for
// every value in here, and `README.md` alongside it for every metric. When the
// two disagree with what is written below, the design files are right and this
// file is stale.
//
// There are two kinds of colour here, and where each comes from matters:
//
//   - The chassis greys and the amber accent are the *design's*. They exist
//     nowhere else in the program and are converted from its hex once, here.
//   - The semantic colours -- meters, warnings -- are *not* converted. They
//     were read out of DrawLevelMeter when the design was made, so the code is
//     the source and the token CSS is the copy. Round-tripping them through
//     hex would move each by a hair and leave the two files quietly
//     disagreeing about which one is right.
namespace theme
{

// 0xRRGGBB as the design writes it, so a token can be checked against the CSS
// by reading rather than by arithmetic.
constexpr ImVec4 Rgb(unsigned hex, float alpha = 1.0f)
{
	return ImVec4(static_cast<float>((hex >> 16) & 0xffu) / 255.0f, static_cast<float>((hex >> 8) & 0xffu) / 255.0f,
		static_cast<float>(hex & 0xffu) / 255.0f, alpha);
}

// --- base chassis (charcoal, brushed steel) ---
constexpr ImVec4 kGray950 = Rgb(0x0e0f11);
constexpr ImVec4 kGray900 = Rgb(0x16171a);
constexpr ImVec4 kGray850 = Rgb(0x1c1d20);
constexpr ImVec4 kGray800 = Rgb(0x222327);
constexpr ImVec4 kGray700 = Rgb(0x2c2d32);
constexpr ImVec4 kGray600 = Rgb(0x38393f);
constexpr ImVec4 kGray500 = Rgb(0x525259);
constexpr ImVec4 kGray400 = Rgb(0x75757c);
constexpr ImVec4 kGray300 = Rgb(0x9a9aa1);
constexpr ImVec4 kGray200 = Rgb(0xc2c2c7);
constexpr ImVec4 kGray100 = Rgb(0xe8e6e0);

// --- brand accent: tube-glow amber (tolex piping / jewel lamp) ---
constexpr ImVec4 kAmber500 = Rgb(0xd68a34);
constexpr ImVec4 kAmber400 = Rgb(0xe0a458);
constexpr ImVec4 kAmber300 = Rgb(0xeabd7f);

// --- secondary accent: brushed chrome, for links and nothing else ---
constexpr ImVec4 kSteel400 = Rgb(0x8ea9c0);

// --- semantic aliases ---
constexpr ImVec4 kBgApp = kGray900;
constexpr ImVec4 kBgSurface = kGray850;
constexpr ImVec4 kBgSurfaceRaised = kGray800;
constexpr ImVec4 kBgInset = kGray950;
constexpr ImVec4 kBorderHairline = kGray700;
constexpr ImVec4 kBorderStrong = kGray600;
constexpr ImVec4 kTextPrimary = kGray100;
constexpr ImVec4 kTextSecondary = kGray300;
constexpr ImVec4 kTextDisabled = kGray500;
constexpr ImVec4 kAccent = kAmber400;
constexpr ImVec4 kAccentHover = kAmber300;
constexpr ImVec4 kAccentActive = kAmber500;
constexpr ImVec4 kAccentOn = kGray950;  // text on top of an amber fill
constexpr ImVec4 kLink = kSteel400;

// The meter's peak-hold marker. Grey rather than accent-coloured on purpose:
// it has to be legible on top of every segment colour, and an amber marker
// disappears into the hot part of the scale, which is where it matters most.
constexpr ImVec4 kPeakMarker = kGray200;

// --- semantic, and taken from the code rather than from the design ---
constexpr ImVec4 kMeterComfortable(0.25f, 0.70f, 0.35f, 1.0f);
constexpr ImVec4 kMeterWarn(0.90f, 0.80f, 0.20f, 1.0f);
constexpr ImVec4 kMeterHot(0.95f, 0.55f, 0.15f, 1.0f);
constexpr ImVec4 kMeterClip(0.90f, 0.25f, 0.25f, 1.0f);

// The board disagreeing with its preset (SF-31), and a saved audio device
// that isn't plugged in (SF-39). Two conditions rather than two severities,
// which is why they are two colours and not one with an alpha.
// In tune, and the only green in the design (SF-81). The tuner is the one
// screen with a right answer, so it is the one place worth having a colour
// that means "yes" rather than "look at this".
constexpr ImVec4 kStatusOk(0.35f, 0.82f, 0.48f, 1.0f);

constexpr ImVec4 kStatusWarning(1.0f, 0.72f, 0.25f, 1.0f);
constexpr ImVec4 kStatusUnavailable(1.0f, 0.60f, 0.40f, 1.0f);

// --- metrics, in unscaled pixels, as the design states them ---
constexpr float kRadiusControl = 3.0f;
constexpr float kRadiusCard = 5.0f;
constexpr float kRadiusModal = 8.0f;

constexpr float kHeaderHeight = 52.0f;

// Fixed, and wide enough for the preset list and the engine table without
// either wrapping. The chain takes whatever is left.
constexpr float kSidebarWidth = 330.0f;

// The spacing scale. Not every step is used by every screen, but a gap that
// isn't on this list is a gap someone guessed at.
constexpr float kSpace1 = 4.0f;
constexpr float kSpace2 = 8.0f;
constexpr float kSpace3 = 12.0f;
constexpr float kSpace4 = 16.0f;
constexpr float kSpace5 = 20.0f;
constexpr float kSpace6 = 24.0f;

}  // namespace theme

// Sets every colour and metric on the current ImGui style. Called once, from
// AppWindow::Open, in place of ImGui::StyleColorsDark().
//
// One function rather than colours pushed where they are used, so that "what
// colour is a button" has exactly one answer. The exceptions are the semantic
// colours above -- a meter segment and a warning line say something with
// colour rather than merely being drawn in one, and those stay at their call
// sites.
void ApplyTheme();

}  // namespace sf
