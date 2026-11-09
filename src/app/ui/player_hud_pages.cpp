// PlayerHud playlist + library pages.
// Split out of player_window.cpp; whole file is ImGui-only.
#ifdef SOAR_WITH_IMGUI

#include "player_hud.h"

namespace soar::app {

void PlayerHud::drawPlaylistOverlay() {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPageBg);
  ImGui::Begin("##pl_page", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoSavedSettings);
  {
    // The arm expires: a user who armed it and walked away must not
    // destroy the queue with their next visit's stray click.
    if (playlist_clear_armed_ &&
        nowMs() - playlist_clear_armed_at_ > std::chrono::seconds(4)) {
      playlist_clear_armed_ = false;
    }

    // Left icon rail (.rail, 74px, --bg0, border-right).
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kNavBg);
    ImGui::BeginChild("##pl_rail", ImVec2(74.0f, 0.0f), ImGuiChildFlags_None);
    {
      // The rail's logo (.rail .wing, 30x30, rounded 9).
      ImGui::SetCursorPos(ImVec2(22.0f, 14.0f));
      drawGradientMark(ImGui::GetWindowDrawList(),
                       ImGui::GetCursorScreenPos(), 30.0f, 9.0f,
                       ImGui::ColorConvertFloat4ToU32(theme::kNavBg));
      ImGui::Dummy(ImVec2(30.0f, 30.0f));
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0f);
      // The four page links; the playlist link is the active one. A click
      // switches the overlay — the prototype's rail is the page navigator.
      if (railButton("##pl_nav_main", RailIcon::Main, false))
        st_.overlay = Overlay::None;
      if (railButton("##pl_nav_playlist", RailIcon::Playlist, true))
        st_.overlay = Overlay::Playlist;
      if (railButton("##pl_nav_library", RailIcon::Library, false))
        st_.overlay = Overlay::Library;
      if (railButton("##pl_nav_settings", RailIcon::Settings, false))
        st_.overlay = Overlay::Settings;
      // .rail border-right: the hairline splitting rail from the queue.
      const ImVec2 rp = ImGui::GetWindowPos();
      const float rh = ImGui::GetWindowHeight();
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(rp.x + 73.5f, rp.y), ImVec2(rp.x + 73.5f, rp.y + rh),
          ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SameLine();

    // Main: the head over the queue over the now-playing bar.
    ImGui::BeginChild("##pl_main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    {
      drawPlaylistHead();
      const std::vector<std::string> entries = playlist_.entries();
      const std::vector<DirGroup> groups = groupByDirectory(entries);
      playlist_rows_ = static_cast<int>(entries.size());
      // The queue child leaves the now-bar's 62px at the bottom.
      const float now_h = 62.0f;
      ImGui::BeginChild("##pl_qlist", ImVec2(0.0f, -now_h),
                        ImGuiChildFlags_None);
      {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        if (playlist_.empty()) {
          ImGui::TextDisabled("%s",
                              "Playlist is empty — opened files queue here.");
        } else if (groups.size() > 1) {
          // Folder tree (v0.2.1): one node per parent directory, first
          // occurrence order. "Play group" starts at the group's first
          // queue entry, which walks the rest in order.
          for (std::size_t g = 0; g < groups.size(); ++g) {
            const DirGroup& group = groups[g];
            ImGui::PushID(static_cast<int>(g));
            const std::string title =
                (group.directory.empty() ? std::string("Other sources")
                                         : baseName(group.directory)) +
                "  (" + std::to_string(group.indices.size()) + ")";
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            const bool opened = ImGui::TreeNode(title.c_str());
            if (ImGui::IsItemHovered() && !group.directory.empty()) {
              ImGui::SetTooltip("%s", group.directory.c_str());
            }
            if (opened) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Play group")) {
                openPlaylistEntry(group.indices[0], nowMs());
              }
              for (const std::size_t idx : group.indices) {
                drawPlaylistRow(entries, idx);
              }
              ImGui::TreePop();
            }
            ImGui::PopID();
          }
        } else {
          // Zero or one folder: keep the flat list (no tree node between
          // the user and the entries). This is the prototype's form.
          for (std::size_t i = 0; i < entries.size(); ++i) {
            drawPlaylistRow(entries, i);
          }
        }
      }
      ImGui::EndChild();
      drawPlaylistNowBar();
    }
    ImGui::EndChild();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

bool PlayerHud::railButton(const char* id, RailIcon icon, bool active) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorPosX(15.0f);  // (74 - 44) / 2
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(44.0f, 44.0f));
  const bool hovered = ImGui::IsItemHovered();
  if (active) {
    ImVec4 a = theme::kAccent;
    a.w = 0.14f;  // --accent-soft
    dl->AddRectFilled(p, ImVec2(p.x + 44.0f, p.y + 44.0f),
                      ImGui::ColorConvertFloat4ToU32(a), 12.0f);
  } else if (hovered) {
    dl->AddRectFilled(p, ImVec2(p.x + 44.0f, p.y + 44.0f),
                      ImGui::ColorConvertFloat4ToU32(theme::kFieldBg),
                      12.0f);
  }
  const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
      active ? theme::kAccentHi
             : (hovered ? theme::kText2 : theme::kText3));
  drawRailIcon(dl, ImVec2(p.x + 22.0f, p.y + 22.0f), icon, fg);
  return clicked;
}

void PlayerHud::drawRailIcon(ImDrawList* dl, const ImVec2& c, RailIcon icon, ImU32 col) {
  const float th = 1.6f;
  switch (icon) {
    case RailIcon::Main: {  // window: rounded rect + title line
      dl->AddRect(ImVec2(c.x - 9.0f, c.y - 7.0f),
                  ImVec2(c.x + 9.0f, c.y + 7.0f), col, 2.0f, 0, th);
      dl->AddLine(ImVec2(c.x - 9.0f, c.y - 3.0f),
                  ImVec2(c.x + 9.0f, c.y - 3.0f), col, th);
      break;
    }
    case RailIcon::Playlist: {  // three lines + play triangle at right
      dl->AddLine(ImVec2(c.x - 9.0f, c.y - 5.0f),
                  ImVec2(c.x + 1.0f, c.y - 5.0f), col, th);
      dl->AddLine(ImVec2(c.x - 9.0f, c.y),
                  ImVec2(c.x + 1.0f, c.y), col, th);
      dl->AddLine(ImVec2(c.x - 9.0f, c.y + 5.0f),
                  ImVec2(c.x + 1.0f, c.y + 5.0f), col, th);
      dl->AddTriangleFilled(ImVec2(c.x + 3.0f, c.y - 3.5f),
                            ImVec2(c.x + 3.0f, c.y + 3.5f),
                            ImVec2(c.x + 9.0f, c.y), col);
      break;
    }
    case RailIcon::Library: {  // hexagon
      const ImVec2 pts[6] = {
          {c.x, c.y - 7.0f},     {c.x + 6.1f, c.y - 3.5f},
          {c.x + 6.1f, c.y + 3.5f}, {c.x, c.y + 7.0f},
          {c.x - 6.1f, c.y + 3.5f}, {c.x - 6.1f, c.y - 3.5f}};
      dl->PathLineTo(pts[0]);
      for (int i = 1; i < 6; ++i) dl->PathLineTo(pts[i]);
      dl->PathStroke(col, 0, th);
      break;
    }
    case RailIcon::Settings: {  // gear: circle + 8 spokes
      dl->AddCircle(c, 3.4f, col, 0, th);
      for (int k = 0; k < 8; ++k) {
        const float a = k * 0.785398f;  // pi/4
        dl->AddLine(ImVec2(c.x + 6.2f * std::cos(a), c.y + 6.2f * std::sin(a)),
                    ImVec2(c.x + 9.0f * std::cos(a), c.y + 9.0f * std::sin(a)),
                    col, th);
      }
      break;
    }
  }
}

void PlayerHud::drawPlaylistHead() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorPos(ImVec2(20.0f, 14.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
  ImGui::TextUnformatted("Playlist");
  ImGui::PopStyleColor();
  const std::string count = std::to_string(playlist_.size()) + " items";
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
  ImGui::TextUnformatted(count.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  drawHeadPill("Auto-advance, queue end stops at Ended");
  // Right cluster: Loop segmented + Shuffle + Add files + Clear. The
  // prototype's .head has them clustered right; the offset reserves the
  // measured cluster width so the Add/Clear land at the 20px edge.
  ImGui::SameLine(ImGui::GetWindowWidth() - 20.0f - 335.0f);
  loopSegmented();
  ImGui::SameLine();
  if (shuffleButton()) {
    playlist_.setShuffle(!playlist_.shuffle());
  }
  ImGui::SameLine();
  if (drawHeadBtn("##pl_add", "Add files O")) {
    openDirPickerFiles();
  }
  ImGui::SameLine();
  if (drawHeadBtn("##pl_clear",
                  playlist_clear_armed_ ? "Clear: sure?" : "Clear")) {
    if (playlist_clear_armed_) {
      playlist_.clear();
      playlist_clear_armed_ = false;
      st_.toast.show("Playlist cleared", nowMs());
    } else {
      playlist_clear_armed_ = true;
      playlist_clear_armed_at_ = nowMs();
    }
  }
  // The head's bottom hairline (14 + 30 + 10 = 54px strip).
  const ImVec2 hp = ImGui::GetWindowPos();
  dl->AddLine(ImVec2(hp.x, hp.y + 54.0f),
              ImVec2(hp.x + ImGui::GetWindowWidth(), hp.y + 54.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
}

void PlayerHud::drawHeadPill(const char* text) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22.0f;
  const float w = ts.x + 18.0f;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                    ImGui::ColorConvertFloat4ToU32(theme::kFieldBg), 99.0f);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h),
              ImGui::ColorConvertFloat4ToU32(theme::kCardLine), 99.0f, 0,
              1.0f);
  dl->AddText(ImVec2(p.x + 9.0f, p.y + (h - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(theme::kText2), text);
  ImGui::Dummy(ImVec2(w, h));
}

bool PlayerHud::drawHeadBtn(const char* id, const char* label) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(label);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 30.0f;
  const float w = ts.x + 24.0f;
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                    ImGui::ColorConvertFloat4ToU32(
                        hovered ? theme::kFieldBgHi : theme::kFieldBg),
                    7.0f);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h),
              ImGui::ColorConvertFloat4ToU32(theme::kCardLine), 7.0f, 0,
              1.0f);
  dl->AddText(ImVec2(p.x + 12.0f, p.y + (h - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(theme::kText), label);
  return clicked;
}

void PlayerHud::loopSegmented() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  static const char* const kLabels[3] = {"Off", "All", "One"};
  const PlaylistStore::Loop kModes[3] = {PlaylistStore::Loop::Off,
                                         PlaylistStore::Loop::All,
                                         PlaylistStore::Loop::One};
  const float seg_h = 22.0f;
  float widths[3] = {0, 0, 0};
  float total = 0.0f;
  for (int i = 0; i < 3; ++i) {
    widths[i] = ImGui::CalcTextSize(kLabels[i]).x + 20.0f;
    total += widths[i];
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 cpos(p.x - 2.0f, p.y - 2.0f);
  dl->AddRectFilled(cpos, ImVec2(cpos.x + total + 8.0f, cpos.y + seg_h + 4.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kFieldBg), 99.0f);
  dl->AddRect(cpos, ImVec2(cpos.x + total + 8.0f, cpos.y + seg_h + 4.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kCardLine), 99.0f, 0,
              1.0f);
  float x = p.x;
  for (int i = 0; i < 3; ++i) {
    const bool on = playlist_.loop() == kModes[i];
    const ImVec2 bp(x, p.y);
    ImGui::SetCursorScreenPos(bp);
    const bool clicked =
        ImGui::InvisibleButton(("##pl_loop" + std::to_string(i)).c_str(),
                               ImVec2(widths[i], seg_h));
    const bool hovered = ImGui::IsItemHovered();
    if (on) {
      dl->AddRectFilled(bp, ImVec2(bp.x + widths[i], bp.y + seg_h),
                        ImGui::ColorConvertFloat4ToU32(theme::kAccent),
                        99.0f);
    }
    const ImVec2 lts = ImGui::CalcTextSize(kLabels[i]);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(
        on ? theme::kInkOnAccent
           : (hovered ? theme::kText2 : theme::kText3));
    dl->AddText(ImVec2(bp.x + (widths[i] - lts.x) * 0.5f,
                       bp.y + (seg_h - lts.y) * 0.5f),
                ink, kLabels[i]);
    if (clicked) playlist_.setLoop(kModes[i]);
    x += widths[i];
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x + total, p.y + seg_h));
}

bool PlayerHud::shuffleButton() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##pl_shuffle",
                                              ImVec2(26.0f, 26.0f));
  const bool hovered = ImGui::IsItemHovered();
  const bool on = playlist_.shuffle();
  const ImU32 ink = ImGui::ColorConvertFloat4ToU32(
      on ? theme::kAccentHi : (hovered ? theme::kText2 : theme::kText3));
  const float th = 1.6f;
  // Two crossing paths + two arrowheads (the prototype's shuffle glyph).
  dl->AddLine(ImVec2(p.x + 3.0f, p.y + 6.5f), ImVec2(p.x + 8.0f, p.y + 6.5f),
              ink, th);
  dl->AddLine(ImVec2(p.x + 3.0f, p.y + 19.5f), ImVec2(p.x + 8.0f, p.y + 19.5f),
              ink, th);
  dl->AddLine(ImVec2(p.x + 12.0f, p.y + 9.5f), ImVec2(p.x + 17.0f, p.y + 14.0f),
              ink, th);
  dl->AddLine(ImVec2(p.x + 12.0f, p.y + 16.5f), ImVec2(p.x + 17.0f, p.y + 12.0f),
              ink, th);
  dl->AddTriangleFilled(ImVec2(p.x + 6.0f, p.y + 4.5f),
                        ImVec2(p.x + 6.0f, p.y + 8.5f),
                        ImVec2(p.x + 2.5f, p.y + 6.5f), ink);
  dl->AddTriangleFilled(ImVec2(p.x + 6.0f, p.y + 17.5f),
                        ImVec2(p.x + 6.0f, p.y + 21.5f),
                        ImVec2(p.x + 2.5f, p.y + 19.5f), ink);
  dl->AddTriangleFilled(ImVec2(p.x + 19.0f, p.y + 9.0f),
                        ImVec2(p.x + 19.0f, p.y + 17.0f),
                        ImVec2(p.x + 23.0f, p.y + 13.0f), ink);
  return clicked;
}

void PlayerHud::drawPlaylistRow(const std::vector<std::string>& entries, std::size_t i) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool now = i == playlist_.current();
  const std::string name = displayName(entries[i]);
  const float row_h = 52.0f;
  const float row_w = ImGui::GetWindowWidth() - 24.0f;
  ImGui::SetCursorPosX(12.0f);
  const float row_top = ImGui::GetCursorPosY();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // The rightmost 70px is the action zone (next-play + remove, revealed
  // on hover): it is not part of the jump button, so a click aimed at an
  // action can never fall through to the row's jump (hover-claim order
  // would give the row the press — measured on 1.91.9b, the later
  // overlapping button loses the click even with AllowOverlap).
  const float jump_w = row_w - 70.0f;
  const bool row_clicked =
      ImGui::InvisibleButton(("##plrow_" + std::to_string(i)).c_str(),
                             ImVec2(jump_w, row_h));
  const bool hovered =
      ImGui::IsMouseHoveringRect(p, ImVec2(p.x + row_w, p.y + row_h));
  if (now) {
    dl->AddRectFilled(p, ImVec2(p.x + row_w, p.y + row_h),
                      ImGui::ColorConvertFloat4ToU32(theme::kFieldBg),
                      10.0f);
    ImVec4 b = theme::kAccent;
    b.w = 0.28f;
    dl->AddRect(p, ImVec2(p.x + row_w, p.y + row_h),
                ImGui::ColorConvertFloat4ToU32(b), 10.0f, 0, 1.0f);
  } else if (hovered) {
    dl->AddRectFilled(p, ImVec2(p.x + row_w, p.y + row_h),
                      ImGui::ColorConvertFloat4ToU32(theme::kFieldBg),
                      10.0f);
  }
  if (row_clicked) openPlaylistEntry(i, nowMs());
  // Index (or EQ bars on the current entry).
  if (now) {
    const float cx = p.x + 22.0f;
    const float cy = p.y + row_h * 0.5f;
    const float t = static_cast<float>(ImGui::GetTime());
    const float hh[3] = {6.0f, 12.0f, 8.0f};
    for (int k = 0; k < 3; ++k) {
      const float ph =
          hh[k] * (0.6f + 0.4f * std::sin(t * 5.0f + k * 1.3f));
      dl->AddRectFilled(
          ImVec2(cx - 6.0f + k * 4.5f, cy - ph * 0.5f),
          ImVec2(cx - 6.0f + k * 4.5f + 2.5f, cy + ph * 0.5f),
          ImGui::ColorConvertFloat4ToU32(theme::kAccent), 1.0f);
    }
  } else {
    char idx[4];
    std::snprintf(idx, sizeof(idx), "%02zu", i + 1);
    const ImVec2 is = ImGui::CalcTextSize(idx);
    dl->AddText(ImVec2(p.x + 22.0f - is.x, p.y + (row_h - is.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(theme::kText4), idx);
  }
  // Thumb (72x40, rounded 6, the poster's dark gradient + cool radial).
  const float ty = p.y + (row_h - 40.0f) * 0.5f;
  const ImVec2 tp(p.x + 40.0f, ty);
  const float th = 40.0f;
  dl->AddRectFilled(tp, ImVec2(tp.x + 72.0f, tp.y + th),
                    ImGui::ColorConvertFloat4ToU32(theme::kFieldBg), 6.0f);
  dl->AddRectFilledMultiColor(tp, ImVec2(tp.x + 72.0f, tp.y + th),
                              IM_COL32(13, 15, 18, 0xFF),   // TL
                              IM_COL32(19, 24, 33, 0xFF),   // TR
                              IM_COL32(14, 16, 19, 0xFF),   // BL
                              IM_COL32(19, 22, 27, 0xFF));  // BR
  dl->PushClipRect(tp, ImVec2(tp.x + 72.0f, tp.y + th), true);
  dl->AddEllipseFilled(ImVec2(tp.x + 50.0f, tp.y + 24.0f),
                       ImVec2(20.0f, 18.0f),
                       IM_COL32(108, 177, 255, 16), 0.0f, 32);
  dl->PopClipRect();
  // Title + meta (the format/extension; the queue store tracks no
  // per-entry duration or resolution).
  const float bx = tp.x + 72.0f + 12.0f;
  const float bw = row_w - (bx - p.x) - 70.0f;  // room for the actions
  const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
  dl->AddText(ImVec2(bx, ty + 4.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kText),
              ellipsize(name, bw).c_str());
  const std::string meta = rowMeta(entries[i]);
  dl->AddText(ImVec2(bx, ty + 4.0f + ts.y + 1.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kText3), meta.c_str());
  // Hover actions: next-play + remove (prototype's .row .ra). Only the
  // hovered row reveals them; the action zone is cut out of the jump
  // button above, so the buttons own their clicks outright.
  if (hovered) {
    const float ax = p.x + row_w - 62.0f;
    if (rowActionButton(dl, "##pl_next", ImVec2(ax, p.y + row_h * 0.5f - 13.0f),
                        true)) {
      const std::size_t ni = playlist_.next();
      if (ni != PlaylistStore::kNone) openPlaylistEntry(ni, nowMs());
    }
    if (rowActionButton(dl, "##pl_rm", ImVec2(ax + 30.0f, p.y + row_h * 0.5f - 13.0f),
                        false)) {
      if (playlist_.remove(i)) {
        st_.toast.show("Removed " + name, nowMs());
      }
    }
  }
  // The hover-only action buttons sit past the flow cursor's row walk;
  // pin the next row's start so the pitch stays row_h + spacing whether
  // or not this row revealed its actions.
  ImGui::SetCursorPosY(row_top + row_h + ImGui::GetStyle().ItemSpacing.y);
}

bool PlayerHud::rowActionButton(ImDrawList* dl, const char* id, const ImVec2& p,
                     bool next) {
  // The hit rect must land on the drawn face: InvisibleButton submits at
  // the flow cursor, so pin it to p explicitly (the faces draw at
  // absolute screen coordinates and would otherwise decorate a rect the
  // clicks never reach).
  ImGui::SetCursorScreenPos(p);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(26.0f, 26.0f));
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    dl->AddRectFilled(p, ImVec2(p.x + 26.0f, p.y + 26.0f),
                      ImGui::ColorConvertFloat4ToU32(theme::kFieldBgHi),
                      7.0f);
  }
  const ImU32 ink = ImGui::ColorConvertFloat4ToU32(
      hovered ? theme::kText : theme::kText3);
  const float th = 1.6f;
  const ImVec2 c(p.x + 13.0f, p.y + 13.0f);
  if (next) {
    // Next-play: a bar + play triangle.
    dl->AddLine(ImVec2(c.x + 5.0f, c.y - 6.0f), ImVec2(c.x + 5.0f, c.y + 6.0f),
                ink, th);
    dl->AddTriangleFilled(ImVec2(c.x - 4.0f, c.y - 5.0f),
                          ImVec2(c.x - 4.0f, c.y + 5.0f),
                          ImVec2(c.x + 3.0f, c.y), ink);
  } else {
    // Remove: an x.
    dl->AddLine(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f),
                ink, th);
    dl->AddLine(ImVec2(c.x + 5.0f, c.y - 5.0f), ImVec2(c.x - 5.0f, c.y + 5.0f),
                ink, th);
  }
  return clicked;
}

void PlayerHud::drawPlaylistNowBar() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wpos = ImGui::GetWindowPos();
  const float ww = ImGui::GetWindowWidth();
  const float h = 62.0f;
  const float top = ImGui::GetWindowHeight() - h;
  const ImVec2 bar(wpos.x, wpos.y + top);
  dl->AddRectFilled(bar, ImVec2(bar.x + ww, bar.y + h),
                    ImGui::ColorConvertFloat4ToU32(theme::kCardBg));
  dl->AddLine(bar, ImVec2(bar.x + ww, bar.y),
              ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
  // Thumb (64x36).
  const ImVec2 tp(bar.x + 18.0f, bar.y + (h - 36.0f) * 0.5f);
  dl->AddRectFilledMultiColor(tp, ImVec2(tp.x + 64.0f, tp.y + 36.0f),
                              IM_COL32(13, 15, 18, 0xFF),   // TL
                              IM_COL32(19, 24, 33, 0xFF),   // TR
                              IM_COL32(14, 16, 19, 0xFF),   // BL
                              IM_COL32(19, 22, 27, 0xFF));  // BR
  dl->PushClipRect(tp, ImVec2(tp.x + 64.0f, tp.y + 36.0f), true);
  dl->AddEllipseFilled(ImVec2(tp.x + 45.0f, tp.y + 22.0f),
                       ImVec2(18.0f, 16.0f), IM_COL32(108, 177, 255, 16),
                       0.0f, 32);
  dl->PopClipRect();
  // Title + meta.
  const std::vector<std::string> entries = playlist_.entries();
  const std::size_t cur = playlist_.current();
  const std::string title =
      cur == PlaylistStore::kNone ? "Nothing playing"
                                  : displayName(entries[cur]);
  const std::string meta = cur == PlaylistStore::kNone
                               ? std::string()
                               : rowMeta(entries[cur]);
  const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
  dl->AddText(ImVec2(tp.x + 64.0f + 14.0f, bar.y + 12.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kText),
              ellipsize(title, 260.0f).c_str());
  if (!meta.empty()) {
    dl->AddText(ImVec2(tp.x + 64.0f + 14.0f, bar.y + 12.0f + ts.y + 1.0f),
                ImGui::ColorConvertFloat4ToU32(theme::kText3), meta.c_str());
  }
  // Mini progress rail (62% prototype fill; live pos/dur).
  const MediaInfo info = player_.mediaInfo();
  const double dur = static_cast<double>(info.duration.count());
  const double pos = static_cast<double>(player_.position().count());
  const float frac = dur > 0.0 ? static_cast<float>(pos / dur) : 0.0f;
  const float rail_w = 200.0f;
  const ImVec2 rp(tp.x + 64.0f + 14.0f + 280.0f, bar.y + h * 0.5f - 2.0f);
  dl->AddRectFilled(rp, ImVec2(rp.x + rail_w, rp.y + 4.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kFieldBgHi),
                    99.0f);
  dl->AddRectFilled(rp, ImVec2(rp.x + rail_w * frac, rp.y + 4.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::kAccent), 99.0f);
  // Timecode.
  const std::string tc = formatClock(player_.position()) + " / " +
                         formatClock(info.duration);
  dl->AddText(ImVec2(rp.x + rail_w + 14.0f, bar.y + (h - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(theme::kText2), tc.c_str());
  // Volume (a thin rail like the prototype's range input, draggable) +
  // play/pause.
  const float vol_x = rp.x + rail_w + 14.0f + 120.0f;
  const float vw = 90.0f;
  ImGui::SetCursorScreenPos(ImVec2(vol_x, bar.y + (h - 20.0f) * 0.5f));
  if (ImGui::InvisibleButton("##pl_vol", ImVec2(vw, 20.0f)) ||
      ImGui::IsItemActive()) {
    const float vfrac = std::clamp(
        (ImGui::GetIO().MousePos.x - vol_x) / vw, 0.0f, 1.0f);
    st_.last_volume = vfrac;
    player_.setVolume(vfrac);
  }
  dl->AddRectFilled(ImVec2(vol_x, bar.y + h * 0.5f - 1.5f),
                    ImVec2(vol_x + vw, bar.y + h * 0.5f + 1.5f),
                    ImGui::ColorConvertFloat4ToU32(theme::kFieldBgHi),
                    99.0f);
  dl->AddRectFilled(
      ImVec2(vol_x, bar.y + h * 0.5f - 1.5f),
      ImVec2(vol_x + vw * static_cast<float>(st_.last_volume),
             bar.y + h * 0.5f + 1.5f),
      ImGui::ColorConvertFloat4ToU32(theme::kText2), 99.0f);
  ImGui::SetCursorScreenPos(ImVec2(vol_x + 100.0f, bar.y + (h - 38.0f) * 0.5f));
  const bool playing = player_.state() == PlaybackState::Playing;
  if (ImGui::InvisibleButton("##pl_pp", ImVec2(38.0f, 38.0f))) {
    togglePlayPause();
  }
  const ImVec2 bc(vol_x + 100.0f + 19.0f, bar.y + (h - 38.0f) * 0.5f + 19.0f);
  dl->AddCircleFilled(bc, 19.0f,
                      ImGui::ColorConvertFloat4ToU32(theme::kAccent), 48);
  const ImU32 ink =
      ImGui::ColorConvertFloat4ToU32(theme::kInkOnAccent);
  if (playing) {
    dl->AddRectFilled(ImVec2(bc.x - 5.0f, bc.y - 7.0f),
                      ImVec2(bc.x - 1.5f, bc.y + 7.0f), ink, 1.0f);
    dl->AddRectFilled(ImVec2(bc.x + 1.5f, bc.y - 7.0f),
                      ImVec2(bc.x + 5.0f, bc.y + 7.0f), ink, 1.0f);
  } else {
    dl->AddTriangleFilled(ImVec2(bc.x - 4.0f, bc.y - 6.5f),
                          ImVec2(bc.x - 4.0f, bc.y + 6.5f),
                          ImVec2(bc.x + 7.0f, bc.y), ink);
  }
}

std::string PlayerHud::rowMeta(const std::string& uri) {
  const std::size_t dot = uri.find_last_of('.');
  const std::size_t slash = uri.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return "media";
  std::string ext = uri.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return ext.empty() ? "media" : ext;
}

std::string PlayerHud::ellipsize(const std::string& s, float maxw) {
  if (ImGui::CalcTextSize(s.c_str()).x <= maxw) return s;
  std::string t = s;
  while (!t.empty() &&
         ImGui::CalcTextSize((t + "...").c_str()).x > maxw) {
    t.pop_back();
  }
  return t + "...";
}

void PlayerHud::drawLibIcon(ImDrawList* dl, const ImVec2& c, LibIcon icon, ImU32 col) {
  const float th = 1.5f;
  switch (icon) {
    case LibIcon::All: {  // hexagon
      const ImVec2 pts[6] = {
          {c.x, c.y - 7.0f},     {c.x + 6.1f, c.y - 3.5f},
          {c.x + 6.1f, c.y + 3.5f}, {c.x, c.y + 7.0f},
          {c.x - 6.1f, c.y + 3.5f}, {c.x - 6.1f, c.y - 3.5f}};
      dl->PathLineTo(pts[0]);
      for (int i = 1; i < 6; ++i) dl->PathLineTo(pts[i]);
      dl->PathStroke(col, 0, th);
      break;
    }
    case LibIcon::Recent: {  // clock
      dl->AddCircle(c, 7.0f, col, 0, th);
      dl->AddLine(c, ImVec2(c.x, c.y - 4.0f), col, th);
      dl->AddLine(c, ImVec2(c.x + 2.8f, c.y + 1.4f), col, th);
      break;
    }
    case LibIcon::Resume: {  // circle + play
      dl->AddCircle(c, 7.0f, col, 0, th);
      dl->AddTriangleFilled(ImVec2(c.x - 1.6f, c.y - 3.4f),
                            ImVec2(c.x - 1.6f, c.y + 3.4f),
                            ImVec2(c.x + 3.4f, c.y), col);
      break;
    }
    case LibIcon::Finished: {  // check
      dl->AddLine(ImVec2(c.x - 6.0f, c.y + 0.6f), ImVec2(c.x - 1.4f, c.y + 4.8f),
                  col, th);
      dl->AddLine(ImVec2(c.x - 1.4f, c.y + 4.8f), ImVec2(c.x + 6.4f, c.y - 4.8f),
                  col, th);
      break;
    }
    case LibIcon::Folder: {  // folder outline
      dl->PathLineTo(ImVec2(c.x - 7.0f, c.y - 4.0f));
      dl->PathLineTo(ImVec2(c.x - 2.0f, c.y - 4.0f));
      dl->PathLineTo(ImVec2(c.x, c.y - 1.4f));
      dl->PathLineTo(ImVec2(c.x + 7.0f, c.y - 1.4f));
      dl->PathLineTo(ImVec2(c.x + 7.0f, c.y + 5.0f));
      dl->PathLineTo(ImVec2(c.x - 7.0f, c.y + 5.0f));
      dl->PathLineTo(ImVec2(c.x - 7.0f, c.y - 4.0f));
      dl->PathStroke(col, 0, th);
      break;
    }
    case LibIcon::Globe: {  // globe
      dl->AddCircle(c, 7.0f, col, 0, th);
      dl->AddLine(ImVec2(c.x - 7.0f, c.y), ImVec2(c.x + 7.0f, c.y), col, th);
      dl->AddEllipse(ImVec2(c.x, c.y), ImVec2(3.2f, 7.0f), col, 0, th);
      break;
    }
  }
}

void PlayerHud::drawLibraryOverlay() {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPageBg);
  ImGui::Begin("##lib_page", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoSavedSettings);
  {
    if (library_ == nullptr) {
      ImGui::TextDisabled("%s", "No library in this session.");
      ImGui::End();
      ImGui::PopStyleColor();
      ImGui::PopStyleVar(4);
      return;
    }
    const std::vector<LibraryEntry> entries = library_->entries();
    const std::vector<std::string> folders = library_->folders();

    // Sidebar (.side, 224px, --bg0, border-right).
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kNavBg);
    ImGui::BeginChild("##lib_side", ImVec2(224.0f, 0.0f), ImGuiChildFlags_None);
    {
      ImGui::SetCursorPos(ImVec2(10.0f, 16.0f));
      const ImVec2 libwing = ImGui::GetCursorScreenPos();
      drawGradientMark(ImGui::GetWindowDrawList(),
                       ImVec2(libwing.x + 8.0f, libwing.y), 26.0f, 8.0f,
                       ImGui::ColorConvertFloat4ToU32(theme::kNavBg));
      ImGui::Dummy(ImVec2(26.0f, 26.0f));
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16.0f);
      // .side .grp "媒体库" -> LIBRARY.
      librarySideGrp("LIBRARY");
      // The four state buckets (library.html's first group). The count is
      // the live total over the same snapshot the grid renders.
      static const char* const kCat[] = {"All", "Recent", "Resume", "Finished"};
      for (int i = 0; i < 4; ++i) {
        const LibraryCategory cat = static_cast<LibraryCategory>(i);
        std::size_t n = 0;
        for (const LibraryEntry& e : entries)
          if (matchesCategory(e, cat)) ++n;
        if (librarySideRow(("##libcat_" + std::string(kCat[i])).c_str(),
                           kCat[i], n, st_.library_category == cat,
                           static_cast<LibIcon>(i))) {
          st_.library_category = cat;
        }
      }
      // .side .grp "类型" -> TYPE.
      librarySideGrp("TYPE");
      static const char* const kSrc[] = {"Local", "Network"};
      for (int i = 0; i < 2; ++i) {
        // LibrarySource::All is index 0; the two pickable rows follow it.
        const LibrarySource src = static_cast<LibrarySource>(i + 1);
        std::size_t n = 0;
        for (const LibraryEntry& e : entries)
          if (matchesSource(e, src)) ++n;
        if (librarySideRow(("##libsrc_" + std::string(kSrc[i])).c_str(),
                           kSrc[i], n, st_.library_source == src,
                           i == 0 ? LibIcon::Folder : LibIcon::Globe)) {
          st_.library_source = src;
        }
      }
      // The management group the prototype leaves implicit: watched
      // folders and the scan/scrape actions. Sits below the browse rows so
      // the category pins keep their measured pitch.
      librarySideGrp("FOLDERS");
      ImGui::SetCursorPosX(10.0f);
      if (ImGui::SmallButton("Add folder...")) openDirPickerWatch();
      ImGui::SameLine();
      if (ImGui::SmallButton("Rescan")) {
        library_->scan();
        library_->save();
      }
      // The network half of the scrape (docs/mvp.md §3): only offered
      // with an endpoint configured, so an offline session never shows a
      // button that could only fail.
      if (meta_ready_) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Scrape")) {
          const std::size_t n =
              library_->scrapeEntries(MediaLibrary::kScrapeBudget);
          st_.toast.show(
              n == 1 ? "Scraped 1 entry"
                     : "Scraped " + std::to_string(n) + " entries",
              nowMs());
        }
      }
      if (folders.empty()) {
        ImGui::TextDisabled("%s", "No folders watched yet.");
      } else {
        ImGui::TextDisabled("Watched: %zu", folders.size());
        for (std::size_t i = 0; i < folders.size(); ++i) {
          ImGui::PushID(static_cast<int>(1000 + i));
          ImGui::Selectable(folders[i].c_str(), false,
                            ImGuiSelectableFlags_AllowOverlap);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", folders[i].c_str());
          // The remove affordance pins to the row's right edge: the
          // watched path is the label, so trailing the text would push
          // the button past the sidebar's clip on a long path and
          // strand the unwatch action.
          ImGui::SameLine(ImGui::GetWindowWidth() - 26.0f);
          if (ImGui::SmallButton("x")) {
            library_->removeFolder(folders[i]);
            library_->save();
          }
          ImGui::PopID();
        }
      }
      // .side border-right: the hairline splitting sidebar from content.
      const ImVec2 sp = ImGui::GetWindowPos();
      const float sh = ImGui::GetWindowHeight();
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(sp.x + 223.5f, sp.y), ImVec2(sp.x + 223.5f, sp.y + sh),
          ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SameLine();

    // Main: the head (title/count/search) over the card-grid child.
    ImGui::BeginChild("##lib_main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    {
      drawLibraryHead(entries);
      drawLibraryEntries(entries, !folders.empty());
    }
    ImGui::EndChild();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

void PlayerHud::librarySideGrp(const char* label) {
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0f);
  ImGui::SetCursorPosX(18.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText4);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
}

bool PlayerHud::librarySideRow(const char* id, const char* label, std::size_t count,
                    bool active, LibIcon icon) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
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
  drawLibIcon(dl, ImVec2(p.x + 18.0f, p.y + 16.0f), icon, fg);
  const ImVec2 ls = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(p.x + 36.0f, p.y + (32.0f - ls.y) * 0.5f), fg, label);
  // .cnt: the mono count right-aligned to the row's 10px padding.
  const std::string cs = std::to_string(count);
  const ImVec2 css = ImGui::CalcTextSize(cs.c_str());
  dl->AddText(ImVec2(p.x + w - 10.0f - css.x, p.y + (32.0f - css.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(
                  active ? theme::kAccentHi : theme::kText4),
              cs.c_str());
  return clicked;
}

void PlayerHud::drawLibraryHead(const std::vector<LibraryEntry>& entries) {
  static const char* const kCat[] = {"All", "Recent", "Resume", "Finished"};
  static const char* const kCatDesc[] = {
      "everything", "by time added", "in progress", "completed"};
  const std::size_t shown_count = [&] {
    std::size_t n = 0;
    for (const LibraryEntry& e : entries) {
      if (!matchesCategory(e, st_.library_category)) continue;
      if (!matchesSource(e, st_.library_source)) continue;
      const NfoInfo* nfo = library_->nfoFor(e.path);
      if (!matchesLibraryFilter(e, nfo, st_.library_filter)) continue;
      ++n;
    }
    return n;
  }();
  const int ci = static_cast<int>(st_.library_category);
  const char* cat = kCat[ci < 4 ? ci : 0];
  const char* cdesc = kCatDesc[ci < 4 ? ci : 0];
  // The head is a fixed 44px strip with a bottom hairline, like the
  // settings .chead.
  ImGui::SetCursorPos(ImVec2(22.0f, 14.0f));
  ImGui::TextUnformatted(cat);
  const ImVec2 ts = ImGui::CalcTextSize(cat);
  ImGui::SameLine();
  ImGui::SetCursorPosY(14.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText3);
  const std::string count_cap =
      std::string(cdesc) + " · " + std::to_string(shown_count) + " items";
  ImGui::TextUnformatted(count_cap.c_str());
  ImGui::PopStyleColor();
  // The spacer pushes the search box to the head's right edge.
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 22.0f - 240.0f);
  ImGui::SetCursorPosY(11.0f);
  // The search box (240x30, --line-strong border). The "/" kbd tip sits at
  // its right; the icon at its left. The box is the focused item when the
  // "/" action's one-shot flag is set.
  const float box_h = 30.0f;
  const ImVec2 bp(ImGui::GetCursorScreenPos());
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kFieldBg);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
  if (st_.library_search_focus) {
    st_.library_search_focus = false;
    ImGui::SetKeyboardFocusHere(0);
  }
  if (blur_text_next_) {
    // Tab moved the focus ring onto the rows (V7b): the caret leaves
    // the box so the next key lands on the ring, not in the needle.
    blur_text_next_ = false;
    ImGui::ClearActiveID();
  }
  // The hint clears the magnifier icon (drawn over the frame's left), so
  // the text origin is pushed right past it for this one input.
  const ImVec2 fp = ImGui::GetStyle().FramePadding;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(26.0f, fp.y));
  ImGui::SetNextItemWidth(240.0f);
  ImGui::InputTextWithHint("##library-filter", "Search media...",
                           st_.library_filter, sizeof(st_.library_filter));
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  const ImVec2 ep(bp.x + 240.0f, bp.y);
  ImGui::GetWindowDrawList()->AddText(
      ImVec2(ep.x + 8.0f, bp.y + (box_h - ImGui::CalcTextSize("/").y) * 0.5f),
      ImGui::ColorConvertFloat4ToU32(theme::kText4), "/");
  // The search icon at the box's left (drawn over the input).
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 u_t4 = ImGui::ColorConvertFloat4ToU32(theme::kText4);
  dl->AddCircle(ImVec2(bp.x + 16.0f, bp.y + 15.0f), 5.0f, u_t4, 0, 1.4f);
  dl->AddLine(ImVec2(bp.x + 19.5f, bp.y + 18.5f),
              ImVec2(bp.x + 23.0f, bp.y + 22.0f), u_t4, 1.4f);
  // The bottom hairline of the head.
  dl->AddLine(ImVec2(0.0f, bp.y + box_h + 9.0f),
              ImVec2(ImGui::GetWindowWidth(), bp.y + box_h + 9.0f),
              ImGui::ColorConvertFloat4ToU32(theme::kCardLine));
  ImGui::SetCursorPosY(bp.y + box_h + 10.0f);
}

void PlayerHud::drawLibraryEntries(const std::vector<LibraryEntry>& entries, bool watching) {
  std::vector<const LibraryEntry*> shown;
  shown.reserve(entries.size());
  for (const LibraryEntry& e : entries) {
    if (!matchesCategory(e, st_.library_category)) continue;
    // The sidebar's Type group narrows by source kind; All is the
    // no-op default, so the bucket view alone is exactly as before.
    if (!matchesSource(e, st_.library_source)) continue;
    // The search box narrows within the bucket: the bucket filters by
    // state, the box by text, and the two compose (library.h owns the
    // match rule).
    const NfoInfo* nfo = library_->nfoFor(e.path);
    if (!matchesLibraryFilter(e, nfo, st_.library_filter)) continue;
    shown.push_back(&e);
  }
  if (st_.library_category == LibraryCategory::Recent) {
    std::sort(shown.begin(), shown.end(),
              [](const LibraryEntry* a, const LibraryEntry* b) {
                return compareRecent(*a, *b);
              });
  }
  // V7b: the source rows are the library's focusable face (the category
  // sidebar and folder rows stay mouse-only this batch).
  library_rows_ = static_cast<int>(shown.size());
  // The .grid body: the search box lives in the head (drawLibraryHead),
  // so this child opens straight onto the card grid at the prototype's
  // 16px 22px padding.
  ImGui::BeginChild("library-entries", ImVec2(0, 0));
  ImGui::SetCursorPos(ImVec2(22.0f, 16.0f));
  if (shown.empty()) {
    // The bucket's filter and the store's emptiness are different facts:
    // a watched folder with no media says one thing, a filtered bucket
    // with nothing left says the other.
    if (!entries.empty()) {
      ImGui::TextDisabled("%s", "No entries in this bucket.");
    } else if (watching) {
      ImGui::TextDisabled("%s", "No media files found.");
    }
  }
  // Card grid (docs/public/prototypes/library.html .grid/.card, the v1
  // library page). Auto-fill columns of ~196px cards, a 16px gutter, each
  // card a 16:9 poster over a meta block. The poster is the placeholder
  // thumb (kThumbBg/kThumbGlyph — the content layer stays deep in both
  // themes) with a source-format pill top-left and a centre play badge on
  // hover. The whole card is one hit target (open), with the remove
  // affordance as a small x in the poster's top-right corner.
  const float kGap = 16.0f;
  const float kMinCardW = 196.0f;
  const float pad_x = 12.0f, pad_t = 10.0f, pad_b = 12.0f;
  const float line_h = ImGui::GetTextLineHeight();
  const float meta_h = pad_t + line_h + 3.0f + line_h + 4.0f + line_h + pad_b;
  float avail = ImGui::GetContentRegionAvail().x;
  int cols = static_cast<int>((avail + kGap) / (kMinCardW + kGap));
  if (cols < 1) cols = 1;
  const float card_w = (avail - kGap * static_cast<float>(cols - 1)) /
                       static_cast<float>(cols);
  const float poster_h = card_w * 9.0f / 16.0f;  // 16:9 aspect
  const float card_h = poster_h + meta_h;
  const ImVec2 start = ImGui::GetCursorPos();
  int focus_i = 0;  // V7b: the focus face's index over `shown` (row-major)
  int idx = 0;
  for (const LibraryEntry* ep : shown) {
    const LibraryEntry& e = *ep;
    const int col = idx % cols;
    const int row = idx / cols;
    ImGui::SetCursorPos(ImVec2(start.x + col * (card_w + kGap),
                               start.y + row * (card_h + kGap)));
    ImGui::PushID(e.path.c_str());
    // One hit target spanning the card: click/Enter opens, hover drives the
    // badge + tooltip, and the focus ring wraps the card rect.
    ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    const bool enter =
        ui_focus_.takesActivation(UiFocus::Face::LibraryRows, focus_i);
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 u_line = ImGui::ColorConvertFloat4ToU32(theme::kBorder);
    const ImU32 u_edge = ImGui::ColorConvertFloat4ToU32(theme::kLineStrong);
    const ImU32 u_card = ImGui::ColorConvertFloat4ToU32(theme::kSurface2);
    const ImU32 u_text = ImGui::ColorConvertFloat4ToU32(theme::kText);
    const ImU32 u_text3 = ImGui::ColorConvertFloat4ToU32(theme::kTextMuted);
    const ImU32 u_net = ImGui::ColorConvertFloat4ToU32(theme::kNet);
    const ImU32 u_accent = ImGui::ColorConvertFloat4ToU32(theme::kAccent);
    const ImU32 u_ink = ImGui::ColorConvertFloat4ToU32(theme::kInkOnAccent);
    // Card frame: surface-2 fill (the 1px border is stroked after the
    // poster so the poster doesn't sit on it). (proto.css .card :69-74)
    const float rad = static_cast<float>(theme::kPanelRounding);
    dl->AddRectFilled(rmin, rmax, u_card, rad);
    // Poster: the prototype's 140deg diagonal gradient, #243047 -> #12161f
    // (58%) -> #0c0e13 (library.html .card .poster), plus the two soft
    // radial accents (cool 72%/62%, warm 20%/30% — .poster::after, clipped
    // to the poster by the .poster overflow:hidden). ImGui's 4-corner
    // bilinear fill reproduces the diagonal, with the corner values matched
    // to the prototype PNG below (the HTML's bright-at-right direction is
    // kept). The bilinear fill is square, so the card's r-md top corners are
    // re-rounded: a window-bg mask square + a solid gradient circle restores
    // each corner (the circle's inboard quarters blend into the surrounding
    // gradient at 10px).
    const ImVec2 pmin(rmin.x + 1.0f, rmin.y + 1.0f);
    const ImVec2 pmax(rmax.x - 1.0f, rmin.y + poster_h);
    const float pw = pmax.x - pmin.x;
    const float ph = poster_h;
    // The prototype PNG's poster renders darker than its own HTML
    // gradient (#243047 -> #0c0e13): the card titles in the same PNG
    // are full-brightness (--t1), so the dim is poster-only — a
    // capture artifact. The acceptance target is the PNG, so the base
    // is matched to the PNG's measured corners (TL 13,15,18 / TR
    // 19,24,33 / BR 19,22,27 / BL 14,16,19) with the HTML's gradient
    // direction (bright at the right) and the radial accents kept.
    const ImU32 u_gl0 = IM_COL32(13, 15, 18, 0xFF);  // PNG TL
    const ImU32 u_gl1 = IM_COL32(19, 24, 33, 0xFF);  // PNG TR
    const ImU32 u_gl2 = IM_COL32(19, 22, 27, 0xFF);  // PNG BR
    const ImU32 u_gl3 = IM_COL32(14, 16, 19, 0xFF);  // PNG BL
    dl->AddRectFilledMultiColor(pmin, pmax, u_gl0, u_gl1, u_gl3, u_gl2);
    // The two radial accents, faked as concentric low-alpha circles (the CSS
    // fades each to transparent at ~55-60% of its radius). Clipped to the
    // poster so no glow bleeds into the gutter/meta.
    dl->PushClipRect(pmin, pmax, true);
    // Warm accent: rgba(255,107,53,.10) fading to transparent at 60% of a
    // 70%x60% ellipse at 20% 30% (.poster::after). One soft low-alpha
    // ellipse stands in for the falloff — the prototype's glow is subtle.
    dl->AddEllipseFilled(ImVec2(pmin.x + pw * 0.20f, pmin.y + ph * 0.30f),
                         ImVec2(pw * 0.42f, ph * 0.36f),
                         IM_COL32(255, 107, 53, 9), 0.0f, 48);
    // Cool accent: rgba(108,177,255,.15), 55%x50% at 72% 62%.
    dl->AddEllipseFilled(ImVec2(pmin.x + pw * 0.72f, pmin.y + ph * 0.62f),
                         ImVec2(pw * 0.33f, ph * 0.30f),
                         IM_COL32(108, 177, 255, 13), 0.0f, 48);
    dl->PopClipRect();
    // Re-round the top corners over the square gradient.
    const ImU32 u_page = ImGui::ColorConvertFloat4ToU32(theme::kPageBg);
    dl->AddRectFilled(ImVec2(pmin.x, pmin.y),
                      ImVec2(pmin.x + rad + 2.0f, pmin.y + rad + 2.0f),
                      u_page);
    dl->AddRectFilled(ImVec2(pmax.x - rad - 2.0f, pmin.y),
                      ImVec2(pmax.x, pmin.y + rad + 2.0f), u_page);
    dl->AddCircleFilled(ImVec2(pmin.x + rad, pmin.y + rad), rad, u_gl0, 16);
    dl->AddCircleFilled(ImVec2(pmax.x - rad, pmin.y + rad), rad, u_gl1, 16);
    // The 1px border over the poster (line -> line-strong on hover).
    dl->AddRect(rmin, rmax, hovered ? u_edge : u_line, rad, 0, 1.0f);
    // Source-format pill, top-left (proto.css .card .poster .kind :76 —
    // a .pill: --line border, --bg3 ground, --t2 ink; a network source
    // takes the .pill.sky variant: net-soft ground, --sky ink).
    const std::string fmt = fileFormatLabel(e.path);
    if (!fmt.empty()) {
      const float pill_h = 18.0f;
      const ImVec2 tsize = ImGui::CalcTextSize(fmt.c_str());
      const ImVec2 ppmin(pmin.x + 8.0f, pmin.y + 8.0f);
      const ImVec2 ppmax(ppmin.x + tsize.x + 12.0f, ppmin.y + pill_h);
      const bool net = matchesSource(e, LibrarySource::Network);
      const ImU32 u_bg3 = ImGui::ColorConvertFloat4ToU32(theme::kFieldBg);
      const ImU32 u_t2 = ImGui::ColorConvertFloat4ToU32(theme::kText2);
      const ImU32 u_line = ImGui::ColorConvertFloat4ToU32(theme::kCardLine);
      const ImU32 u_net_rgb = u_net & 0x00FFFFFF;
      const ImU32 p_bg = net ? (u_net_rgb | 0x1F000000) : u_bg3;
      const ImU32 p_edge = net ? (u_net_rgb | 0x66000000) : u_line;
      const ImU32 p_ink = net ? u_net : u_t2;
      dl->AddRectFilled(ppmin, ppmax, p_bg, pill_h * 0.5f);
      dl->AddRect(ppmin, ppmax, p_edge, pill_h * 0.5f, 0, 1.0f);
      dl->AddText(ImVec2(ppmin.x + 6.0f, ppmin.y + (pill_h - tsize.y) * 0.5f),
                  p_ink, fmt.c_str());
    }
    // Remove affordance, top-right of the poster: a small x that reads as
    // part of the poster chrome (click zones are resolved below).
    dl->AddText(ImVec2(pmax.x - 18.0f, pmin.y + 6.0f), u_text3, "x");
    // Centre play badge on hover (proto.css .card .play-badge :86-92).
    if (hovered) {
      const ImVec2 bc((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
      dl->AddCircleFilled(bc, 20.0f, u_accent, 48);
      dl->AddTriangleFilled(ImVec2(bc.x - 4.0f, bc.y - 6.0f),
                            ImVec2(bc.x - 4.0f, bc.y + 6.0f),
                            ImVec2(bc.x + 7.0f, bc.y), u_ink);
    }
    // Meta: title (ellipsis via clip rect), row2 (format + duration),
    // sub line (resume/source) — proto.css .card .meta :77-84.
    const NfoInfo* nfo = library_->nfoFor(e.path);
    const std::string title =
        (nfo != nullptr && !nfo->title.empty()) ? nfo->title
                                                : displayName(e.path);
    const ImVec2 tpos(rmin.x + pad_x, pmax.y + pad_t);
    const ImU32 clip_r = 0;
    (void)clip_r;
    dl->PushClipRect(ImVec2(tpos.x, tpos.y),
                     ImVec2(rmax.x - pad_x, tpos.y + line_h), true);
    dl->AddText(tpos, u_text, title.c_str());
    dl->PopClipRect();
    const float row2_y = tpos.y + line_h + 3.0f;
    dl->AddText(ImVec2(tpos.x, row2_y), u_text3, fmt.c_str());
    if (e.duration_ms > 0) {
      const std::string dur =
          formatClock(std::chrono::milliseconds(e.duration_ms));
      const ImVec2 dsz = ImGui::CalcTextSize(dur.c_str());
      dl->AddText(ImVec2(rmax.x - pad_x - dsz.x, row2_y), u_text3,
                  dur.c_str());
    }
    const std::int64_t resume = resumePositionMs(e);
    std::string sub;
    if (resume > 0) {
      sub = "at " + formatClock(std::chrono::milliseconds(resume));
    } else {
      sub = parentFolderLabel(e.path);
    }
    if (!sub.empty()) {
      dl->AddText(ImVec2(tpos.x, row2_y + line_h + 4.0f), u_net, sub.c_str());
    }
    if (ui_focus_.holds(UiFocus::Face::LibraryRows, focus_i)) {
      drawFocusRing();
    }
    if (hovered) {
      std::string tip = e.path;
      if (e.duration_ms > 0) {
        tip += "\nduration " +
               formatClock(std::chrono::milliseconds(e.duration_ms));
      }
      if (e.play_count > 0) {
        tip += "\nplayed " + std::to_string(e.play_count) + "x";
      }
      if (nfo != nullptr && !nfo->plot.empty()) {
        tip += "\n" + nfo->plot;
      }
      ImGui::SetTooltip("%s", tip.c_str());
    }
    // Click zones: the poster's top-right x removes, anywhere else opens.
    if ((clicked || enter)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const bool in_x = mouse.x >= pmax.x - 22.0f && mouse.y <= pmin.y + 22.0f;
      if (clicked && in_x) {
        library_->removeEntry(e.path);
        library_->save();
      } else {
        openSource(e.path);
        st_.overlay = Overlay::None;
      }
    }
    ImGui::PopID();
    ++focus_i;
    ++idx;
  }
  // Reserve the grid's full height so the child scrolls past the last row.
  const int total_rows = (idx + cols - 1) / cols;
  ImGui::SetCursorPos(
      ImVec2(start.x, start.y + total_rows * (card_h + kGap)));
  ImGui::EndChild();
}

}  // namespace soar::app

#endif  // SOAR_WITH_IMGUI
