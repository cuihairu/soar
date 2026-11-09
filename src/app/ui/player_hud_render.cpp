// PlayerHud per-frame rendering: HUD, chips, top bar, toast.
// Split out of player_window.cpp; whole file is ImGui-only.
#ifdef SOAR_WITH_IMGUI

#include "player_hud.h"

namespace soar::app {

void PlayerHud::drawPauseOverlay() {
  if (player_.state() != PlaybackState::Paused) return;
  const ImGuiIO& io = ImGui::GetIO();
  const float cx = io.DisplaySize.x * 0.5f;
  const float cy = io.DisplaySize.y * 0.5f - 16.0f;

  ImGui::SetNextWindowPos(ImVec2(cx, cy), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (ImGui::Begin("##pauseplay", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    // Bitmap fonts carry no triangle glyph, so the face is drawn: a
    // filled circle in the accent (brightened on hover/press) with the
    // ink play triangle. InvisibleButton supplies the hit box.
    if (ImGui::InvisibleButton("##resume", ImVec2(64.0f, 64.0f))) {
      togglePlayPause();
    }
    const bool hover = ImGui::IsItemHovered();
    const bool press = ImGui::IsItemActive();
    ImVec4 face = theme::kAccent;
    face.x += press ? 0.10f : (hover ? 0.06f : 0.0f);
    face.y += press ? 0.10f : (hover ? 0.06f : 0.0f);
    face.z += press ? 0.10f : (hover ? 0.06f : 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bmin = ImGui::GetItemRectMin();
    const ImVec2 center(bmin.x + 32.0f, bmin.y + 32.0f);
    dl->AddCircleFilled(center, 32.0f,
                        ImGui::ColorConvertFloat4ToU32(face), 48);
    dl->AddTriangleFilled(ImVec2(center.x - 6.0f, center.y - 9.0f),
                          ImVec2(center.x - 6.0f, center.y + 9.0f),
                          ImVec2(center.x + 10.0f, center.y),
                          ImGui::ColorConvertFloat4ToU32(theme::kInkOnAccent));
  }
  ImGui::End();
  ImGui::PopStyleVar();

  ImGui::SetNextWindowPos(ImVec2(cx, cy + 32.0f + 14.0f + 11.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // The prototype's .floatpill: a full pill. Drawn on the draw list, for
  // the same reason as chip() — a 999 WindowRounding would stretch the
  // auto-resize window to the viewport (see chip's comment).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 5.0f));
  if (ImGui::Begin("##pausehint", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoInputs)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + ImGui::GetWindowWidth(),
                    p0.y + ImGui::GetWindowHeight());
    dl->AddRectFilled(p0, p1,
                      ImGui::ColorConvertFloat4ToU32(theme::kPanelBg),
                      theme::kPillRounding);
    // The prototype's .floatpill keeps its 1px hairline over the pill.
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::kBorder),
                theme::kPillRounding);
    ImGui::TextUnformatted("Paused - Space or click to resume");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);

  // Begun last so it is the back-most of the three (see the ordering note
  // above). NoInputs: a capturing fullscreen window would set
  // WantCaptureMouse and swallow the app-level click gestures (OSC toggle,
  // double-click fullscreen) while paused.
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (ImGui::Begin("##pausedim", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs)) {
    // rgba(4,5,7,.38) over the video, as in the prototype.
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f),
                                              io.DisplaySize,
                                              IM_COL32(4, 5, 7, 97));
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void PlayerHud::uploadThumbFrame(const ThumbnailFrame& f) {
  if (f.width <= 0 || f.height <= 0) return;
  if (thumb_texture_ == nullptr || thumb_tex_w_ != f.width ||
      thumb_tex_h_ != f.height) {
    if (thumb_texture_ != nullptr) {
      SDL_DestroyTexture(thumb_texture_);
      thumb_texture_ = nullptr;
    }
    thumb_texture_ =
        SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
                          SDL_TEXTUREACCESS_STREAMING, f.width, f.height);
    if (thumb_texture_ == nullptr) return;  // out of memory: no preview
    thumb_tex_w_ = f.width;
    thumb_tex_h_ = f.height;
  }
  SDL_UpdateYUVTexture(thumb_texture_, nullptr, f.y.data(), f.stride_y,
                       f.u.data(), f.stride_u, f.v.data(), f.stride_v);
}

void PlayerHud::drawThumbPreview(milliseconds pos, float frac, const ImVec2& rail_min,
                      const ImVec2& rail_max) {
  if (thumb_texture_ == nullptr) return;
  const float tw = 160.0f, th = 90.0f;  // the grab's native size
  const float frame = 4.0f;             // card frame around the image
  const float strip = 18.0f;            // the timecode strip
  const float card_w = tw + 2.0f * frame;
  const float card_h = th + frame + strip;
  const float gap = 10.0f;  // between the card and the rail
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float cx = std::clamp(rail_min.x + frac * (rail_max.x - rail_min.x),
                              card_w * 0.5f + 6.0f,
                              disp.x - card_w * 0.5f - 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowPos(
      ImVec2(cx - card_w * 0.5f, rail_min.y - gap - card_h), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(card_w, card_h), ImGuiCond_Always);
  if (ImGui::Begin("##thumbprev", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoScrollbar)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pmin = ImGui::GetWindowPos();
    dl->AddRectFilled(pmin, ImVec2(pmin.x + card_w, pmin.y + card_h),
                      ImGui::ColorConvertFloat4ToU32(kThumbBg), 8.0f);
    dl->AddImage((ImTextureID)(intptr_t)thumb_texture_,
                 ImVec2(pmin.x + frame, pmin.y + frame),
                 ImVec2(pmin.x + frame + tw, pmin.y + frame + th));
    const std::string tc = formatClock(pos);
    const float tc_w = ImGui::CalcTextSize(tc.c_str()).x;
    dl->AddText(
        ImVec2(pmin.x + (card_w - tc_w) * 0.5f,
               pmin.y + frame + th + (strip - ImGui::GetTextLineHeight()) * 0.5f),
        IM_COL32(255, 255, 255, 235), tc.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleVar();
}

void PlayerHud::drawHud(milliseconds now) {
  const ImGuiIO& io = ImGui::GetIO();
  const float w = io.DisplaySize.x;
  const float h = io.DisplaySize.y;
  // OSC geometry per redesign/desktop-main.html:62-63 (.osc): width
  // min(97%, 1240px), bottom 26px. The old 920px/16px came from the v1
  // draft; the v2 spec is wider and sits a touch higher off the edge.
  const float bar_w = std::min(w * 0.97f, 1240.0f);
  const float bar_h = 80.0f;  // seek row + button row

  const MediaInfo info = player_.mediaInfo();
  const double dur = static_cast<double>(info.duration.count());
  const double pos = static_cast<double>(player_.position().count());
  const double shown = st_.seek_dragging ? st_.seek_scrub : pos;

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st_.hud_alpha);
  ImGui::SetNextWindowPos(ImVec2((w - bar_w) * 0.5f, h - bar_h - 26.0f),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(bar_w, bar_h), ImGuiCond_Always);
  if (ImGui::Begin("##osc", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoScrollbar)) {
    // Row 1: elapsed | seek rail | -remaining, the prototype's seekrow
    // (redesign/desktop-main.html). The clocks used to sit in row 2; the
    // audit's L3 moved them up beside the bar. Hover/drag tooltip shows
    // the time under the pointer; dragging previews, release commits
    // (docs §3.1).
    const bool seekable = info.seekable && dur > 0.0;
    const std::string left_tc =
        (st_.seek_dragging ? "> " : "") +
        formatClock(milliseconds(static_cast<long long>(shown)));
    std::string right_tc;
    if (dur > 0.0) {
      const auto remaining = std::max(
          milliseconds(0),
          info.duration - milliseconds(static_cast<long long>(shown)));
      right_tc = "-" + formatClock(remaining);
    }
    ImGui::TextUnformatted(left_tc.c_str());
    ImGui::SameLine();
    if (!seekable) ImGui::BeginDisabled();
    const float right_w =
        right_tc.empty() ? 0.0f : ImGui::CalcTextSize(right_tc.c_str()).x;
    const float rail_w =
        ImGui::GetContentRegionAvail().x - right_w -
        (right_w > 0.0f ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
    ImGui::PushItemWidth(std::max(rail_w, 40.0f));
    double v = shown;
    const double v_min = 0.0;
    // Core imgui has no SliderDouble; the generic double slider does.
    if (ImGui::SliderScalar("##seek", ImGuiDataType_Double, &v, &v_min, &dur,
                            "",
                            ImGuiSliderFlags_AlwaysClamp |
                                ImGuiSliderFlags_NoInput)) {
      st_.seek_dragging = true;
      st_.seek_scrub = v;
    }
    if (seekable && ImGui::IsItemHovered()) {
      const ImVec2 rmin = ImGui::GetItemRectMin();
      const ImVec2 rmax = ImGui::GetItemRectMax();
      const float frac =
          std::clamp((io.MousePos.x - rmin.x) / std::max(rmax.x - rmin.x, 1.0f),
                     0.0f, 1.0f);
      // Seek-hover thumbnail (docs §1.6-5): the hovered position is
      // quantized to ~250ms so a still pointer decodes once instead of
      // every frame; the service dedupes identical requests and answers
      // repeat scrubs from its LRU. While the new frame decodes the old
      // preview hides (a stale image over a new position is worse than
      // none), then the card appears as soon as the frame lands.
      const milliseconds hover_pos(static_cast<long long>(frac * dur));
      const milliseconds q_pos((hover_pos.count() / 250) * 250);
      if (q_pos != thumb_requested_) {
        thumb_requested_ = q_pos;
        thumb_shown_ = milliseconds(-1);
        if (thumb_service_ != nullptr) thumb_service_->request(q_pos);
      }
      ThumbnailFrame tf;
      if (thumb_service_ != nullptr && thumb_service_->tryGet(q_pos, tf) &&
          thumb_shown_ != q_pos) {
        uploadThumbFrame(tf);
        thumb_shown_ = q_pos;
      }
      if (thumb_shown_ == q_pos) drawThumbPreview(q_pos, frac, rmin, rmax);
    } else if (ImGui::IsItemActive()) {
      // Dragging keeps the small time tooltip (the preview is a hover
      // affordance; a drag is already "here, now", docs §1.6-5).
      const ImVec2 rmin = ImGui::GetItemRectMin();
      const ImVec2 rmax = ImGui::GetItemRectMax();
      const float frac =
          std::clamp((io.MousePos.x - rmin.x) / std::max(rmax.x - rmin.x, 1.0f),
                     0.0f, 1.0f);
      ImGui::SetTooltip(
          "%s",
          formatClock(milliseconds(static_cast<long long>(frac * dur))).c_str());
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      seekTo(milliseconds(static_cast<long long>(st_.seek_scrub)), now);
      st_.seek_dragging = false;
    } else if (ImGui::IsItemDeactivated()) {
      st_.seek_dragging = false;  // cancelled drag: drop the pending scrub
    }
    if (!seekable) ImGui::EndDisabled();
    ImGui::PopItemWidth();
    if (!right_tc.empty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted(right_tc.c_str());
    }

    // Row 2: transport, volume, rate, tracks, overlays, tools.
    const bool playing = player_.state() == PlaybackState::Playing;
    // Play/pause as the prototype's .tplay: a 38px circular accent button
    // with an ink glyph (redesign/desktop-main.html:95-99). The bitmap font
    // carries no triangle/pause glyph, so the face + glyph are drawn on the
    // draw list the same way drawPauseOverlay does.
    if (ImGui::InvisibleButton("##playpause", ImVec2(38.0f, 38.0f))) {
      togglePlayPause();
    }
    {
      const bool phover = ImGui::IsItemHovered();
      const bool ppress = ImGui::IsItemActive();
      ImVec4 face = theme::kAccent;
      face.x += ppress ? 0.10f : (phover ? 0.06f : 0.0f);
      face.y += ppress ? 0.10f : (phover ? 0.06f : 0.0f);
      face.z += ppress ? 0.10f : (phover ? 0.06f : 0.0f);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 bmin = ImGui::GetItemRectMin();
      const ImVec2 center(bmin.x + 19.0f, bmin.y + 19.0f);
      const ImU32 faceU32 = ImGui::ColorConvertFloat4ToU32(face);
      const ImU32 inkU32 = ImGui::ColorConvertFloat4ToU32(theme::kInkOnAccent);
      dl->AddCircleFilled(center, 19.0f, faceU32, 48);
      if (playing) {
        // Pause: two vertical ink bars (prototype's pause glyph).
        dl->AddRectFilled(ImVec2(center.x - 5.0f, center.y - 7.0f),
                          ImVec2(center.x - 1.5f, center.y + 7.0f), inkU32, 1.0f);
        dl->AddRectFilled(ImVec2(center.x + 1.5f, center.y - 7.0f),
                          ImVec2(center.x + 5.0f, center.y + 7.0f), inkU32, 1.0f);
      } else {
        // Play: ink triangle.
        dl->AddTriangleFilled(ImVec2(center.x - 4.0f, center.y - 6.5f),
                              ImVec2(center.x - 4.0f, center.y + 6.5f),
                              ImVec2(center.x + 7.0f, center.y), inkU32);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(46, 0))) player_.stop();
    ImGui::SameLine();
    // Prev/next as buttons (prototype's transport cluster; the N /
    // Shift+N shortcuts behind them — same playlistStep, same boundary
    // toasts).
    if (ImGui::Button("|<", ImVec2(30, 0))) playlistStep(true, now);
    ImGui::SameLine();
    if (ImGui::Button(">|", ImVec2(30, 0))) playlistStep(false, now);
    ImGui::SameLine();
    // (The clock used to sit here; it moved to the seek row when the
    // audit's L3 flanked the rail with timecodes — no duplicate.)
    if (ImGui::Button(st_.last_muted ? "Unmute" : "Mute")) toggleMute(now);
    ImGui::SameLine();
    ImGui::PushItemWidth(56.0f);
    const double vol_min = 0.0;
    const double vol_max = 1.0;
    if (ImGui::SliderScalar("##vol", ImGuiDataType_Double, &st_.last_volume,
                            &vol_min, &vol_max, "",
                            ImGuiSliderFlags_AlwaysClamp |
                                ImGuiSliderFlags_NoInput)) {
      player_.setVolume(st_.last_volume);
      st_.toast.show(
          "Volume " + std::to_string(static_cast<int>(st_.last_volume * 100)) + "%",
          now);
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushItemWidth(56.0f);
    drawRateCombo(now);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(70.0f);
    drawTrackCombo(TrackType::Audio, now);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(70.0f);
    drawTrackCombo(TrackType::Subtitle, now);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    // Sub overlay button (v0.2): track combo selects the stream, this page
    // holds the presentation knobs (font size, offset, visibility).
    if (ImGui::Button("Sub")) toggleOverlay(Overlay::Subtitle);
    ImGui::SameLine();
    // Info/Fullscreen moved to the top bar when it landed (M1): the
    // prototype has the four tools there, not in the transport row.
    // A-B loop, subtitle visibility and screenshot as buttons (the L / V / S
    // shortcuts behind them — same actions, same toasts). The CC button is
    // the prototype's caption toggle: it flips whether canvas subtitles
    // draw, independent of which subtitle stream the Sub combo selected.
    if (ImGui::Button("A-B")) cycleLoopAB(now);
    ImGui::SameLine();
    if (ImGui::Button("CC")) toggleSubtitleVisibility(now);
    ImGui::SameLine();
    if (ImGui::Button("Shot")) takeScreenshot(now);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void PlayerHud::drawRateCombo(milliseconds now) {
  static const double kRates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
  char label[16];
  std::snprintf(label, sizeof(label), "%.2gx", st_.last_rate);
  if (!ImGui::BeginCombo("##rate", label)) return;
  menu_rows_ = 0;  // the open popup republishes its rows every frame
  for (const double r : kRates) {
    const bool selected = std::abs(r - st_.last_rate) < 1e-9;
    char item[16];
    std::snprintf(item, sizeof(item), "%.2gx", r);
    const bool enter = menuRowEnter();
    if ((ImGui::Selectable(item, selected) || enter) && !selected) {
      st_.last_rate = r;
      player_.setRate(r);
      st_.toast.show(std::string(item) + " speed", now);
    }
    menuRowRing();
  }
  menu_count_ = menu_rows_;
  ImGui::EndCombo();
}

void PlayerHud::drawTrackCombo(TrackType type, milliseconds now) {
  const MediaInfo info = player_.mediaInfo();
  std::vector<const TrackInfo*> tracks;
  for (const auto& t : info.tracks) {
    if (t.type == type) tracks.push_back(&t);
  }
  const TrackId selected =
      type == TrackType::Audio ? info.selected_audio : info.selected_subtitle;
  char label[40];
  if (type == TrackType::Audio) {
    std::snprintf(label, sizeof(label), "Audio %d/%d",
                  tracks.empty() ? 0 : static_cast<int>(selected),
                  static_cast<int>(tracks.size()));
  } else {
    std::snprintf(label, sizeof(label), "Sub %d/%d",
                  selected < 0 ? 0 : static_cast<int>(selected),
                  static_cast<int>(tracks.size()));
  }
  if (!ImGui::BeginCombo(type == TrackType::Audio ? "##audio" : "##sub", label)) {
    return;
  }
  menu_rows_ = 0;  // the open popup republishes its rows every frame
  // Current value marked, one level of nesting max (docs §1.4-4).
  if (type == TrackType::Subtitle) {
    const bool off = selected < 0;
    const bool enter = menuRowEnter();
    if ((ImGui::Selectable("Off", off) || enter) && !off) {
      player_.disableSubtitles();
      st_.toast.show("Subtitles off", now);
    }
    menuRowRing();
    ImGui::Separator();
  }
  for (const auto* t : tracks) {
    std::string item =
        "#" + std::to_string(t->id) + " " + t->codec;
    if (!t->language.empty()) item += " (" + t->language + ")";
    const bool is_sel = t->id == selected;
    const bool enter = menuRowEnter();
    if ((ImGui::Selectable(item.c_str(), is_sel) || enter) && !is_sel) {
      if (player_.selectTrack(type, t->id)) {
        st_.toast.show((type == TrackType::Audio ? "Audio track "
                                                 : "Subtitle track ") +
                           std::to_string(t->id),
                       now);
      } else {
        st_.toast.show("Track switch failed", now);
      }
    }
    menuRowRing();
  }
  // Output endpoint under the audio combo (v0.2): the tracks above pick
  // the stream, these entries pick where it comes out. Enumerated only
  // while the combo is open — SDL device discovery is not per-frame
  // material. Host names can be long descriptive strings; the label is
  // clipped but the selection carries the full name.
  if (type == TrackType::Audio) {
    ImGui::Separator();
    const std::string current = player_.currentAudioOutputDevice();
    auto deviceItem = [&](const char* label, const std::string& value) {
      const bool is_sel = current == value;
      const bool enter = menuRowEnter();
      if ((ImGui::Selectable(label, is_sel) || enter) && !is_sel) {
        if (player_.selectAudioOutputDevice(value)) {
          st_.toast.show(value.empty() ? "Output: default"
                                       : "Output: " + value.substr(0, 24),
                         now);
        } else {
          st_.toast.show("Device switch failed", now);
        }
      }
      menuRowRing();
    };
    deviceItem("Output: default", "");
    for (const auto& d : player_.audioOutputDevices()) {
      const std::string label = d.substr(0, 28);
      deviceItem(label.c_str(), d);
    }
  } else {
    // Sidecar subtitle files: the loader for tracks that are not in the
    // container (docs/mvp.md §6).
    drawSidecarEntries(info, now);
  }
  menu_count_ = menu_rows_;
  ImGui::EndCombo();
}

void PlayerHud::drawPoster() {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("##poster", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs)) {
    if (firstRunEmpty()) {
      // GUI start with no source (BUGS.md #3): the poster is the open
      // entry itself — click / O / drop all land in openFilePicker().
      ImGui::TextUnformatted("Open a file");
      ImGui::TextDisabled("%s", stateName(player_.state()));
      ImGui::Spacing();
#ifdef _WIN32
      ImGui::TextDisabled("%s",
                          "Click, press O (file) or Ctrl+O (folder), or drop");
#else
      ImGui::TextDisabled("%s", "Drop a file here - Ctrl+O folder - R recent - H shortcuts");
#endif
    } else {
      ImGui::TextUnformatted(displayName(current_uri_).c_str());
      if (torrent_pending_ && torrent_->phase() ==
              soar::p2p::TorrentPhase::Connecting) {
        // Async P2P source not yet serving: the wait itself is the state.
        char line[64];
        std::snprintf(line, sizeof(line), "connecting to swarm - %d peers",
                      torrent_->status().peers);
        ImGui::TextDisabled("%s", line);
      } else {
        ImGui::TextDisabled("%s", stateName(player_.state()));
      }
      ImGui::Spacing();
      ImGui::TextDisabled("%s", "Drop a file here - Ctrl+O folder - R recent - H shortcuts");
    }
  }
  ImGui::End();
}

void PlayerHud::drawChips(milliseconds now) {
  // The prototype's .statechip (desktop-main.html:166-169): one persistent
  // pill centered under the top bar — state word, both clocks, and the P2P
  // download share while the disk cache is still filling. The standalone
  // Buffering/Downloading chips the old bar showed folded in here. It is
  // pure information (NoInputs): clicks fall through to the video beneath.
  // Casting keeps its own pill below — functional state the prototype does
  // not model, so it stays rather than folding in.
  if (!current_uri_.empty()) {
    const bool buffering =
        cfg_.buffering && cfg_.buffering->load(std::memory_order_relaxed);
    const std::uint64_t total =
        cfg_.download_total ? cfg_.download_total->load(std::memory_order_relaxed) : 0;
    const std::uint64_t have =
        cfg_.download_bytes ? cfg_.download_bytes->load(std::memory_order_relaxed) : 0;
    const bool downloading = total > 0 && have < total;

    const char* state = "Stopped";
    ImVec4 dot = theme::kTextMuted;
    switch (player_.state()) {
      case PlaybackState::Playing:
        if (buffering) {
          state = "Buffering";
          dot = theme::kWarn;
        } else {
          state = "Playing";
          dot = theme::kOk;
        }
        break;
      case PlaybackState::Paused:
        state = "Paused";
        dot = theme::kTextDim;
        break;
      case PlaybackState::Ended:
        state = "Ended";
        dot = theme::kTextMuted;
        break;
      case PlaybackState::Error:
        state = "Error";
        dot = theme::kDanger;
        break;
      case PlaybackState::Stopped:
        break;
    }

    std::string text = state;
    const MediaInfo info = player_.mediaInfo();
    if (info.duration > milliseconds(0)) {
      text += " - " + formatClock(player_.position()) + " / " +
              formatClock(info.duration);
    }
    if (downloading) {
      char p2p[24];
      std::snprintf(p2p, sizeof(p2p), " - P2P %llu%%",
                    static_cast<unsigned long long>(have * 100 / total));
      text += p2p;
    }
    stateChip("##statechip", text, dot);
  }
  // Cast chip while a DLNA session is live (docs/mvp.md §3): the TV
  // plays even when this window is minimized, so the state stays visible.
  if (cast_active_ &&
      cast_active_->phase() == soar::cast::CastSession::Phase::Casting) {
    chip("##cast", "Casting", 94.0f);
  }
}

void PlayerHud::drawSubtitles(milliseconds now) {
  if (!cfg_.ffmpeg) return;
  if (!st_.subtitle_visible) return;

  // Sync config to local state (config is owned by caller, may be updated externally)
  if (cfg_.subtitle_font_size) {
    st_.subtitle_font_size = cfg_.subtitle_font_size->load(std::memory_order_relaxed);
  }
  if (cfg_.subtitle_offset_ms) {
    st_.subtitle_offset_ms = cfg_.subtitle_offset_ms->load(std::memory_order_relaxed);
  }
  if (cfg_.subtitle_visible) {
    st_.subtitle_visible = cfg_.subtitle_visible->load(std::memory_order_relaxed);
  }

  // Canvas mode (batch 1c): the libass overlay holds the selected
  // subtitle — an ASS document or a synthesized text sidecar — so
  // drawing the queue's plain-text copy on top would render every line
  // twice. Returning here also parks the sidecar pump (this window is
  // its only driver in production) and leaves embedded text frames to
  // age out of the FIFO undrawn; the queue itself is untouched.
#ifdef SOAR_WITH_FFMPEG
  if (cfg_.ffmpeg && cfg_.ffmpeg->subtitleDocumentActive()) {
    return;
  }
#endif

  // Poll for new subtitle frame. The dereference needs the full backend
  // type, which only exists in FFmpeg builds (the header keeps the class
  // opaque so the window compiles without the libav* dev packages).
#ifdef SOAR_WITH_FFMPEG
  soar::DecodedSubtitleFrame sub_frame;
  if (cfg_.ffmpeg->tryGetSubtitleFrame(sub_frame)) {
    // Store the latest subtitle frame with its timing
    st_.current_subtitle = sub_frame.text;
    st_.subtitle_pts = sub_frame.pts;
    st_.subtitle_duration = sub_frame.duration;
  }
#endif

  // Check if current subtitle should be displayed based on position + offset
  const auto pos = player_.position();
  const auto adjusted_pos = pos + std::chrono::milliseconds(st_.subtitle_offset_ms);

  if (st_.current_subtitle.empty()) return;

  // Display subtitle if current position is within subtitle's time range
  const auto sub_start = st_.subtitle_pts;
  const auto sub_end = st_.subtitle_duration > milliseconds(0)
                           ? st_.subtitle_pts + st_.subtitle_duration
                           : st_.subtitle_pts + std::chrono::milliseconds(5000); // default 5s if no duration

  if (adjusted_pos < sub_start || adjusted_pos >= sub_end) {
    return; // Not time to show this subtitle yet, or already past it
  }

  // Render subtitle at bottom of video area
  const ImGuiIO& io = ImGui::GetIO();
  const float video_bottom = io.DisplaySize.y * 0.9f; // 90% down

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, video_bottom),
                          ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.7f);

  // Select font based on configured size (v0.2)
  ImFont* sub_font = subtitle_fonts_.size_24;
  float fs = st_.subtitle_font_size;
  if (fs <= 18.0f) sub_font = subtitle_fonts_.size_16;
  else if (fs <= 22.0f) sub_font = subtitle_fonts_.size_20;
  else if (fs <= 26.0f) sub_font = subtitle_fonts_.size_24;
  else if (fs <= 30.0f) sub_font = subtitle_fonts_.size_28;
  else if (fs <= 34.0f) sub_font = subtitle_fonts_.size_32;
  else if (fs <= 42.0f) sub_font = subtitle_fonts_.size_36;
  else sub_font = subtitle_fonts_.size_48;
  if (sub_font) ImGui::PushFont(sub_font);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.9f, 0));


  if (ImGui::Begin("##subtitle", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoInputs)) {
    ImGui::PushTextWrapPos(io.DisplaySize.x * 0.85f);
    ImGui::TextWrapped("%s", st_.current_subtitle.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  ImGui::PopFont();
}

void PlayerHud::chip(const char* name, const char* text, float y) {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, y), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  // The prototype's .statechip: a full pill. Drawn on the draw list rather
  // than through WindowRounding — Begin() floors a window's minimum height
  // at (rounding - 1) [imgui.cpp CalcWindowMinSize], so a 999 rounding
  // stretches an auto-resize window to the viewport (pixel-probed: a
  // 249px-wide pill rendered 540px tall). PathRect clamps the same 999 to
  // the rect's own half-height, which is exactly the pill.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  if (ImGui::Begin(name, nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    ImVec4 bg = theme::kPanelBg;
    bg.w = 0.72f;  // .statechip bg rgba(9,10,12,.72) (desktop-main.html:39)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + ImGui::GetWindowWidth(),
                    p0.y + ImGui::GetWindowHeight());
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(bg),
                      theme::kPillRounding);
    // The prototype's .statechip keeps its 1px hairline over the pill.
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::kBorder),
                theme::kPillRounding);
    ImGui::TextUnformatted(text);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void PlayerHud::stateChip(const char* name, const std::string& text, const ImVec4& dot) {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 64.0f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (ImGui::Begin(name, nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoInputs)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 14.0f;
    const float dot_r = 3.5f;  // 7px dot (.statechip .dot)
    const float gap = 9.0f;
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    // Reserve the pill's footprint; the window auto-sizes to this Dummy.
    ImGui::Dummy(ImVec2(pad * 2 + dot_r * 2 + gap + ts.x, ts.y + 12.0f));
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + ImGui::GetWindowWidth(),
                    p0.y + ImGui::GetWindowHeight());
    ImVec4 bg = theme::kPanelBg;
    bg.w = 0.72f;  // .statechip bg rgba(9,10,12,.72) (desktop-main.html:39)
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(bg),
                      theme::kPillRounding);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::kBorder),
                theme::kPillRounding);
    // Pulse: a ~1.6s breathing of the dot's radius (the prototype's
    // .statechip .dot). Only the radius moves — the ink stays steady.
    const double t = ImGui::GetTime();
    const float ph =
        static_cast<float>(std::fmod(t / 1.6, 1.0) * 6.2831853);
    const float r = dot_r * (1.0f + 0.20f * std::sin(ph));
    const float cy = (p0.y + p1.y) * 0.5f;
    dl->AddCircleFilled(ImVec2(p0.x + pad + dot_r, cy), r,
                        ImGui::ColorConvertFloat4ToU32(dot));
    dl->AddText(ImVec2(p0.x + pad + dot_r * 2 + gap, cy - ts.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(theme::kTextDim), text.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

float PlayerHud::topPill(ImDrawList* dl, float x, float cy, const char* text,
              const ImVec4& fg, bool tinted, bool play) {
  const float pad = 10.0f;
  const float ph = 22.0f;  // .pill height
  const float py = cy - ph * 0.5f;
  const float triW = play ? 11.0f : 0.0f;
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const float pw = pad + triW + ts.x + pad;
  ImU32 bg;
  if (tinted) {
    bg = ImGui::ColorConvertFloat4ToU32(ImVec4(fg.x, fg.y, fg.z, 0.14f));
  } else {
    bg = IM_COL32(255, 255, 255, 20);  // rgba(255,255,255,.08) .pill ground
  }
  const ImU32 fgU32 = ImGui::ColorConvertFloat4ToU32(fg);
  dl->AddRectFilled(ImVec2(x, py), ImVec2(x + pw, py + ph), bg,
                    theme::kPillRounding);
  dl->AddRect(ImVec2(x, py), ImVec2(x + pw, py + ph),
              ImGui::ColorConvertFloat4ToU32(theme::kBorder), theme::kPillRounding);
  float tx = x + pad;
  if (play) {
    dl->AddTriangleFilled(ImVec2(tx, cy - 4.0f), ImVec2(tx, cy + 4.0f),
                          ImVec2(tx + 7.0f, cy), fgU32);
    tx += triW;
  }
  dl->AddText(ImVec2(tx, cy - ts.y * 0.5f), fgU32, text);
  return x + pw;
}

bool PlayerHud::topTool(const char* id, char glyph, const char* tip, const ImVec2& pos) {
  const ImVec2 wp = ImGui::GetWindowPos();
  ImGui::SetCursorPos(ImVec2(pos.x - wp.x, pos.y - wp.y));
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(30.0f, 30.0f));
  const bool hovered = ImGui::IsItemHovered();
  if (hovered && tip) ImGui::SetTooltip("%s", tip);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hovered || ImGui::IsItemActive()) {
    const float a = ImGui::IsItemActive() ? 0.16f : 0.08f;
    dl->AddRectFilled(pos, ImVec2(pos.x + 30.0f, pos.y + 30.0f),
                      IM_COL32(255, 255, 255, static_cast<int>(a * 255)),
                      theme::kControlRounding);
  }
  const char g[2] = {glyph, 0};
  const ImVec2 ts = ImGui::CalcTextSize(g);
  dl->AddText(ImVec2(pos.x + (30.0f - ts.x) * 0.5f, pos.y + (30.0f - ts.y) * 0.5f),
              ImGui::ColorConvertFloat4ToU32(theme::kText), g);
  return clicked;
}

void PlayerHud::drawTopBar() {
  const ImGuiIO& io = ImGui::GetIO();
  const float w = io.DisplaySize.x;
  const float h = 62.0f;  // .topbar padding 16 + 30 content + 16

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (ImGui::Begin("##topbar", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoScrollbar)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // linear-gradient(180deg, rgba(0,0,0,.55), transparent): a 140/255
    // alpha at the top fading to nothing at the strip's bottom edge.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h),
                                IM_COL32(0, 0, 0, 140), IM_COL32(0, 0, 0, 140),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));

    const float cy = h * 0.5f;
    float x = 22.0f;  // .topbar padding-left

    // .app-title: the 22px wing mark — an orange-gradient rounded square
    // (the bitmap font has no wing glyph, so the face is drawn; the ink
    // is a small chevron suggesting the wing).
    const ImVec2 logo(x, cy - 11.0f);
    const ImU32 acc = ImGui::ColorConvertFloat4ToU32(theme::kAccent);
    const ImU32 accHi = ImGui::ColorConvertFloat4ToU32(theme::kAccentHover);
    dl->AddRectFilledMultiColor(logo, ImVec2(x + 22.0f, cy + 11.0f), acc,
                                accHi, accHi, acc);
    dl->AddTriangleFilled(ImVec2(x + 6.0f, cy - 5.0f), ImVec2(x + 6.0f, cy + 5.0f),
                          ImVec2(x + 15.0f, cy),
                          ImGui::ColorConvertFloat4ToU32(theme::kInkOnAccent));
    x += 22.0f + 10.0f;

    // "soar" wordmark (the .app-title's title).
    const ImVec2 ts = ImGui::CalcTextSize("soar");
    dl->AddText(ImVec2(x, cy - ts.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(theme::kText), "soar");
    x += ts.x + 16.0f;

    // Left pills — state, media, P2P — only when a source is loaded (the
    // prototype models the playing page).
    if (!current_uri_.empty()) {
      const bool buffering =
          cfg_.buffering && cfg_.buffering->load(std::memory_order_relaxed);
      const std::uint64_t total =
          cfg_.download_total ? cfg_.download_total->load(std::memory_order_relaxed) : 0;
      const std::uint64_t have =
          cfg_.download_bytes ? cfg_.download_bytes->load(std::memory_order_relaxed) : 0;
      const bool downloading = total > 0 && have < total;

      // State pill: ok-green "Playing" (or warn while buffering); the
      // non-playing states go neutral — the prototype's .pill.ok only
      // colors the playing row.
      const char* state = "Playing";
      ImVec4 stateFg = theme::kOk;
      bool stateTinted = true;
      // The .pill.ok's play glyph belongs to the moving state only; the
      // non-playing pills are plain text.
      bool statePlaying = true;
      switch (player_.state()) {
        case PlaybackState::Playing:
          if (buffering) {
            state = "Buffering";
            stateFg = theme::kWarn;
          }
          break;
        case PlaybackState::Paused:
          state = "Paused";
          stateFg = theme::kTextDim;
          stateTinted = false;
          statePlaying = false;
          break;
        case PlaybackState::Ended:
          state = "Ended";
          stateFg = theme::kTextMuted;
          stateTinted = false;
          statePlaying = false;
          break;
        case PlaybackState::Error:
          state = "Error";
          stateFg = theme::kDanger;
          break;
        case PlaybackState::Stopped:
          state = "Stopped";
          stateFg = theme::kTextMuted;
          stateTinted = false;
          statePlaying = false;
          break;
      }
      x = topPill(dl, x, cy, state, stateFg, stateTinted, /*play=*/statePlaying);

      // Media pill: the source's display name (the prototype's plain pill).
      const std::string name = clipEllipsis(displayName(current_uri_), 26);
      x = topPill(dl, x, cy, name.c_str(), theme::kTextDim, /*tinted=*/false,
                  /*play=*/false);

      // P2P pill (the prototype's .pill.sky) while the download is live.
      if (downloading) {
        char p2p[24];
        std::snprintf(p2p, sizeof(p2p), "P2P %llu%%",
                      static_cast<unsigned long long>(have * 100 / total));
        x = topPill(dl, x, cy, p2p, theme::kNet, /*tinted=*/true,
                    /*play=*/false);
      }
    }

    // Right: the four .toolbtn (30x30, radius 8, hover white .08): Info,
    // Recent, Help, Fullscreen. The letters are the shortcuts (the bitmap
    // font has no icon set); the tooltip names the action.
    const float bw = 30.0f;
    const float gap = 8.0f;
    float bx = w - 22.0f - (4 * bw + 3 * gap);
    if (topTool("##tb_info", 'I', "Info (I)", ImVec2(bx, cy - 15.0f))) {
      toggleOverlay(Overlay::Info);
    }
    bx += bw + gap;
    if (topTool("##tb_recent", 'R', "Recent (R)", ImVec2(bx, cy - 15.0f))) {
      toggleOverlay(Overlay::Recent);
    }
    bx += bw + gap;
    if (topTool("##tb_help", 'H', "Help (H)", ImVec2(bx, cy - 15.0f))) {
      toggleOverlay(Overlay::Help);
    }
    bx += bw + gap;
    if (topTool("##tb_full", 'F', "Fullscreen (F)", ImVec2(bx, cy - 15.0f))) {
      setFullscreen(!st_.fullscreen);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

std::string PlayerHud::clipEllipsis(const std::string& s, std::size_t max_chars) {
  if (s.size() <= max_chars) return s;
  const std::size_t keep = max_chars > 3 ? max_chars - 3 : 1;
  return s.substr(0, keep) + "...";
}

void PlayerHud::drawToast(milliseconds now) {
  if (!st_.toast.active(now)) return;
  const ImGuiIO& io = ImGui::GetIO();
  const double age = st_.toast.age(now);
  // Full opacity for the first 75%, a 25% fade-out tail (docs §4).
  const float alpha =
      static_cast<float>(age < 0.75 ? 1.0 : 1.0 - (age - 0.75) / 0.25);
  // The toast sits below the top bar and the state chip (both are
  // persistent now), so a status message never collides with either.
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 104.0f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::SetNextWindowBgAlpha(0.6f);
  if (ImGui::Begin("##toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(st_.toast.text().c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace soar::app

#endif  // SOAR_WITH_IMGUI
