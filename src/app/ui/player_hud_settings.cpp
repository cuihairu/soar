// PlayerHud settings page (nav, tabs, endpoint rows).
// Split out of player_window.cpp; whole file is ImGui-only.
#ifdef SOAR_WITH_IMGUI

#include "player_hud.h"

namespace soar::app {

bool PlayerHud::settingsSwitch(const char* id, bool* on) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(34.0f, 20.0f));
  const bool hovered = ImGui::IsItemHovered();
  ImU32 bg;
  ImU32 border;
  if (*on) {
    bg = ImGui::ColorConvertFloat4ToU32(theme::kAccent);
    border = bg;
  } else {
    bg = ImGui::ColorConvertFloat4ToU32(hovered ? theme::kFieldBgHi
                                                : theme::kFieldBg);
    border = ImGui::ColorConvertFloat4ToU32(theme::kLineStrong);
  }
  dl->AddRectFilled(p, ImVec2(p.x + 34.0f, p.y + 20.0f), bg,
                    theme::kPillRounding);
  dl->AddRect(p, ImVec2(p.x + 34.0f, p.y + 20.0f), border,
              theme::kPillRounding);
  const float kx = *on ? p.x + 34.0f - 2.0f - 14.0f : p.x + 2.0f;
  dl->AddCircleFilled(
      ImVec2(kx + 7.0f, p.y + 10.0f), 7.0f,
      ImGui::ColorConvertFloat4ToU32(*on ? theme::kInkOnAccent
                                         : theme::kText3));
  if (clicked) *on = !*on;
  return clicked;
}

bool PlayerHud::settingsStepper(const char* id, int* val, int delta, int lo, int hi,
                     const char* fmt) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), fmt, *val);
  const std::string key(id);
  bool changed = false;
  ImGui::PushStyleColor(ImGuiCol_Button, theme::kFieldBg);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kFieldBgHi);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kFieldBgHi);
  ImGui::PushStyleColor(ImGuiCol_Border, theme::kCardLine);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText2);
  if (ImGui::Button((std::string("-") + key + "_m").c_str(), ImVec2(24.0f, 24.0f))) {
    if (*val - delta >= lo) {
      *val -= delta;
      changed = true;
    }
  }
  ImGui::SameLine(0.0f, 2.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(44.0f, 24.0f));  // .val field
  ImGui::SameLine(0.0f, 2.0f);
  if (ImGui::Button((std::string("+") + key + "_p").c_str(), ImVec2(24.0f, 24.0f))) {
    if (*val + delta <= hi) {
      *val += delta;
      changed = true;
    }
  }
  ImGui::PopStyleColor(5);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(buf);
  dl->AddText(ImVec2(p.x + (44.0f - ts.x) * 0.5f, p.y + (24.0f - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(theme::kText), buf);
  return changed;
}

void PlayerHud::configureHttpSubtitles(bool enabled) {
  soar::HttpSubtitleConfig sub = subtitle_env_cfg_;
  if (!enabled) sub.endpoint.clear();
  external_.configure(std::move(sub));
}

void PlayerHud::resetSettingsDefaults() {
  st_.remember_last_media = true;
  st_.esc_exits_fullscreen = true;
  st_.subtitle_visible = true;
  st_.subtitle_font_size = 24.0f;
  st_.subtitle_offset_ms = 0;
  st_.subtitle_autoshow = true;
  st_.subtitle_autodl = true;
  configureHttpSubtitles(true);
  st_.recent_cap = 15;
  recent_.setMaxEntries(15);
  recent_.save();
  saveRecentCap(15);
  if (cfg_.subtitle_visible) {
    cfg_.subtitle_visible->store(true, std::memory_order_relaxed);
  }
  if (cfg_.subtitle_font_size) {
    cfg_.subtitle_font_size->store(24.0f, std::memory_order_relaxed);
  }
  if (cfg_.subtitle_offset_ms) {
    cfg_.subtitle_offset_ms->store(0, std::memory_order_relaxed);
  }
  if (theme::g_light) {
    theme::applyPalette(false);
    applyTheme();
    saveThemePref();
  }
  st_.last_rate = 1.0;
  player_.setRate(1.0);
  playlist_.setLoop(PlaylistStore::Loop::Off);
  if (playlist_.shuffle()) playlist_.setShuffle(false);
}

void PlayerHud::settingsNavIcon(ImDrawList* dl, const ImVec2 c, SettingsTab tab,
                     ImU32 col) {
  const float th = 1.5f;
  switch (tab) {
    case SettingsTab::General: {  // sun: a small disc + eight rays
      dl->AddCircleFilled(c, 2.5f, col);
      for (int i = 0; i < 8; ++i) {
        const float a = i * 0.7853981634f;  // pi/4
        dl->AddLine(ImVec2(c.x + std::cos(a) * 4.5f, c.y + std::sin(a) * 4.5f),
                    ImVec2(c.x + std::cos(a) * 7.0f, c.y + std::sin(a) * 7.0f),
                    col, th);
      }
      break;
    }
    case SettingsTab::Playback: {  // play outline + the leading bar
      dl->AddTriangle(ImVec2(c.x + 5.3f, c.y + 3.7f),
                      ImVec2(c.x + 5.3f, c.y + 12.3f),
                      ImVec2(c.x + 12.7f, c.y + 8.0f), col, th);
      dl->AddLine(ImVec2(c.x + 2.7f, c.y + 3.3f),
                  ImVec2(c.x + 2.7f, c.y + 12.7f), col, th);
      break;
    }
    case SettingsTab::Subtitles: {  // rounded screen + two line pairs
      dl->AddRect(ImVec2(c.x + 1.3f, c.y + 2.0f),
                  ImVec2(c.x + 14.7f, c.y + 11.3f), col, 2.0f, 0, th);
      dl->AddLine(ImVec2(c.x + 3.3f, c.y + 5.8f), ImVec2(c.x + 6.0f, c.y + 5.8f),
                  col, th);
      dl->AddLine(ImVec2(c.x + 8.7f, c.y + 5.8f),
                  ImVec2(c.x + 11.3f, c.y + 5.8f), col, th);
      dl->AddLine(ImVec2(c.x + 3.3f, c.y + 8.5f), ImVec2(c.x + 8.0f, c.y + 8.5f),
                  col, th);
      break;
    }
    case SettingsTab::Network: {  // globe: disc + equator + two meridians
      dl->AddCircle(c, 6.0f, col, 0, th);
      dl->AddLine(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x + 6.0f, c.y), col, th);
      dl->AddEllipse(c, ImVec2(2.7f, 6.0f), col, 0.0f, 0, th);
      dl->AddEllipse(c, ImVec2(1.0f, 6.0f), col, 0.0f, 0, th);
      break;
    }
    case SettingsTab::About: {  // info: circle + stem + dot
      dl->AddCircle(c, 6.0f, col, 0, th);
      dl->AddLine(ImVec2(c.x, c.y + 2.2f), ImVec2(c.x, c.y + 4.6f), col, th);
      dl->AddCircleFilled(ImVec2(c.x, c.y - 2.6f), 0.8f, col);
      break;
    }
  }
}

void PlayerHud::drawGradientMark(ImDrawList* dl, const ImVec2& p, float size, float r,
                      ImU32) {
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                    ImGui::ColorConvertFloat4ToU32(theme::kAccent), r);
  const ImU32 ink = ImGui::ColorConvertFloat4ToU32(
      ImVec4(0.078f, 0.039f, 0.016f, 1.0f));  // #140A04
  const ImVec2 ss = ImGui::CalcTextSize("S");
  dl->AddText(ImVec2(p.x + (size - ss.x) * 0.5f, p.y + (size - ss.y) * 0.5f),
              ink, "S");
}

bool PlayerHud::settingsNavRow(const char* id, SettingsTab tab, const char* label,
                    bool active) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // .nav padding 10px on each side: the row fills the nav's content box,
  // whatever the previous item left the cursor at.
  ImGui::SetCursorPosX(10.0f);
  const float w = ImGui::GetWindowWidth() - 20.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, 32.0f));
  const bool hovered = ImGui::IsItemHovered();
  if (active) {
    ImVec4 a = theme::kAccent;
    a.w = 0.14f;  // --accent-soft
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 32.0f),
                      ImGui::ColorConvertFloat4ToU32(a), 8.0f);
  } else if (hovered) {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 32.0f),
                      ImGui::ColorConvertFloat4ToU32(theme::kFieldBg), 8.0f);
  }
  const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
      active ? theme::kAccentHi : (hovered ? theme::kText2 : theme::kText3));
  // icon 16px at the row's 10px padding, then the 10px gap -> text at 36
  settingsNavIcon(dl, ImVec2(p.x + 10.0f, p.y + 16.0f), tab, fg);
  const ImVec2 ls = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(p.x + 36.0f, p.y + (32.0f - ls.y) * 0.5f), fg, label);
  return clicked;
}

void PlayerHud::settingsGrp(const char* label) {
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
  // .nav .grp margin-left 8 on the nav's 10px padding.
  ImGui::SetCursorPosX(18.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText4);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
}

void PlayerHud::settingCard(const char* id, const char* title, const char* desc,
                 float control_w, float control_h,
                 const std::function<void()>& control) {
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kPanelRounding);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kCardBg);
  ImGui::PushStyleColor(ImGuiCol_Border, theme::kCardLine);
  if (ImGui::BeginChild(id, ImVec2(-1.0f, 52.0f),
                        ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::SetCursorPos(ImVec2(12.0f, 8.0f));
    ImGui::TextUnformatted(title);
    ImGui::SetCursorPos(ImVec2(12.0f, 28.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
    ImGui::TextUnformatted(desc);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(
        ImVec2(ImGui::GetContentRegionAvail().x - control_w - 10.0f,
               (52.0f - control_h) * 0.5f));
    control();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

bool PlayerHud::dangerButton(const char* label, ImVec2 size) {
  ImGui::PushStyleColor(
      ImGuiCol_Button, ImGui::ColorConvertFloat4ToU32(ImVec4(
                           theme::kDanger.x, theme::kDanger.y,
                           theme::kDanger.z, 0.10f)));
  ImGui::PushStyleColor(
      ImGuiCol_ButtonHovered, ImGui::ColorConvertFloat4ToU32(ImVec4(
                                  theme::kDanger.x, theme::kDanger.y,
                                  theme::kDanger.z, 0.18f)));
  ImGui::PushStyleColor(
      ImGuiCol_ButtonActive, ImGui::ColorConvertFloat4ToU32(ImVec4(
                                 theme::kDanger.x, theme::kDanger.y,
                                 theme::kDanger.z, 0.18f)));
  ImGui::PushStyleColor(
      ImGuiCol_Border, ImGui::ColorConvertFloat4ToU32(ImVec4(
                           theme::kDanger.x, theme::kDanger.y,
                           theme::kDanger.z, 0.40f)));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  return clicked;
}

bool PlayerHud::settingsSelectBox(const char* id, const char* label, float w,
                       bool disabled) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = false;
  if (disabled) {
    ImGui::Dummy(ImVec2(w, 26.0f));
  } else {
    clicked = ImGui::InvisibleButton(id, ImVec2(w, 26.0f));
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool hov = !disabled && ImGui::IsItemHovered();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 26.0f),
                    ImGui::ColorConvertFloat4ToU32(hov ? theme::kFieldBgHi
                                                       : theme::kFieldBg),
                    kControlRounding);
  dl->AddRect(p, ImVec2(p.x + w, p.y + 26.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kLineStrong),
              kControlRounding);
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(p.x + 8.0f, p.y + (26.0f - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(disabled ? theme::kText2
                                                      : theme::kText),
              label);
  return clicked;
}

void PlayerHud::settingsPill(const char* label, const ImVec4& ink) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(label);
  const float w = ts.x + 16.0f;
  const float h = ts.y + 6.0f;
  ImVec4 face = ink;
  face.w = 0.14f;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                    ImGui::ColorConvertFloat4ToU32(face), 99.0f);
  dl->AddText(ImVec2(p.x + 8.0f, p.y + 3.0f),
              ImGui::ColorConvertFloat4ToU32(ink), label);
  ImGui::Dummy(ImVec2(w, h));
}

void PlayerHud::settingsTextInput(const char* value, float w) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, 26.0f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 26.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kFieldBg),
                    kControlRounding);
  dl->AddRect(p, ImVec2(p.x + w, p.y + 26.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kLineStrong),
              kControlRounding);
  const int max_chars = std::max(4, static_cast<int>((w - 18.0f) / 8.0f));
  const std::string text = clipEllipsis(value, max_chars);
  const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  dl->AddText(ImVec2(p.x + 9.0f, p.y + (26.0f - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(theme::kText2), text.c_str());
}

void PlayerHud::settingsSect(const char* label) {
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText4);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

void PlayerHud::versionRow(const char* id, const char* k, const char* v) {
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kPanelRounding);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kPageBg);
  ImGui::PushStyleColor(ImGuiCol_Border, theme::kCardLine);
  if (ImGui::BeginChild(id, ImVec2(-1.0f, 32.0f),
                        ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::SetCursorPos(ImVec2(12.0f, 7.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
    ImGui::TextUnformatted(k);
    ImGui::PopStyleColor();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 vs = ImGui::CalcTextSize(v);
    ImGui::SetCursorPos(ImVec2(w - vs.x - 10.0f, 7.0f));
    ImGui::TextUnformatted(v);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

void PlayerHud::endpointRow(const char* id, const char* tag, const char* value,
                 milliseconds now) {
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kPanelRounding);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kCardBg);
  ImGui::PushStyleColor(ImGuiCol_Border, theme::kCardLine);
  if (ImGui::BeginChild(id, ImVec2(-1.0f, 40.0f),
                        ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse)) {
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPos(ImVec2(12.0f, 11.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
    ImGui::TextUnformatted(tag);
    ImGui::PopStyleColor();
    // The tag column tracks the label's real width (the 16px bitmap font
    // is wider than the prototype's 10px .tag, so the fixed 52px column
    // would overlap the value).
    const float tag_w = ImGui::CalcTextSize(tag).x;
    const float value_w = avail - (12.0f + tag_w + 16.0f) - 68.0f;
    const int max_chars = std::max(8, static_cast<int>(value_w / 8.0f));
    ImGui::SetCursorPos(ImVec2(12.0f + tag_w + 16.0f, 11.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kText4);
    ImGui::TextUnformatted(clipEllipsis(value, max_chars).c_str());
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(avail - 58.0f, 8.0f));
    if (dangerButton("Edit##ep", ImVec2(48.0f, 24.0f))) {
      st_.toast.show("Endpoint not configured", now);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

void PlayerHud::drawSettingsOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  // The full-height page must sit above the video/pause layers; the
  // focus request puts the page at the front of the z-order every frame
  // it is open.
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPageBg);
  ImGui::PushStyleColor(ImGuiCol_Border, 0);
  if (ImGui::Begin("Settings", &open,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoSavedSettings)) {
    // ---- nav sidebar (settings.html .nav: 196px --bg0) ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kNavBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##set_nav", ImVec2(196.0f, 0.0f), ImGuiChildFlags_None);
    {
      // .nav padding 16px 10px, set explicitly (a child window's content
      // origin does not track the pushed WindowPadding the way a top
      // window's does, so the wing/rows would otherwise start at the
      // child's corner).
      ImGui::SetCursorPos(ImVec2(10.0f, 16.0f));
      ImDrawList* dl = ImGui::GetWindowDrawList();
      // .nav .wing: the 26px rounded gradient mark, indented 8px more than
      // the rows (.wing margin-left 8 on the nav's 10px padding).
      const ImVec2 lp = ImGui::GetCursorScreenPos();
      drawGradientMark(dl, ImVec2(lp.x + 8.0f, lp.y), 26.0f, 8.0f,
                       ImGui::ColorConvertFloat4ToU32(theme::kNavBg));
      ImGui::Dummy(ImVec2(26.0f, 26.0f));
      // .wing margin-bottom: 16px below the mark before the first tab.
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16.0f);
      if (settingsNavRow("##st_general", SettingsTab::General, "General",
                         st_.settings_tab == SettingsTab::General)) {
        st_.settings_tab = SettingsTab::General;
      }
      if (settingsNavRow("##st_playback", SettingsTab::Playback, "Playback",
                         st_.settings_tab == SettingsTab::Playback)) {
        st_.settings_tab = SettingsTab::Playback;
      }
      if (settingsNavRow("##st_subtitles", SettingsTab::Subtitles, "Subtitles",
                         st_.settings_tab == SettingsTab::Subtitles)) {
        st_.settings_tab = SettingsTab::Subtitles;
      }
      settingsGrp("NETWORK");
      if (settingsNavRow("##st_network", SettingsTab::Network,
                         "Network & Download",
                         st_.settings_tab == SettingsTab::Network)) {
        st_.settings_tab = SettingsTab::Network;
      }
      settingsGrp("ABOUT");
      if (settingsNavRow("##st_about", SettingsTab::About, "About",
                         st_.settings_tab == SettingsTab::About)) {
        st_.settings_tab = SettingsTab::About;
      }
      // .nav border-right: the hairline splitting nav from content.
      const ImVec2 np = ImGui::GetWindowPos();
      const float nh = ImGui::GetWindowHeight();
      dl->AddLine(ImVec2(np.x + 195.5f, np.y), ImVec2(np.x + 195.5f, np.y + nh),
                  ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // ---- content column (.main: chead + body) ----
    ImGui::BeginChild("##set_main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    const char* title = "General";
    const char* desc = "Startup & interface";
    switch (st_.settings_tab) {
      case SettingsTab::General: title = "General"; desc = "Startup & interface"; break;
      case SettingsTab::Playback: title = "Playback"; desc = "Transport, loop & queue"; break;
      case SettingsTab::Subtitles: title = "Subtitles"; desc = "Rendering, timing, download"; break;
      case SettingsTab::Network: title = "Network"; desc = "Local first, explicit endpoints"; break;
      case SettingsTab::About: title = "About"; desc = "soar"; break;
    }
    // chead (settings.html .chead: 14/24/12 padding + bottom hairline):
    // title + muted desc, the Reset defaults button (--bg3 face) and the
    // accent Done button right-aligned.
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 mp = ImGui::GetWindowPos();
      const float mw = ImGui::GetWindowWidth();
      const float chead_h = 44.0f;
      const float btn_h = 28.0f;
      ImGui::SetCursorPos(ImVec2(24.0f, 14.0f));
      ImGui::TextUnformatted(title);
      ImGui::SameLine(0.0f, 10.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
      ImGui::TextUnformatted(desc);
      ImGui::PopStyleColor();
      const float done_w = ImGui::CalcTextSize("Done").x + 24.0f;
      const float reset_w = ImGui::CalcTextSize("Reset defaults").x + 24.0f;
      const ImVec2 done_p(mp.x + mw - 24.0f - done_w, mp.y + 14.0f);
      ImGui::SetCursorScreenPos(done_p);
      ImGui::InvisibleButton("##set_done", ImVec2(done_w, btn_h));
      if (ImGui::IsItemClicked()) st_.overlay = Overlay::None;
      dl->AddRectFilled(done_p, ImVec2(done_p.x + done_w, done_p.y + btn_h),
                        ImGui::ColorConvertFloat4ToU32(
                            ImGui::IsItemHovered() ? theme::kAccentHover
                                                   : theme::kAccent),
                        kControlRounding);
      const ImVec2 dts = ImGui::CalcTextSize("Done");
      dl->AddText(ImVec2(done_p.x + (done_w - dts.x) * 0.5f,
                         done_p.y + (btn_h - dts.y) * 0.5f),
                  ImGui::ColorConvertFloat4ToU32(theme::kInkOnAccent), "Done");
      const ImVec2 rst_p(done_p.x - 10.0f - reset_w, mp.y + 14.0f);
      ImGui::SetCursorScreenPos(rst_p);
      ImGui::InvisibleButton("##set_reset", ImVec2(reset_w, btn_h));
      if (ImGui::IsItemClicked()) resetSettingsDefaults();
      const bool rst_hov = ImGui::IsItemHovered();
      dl->AddRectFilled(rst_p, ImVec2(rst_p.x + reset_w, rst_p.y + btn_h),
                        ImGui::ColorConvertFloat4ToU32(rst_hov ? theme::kFieldBgHi
                                                               : theme::kFieldBg),
                        kControlRounding);
      dl->AddRect(rst_p, ImVec2(rst_p.x + reset_w, rst_p.y + btn_h),
                  ImGui::ColorConvertFloat4ToU32(theme::kCardLine),
                  kControlRounding);
      const ImVec2 rts = ImGui::CalcTextSize("Reset defaults");
      dl->AddText(ImVec2(rst_p.x + (reset_w - rts.x) * 0.5f,
                         rst_p.y + (btn_h - rts.y) * 0.5f),
                  ImGui::ColorConvertFloat4ToU32(rst_hov ? theme::kText
                                                         : theme::kText2),
                  "Reset defaults");
      // the chead's bottom hairline spans the whole column
      dl->AddLine(ImVec2(mp.x, mp.y + chead_h),
                  ImVec2(mp.x + mw, mp.y + chead_h),
                  ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
    }
    // body (.body: 18/24/24 padding) — the per-tab scroll region
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetCursorPos(ImVec2(24.0f, 62.0f));
    ImGui::BeginChild("##set_scroll", ImVec2(-24.0f, -24.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    switch (st_.settings_tab) {
      case SettingsTab::General: {
        // sect h2 启动与界面 — the four prototype rows (theme stays on
        // the y shortcut; the prototype page has no theme row either).
        settingsSect("STARTUP & INTERFACE");
        settingCard(
            "##g_remember", "Remember last media",
            "Restore the last file and position on startup", 34.0f, 20.0f,
            [&] { settingsSwitch("##g_remember_sw", &st_.remember_last_media); });
        settingCard(
            "##g_esc", "ESC exits fullscreen",
            "Only in fullscreen; in-window ESC closes settings", 34.0f, 20.0f,
            [&] { settingsSwitch("##g_esc_sw", &st_.esc_exits_fullscreen); });
        // 界面语言 — ASCII-only build: the shipped label set is English
        // (the bitmap font has no CJK), so the select has one honest row.
        settingCard(
            "##g_lang", "Interface language",
            "Follows the system; kept across restarts", 120.0f, 26.0f, [&] {
              settingsSelectBox("##g_lang_sel", "English", 120.0f, true);
            });
        settingCard("##g_recent", "Recent playback cap",
                    "MRU list size (O key opens the list)", 96.0f, 24.0f, [&] {
                      int cap = st_.recent_cap;
                      if (settingsStepper("##g_recent_step", &cap, 1, 5, 50, "%d")) {
                        st_.recent_cap = cap;
                        recent_.setMaxEntries(static_cast<std::size_t>(cap));
                        recent_.save();
                        saveRecentCap(cap);
                      }
                    });
        break;
      }
      case SettingsTab::Playback: {
        // Real transport rows: the rate select cycles the same kRates
        // table the ,/. shortcuts do; loop mode walks off/all/one and
        // shuffle flips the queue's round order — all live stores.
        settingsSect("TRANSPORT & QUEUE");
        settingCard("##p_rate", "Playback rate",
                    "Cycles the transport speed (, and . keys)", 120.0f, 26.0f,
                    [&] {
                      static const double kRates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
                      const std::size_t count = sizeof(kRates) / sizeof(kRates[0]);
                      std::size_t idx = 2;
                      for (std::size_t i = 0; i < count; ++i) {
                        if (std::abs(kRates[i] - st_.last_rate) < 1e-9) idx = i;
                      }
                      char cur[16];
                      std::snprintf(cur, sizeof(cur), "%.2gx", st_.last_rate);
                      if (settingsSelectBox("##p_rate_sel", cur, 120.0f, false)) {
                        const double next = kRates[(idx + 1) % count];
                        st_.last_rate = next;
                        player_.setRate(next);
                        std::snprintf(cur, sizeof(cur), "Rate %.2gx", next);
                        st_.toast.show(cur, nowMs());
                      }
                    });
        settingCard("##p_loop", "Loop mode",
                    "Off, all (whole queue) or one (this file)", 120.0f, 26.0f,
                    [&] {
                      const char* names[] = {"Off", "All", "One"};
                      int idx = static_cast<int>(playlist_.loop());
                      if (settingsSelectBox("##p_loop_sel", names[idx], 120.0f,
                                            false)) {
                        const auto next =
                            static_cast<PlaylistStore::Loop>((idx + 1) % 3);
                        playlist_.setLoop(next);
                        st_.toast.show(next == PlaylistStore::Loop::Off ? "Loop off"
                                       : next == PlaylistStore::Loop::All
                                           ? "Loop all"
                                           : "Loop one",
                                       nowMs());
                      }
                    });
        settingCard("##p_shuffle", "Shuffle queue",
                    "Random round order (P overlay honours it)", 34.0f, 20.0f,
                    [&] {
                      bool sh = playlist_.shuffle();
                      if (settingsSwitch("##p_shuffle_sw", &sh)) {
                        playlist_.setShuffle(sh);
                      }
                    });
        break;
      }
      case SettingsTab::Subtitles: {
        // The prototype's primary frame: a 2-column grid (渲染 / 时长对齐
        // + 获取) with the endpoints section full-width below.
        ImGui::BeginChild("##s_grid", ImVec2(-1.0f, 316.0f),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        const float col_w = (ImGui::GetContentRegionAvail().x - 14.0f) * 0.5f;
        ImGui::BeginChild("##s_cl", ImVec2(col_w, 0.0f), ImGuiChildFlags_None);
        // sect h2 渲染
        settingsSect("RENDERING");
        settingCard(
            "##s_show", "Show subtitles",
            "Render at the bottom edge", 34.0f,
            20.0f, [&] {
              if (settingsSwitch("##s_show_sw", &st_.subtitle_visible)) {
                if (cfg_.subtitle_visible) {
                  cfg_.subtitle_visible->store(st_.subtitle_visible,
                                               std::memory_order_relaxed);
                }
              }
            });
        settingCard(
            "##s_autoshow", "Auto-show first subtitle",
            "Shows the first selected track", 34.0f, 20.0f, [&] {
              settingsSwitch("##s_autoshow_sw", &st_.subtitle_autoshow);
            });
        settingCard(
            "##s_font", "Subtitle font",
            "System font fallback", 120.0f, 26.0f, [&] {
              settingsSelectBox("##s_font_sel", "Noto Sans SC", 120.0f, true);
            });
        settingCard("##s_size", "Font size",
                    "Scales with window height", 96.0f, 24.0f,
                    [&] {
                      int px = static_cast<int>(st_.subtitle_font_size);
                      if (settingsStepper("##s_size_step", &px, 1, 12, 48, "%d px")) {
                        st_.subtitle_font_size = static_cast<float>(px);
                        if (cfg_.subtitle_font_size) {
                          cfg_.subtitle_font_size->store(
                              static_cast<float>(px), std::memory_order_relaxed);
                        }
                      }
                    });
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::BeginChild("##s_cr", ImVec2(col_w, 0.0f), ImGuiChildFlags_None);
        // sect h2 时长对齐
        settingsSect("TIMING");
        settingCard("##s_offset", "Global sync offset",
                    "+ leads / - lags", 96.0f, 24.0f, [&] {
                      int ms = st_.subtitle_offset_ms;
                      if (settingsStepper("##s_offset_step", &ms, 100, -5000, 5000,
                                          "%d ms")) {
                        st_.subtitle_offset_ms = ms;
                        if (cfg_.subtitle_offset_ms) {
                          cfg_.subtitle_offset_ms->store(ms,
                                                         std::memory_order_relaxed);
                        }
                      }
                    });
        settingCard(
            "##s_style", "Render style",
            "Plain text first", 120.0f, 26.0f,
            [&] { settingsSelectBox("##s_style_sel", "Plain text", 120.0f, true); });
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        // sect h2 获取
        settingsSect("FETCH");
        settingCard("##s_dir", "Subtitle directory",
                    "Scans .srt/.ass/.vtt", 150.0f, 26.0f, [&] {
                      settingsTextInput("~/.local/share/soar/subtitles", 150.0f);
                    });
        settingCard(
            "##s_auto", "Auto download",
            "Matched by hash + name", 34.0f, 20.0f,
            [&] {
              if (settingsSwitch("##s_auto_sw", &st_.subtitle_autodl)) {
                configureHttpSubtitles(st_.subtitle_autodl);
              }
            });
        ImGui::EndChild();
        ImGui::EndChild();
        // sect h2 端点 — full width below the grid
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        settingsSect("ENDPOINTS");
        endpointRow("##ep_download", "Download",
                    "Not configured - no automatic subtitle fetch", nowMs());
        endpointRow("##ep_translate", "Translate",
                    "Not configured - no translated track", nowMs());
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kText4);
        ImGui::TextUnformatted(
            "Translate endpoint is optional: offline beats overreach; keys stay local");
        ImGui::PopStyleColor();
        break;
      }
      case SettingsTab::Network: {
        // sect h2 本地优先（默认态，无任何外部依赖）
        settingsSect("LOCAL FIRST (THE DEFAULT HAS NO DEPENDENCIES)");
        settingCard("##n_trackers", "P2P / DHT trackers",
                    "Empty = local files and direct URLs only", 150.0f, 24.0f,
                    [&] {
                      settingsPill("0 trackers", theme::kNet);
                      ImGui::SameLine(0.0f, 8.0f);
                      if (dangerButton("Add##n_add", ImVec2(44.0f, 24.0f))) {
                        st_.toast.show("No tracker list yet", nowMs());
                      }
                    });
        settingCard("##n_port", "Inbound port",
                    "Default 0 = random high port", 96.0f, 24.0f, [&] {
                      int port = 0;
                      ImGui::BeginDisabled(true);
                      settingsStepper("##n_port_step", &port, 1, 0, 65535, "%d");
                      ImGui::EndDisabled();
                    });
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        settingsSect("ENDPOINTS (NOTHING CALLS OUT BY DEFAULT)");
        endpointRow("##n_ep_sub", "Subtitles", "Not configured - no subtitle fetch",
                    nowMs());
        endpointRow("##n_ep_translate", "Translate", "Not configured - no translated track",
                    nowMs());
        endpointRow("##n_ep_stats", "Stats", "Not configured - no telemetry, all local",
                    nowMs());
        // The green zero-external summary row (border/face tinted with
        // ok, the title in the ok ink, an ok pill on the right).
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kPanelRounding);
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImGui::ColorConvertFloat4ToU32(ImVec4(
                theme::kOk.x, theme::kOk.y, theme::kOk.z, 0.06f)));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImGui::ColorConvertFloat4ToU32(ImVec4(
                theme::kOk.x, theme::kOk.y, theme::kOk.z, 0.35f)));
        if (ImGui::BeginChild("##n_zero", ImVec2(-1.0f, 52.0f),
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse)) {
          ImGui::SetCursorPos(ImVec2(12.0f, 8.0f));
          ImGui::PushStyleColor(ImGuiCol_Text, theme::kOk);
          ImGui::TextUnformatted("* Currently zero-external");
          ImGui::PopStyleColor();
          ImGui::SetCursorPos(ImVec2(12.0f, 28.0f));
          ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
          ImGui::TextUnformatted(
              "The only network is the URL you opened; no background requests");
          ImGui::PopStyleColor();
          ImGui::SetCursorPos(
              ImVec2(ImGui::GetContentRegionAvail().x - 10.0f -
                         ImGui::CalcTextSize("Offline capable").x - 16.0f,
                     (52.0f - ImGui::CalcTextSize("Offline capable").y - 6.0f) *
                         0.5f));
          settingsPill("Offline capable", theme::kOk);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        break;
      }
      case SettingsTab::About: {
        // The prototype's .about block: centered mark, name, version,
        // description, then the version rows. All positions derive from
        // the column's own origin so the block stays centred on any
        // window size.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cw = ImGui::GetContentRegionAvail().x;
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const float cx = sp.x + cw * 0.5f;
        const float top = sp.y + 20.0f;
        // .about .logo: 56px rounded gradient mark with the "S" initial.
        drawGradientMark(dl, ImVec2(cx - 28.0f, top), 56.0f, 16.0f,
                         ImGui::ColorConvertFloat4ToU32(theme::kPageBg));
        const ImVec2 ns = ImGui::CalcTextSize("soar");
        dl->AddText(ImVec2(cx - ns.x * 0.5f, top + 66.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kText), "soar");
        const std::string ver = std::string("v") + SOAR_APP_VERSION;
        const ImVec2 vs = ImGui::CalcTextSize(ver.c_str());
        dl->AddText(ImVec2(cx - vs.x * 0.5f, top + 90.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kText3), ver.c_str());
        const char* d1 = "Universal media player on FFmpeg + SDL2.";
        const char* d2 = "Local first; nothing dials out unconfigured.";
        const ImVec2 s1 = ImGui::CalcTextSize(d1);
        const ImVec2 s2 = ImGui::CalcTextSize(d2);
        dl->AddText(ImVec2(cx - s1.x * 0.5f, top + 116.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kText3), d1);
        dl->AddText(ImVec2(cx - s2.x * 0.5f, top + 136.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kText3), d2);
        ImGui::SetCursorScreenPos(ImVec2(sp.x, top + 170.0f));
        versionRow("##about_version", "Version", ver.c_str());
        versionRow("##about_backend", "Backend", "FFmpeg + SDL2 (libass / libtorrent)");
        versionRow("##about_ui", "Interface", "Dear ImGui + SDL2 renderer");
        break;
      }
    }
    ImGui::EndChild();  // ##set_scroll
    ImGui::PopStyleVar();
    ImGui::EndChild();  // ##set_main
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
  if (!open) st_.overlay = Overlay::None;
}

}  // namespace soar::app

#endif  // SOAR_WITH_IMGUI
