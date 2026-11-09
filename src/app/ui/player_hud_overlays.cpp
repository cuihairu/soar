// PlayerHud overlay pages: info, recent, dirpick, help.
// Split out of player_window.cpp; whole file is ImGui-only.
#ifdef SOAR_WITH_IMGUI

#include "player_hud.h"

namespace soar::app {

void PlayerHud::drawOverlays() {
  // V7b focus upkeep: a face exists only while its surface is up — the
  // overlay enum is authoritative (overlays close by enum writes in the
  // row handlers), a popup menu by ImGui's own verdict — and focus
  // clamps to the counts the last draw published.
  const bool popup_open = ImGui::IsPopupOpen(
      nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (ui_focus_.face() == UiFocus::Face::PlaylistRows &&
      st_.overlay != Overlay::Playlist) {
    ui_focus_.clear();
  }
  if (ui_focus_.face() == UiFocus::Face::LibraryRows &&
      st_.overlay != Overlay::Library) {
    ui_focus_.clear();
  }
  if (ui_focus_.face() == UiFocus::Face::MenuItems && !popup_open) {
    ui_focus_.clear();
  }
  ui_focus_.clamp(UiFocus::Face::PlaylistRows, playlist_rows_);
  ui_focus_.clamp(UiFocus::Face::LibraryRows, library_rows_);
  if (popup_open) ui_focus_.clamp(UiFocus::Face::MenuItems, menu_count_);
  switch (st_.overlay) {
    case Overlay::None: return;
    case Overlay::Info: drawInfoOverlay(); return;
    case Overlay::Recent: drawRecentOverlay(); return;
    case Overlay::Help: drawHelpOverlay(); return;
    case Overlay::Subtitle: drawSubtitleOverlay(); return;
    case Overlay::Playlist: drawPlaylistOverlay(); return;
    case Overlay::DirPick: drawDirPickOverlay(); return;
    case Overlay::Library: drawLibraryOverlay(); return;
    case Overlay::Cast: drawCastOverlay(); return;
    case Overlay::Settings: drawSettingsOverlay(); return;
  }
}

void PlayerHud::drawInfoOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(360, 200), ImVec2(480, 800));
  if (ImGui::Begin("Media Info", &open,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    const MediaInfo info = player_.mediaInfo();
    ImGui::TextWrapped("%s", current_uri_.c_str());
    ImGui::Separator();
    ImGui::Text("State: %s", stateName(player_.state()));
    ImGui::Text("Position: %s / %s", formatClock(player_.position()).c_str(),
                formatClock(info.duration).c_str());
    ImGui::Text("Seekable: %s", info.seekable ? "yes" : "no");
    ImGui::Text("Backend: %s", cfg_.backend_label.c_str());
    if (!cfg_.cache_dir.empty()) ImGui::Text("Cache: %s", cfg_.cache_dir.c_str());
    ImGui::Text("Rate: %.2gx  Volume: %d%%%s", st_.last_rate,
                static_cast<int>(st_.last_volume * 100),
                st_.last_muted ? " (muted)" : "");
    {
      std::chrono::milliseconds loop_a{0}, loop_b{0};
      if (player_.loopAB(loop_a, loop_b)) {
        ImGui::Text("A-B Loop: %s - %s", formatClock(loop_a).c_str(),
                    formatClock(loop_b).c_str());
      }
    }
    if (!playlist_.empty()) {
      const char* loop_name = playlist_.loop() == PlaylistStore::Loop::Off
                                  ? "off"
                                  : playlist_.loop() == PlaylistStore::Loop::All
                                      ? "all"
                                      : "one";
      ImGui::Text("Playlist: %d entr%s (current %d, loop %s%s)",
                  static_cast<int>(playlist_.size()),
                  playlist_.size() == 1 ? "y" : "ies",
                  static_cast<int>(playlist_.current()) + 1, loop_name,
                  playlist_.shuffle() ? ", shuffle" : "");
    }
    const std::string err = player_.lastError();
    if (!err.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
      ImGui::TextWrapped("Error: %s", err.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::Text("Tracks: %d", static_cast<int>(info.tracks.size()));
    for (const auto& t : info.tracks) {
      const bool selected =
          (t.type == TrackType::Video && t.id == info.selected_video) ||
          (t.type == TrackType::Audio && t.id == info.selected_audio) ||
          (t.type == TrackType::Subtitle && t.id == info.selected_subtitle);
      std::string line = std::string("  [") + trackTypeName(t.type) + "] #" +
                         std::to_string(t.id) + " " + t.codec;
      if (!t.language.empty()) line += " (" + t.language + ")";
      if (!t.title.empty()) line += " - " + t.title;
      if (selected) {
        ImGui::TextUnformatted((line + "  *").c_str());
      } else {
        ImGui::TextDisabled("%s", line.c_str());
      }
    }
  }
  ImGui::End();
  if (!open) st_.overlay = Overlay::None;
}

void PlayerHud::drawRecentOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460, 340), ImGuiCond_Appearing);
  if (ImGui::Begin("Recent", &open, ImGuiWindowFlags_NoSavedSettings)) {
    if (recent_.entries().empty()) {
      ImGui::TextDisabled("%s", "No recent files yet.");
    }
    // Snapshot before iterating: picking an entry rewrites the list
    // (move-to-front plus a save), and iterating the very vector that
    // the pick mutates leaves both the loop's iterator and the `uri`
    // reference dangling.
    const std::vector<std::string> entries = recent_.entries();
    for (const std::string& uri : entries) {
      if (ImGui::Selectable(displayName(uri).c_str())) {
        openSource(uri);
        st_.overlay = Overlay::None;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", uri.c_str());
    }
  }
  ImGui::End();
  if (!open) st_.overlay = Overlay::None;
}

void PlayerHud::drawDirPickOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
  if (ImGui::Begin(dirpick_files_ ? "Add files" : "Open folder", &open,
                   ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextDisabled("%s", dirpick_path_.c_str());
    // Library watch mode is always recursive (a library watches a tree,
    // not a folder shelf); file mode picks individual files, so the
    // toggle governs nothing there either. It only governs queue imports.
    if (!dirpick_for_library_ && !dirpick_files_) {
      ImGui::Checkbox("Include subfolders", &dirpick_recursive_);
    }
    const bool effective_recursive = dirpick_for_library_ || dirpick_recursive_;
    ImGui::Separator();

    const std::vector<std::string>& dirs = recent_dirs_.entries();
    if (!dirs.empty() && ImGui::CollapsingHeader("Recent folders")) {
      const std::vector<std::string> snapshot = dirs;  // pick rewrites it
      for (const std::string& d : snapshot) {
        if (ImGui::Selectable(baseName(d).c_str())) dirpick_path_ = d;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.c_str());
      }
    }

    // Directory rows, and in file mode the current directory's media
    // files below them. The child region keeps the action button pinned
    // below the scrollable list. The files list flat (no recursion) —
    // a deep pick walks the tree directory by directory, which is what
    // the checkboxes are for.
    if (ImGui::BeginChild("dirs", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
      const std::string up = parentOfDir(dirpick_path_);
      if (!up.empty() && ImGui::Selectable("..")) dirpick_path_ = up;
      const std::vector<std::string> subs = listSubdirectories(dirpick_path_);
      for (const std::string& s : subs) {
        if (ImGui::Selectable((s + "/").c_str())) dirpick_path_ = joinDir(dirpick_path_, s);
      }
      if (up.empty() && subs.empty()) ImGui::TextDisabled("%s", "No subfolders");
      if (dirpick_files_) {
        for (const std::string& f : listMediaFiles(dirpick_path_, false)) {
          // Membership by scan, mutation by the checkbox's own verdict:
          // the vector keeps click order across directory changes, which
          // is the order the queued set will take.
          const auto at = std::find(dirpick_checked_.begin(),
                                    dirpick_checked_.end(), f);
          bool checked = at != dirpick_checked_.end();
          if (ImGui::Checkbox(baseName(f).c_str(), &checked)) {
            if (checked && at == dirpick_checked_.end()) {
              dirpick_checked_.push_back(f);
            } else if (!checked && at != dirpick_checked_.end()) {
              dirpick_checked_.erase(at);
            }
          }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
        }
      }
    }
    ImGui::EndChild();

    if (dirpick_files_) {
      // File mode's action: the checked set, not the walked tree — no
      // recursive count to cache, the vector's size is the count.
      ImGui::BeginDisabled(dirpick_checked_.empty());
      if (ImGui::Button(("Add " + std::to_string(dirpick_checked_.size()) +
                         " file" + (dirpick_checked_.size() == 1 ? "" : "s"))
                            .c_str())) {
        addCheckedFiles(nowMs());
      }
      ImGui::EndDisabled();
    } else {
      if (dirpick_count_valid_ && dirpick_counted_path_ == dirpick_path_ &&
          dirpick_counted_recursive_ == effective_recursive) {
        // cache hit — the walk below is skipped
      } else {
        dirpick_count_ = listMediaFiles(dirpick_path_, effective_recursive).size();
        dirpick_counted_path_ = dirpick_path_;
        dirpick_counted_recursive_ = effective_recursive;
        dirpick_count_valid_ = true;
      }
      ImGui::BeginDisabled(dirpick_count_ == 0);
      char label[160];
      std::snprintf(label, sizeof(label), "%s this folder (%zu media file%s%s)",
                    dirpick_for_library_ ? "Watch" : "Import",
                    dirpick_count_, dirpick_count_ == 1 ? "" : "s",
                    effective_recursive ? ", with subfolders" : "");
      if (ImGui::Button(label)) {
        if (dirpick_for_library_) {
          watchDirectory(dirpick_path_, nowMs());
        } else {
          importDirectory(dirpick_path_, nowMs());
        }
      }
      ImGui::EndDisabled();
    }
  }
  ImGui::End();
  if (!open) st_.overlay = Overlay::None;
}

void PlayerHud::drawHelpOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(380, 400), ImGuiCond_Appearing);
  if (ImGui::Begin("Shortcuts", &open, ImGuiWindowFlags_NoSavedSettings)) {
    // Rows mirror the shipped table; the key column renders from the
    // live map, so a rebound config relabels the help (docs/ui-design.md
    // §3.2). With the default config the column is byte-identical to the
    // static strings that used to live here (the X11 drive tests pin
    // this text via label hashing).
    struct HelpRow {
      ShortcutAction a;
      ShortcutAction b;  // None = single-action row
      const char* label;
    };
    static const HelpRow kRows[] = {
        {ShortcutAction::PlayPause, ShortcutAction::None, "Play / pause"},
        {ShortcutAction::OpenFile, ShortcutAction::None,
         "Open a file (Windows dialog)"},
        {ShortcutAction::OpenFolder, ShortcutAction::None,
         "Open a folder into the queue"},
        {ShortcutAction::SeekBack5, ShortcutAction::SeekFwd5,
         "Seek -5s / +5s"},
        {ShortcutAction::SeekBack1, ShortcutAction::SeekFwd1,
         "Seek -1s / +1s"},
        {ShortcutAction::SeekBack60, ShortcutAction::SeekFwd60,
         "Seek -60s / +60s"},
        {ShortcutAction::SeekStart, ShortcutAction::None, "Go to start"},
        {ShortcutAction::PercentSeek, ShortcutAction::None,
         "Jump to 0% - 90%"},
        {ShortcutAction::VolumeUp, ShortcutAction::VolumeDown,
         "Volume -5% / +5%"},
        {ShortcutAction::Mute, ShortcutAction::None, "Mute"},
        {ShortcutAction::RateDown, ShortcutAction::RateUp,
         "Speed down / up"},
        {ShortcutAction::CycleAudio, ShortcutAction::None,
         "Cycle audio track"},
        {ShortcutAction::CycleSubtitle, ShortcutAction::None,
         "Cycle subtitles (incl. off)"},
        {ShortcutAction::Screenshot, ShortcutAction::None,
         "Screenshot (PNG)"},
        {ShortcutAction::SubtitleVisibility, ShortcutAction::None,
         "Toggle subtitles on/off"},
        {ShortcutAction::SubtitleOffsetBack, ShortcutAction::SubtitleOffsetFwd,
         "Subtitle offset -500ms / +500ms"},
        {ShortcutAction::LoopAB, ShortcutAction::None,
         "A-B loop (arm A, arm B, clear)"},
        {ShortcutAction::Fullscreen, ShortcutAction::None, "Fullscreen"},
        {ShortcutAction::Info, ShortcutAction::None, "Media info"},
        {ShortcutAction::Recent, ShortcutAction::None, "Recent files"},
        {ShortcutAction::Playlist, ShortcutAction::None, "Playlist queue"},
        {ShortcutAction::Library, ShortcutAction::None, "Media library"},
        {ShortcutAction::Cast, ShortcutAction::None,
         "Cast to a DLNA renderer"},
        {ShortcutAction::QueueNext, ShortcutAction::QueuePrev,
         "Queue next / previous"},
        {ShortcutAction::Help, ShortcutAction::None, "This help"},
        {ShortcutAction::Escape, ShortcutAction::None,
         "Leave fullscreen / quit"},
        {ShortcutAction::Quit, ShortcutAction::None, "Quit"},
        {ShortcutAction::SearchLibrary, ShortcutAction::None,
         "Focus the library search"},
    };
    std::vector<std::string> keys;
    keys.reserve(sizeof(kRows) / sizeof(kRows[0]));
    for (const HelpRow& row : kRows) {
      std::string key = shortcuts_.bindingNames(row.a);
      if (row.b != ShortcutAction::None) {
        key += " / ";
        key += shortcuts_.bindingNames(row.b);
      }
      keys.push_back(std::move(key));
    }
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg)) {
      std::size_t i = 0;
      for (const HelpRow& row : kRows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(keys[i].c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", row.label);
        ++i;
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
  if (!open) st_.overlay = Overlay::None;
}

void PlayerHud::drawSubtitleOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(360, 240), ImVec2(480, 400));
  if (ImGui::Begin("Subtitle Settings", &open,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    const MediaInfo info = player_.mediaInfo();

    // Subtitle track selection (mirror of the track combo)
    std::vector<const TrackInfo*> sub_tracks;
    for (const auto& t : info.tracks) {
      if (t.type == TrackType::Subtitle) sub_tracks.push_back(&t);
    }

    if (sub_tracks.empty()) {
      ImGui::TextDisabled("No subtitle tracks available");
      // A media with no subtitle track of its own is exactly the case a
      // sidecar is for, so they go in the open overlay body instead of
      // behind a combo that has nothing to select.
      drawSidecarEntries(info, nowMs());
    } else {
      ImGui::Text("Track:");
      ImGui::SameLine();
      const TrackId selected = info.selected_subtitle;
      char label[40];
      std::snprintf(label, sizeof(label), "Sub %d/%d",
                    selected < 0 ? 0 : static_cast<int>(selected),
                    static_cast<int>(sub_tracks.size()));
      if (ImGui::BeginCombo("##sub_select", label)) {
        menu_rows_ = 0;  // the open popup republishes its rows every frame
        const bool enter = menuRowEnter();
        if ((ImGui::Selectable("Off", selected < 0) || enter) && selected >= 0) {
          player_.disableSubtitles();
          st_.toast.show("Subtitles off", nowMs());
        }
        menuRowRing();
        ImGui::Separator();
        for (const auto* t : sub_tracks) {
          std::string item = "#" + std::to_string(t->id) + " " + t->codec;
          if (!t->language.empty()) item += " (" + t->language + ")";
          if (!t->title.empty()) item += " - " + t->title;
          const bool is_sel = t->id == selected;
          const bool enter = menuRowEnter();
          if ((ImGui::Selectable(item.c_str(), is_sel) || enter) && !is_sel) {
            if (player_.selectTrack(TrackType::Subtitle, t->id)) {
              st_.toast.show("Subtitle track " + std::to_string(t->id), nowMs());
            } else {
              st_.toast.show("Subtitle switch failed", nowMs());
            }
          }
          menuRowRing();
        }
        // Sidecar files join the container's own tracks in the same
        // popup; the open popup is what gates the directory walk.
        drawSidecarEntries(info, nowMs());
        menu_count_ = menu_rows_;
        ImGui::EndCombo();
      }
    }

    ImGui::Separator();

    // Font size
    ImGui::Text("Font Size:");
    ImGui::SameLine();
    float font_size = st_.subtitle_font_size;
    if (ImGui::SliderFloat("##sub_font", &font_size, 12.0f, 48.0f, "%.0f px")) {
      st_.subtitle_font_size = font_size;
      if (cfg_.subtitle_font_size) {
        cfg_.subtitle_font_size->store(font_size, std::memory_order_relaxed);
      }
    }

    // Sync offset
    ImGui::Text("Sync Offset:");
    ImGui::SameLine();
    int offset_ms = st_.subtitle_offset_ms;
    if (ImGui::SliderInt("##sub_offset", &offset_ms, -5000, 5000, "%d ms")) {
      st_.subtitle_offset_ms = offset_ms;
      if (cfg_.subtitle_offset_ms) {
        cfg_.subtitle_offset_ms->store(offset_ms, std::memory_order_relaxed);
      }
    }

    // Visibility toggle
    ImGui::Text("Visible:");
    ImGui::SameLine();
    bool visible = st_.subtitle_visible;
    if (ImGui::Checkbox("##sub_visible", &visible)) {
      st_.subtitle_visible = visible;
      if (cfg_.subtitle_visible) {
        cfg_.subtitle_visible->store(visible, std::memory_order_relaxed);
      }
    }

    ImGui::Separator();
    ImGui::TextDisabled("C: cycle  |  V: show/hide  |  [ / ]: offset  |  S: screenshot");
  }
  ImGui::End();
  if (!open) st_.overlay = Overlay::None;
}

}  // namespace soar::app

#endif  // SOAR_WITH_IMGUI
