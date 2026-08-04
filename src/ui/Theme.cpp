#include "Theme.h"

namespace sf
{

void ApplyTheme()
{
	ImGuiStyle& style = ImGui::GetStyle();

	// Start from Dark so that anything not named below is at least dark rather
	// than at ImGui's default blue-grey. Every colour that shows on either
	// screen is named; the ones that aren't belong to features this program
	// doesn't have (docking, tabs, plots).
	ImGui::StyleColorsDark();

	ImVec4* colours = style.Colors;

	colours[ImGuiCol_Text] = theme::kTextPrimary;
	colours[ImGuiCol_TextDisabled] = theme::kTextDisabled;

	colours[ImGuiCol_WindowBg] = theme::kBgApp;

	// Transparent, not a colour: child windows here are scroll regions and
	// bands, and each one paints its own background when it wants one. A
	// default fill would mean every band that wanted the app colour had to
	// override back to it.
	colours[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	colours[ImGuiCol_PopupBg] = theme::kBgSurface;

	colours[ImGuiCol_Border] = theme::kBorderHairline;
	colours[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

	// A slider track, a text box, a combo -- all of them are wells sunk into
	// the chassis in this design, so they start at the inset colour and step
	// *up* on hover rather than down.
	colours[ImGuiCol_FrameBg] = theme::kBgInset;
	colours[ImGuiCol_FrameBgHovered] = theme::kGray800;
	colours[ImGuiCol_FrameBgActive] = theme::kGray700;

	colours[ImGuiCol_TitleBg] = theme::kBgSurface;
	colours[ImGuiCol_TitleBgActive] = theme::kBgSurface;
	colours[ImGuiCol_TitleBgCollapsed] = theme::kBgSurface;
	colours[ImGuiCol_MenuBarBg] = theme::kBgSurface;

	colours[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	colours[ImGuiCol_ScrollbarGrab] = theme::kBorderStrong;
	colours[ImGuiCol_ScrollbarGrabHovered] = theme::kGray500;
	colours[ImGuiCol_ScrollbarGrabActive] = theme::kGray400;

	// The three places the accent actually appears on the board: a ticked
	// bypass box, a slider handle, and the keyboard focus ring. Used sparingly
	// on purpose -- an amber that is everywhere stops meaning "this one".
	colours[ImGuiCol_CheckMark] = theme::kAccent;
	colours[ImGuiCol_SliderGrab] = theme::kAccent;
	colours[ImGuiCol_SliderGrabActive] = theme::kAccentHover;

	// The secondary weight, which is what a bare ImGui::Button gets. Ghost and
	// primary are drawn by their own helpers (SF-63) because ImGui has one
	// button colour and the design has three.
	colours[ImGuiCol_Button] = theme::kBgSurfaceRaised;
	colours[ImGuiCol_ButtonHovered] = theme::kGray700;
	colours[ImGuiCol_ButtonActive] = theme::kGray600;

	// Selectables and collapsing headers.
	colours[ImGuiCol_Header] = theme::kBgSurfaceRaised;
	colours[ImGuiCol_HeaderHovered] = theme::kGray700;
	colours[ImGuiCol_HeaderActive] = theme::kGray700;

	colours[ImGuiCol_Separator] = theme::kBorderHairline;
	colours[ImGuiCol_SeparatorHovered] = theme::kBorderStrong;
	colours[ImGuiCol_SeparatorActive] = theme::kBorderStrong;

	colours[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	colours[ImGuiCol_ResizeGripHovered] = theme::kBorderStrong;
	colours[ImGuiCol_ResizeGripActive] = theme::kBorderStrong;

	colours[ImGuiCol_TableHeaderBg] = theme::kBgSurface;
	colours[ImGuiCol_TableBorderStrong] = theme::kBorderStrong;
	colours[ImGuiCol_TableBorderLight] = theme::kBorderHairline;
	colours[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	colours[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);

	colours[ImGuiCol_TextSelectedBg] = ImVec4(theme::kAccent.x, theme::kAccent.y, theme::kAccent.z, 0.30f);
	colours[ImGuiCol_DragDropTarget] = theme::kAccent;

	// Keyboard reachability matters on a board that may be driven without a
	// mouse, so the focus ring is the accent at full strength rather than a
	// dimmed hint.
	colours[ImGuiCol_NavCursor] = theme::kAccent;

	// Plain black at 60%, and **no blur** -- the design says so explicitly, and
	// a blurred scrim over a signal chain hides the thing the modal is about.
	colours[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);

	// ProgressBar's fill, which is what the old meter used. Kept pointing at
	// the accent so anything still using it looks deliberate until SF-64
	// replaces the meters outright.
	colours[ImGuiCol_PlotHistogram] = theme::kAccent;
	colours[ImGuiCol_PlotHistogramHovered] = theme::kAccentHover;

	// --- metrics ---

	// Three radii, and each one means a scale of thing: a control, a card, a
	// modal. Anything rounded to a fourth value is a mistake.
	style.FrameRounding = theme::kRadiusControl;
	style.GrabRounding = theme::kRadiusControl;
	style.ScrollbarRounding = theme::kRadiusControl;
	style.TabRounding = theme::kRadiusControl;
	style.ChildRounding = theme::kRadiusCard;
	style.WindowRounding = theme::kRadiusModal;
	style.PopupRounding = theme::kRadiusModal;

	// Hairlines everywhere. The design draws borders as 1px at every scale,
	// and a 2px border on a 3px-radius control reads as a mistake.
	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.SeparatorTextBorderSize = 1.0f;

	style.WindowPadding = ImVec2(theme::kSpace4, theme::kSpace4);
	style.FramePadding = ImVec2(10.0f, 6.0f);
	style.CellPadding = ImVec2(theme::kSpace2, 6.0f);
	style.ItemSpacing = ImVec2(theme::kSpace2, theme::kSpace2);
	style.ItemInnerSpacing = ImVec2(theme::kSpace2, 6.0f);

	// 24px, which is the indent the design gives a stage card's expanded body
	// -- the one place indentation carries meaning here.
	style.IndentSpacing = theme::kSpace6;

	style.ScrollbarSize = 12.0f;
	style.GrabMinSize = 10.0f;

	// A disabled control has to be legible enough to read its own tooltip
	// explaining why it is disabled, which ImGui's 0.6 default makes a
	// squint at these text colours.
	style.DisabledAlpha = 0.45f;

	// Off-centre labels read as sloppy at these type sizes.
	style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

	// Sharp edges on text and lines. The whole design is hairlines and 1px
	// markers, and unsharpened they turn into grey mush.
	style.AntiAliasedLines = true;
	style.AntiAliasedLinesUseTex = true;
	style.AntiAliasedFill = true;
}

}  // namespace sf
