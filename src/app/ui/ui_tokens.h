// Design tokens with no ImGui dependency: the radii system and the font
// floor. The colour palette lives beside them in player_window.cpp's theme
// namespace (it needs ImVec4); the geometry tokens stay here so the suite
// can pin their values without linking the window layer.
//
// Values are the redesign v2 contract (docs/prototypes.md 2026-10-06:
// "字号地板 11px、圆角成文，面板 10 / 控件 7 / 徽标 pill / 图标钮圆形"),
// matched by the prototype's --r-panel / --r-ctl in
// prototypes/redesign/assets/proto.css. The icon-button radius has no
// consumer yet — the OSC's transport is still text buttons (the audit's V6),
// and the paused play button is a drawn circle, not a rounded frame.

#ifndef SOAR_APP_UI_TOKENS_H_
#define SOAR_APP_UI_TOKENS_H_

namespace soar::app {

// 4px grid panels: the OSC bar, the popup pages, the poster and toast shells.
constexpr float kPanelRounding = 10.0f;
// Inline controls: buttons, sliders, combos, headers, slider grabs.
constexpr float kControlRounding = 7.0f;
// Badges and floating hints: the state chips and the paused hint pill
// (the prototype's .statechip / .floatpill both use border-radius:999px).
// Larger than half of any chip height, so it renders as a full pill.
constexpr float kPillRounding = 999.0f;
// No UI text below this size.
constexpr float kMinFontSize = 11.0f;
// The window layer's one font size (labels, buttons, menus). Subtitle
// rendering loads its own larger set, so this is the smallest text drawn
// outside the subtitle canvas — and therefore the size the floor pins.
constexpr float kUiFontSize = 16.0f;
static_assert(kUiFontSize >= kMinFontSize,
              "the UI font must sit at or above the floor");
// The focus-visible ring (V7b, docs/ui-design.md §5.2): the prototype's
// :focus-visible outline — 2px accent drawn 4px outward from the focused
// row (the same geometry ImGui's own nav cursor uses, kept because the
// app draws the ring itself and nav stays off).
constexpr float kFocusRingWidth = 2.0f;
constexpr float kFocusRingExpand = 4.0f;
static_assert(kFocusRingWidth > 0.0f && kFocusRingExpand >= kFocusRingWidth,
              "the focus ring must be visible past the row it names");

}  // namespace soar::app

#endif  // SOAR_APP_UI_TOKENS_H_
