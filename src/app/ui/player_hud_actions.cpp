// PlayerHud actions: transport, subtitle/translate, cast,
// Split out of player_window.cpp; whole file is ImGui-only.
#ifdef SOAR_WITH_IMGUI

#include "player_hud.h"

namespace soar::app {

void PlayerHud::togglePlayPause() {
  if (player_.state() == PlaybackState::Playing) {
    player_.pause();
  } else {
    if (player_.state() == PlaybackState::Ended) {
      player_.seek(milliseconds(0));  // replay from the top, mpv style
    }
    player_.play();
  }
}

void PlayerHud::seekTo(milliseconds target, milliseconds now) {
  const MediaInfo info = player_.mediaInfo();
  if (!info.seekable || info.duration <= milliseconds(0)) {
    st_.toast.show("Not seekable", now);
    return;
  }
  const auto clamped = std::clamp(target, milliseconds(0), info.duration);
  if (player_.seek(clamped)) {
    st_.toast.show(formatClock(clamped), now);  // mpv OSD: show the target
  } else {
    st_.toast.show("Seek failed", now);
  }
}

void PlayerHud::nudgeSeek(long long delta_ms, milliseconds now) {
  seekTo(player_.position() + milliseconds(delta_ms), now);
}

void PlayerHud::percentSeek(int percent, milliseconds now) {
  const MediaInfo info = player_.mediaInfo();
  seekTo(info.duration * percent / 100, now);
}

void PlayerHud::nudgeVolume(double delta, milliseconds now) {
  st_.last_volume = std::clamp(st_.last_volume + delta, 0.0, 1.0);
  player_.setVolume(st_.last_volume);
  st_.toast.show(
      "Volume " + std::to_string(static_cast<int>(st_.last_volume * 100)) + "%",
      now);
}

void PlayerHud::toggleMute(milliseconds now) {
  st_.last_muted = !st_.last_muted;
  player_.setMuted(st_.last_muted);
  st_.toast.show(st_.last_muted ? "Muted" : "Unmuted", now);
}

void PlayerHud::nudgeRate(int step, milliseconds now) {
  static const double kRates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
  std::size_t idx = 2;  // default 1.0x
  for (std::size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); ++i) {
    if (std::abs(kRates[i] - st_.last_rate) < 1e-9) {
      idx = i;
      break;
    }
  }
  const std::size_t count = sizeof(kRates) / sizeof(kRates[0]);
  idx = step > 0 ? std::min(idx + 1, count - 1)
                 : (idx == 0 ? count - 1 : idx - 1);
  st_.last_rate = kRates[idx];
  player_.setRate(st_.last_rate);
  char item[16];
  std::snprintf(item, sizeof(item), "%.2gx", st_.last_rate);
  st_.toast.show(std::string(item) + " speed", now);
}

const std::vector<SubtitleCandidate>& PlayerHud::remoteCandidates() {
  if (!remote_searched_ || searched_uri_ != current_uri_) {
    remote_searched_ = true;
    searched_uri_ = current_uri_;
    remote_candidates_ = external_.findCandidates(MediaSource{current_uri_, {}});
  }
  return remote_candidates_;
}

std::vector<std::pair<const TrackInfo*, const std::string*>> PlayerHud::translatableTracks(
    const MediaInfo& info) const {
  std::vector<std::pair<const TrackInfo*, const std::string*>> out;
  if (!translate_ready_) {
    return out;
  }
  // External rows first, embedded rows after: the rows that predate the
  // embedded slice keep their exact popup positions (the window's
  // coordinate-pinned drivers probe them), and the section reads in the
  // order the feature arrived.
  std::vector<const TrackInfo*> embedded;
  for (const auto& t : info.tracks) {
    if (t.type != TrackType::Subtitle) {
      continue;
    }
    const auto path = loaded_external_paths_.find(t.title);
    if (path == loaded_external_paths_.end()) {
      if (embeddedTextSubtitleCodec(t.codec) &&
          std::find(translated_ids_.begin(), translated_ids_.end(),
                    t.id) == translated_ids_.end()) {
        embedded.push_back(&t);
      }
      continue;
    }
    // ASS/SSA documents and TTML/DFXP files are not translator input
    // (the batch path speaks numbered SubRip/WebVTT cues; the translator
    // rejects both up front), so no row that can only fail.
    const SubtitleFormat source_format = subtitleFormatFromPath(path->second);
    if (source_format == SubtitleFormat::Ass ||
        source_format == SubtitleFormat::Ttml) {
      continue;
    }
    if (std::find(translated_titles_.begin(), translated_titles_.end(),
                  t.title) != translated_titles_.end()) {
      continue;
    }
    out.emplace_back(&t, &path->second);
  }
  for (const TrackInfo* t : embedded) {
    out.emplace_back(t, nullptr);
  }
  return out;
}

void PlayerHud::drawSidecarEntries(const MediaInfo& info, milliseconds now) {
  const std::vector<SubtitleCandidate> candidates =
      sidecar_.findCandidates(MediaSource{current_uri_, {}});
  const std::vector<SubtitleCandidate>& remote = remoteCandidates();
  const auto translatable = translatableTracks(info);
  if (candidates.empty() && remote.empty() && translatable.empty()) {
    ImGui::Separator();
    ImGui::TextDisabled("No sidecar subtitle files");
    return;
  }
  // A loaded external track is titled with its file name, and the
  // candidates all come from the media's own directory, so a name match
  // identifies one without the track list having to carry the path.
  std::vector<const SubtitleCandidate*> pending;
  for (const auto& c : candidates) {
    const bool loaded = std::any_of(
      info.tracks.begin(),
      info.tracks.end(),
      [&](const TrackInfo& t) {
        return t.type == TrackType::Subtitle && t.title == c.title;
      });
    if (!loaded) pending.push_back(&c);
  }
  if (pending.empty() && remote.empty() && translatable.empty()) {
    return;
  }
  // The leading separator belongs to the candidate lists (the rule the
  // menu predates [translate] with): it printed whenever either list had
  // anything to show, even when every remote row then filtered out. A
  // translate-only popup opens without it, keeping the probed positions
  // of every row above unchanged.
  if (!pending.empty() || !remote.empty()) {
    ImGui::Separator();
  }
  for (const auto* c : pending) {
    std::string item = c->title;
    if (!c->language.empty()) {
      item += " (" + c->language + ")";
    }
    const bool enter = menuRowEnter();
    if (ImGui::Selectable(item.c_str()) || enter) {
      TrackId id = -1;
      if (player_.loadExternalSubtitle(c->path, id) &&
          player_.selectTrack(TrackType::Subtitle, id)) {
        loaded_external_paths_[externalTrackTitle(c->path)] = c->path;
        st_.toast.show("Loaded " + c->title, now);
      } else {
        // A missing or cue-less file is a normal outcome, so this is a
        // toast, not an error dialog.
        st_.toast.show("Sidecar load failed", now);
      }
    }
    menuRowRing();
  }

  // Remote candidates: everything already downloaded this session is
  // filtered, mirroring the loaded-sidecar rule above.
  bool any_remote = false;
  for (const auto& c : remote) {
    const bool downloaded = std::find(downloaded_titles_.begin(),
                                      downloaded_titles_.end(), c.title) !=
                            downloaded_titles_.end();
    if (downloaded) {
      continue;
    }
    if (!any_remote) {
      ImGui::Separator();
      any_remote = true;
    }
    std::string item = "[download] " + c.title;
    if (!c.language.empty()) {
      item += " (" + c.language + ")";
    }
    const bool enter = menuRowEnter();
    if (ImGui::Selectable(item.c_str()) || enter) {
      downloadAndLoad(c, now);
    }
    menuRowRing();
  }

  // The [translate] section: one row per loaded external track that has
  // not been translated yet, then one per embedded text track. Picking an
  // external row sends the track's file through SubtitleTranslator;
  // picking an embedded row demuxes the stream out first ("内嵌轨先导出")
  // and translates that. Both load the result as a further external track.
  if (!translatable.empty()) {
    ImGui::Separator();
  }
  for (const auto& [track, path] : translatable) {
    std::string item = "[translate] " + track->title;
    if (path == nullptr && !track->language.empty() &&
        track->language != "und") {
      item += " (" + track->language + ")";
    }
    item += " -> " + target_language_;
    if (path == nullptr) {
      // Container titles repeat ("Subtitles" twice is normal); the hidden
      // suffix keeps ImGui's ids distinct without changing the label.
      item += "##tr" + std::to_string(track->id);
    }
    const bool enter = menuRowEnter();
    if (ImGui::Selectable(item.c_str()) || enter) {
      if (path != nullptr) {
        translateAndLoad(track->title, *path, now);
      } else {
        exportTranslateAndLoad(*track, now);
      }
    }
    menuRowRing();
  }
}

void PlayerHud::translateAndLoad(const std::string& title, const std::string& path,
                      milliseconds now) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    st_.toast.show("Translation failed", now);
    return;
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  std::string lower;
  lower.resize(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(path[i])));
  }
  const SubtitleFormat format =
      lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".vtt") == 0
          ? SubtitleFormat::WebVtt
          : SubtitleFormat::SubRip;
  if (translateTextAndLoad(title, text, format, now)) {
    translated_titles_.push_back(title);
  }
}

void PlayerHud::exportTranslateAndLoad(const TrackInfo& track, milliseconds now) {
  std::string srt;
  if (!player_.exportSubtitleText(track.id, srt) || srt.empty()) {
    st_.toast.show("Translation failed", now);
    return;
  }
  if (translateTextAndLoad(track.title, srt, SubtitleFormat::SubRip, now)) {
    translated_ids_.push_back(track.id);
  }
}

bool PlayerHud::translateTextAndLoad(const std::string& title, const std::string& text,
                          SubtitleFormat format, milliseconds now) {
  std::string out;
  if (!translator_.translate(text, target_language_, &out, nullptr)) {
    st_.toast.show("Translation failed", now);
    return false;
  }
  SubtitleCandidate c;
  // The stored name drops the source extension (storeExternalSubtitle
  // re-appends one from c.format) and carries the target language, so
  // the new track's menu title reads "source (Klingon)".
  std::string stem = title;
  const size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot != 0) {
    stem = stem.substr(0, dot);
  }
  c.title = stem + " (" + target_language_ + ")";
  c.language = target_language_;
  c.format = format;
  const std::string stored = storeExternalSubtitle({}, c, out);
  TrackId id = -1;
  if (stored.empty() || !player_.loadExternalSubtitle(stored, id) ||
      !player_.selectTrack(TrackType::Subtitle, id)) {
    st_.toast.show("Translation failed", now);
    return false;
  }
  loaded_external_paths_[externalTrackTitle(stored)] = stored;
  st_.toast.show("Translated " + title, now);
  return true;
}

std::string PlayerHud::externalTrackTitle(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

void PlayerHud::downloadAndLoad(const SubtitleCandidate& c, milliseconds now) {
  std::string text;
  if (!external_.fetch(c, text)) {
    st_.toast.show("Download failed", now);
    return;
  }
  const std::string path = storeExternalSubtitle({}, c, text);
  TrackId id = -1;
  if (path.empty() || !player_.loadExternalSubtitle(path, id) ||
      !player_.selectTrack(TrackType::Subtitle, id)) {
    st_.toast.show("Download failed", now);
    return;
  }
  loaded_external_paths_[externalTrackTitle(path)] = path;
  downloaded_titles_.push_back(c.title);
  st_.toast.show("Downloaded " + c.title, now);
}

void PlayerHud::cycleTrack(TrackType type, milliseconds now) {
  const MediaInfo info = player_.mediaInfo();
  std::vector<TrackId> ids;
  for (const auto& t : info.tracks) {
    if (t.type == type) ids.push_back(t.id);
  }
  if (ids.empty()) {
    st_.toast.show(
        type == TrackType::Audio ? "No audio tracks" : "No subtitle tracks",
        now);
    return;
  }
  if (type == TrackType::Audio) {
    const auto it = std::find(ids.begin(), ids.end(), info.selected_audio);
    const TrackId next =
        it == ids.end() || it + 1 == ids.end() ? ids.front() : *(it + 1);
    if (player_.selectTrack(TrackType::Audio, next)) {
      st_.toast.show("Audio track " + std::to_string(next), now);
    } else {
      st_.toast.show("Audio switch failed", now);
    }
    return;
  }
  // Subtitle cycle includes Off: off -> 1 -> ... -> off (mpv `j`).
  if (info.selected_subtitle < 0) {
    if (player_.selectTrack(TrackType::Subtitle, ids.front())) {
      st_.toast.show("Subtitle track " + std::to_string(ids.front()), now);
    } else {
      st_.toast.show("Subtitle switch failed", now);
    }
    return;
  }
  const auto it = std::find(ids.begin(), ids.end(), info.selected_subtitle);
  if (it != ids.end() && it + 1 != ids.end()) {
    if (player_.selectTrack(TrackType::Subtitle, *(it + 1))) {
      st_.toast.show("Subtitle track " + std::to_string(*(it + 1)), now);
    } else {
      st_.toast.show("Subtitle switch failed", now);
    }
  } else {
    player_.disableSubtitles();
    st_.toast.show("Subtitles off", now);
  }
}

void PlayerHud::openFilePicker() {
#ifdef _WIN32
  wchar_t picked[4096] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  SDL_SysWMinfo wmi{};
  SDL_VERSION(&wmi.version);
  if (SDL_GetWindowWMInfo(window_, &wmi)) {
    ofn.hwndOwner = wmi.info.win.window;
  }
  ofn.lpstrFile = picked;
  ofn.nMaxFile = static_cast<DWORD>(std::size(picked));
  ofn.lpstrFilter =
      L"Media files\0*.mp4;*.m4v;*.mkv;*.webm;*.avi;*.mov;*.flv;*.wmv;"
      L"*.ts;*.m2ts;*.mp3;*.m4a;*.aac;*.flac;*.wav;*.ogg;*.opus;"
      L"*.m3u8;*.mpd;*.torrent\0"
      L"All files\0*.*\0";
  ofn.lpstrTitle = L"Open media file";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&ofn)) {
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, picked, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
      std::string path(static_cast<std::size_t>(bytes), '\0');
      WideCharToMultiByte(
          CP_UTF8, 0, picked, -1, path.data(), bytes, nullptr, nullptr);
      path.resize(static_cast<std::size_t>(bytes) - 1);  // drop the nul
      openSource(path);
    }
  }
  // Cancel leaves the window exactly as it was: no toast, no state.
#else
  st_.toast.show("No file dialog in this build - drop a file to open",
                 nowMs());
#endif
}

void PlayerHud::openDirPicker() {
  dirpick_for_library_ = false;  // a plain open imports into the queue
  dirpick_files_ = false;        // ... and picks folders, not files
  if (dirpick_path_.empty()) {
    const std::vector<std::string>& dirs = recent_dirs_.entries();
    if (!dirs.empty()) {
      dirpick_path_ = dirs.front();
    } else if (const char* home = std::getenv("HOME"); home && *home) {
      dirpick_path_ = home;
    } else {
      dirpick_path_ = ".";
    }
  }
  toggleOverlay(Overlay::DirPick);
}

void PlayerHud::openDirPickerWatch() {
  openDirPicker();
  dirpick_for_library_ = true;
}

void PlayerHud::openDirPickerFiles() {
  openDirPicker();
  dirpick_files_ = true;
  dirpick_checked_.clear();
}

void PlayerHud::addCheckedFiles(milliseconds now) {
  if (recent_dirs_.add(dirpick_path_)) recent_dirs_.save();
  const bool was_empty =
      playlist_.empty() || playlist_.current() == PlaylistStore::kNone;
  const std::size_t n = dirpick_checked_.size();
  for (const std::string& f : dirpick_checked_) playlist_.add(f);
  dirpick_checked_.clear();
  st_.overlay = Overlay::Playlist;
  // A fresh queue plays its first pick immediately (importDirectory's
  // rule); on an open failure the backend's toast is the one to keep.
  if (was_empty && !openPlaylistEntry(playlist_.size() - n, now)) return;
  st_.toast.show((was_empty ? "Imported " : "Added ") + std::to_string(n) +
                     " file" + (n == 1 ? "" : "s") +
                     (was_empty ? "" : " to queue"),
                 now);
}

void PlayerHud::watchDirectory(const std::string& dir, milliseconds now) {
  if (library_ == nullptr) return;
  library_->addFolder(dir);
  library_->scan();
  library_->save();
  if (recent_dirs_.add(dir)) recent_dirs_.save();
  library_scanned_ = true;
  st_.overlay = Overlay::Library;
  st_.toast.show("Watching " + baseName(dir) + " - " +
                     std::to_string(library_->entries().size()) +
                     " file" + (library_->entries().size() == 1 ? "" : "s") +
                     " in library",
                 now);
}

void PlayerHud::openLibrary() {
  if (library_ != nullptr && !library_scanned_) {
    library_->scan();
    library_->save();
    library_scanned_ = true;
  }
  toggleOverlay(Overlay::Library);
}

void PlayerHud::openCast() { toggleOverlay(Overlay::Cast); }

void PlayerHud::beginCastSearch() {
  if (cast_discover_thread_.joinable()) {
    if (cast_searching_.load(std::memory_order_acquire)) return;  // one at a time
    cast_discover_thread_.join();  // finished: reap before respawning
  }
  {
    std::lock_guard<std::mutex> lock(cast_mu_);
    cast_found_.clear();
  }
  cast_searching_.store(true, std::memory_order_release);
  cast_discover_thread_ = std::thread([this] {
    std::vector<soar::cast::DiscoveredRenderer> found;
    std::string err;
    soar::cast::discoverRenderers(
        3000, &found, &err,
        cast_iface_.empty() ? nullptr : cast_iface_.c_str());
    std::lock_guard<std::mutex> lock(cast_mu_);
    cast_found_ = std::move(found);
    cast_searching_.store(false, std::memory_order_release);
  });
}

void PlayerHud::endCastSearch() {
  if (cast_discover_thread_.joinable()) cast_discover_thread_.join();
}

void PlayerHud::startCastTo(const std::string& location, milliseconds now) {
  if (torrent_ == nullptr || torrent_->phase() != soar::p2p::TorrentPhase::Serving) {
    st_.toast.show("The torrent source is not serving", now);
    return;
  }
  if (cast_active_ && cast_active_->phase() == soar::cast::CastSession::Phase::Casting) {
    return;  // already casting; stop first
  }
  if (cast_active_) cast_active_->stop();
  auto session = std::make_unique<soar::cast::CastSession>();
  soar::cast::CastSession::Params p;
  p.bridge = torrent_;
  p.renderer_location = location;
  p.title = source_label_;
  p.transcode = cast_transcode_;
  p.on_position = [this](std::int64_t pos, std::int64_t dur) {
    cast_pos_ms_.store(pos, std::memory_order_relaxed);
    cast_dur_ms_.store(dur, std::memory_order_relaxed);
  };
  if (session->start(p)) {
    st_.toast.show("Casting to " + session->rendererName(), now);
    cast_active_ = std::move(session);
  } else {
    st_.toast.show("Cast failed: " + session->lastError(), now);
  }
}

void PlayerHud::stopCast(milliseconds now) {
  if (!cast_active_) return;
  cast_active_->stop();
  st_.toast.show("Cast stopped", now);
}

void PlayerHud::pauseCast(milliseconds now) {
  if (!cast_active_) return;
  if (cast_active_->pause()) st_.toast.show("Cast paused", now);
  else st_.toast.show("Pause rejected: " + cast_active_->lastError(), now);
}

void PlayerHud::resumeCast(milliseconds now) {
  if (!cast_active_) return;
  if (cast_active_->resume()) st_.toast.show("Cast resumed", now);
  else st_.toast.show("Resume rejected: " + cast_active_->lastError(), now);
}

void PlayerHud::handleCastTick(milliseconds now) {
  if (cast_location_.empty() || cast_auto_done_) return;
  if (torrent_ == nullptr ||
      torrent_->phase() != soar::p2p::TorrentPhase::Serving) {
    return;  // the pre-armed request waits for a serving bridge
  }
  if (cast_location_ != "auto") {
    cast_auto_done_ = true;
    startCastTo(cast_location_, now);
    return;
  }
  if (cast_searching_.load(std::memory_order_acquire)) return;
  if (!cast_discover_started_) {
    cast_discover_started_ = true;
    beginCastSearch();
    return;
  }
  cast_auto_done_ = true;  // the burst is over either way
  std::vector<soar::cast::DiscoveredRenderer> found;
  {
    std::lock_guard<std::mutex> lock(cast_mu_);
    found = cast_found_;
  }
  if (found.empty()) {
    st_.toast.show("No DLNA renderer answered", now);
    return;
  }
  startCastTo(found[0].location, now);
}

void PlayerHud::drawCastOverlay() {
  bool open = true;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
  if (ImGui::Begin("Cast", &open, ImGuiWindowFlags_NoSavedSettings)) {
    const auto now = nowMs();
    if (torrent_ == nullptr) {
      ImGui::TextDisabled("%s",
          "Casting streams a torrent source through the local bridge.\n"
          "Open a .torrent or magnet: source first - this build casts\n"
          "torrent sources only.");
    } else {
      const std::string what = source_label_.empty()
                                   ? std::string("the torrent source")
                                   : source_label_;
      ImGui::Text("Cast '%s' to a DLNA renderer", what.c_str());
      if (torrent_->phase() != soar::p2p::TorrentPhase::Serving) {
        ImGui::TextDisabled("%s", "The torrent source is not serving yet.");
      } else if (cast_active_) {
        switch (cast_active_->phase()) {
          case soar::cast::CastSession::Phase::Discovering:
            ImGui::TextDisabled("%s", "Connecting to the renderer...");
            break;
          case soar::cast::CastSession::Phase::Casting: {
            const std::string name = cast_active_->rendererName();
            ImGui::Text("Casting to %s", name.c_str());
            const std::int64_t pos = cast_pos_ms_.load(std::memory_order_relaxed);
            const std::int64_t dur = cast_dur_ms_.load(std::memory_order_relaxed);
            if (pos >= 0) {
              std::string line = "at " + formatClock(milliseconds(pos));
              if (dur > 0) line += " / " + formatClock(milliseconds(dur));
              ImGui::TextDisabled("%s", line.c_str());
            }
            if (cast_active_->paused()) {
              if (ImGui::SmallButton("Resume")) resumeCast(now);
            } else {
              if (ImGui::SmallButton("Pause")) pauseCast(now);
            }
            if (ImGui::SmallButton("Stop casting")) stopCast(now);
            break;
          }
          case soar::cast::CastSession::Phase::Failed:
            ImGui::Text("Cast failed: %s", cast_active_->lastError().c_str());
            ImGui::TextDisabled("%s", "Pick a renderer below to retry.");
            break;
          case soar::cast::CastSession::Phase::Stopped:
            ImGui::TextDisabled("%s", "Cast stopped. Pick a renderer to start again.");
            break;
          case soar::cast::CastSession::Phase::Idle:
            break;  // between attempts; the list below is the way forward
        }
      }
      ImGui::Separator();
      if (ImGui::SmallButton("Search again")) beginCastSearch();
      ImGui::SameLine();
      ImGui::TextDisabled("%s", cast_searching_.load(std::memory_order_acquire)
                                    ? "searching..."
                                    : "SSDP M-SEARCH, 3s window");
      // Sender-side transcoding: for renderers that cannot decode the
      // source itself. Session-local, off by default — transcoding costs
      // CPU and gives up Seek.
      ImGui::Checkbox("Transcode to H.264/AAC (no seek)", &cast_transcode_);
      std::vector<soar::cast::DiscoveredRenderer> found;
      {
        std::lock_guard<std::mutex> lock(cast_mu_);
        found = cast_found_;
      }
      if (found.empty()) {
        ImGui::TextDisabled("%s", "No renderers found yet.");
      }
      for (const soar::cast::DiscoveredRenderer& d : found) {
        ImGui::PushID(d.location.c_str());
        const std::string label =
            d.friendly_name.empty() ? d.location : d.friendly_name;
        if (ImGui::Selectable(label.c_str(), false,
                              ImGuiSelectableFlags_AllowOverlap)) {
          startCastTo(d.location, now);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.location.c_str());
        ImGui::PopID();
      }
    }
  }
  ImGui::End();
  if (!open) st_.overlay = Overlay::None;
}

void PlayerHud::importDirectory(const std::string& dir, milliseconds now) {
  const std::vector<std::string> files = listMediaFiles(dir, dirpick_recursive_);
  if (recent_dirs_.add(dir)) recent_dirs_.save();
  if (files.empty()) {
    st_.toast.show("No media files in " + baseName(dir), now);
    return;
  }
  const bool was_empty = playlist_.empty() || playlist_.current() == PlaylistStore::kNone;
  for (const std::string& f : files) playlist_.add(f);
  st_.overlay = Overlay::None;
  // Playing into a fresh queue happens first; on an open failure the
  // backend's "Open failed" toast is the thing that must stay up, so the
  // success line is skipped entirely. Otherwise the import toast runs
  // second on purpose: which fact matters more ("5 files imported") is
  // the import, not the "Playlist i/n" position line beneath it.
  if (was_empty && !openPlaylistEntry(playlist_.size() - files.size(), now)) return;
  st_.toast.show((was_empty ? "Imported " : "Added ") +
                     std::to_string(files.size()) + " file" +
                     (files.size() == 1 ? "" : "s") +
                     (was_empty ? "" : " to queue"),
                 now);
}

void PlayerHud::openSource(const std::string& uri) {
  if (player_.open(soar::MediaSource{uri, cfg_.cache_dir, cfg_.hwdec})) {
    recordOpen(uri);
    playlist_.add(uri);
    playlist_.setCurrent(playlist_.size() - 1);
    st_.loop_a_set = false;  // a fresh source starts without a pending A
    player_.play();
    // Resume first: when the jump happens, "Resumed at m:ss" replaces
    // the plain "Opened" toast (the jump is the story).
    if (!applyResume(uri, nowMs())) {
      st_.toast.show("Opened " + displayName(uri), nowMs());
    }
  } else {
    st_.toast.show("Open failed: " + player_.lastError(), nowMs());
  }
}

bool PlayerHud::openPlaylistEntry(std::size_t index, milliseconds now) {
  const std::vector<std::string> entries = playlist_.entries();
  if (index >= entries.size()) return false;
  if (!player_.open(soar::MediaSource{entries[index], cfg_.cache_dir, cfg_.hwdec})) {
    st_.toast.show("Open failed: " + player_.lastError(), now);
    return false;
  }
  recordOpen(entries[index]);
  playlist_.setCurrent(index);
  st_.loop_a_set = false;
  player_.play();
  if (!applyResume(entries[index], now)) {
    st_.toast.show("Playlist " + std::to_string(index + 1) + "/" +
                       std::to_string(entries.size()) + ": " +
                       displayName(entries[index]),
                   now);
  }
  return true;
}

void PlayerHud::playlistStep(bool backward, milliseconds now) {
  if (playlist_.empty()) {
    st_.toast.show("Playlist empty", now);
    return;
  }
  const std::size_t index = backward ? playlist_.prev() : playlist_.next();
  if (index == PlaylistStore::kNone) {
    st_.toast.show(backward ? "Start of playlist" : "End of playlist", now);
    return;
  }
  openPlaylistEntry(index, now);
}

void PlayerHud::maybeAdvancePlaylist(milliseconds now) {
  if (player_.state() != PlaybackState::Ended) {
    st_.playlist_end_handled = false;
    return;
  }
  if (st_.playlist_end_handled) return;
  st_.playlist_end_handled = true;
  const std::size_t index = playlist_.advance();
  if (index != PlaylistStore::kNone) openPlaylistEntry(index, now);
}

void PlayerHud::setFullscreen(bool on) {
  st_.fullscreen = on;
  SDL_SetWindowFullscreen(window_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void PlayerHud::toggleOverlay(Overlay which) {
  st_.overlay = st_.overlay == which ? Overlay::None : which;
}

void PlayerHud::cycleLoopAB(milliseconds now) {
  std::chrono::milliseconds a{0}, b{0};
  if (player_.loopAB(a, b)) {
    if (player_.clearLoopAB()) {
      st_.loop_a_set = false;
      st_.toast.show("A-B loop cleared", now);
    }
    return;
  }
  if (!st_.loop_a_set) {
    st_.loop_a_set = true;
    st_.loop_a_ms = player_.position();
    st_.toast.show(
        std::string("Loop point A set at ") + formatClock(st_.loop_a_ms), now);
    return;
  }
  st_.loop_a_set = false;
  if (player_.setLoopAB(st_.loop_a_ms, player_.position())) {
    st_.toast.show(
        std::string("A-B loop ") + formatClock(st_.loop_a_ms) + " - " +
            formatClock(player_.position()),
        now);
  } else {
    st_.toast.show(player_.lastError(), now);
  }
}

void PlayerHud::nudgeSubtitleOffset(int delta_ms, milliseconds now) {
  st_.subtitle_offset_ms += delta_ms;
  if (cfg_.subtitle_offset_ms) {
    cfg_.subtitle_offset_ms->store(st_.subtitle_offset_ms, std::memory_order_relaxed);
  }
  char text[32];
  std::snprintf(text, sizeof(text), "Sub offset %+d ms", st_.subtitle_offset_ms);
  st_.toast.show(text, now);
}

void PlayerHud::toggleSubtitleVisibility(milliseconds now) {
  st_.subtitle_visible = !st_.subtitle_visible;
  if (cfg_.subtitle_visible) {
    cfg_.subtitle_visible->store(st_.subtitle_visible, std::memory_order_relaxed);
  }
  st_.toast.show(st_.subtitle_visible ? "Subtitles on" : "Subtitles off", now);
}

void PlayerHud::takeScreenshot(milliseconds now) {
#ifdef SOAR_WITH_FFMPEG
  if (cfg_.ffmpeg) {
    if (cfg_.ffmpeg->saveScreenshot("screenshot.png")) {
      st_.toast.show("Saved screenshot.png", now);
    } else {
      st_.toast.show("Screenshot failed: " + player_.lastError(), now);
    }
    return;
  }
#endif
  st_.toast.show("Screenshot needs the FFmpeg backend", now);
}

}  // namespace soar::app

#endif  // SOAR_WITH_IMGUI
