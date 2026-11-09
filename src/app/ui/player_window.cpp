// Windowed player UI implementation (docs/ui-design.md). One renderer, two
// layers: the YUV video texture (letterboxed) is copied first, then ImGui
// draws the OSC/overlays on top through imgui_impl_sdlrenderer2.
//
// Input routing: every SDL event goes to ImGui first (its widgets own the
// pointer over their rects); app-level shortcuts then act unless a UI
// popup owns the keyboard, and pointer gestures (click / double-click /
// wheel) act only over the video area (io.WantCaptureMouse == false).
//
// Layout note: the OSC is two rows (full-width seek bar above the button
// row), the shape both IINA's floating controller and vidstack's default
// layout converged on — it gives the seek slider a full-width grab area
// and the buttons their own density (docs/ui-design.md §1.4, §2).

#include "player_window.h"

#include "soar/core/metadata_scraper.h"
#include "soar/core/player.h"
#include "soar/core/subtitle_provider.h"
#include "startup_report.h"
#include "ui/dir_scan.h"
#include "ui/library.h"
#include "ui/shortcuts.h"
#include "ui/thumbnail_service.h"
#include "ui_tokens.h"
#include "ui_state.h"
#include "uri_decode.h"
#include "player_hud.h"

#include <fmt/format.h>

#include <cmath>
#include <functional>

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>

#  ifdef _WIN32
// The open-file entry (BUGS.md #3): SDL_syswm.h brings windows.h for the
// HWND, commdlg.h declares GetOpenFileNameW (comdlg32, linked in
// CMakeLists.txt for both front ends).
#    include <SDL_syswm.h>
#    include <commdlg.h>
#  endif

#  include "torrent_stream.h"

#  ifdef SOAR_WITH_IMGUI
#    include "cast_session.h"   // DLNA control point (docs/mvp.md §3)
#    include "renderer.h"       // DiscoveredRenderer for the cast overlay
#  endif

#  include <algorithm>
#  include <atomic>
#  include <cctype>
#  include <chrono>
#  include <cmath>
#  include <cstdio>
#  include <cstdlib>
#  include <ctime>
#  include <filesystem>
#  include <fstream>
#  include <iterator>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <string>
#  include <thread>
#  include <utility>
#  include <vector>

#  ifdef SOAR_WITH_FFMPEG
#    include "soar/core/ffmpeg_backend.h"
#  endif

#  ifdef SOAR_WITH_IMGUI
#    include "imgui.h"
#    include "imgui_internal.h"  // ClearActiveID: the Tab escape from the search box
#    include "backends/imgui_impl_sdl2.h"
#    include "backends/imgui_impl_sdlrenderer2.h"
#  endif

namespace soar::app {
namespace {

#ifdef SOAR_WITH_SDL2
// The keycode table in ui/shortcuts.h mirrors SDL_Keycode by value — the
// keydown handler below hands SDL event syms straight to ShortcutMap (the
// pure-logic library stays SDL-free; SOAR_ENABLE_SDL2 is optional). Drift
// between the two tables fails here instead of silently rebinding keys.
static_assert(static_cast<int>(SDLK_TAB) == static_cast<int>(kKeyTab));
static_assert(static_cast<int>(SDLK_BACKSPACE) == static_cast<int>(kKeyBackspace));
static_assert(static_cast<int>(SDLK_RETURN) == static_cast<int>(kKeyReturn));
static_assert(static_cast<int>(SDLK_ESCAPE) == static_cast<int>(kKeyEscape));
static_assert(static_cast<int>(SDLK_SPACE) == static_cast<int>(kKeySpace));
static_assert(static_cast<int>(SDLK_QUOTE) == static_cast<int>(kKeyApostrophe));
static_assert(static_cast<int>(SDLK_COMMA) == static_cast<int>(kKeyComma));
static_assert(static_cast<int>(SDLK_MINUS) == static_cast<int>(kKeyMinus));
static_assert(static_cast<int>(SDLK_PERIOD) == static_cast<int>(kKeyPeriod));
static_assert(static_cast<int>(SDLK_SLASH) == static_cast<int>(kKeySlash));
static_assert(static_cast<int>(SDLK_0) == static_cast<int>(kKey0));
static_assert(static_cast<int>(SDLK_9) == static_cast<int>(kKey9));
static_assert(static_cast<int>(SDLK_SEMICOLON) == static_cast<int>(kKeySemicolon));
static_assert(static_cast<int>(SDLK_EQUALS) == static_cast<int>(kKeyEquals));
static_assert(static_cast<int>(SDLK_LEFTBRACKET) == static_cast<int>(kKeyLeftBracket));
static_assert(static_cast<int>(SDLK_BACKSLASH) == static_cast<int>(kKeyBackslash));
static_assert(static_cast<int>(SDLK_RIGHTBRACKET) == static_cast<int>(kKeyRightBracket));
static_assert(static_cast<int>(SDLK_BACKQUOTE) == static_cast<int>(kKeyGrave));
static_assert(static_cast<int>(SDLK_a) == static_cast<int>(kKeyA));
static_assert(static_cast<int>(SDLK_b) == static_cast<int>(kKeyB));
static_assert(static_cast<int>(SDLK_c) == static_cast<int>(kKeyC));
static_assert(static_cast<int>(SDLK_d) == static_cast<int>(kKeyD));
static_assert(static_cast<int>(SDLK_e) == static_cast<int>(kKeyE));
static_assert(static_cast<int>(SDLK_f) == static_cast<int>(kKeyF));
static_assert(static_cast<int>(SDLK_g) == static_cast<int>(kKeyG));
static_assert(static_cast<int>(SDLK_h) == static_cast<int>(kKeyH));
static_assert(static_cast<int>(SDLK_i) == static_cast<int>(kKeyI));
static_assert(static_cast<int>(SDLK_j) == static_cast<int>(kKeyJ));
static_assert(static_cast<int>(SDLK_k) == static_cast<int>(kKeyK));
static_assert(static_cast<int>(SDLK_l) == static_cast<int>(kKeyL));
static_assert(static_cast<int>(SDLK_m) == static_cast<int>(kKeyM));
static_assert(static_cast<int>(SDLK_n) == static_cast<int>(kKeyN));
static_assert(static_cast<int>(SDLK_o) == static_cast<int>(kKeyO));
static_assert(static_cast<int>(SDLK_p) == static_cast<int>(kKeyP));
static_assert(static_cast<int>(SDLK_q) == static_cast<int>(kKeyQ));
static_assert(static_cast<int>(SDLK_r) == static_cast<int>(kKeyR));
static_assert(static_cast<int>(SDLK_s) == static_cast<int>(kKeyS));
static_assert(static_cast<int>(SDLK_t) == static_cast<int>(kKeyT));
static_assert(static_cast<int>(SDLK_u) == static_cast<int>(kKeyU));
static_assert(static_cast<int>(SDLK_v) == static_cast<int>(kKeyV));
static_assert(static_cast<int>(SDLK_w) == static_cast<int>(kKeyW));
static_assert(static_cast<int>(SDLK_x) == static_cast<int>(kKeyX));
static_assert(static_cast<int>(SDLK_y) == static_cast<int>(kKeyY));
static_assert(static_cast<int>(SDLK_z) == static_cast<int>(kKeyZ));
static_assert(static_cast<int>(SDLK_DELETE) == static_cast<int>(kKeyDelete));
static_assert(static_cast<int>(SDLK_CAPSLOCK) == static_cast<int>(kKeyCapsLock));
static_assert(static_cast<int>(SDLK_F1) == static_cast<int>(kKeyF1));
static_assert(static_cast<int>(SDLK_F12) == static_cast<int>(kKeyF12));
static_assert(static_cast<int>(SDLK_PRINTSCREEN) == static_cast<int>(kKeyPrintScreen));
static_assert(static_cast<int>(SDLK_SCROLLLOCK) == static_cast<int>(kKeyScrollLock));
static_assert(static_cast<int>(SDLK_PAUSE) == static_cast<int>(kKeyPause));
static_assert(static_cast<int>(SDLK_INSERT) == static_cast<int>(kKeyInsert));
static_assert(static_cast<int>(SDLK_HOME) == static_cast<int>(kKeyHome));
static_assert(static_cast<int>(SDLK_PAGEUP) == static_cast<int>(kKeyPageUp));
static_assert(static_cast<int>(SDLK_END) == static_cast<int>(kKeyEnd));
static_assert(static_cast<int>(SDLK_PAGEDOWN) == static_cast<int>(kKeyPageDown));
static_assert(static_cast<int>(SDLK_RIGHT) == static_cast<int>(kKeyRight));
static_assert(static_cast<int>(SDLK_LEFT) == static_cast<int>(kKeyLeft));
static_assert(static_cast<int>(SDLK_DOWN) == static_cast<int>(kKeyDown));
static_assert(static_cast<int>(SDLK_UP) == static_cast<int>(kKeyUp));
static_assert(static_cast<int>(SDLK_KP_DIVIDE) == static_cast<int>(kKeyNumDivide));
static_assert(static_cast<int>(SDLK_KP_MULTIPLY) == static_cast<int>(kKeyNumMultiply));
static_assert(static_cast<int>(SDLK_KP_MINUS) == static_cast<int>(kKeyNumMinus));
static_assert(static_cast<int>(SDLK_KP_PLUS) == static_cast<int>(kKeyNumPlus));
static_assert(static_cast<int>(SDLK_KP_ENTER) == static_cast<int>(kKeyNumEnter));
static_assert(static_cast<int>(SDLK_KP_1) == static_cast<int>(kKeyNum1));
static_assert(static_cast<int>(SDLK_KP_9) == static_cast<int>(kKeyNum9));
static_assert(static_cast<int>(SDLK_KP_0) == static_cast<int>(kKeyNum0));
static_assert(static_cast<int>(SDLK_KP_PERIOD) == static_cast<int>(kKeyNumPeriod));
static_assert(static_cast<int>(SDLK_MENU) == static_cast<int>(kKeyMenu));
static_assert(static_cast<int>(SDLK_KP_EQUALS) == static_cast<int>(kKeyNumEquals));
#endif

using milliseconds = std::chrono::milliseconds;

// nowMs/stateName/trackTypeName, the theme/pref helpers, the subtitle
// font loading and PlayerHud moved to player_hud.h; its core members
// (construction, event routing, frame driving) stay below.
SDL_Rect letterboxRect(SDL_Renderer* renderer, SDL_Texture* texture) {
  int rw = 0;
  int rh = 0;
  int tw = 0;
  int th = 0;
  SDL_GetRendererOutputSize(renderer, &rw, &rh);
  SDL_QueryTexture(texture, nullptr, nullptr, &tw, &th);
  if (rw <= 0 || rh <= 0 || tw <= 0 || th <= 0) return SDL_Rect{0, 0, rw, rh};
  const double scale = std::min(static_cast<double>(rw) / tw,
                                static_cast<double>(rh) / th);
  SDL_Rect dst;
  dst.w = static_cast<int>(tw * scale);
  dst.h = static_cast<int>(th * scale);
  dst.x = (rw - dst.w) / 2;
  dst.y = (rh - dst.h) / 2;
  return dst;
}

// Brings `texture` in sync with `frame` (recreating on resolution change)
// and blits it letterboxed. When `ass` is non-null and libass is compiled
// in, its canvas is rendered at `ass_pts_ms` — sized to the letterbox rect
// (the idempotent setFrameSize makes the per-frame call cheap) — and
// blended over the video: the style-faithful ASS path (docs/mvp.md §6).
// `ass_pts_ms` already carries the subtitle sync offset and `subs_visible`
// is the V-toggle state, so canvas subtitles obey both exactly like the
// plain-text ones. The overlay texture lives in *ass_texture (recreated on
// size change, re-uploaded only when the canvas reports changed). Returns
// false when no frame was ready.
bool presentVideoFrame(SDL_Renderer* renderer, SDL_Texture** texture,
                       const void* frame_ptr, SDL_Texture** ass_texture,
                       soar::AssRenderer* ass, std::int64_t ass_pts_ms,
                       bool subs_visible) {
  if (frame_ptr == nullptr) return false;
#ifdef SOAR_WITH_FFMPEG
  const auto& frame =
      *static_cast<const soar::DecodedVideoFrame*>(frame_ptr);
  if (frame.width <= 0 || frame.height <= 0) return false;
  int tw = 0;
  int th = 0;
  if (!*texture || SDL_QueryTexture(*texture, nullptr, nullptr, &tw, &th) != 0 ||
      tw != frame.width || th != frame.height) {
    if (*texture) SDL_DestroyTexture(*texture);
    *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, frame.width,
                                 frame.height);
    if (!*texture) {
      fmt::print(stderr, "SDL_CreateTexture failed: {}\n", SDL_GetError());
      return false;
    }
  }
  if (SDL_UpdateYUVTexture(*texture, nullptr, frame.y.data(),
                           static_cast<int>(frame.stride_y), frame.u.data(),
                           static_cast<int>(frame.stride_u), frame.v.data(),
                           static_cast<int>(frame.stride_v)) != 0) {
    return false;
  }
  const SDL_Rect dst = letterboxRect(renderer, *texture);
  SDL_RenderCopy(renderer, *texture, nullptr, &dst);

  if (subs_visible && ass != nullptr && ass->available() &&
      ass_texture != nullptr) {
    ass->setFrameSize(dst.w, dst.h);
    soar::AssFrame f;
    if (ass->renderAt(ass_pts_ms, &f) && !f.rgba.empty()) {
      int aw = 0;
      int ah = 0;
      if (!*ass_texture ||
          SDL_QueryTexture(*ass_texture, nullptr, nullptr, &aw, &ah) != 0 ||
          aw != f.width || ah != f.height) {
        if (*ass_texture) SDL_DestroyTexture(*ass_texture);
        // The canvas is RGBA byte order: R in the first byte, which on a
        // little-endian renderer is SDL's ABGR8888 packing.
        *ass_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         f.width, f.height);
        if (*ass_texture) {
          SDL_SetTextureBlendMode(*ass_texture, SDL_BLENDMODE_BLEND);
        }
      }
      if (*ass_texture && f.changed) {
        SDL_UpdateTexture(*ass_texture, nullptr, f.rgba.data(),
                          static_cast<int>(f.width) * 4);
      }
      if (*ass_texture) {
        // changed=false still copies: the texture holds the previous
        // render's pixels, which are the correct overlay.
        SDL_RenderCopy(renderer, *ass_texture, nullptr, &dst);
      }
    }
    // Nothing visible at this pts: nothing is copied and the video shows
    // through — the overlay texture is simply left stale.
  }
  return true;
#else
  (void)renderer;
  (void)texture;
  (void)ass_texture;
  (void)ass;
  (void)ass_pts_ms;
  (void)subs_visible;
  return false;
#endif
}
}  // namespace

// PlayerHud core members. Out-of-class member definitions
// must live in the class namespace, not the anonymous one.
#ifdef SOAR_WITH_IMGUI

PlayerHud::PlayerHud(soar::Player& player, const WindowUiConfig& cfg, SDL_Window* window,
          SDL_Renderer* renderer)
    : player_(player), cfg_(cfg), window_(window), renderer_(renderer),
      recent_(cfg.recent_path),
      current_uri_(cfg.initial_uri), source_label_(cfg.source_label),
      torrent_(cfg.torrent), torrent_pending_(cfg.torrent != nullptr),
      cast_location_(cfg.cast_location),
      cast_transcode_(cfg.cast_transcode),
      cast_iface_(cfg.cast_iface),
      open_dialog_on_start_(cfg.open_dialog_on_start),
      recent_dirs_(defaultRecentDirsPath(), 8),
      library_(cfg.library),
      subtitle_fonts_(loadSubtitleFonts()) {
  // Every toast lands on the CLI event trail (main.cpp prints the
  // player-side events; the UI side mirrors its own here) so the
  // walkthrough tests assert OSD feedback from the teed stderr alone.
  st_.toast.mirror = [](const std::string& text) {
    fmt::print(stderr, "event: toast={}\n", text);
  };
  // The MRU cap is a persisted setting, not a per-run constant: apply it
  // before load() so an over-long recent.txt is truncated by the same cap
  // the settings page shows (a saved 20 would otherwise reload as the 15
  // default, silently dropping the extra rows the file still holds).
  st_.recent_cap = loadRecentCap();
  recent_.setMaxEntries(static_cast<std::size_t>(st_.recent_cap));
  recent_.load();
  recent_dirs_.load();
  // Keyboard shortcuts (docs/ui-design.md §3.2): the config lives next
  // to recent.txt/library.txt; --shortcuts= moves it, --no-shortcuts is
  // the kill switch and wins over a `shortcuts = on` line in the file.
  shortcuts_.load(cfg.shortcuts_path.empty() ? defaultShortcutsPath()
                                             : cfg.shortcuts_path);
  shortcuts_.setEnabled(cfg.shortcuts_enabled);
  if (library_ != nullptr) library_->load();
  if (cfg.torrent == nullptr) {
    recordOpen(cfg.initial_uri);
    // Playlist seeding (docs/mvp.md §2): the CLI-opened source plays
    // first; further positionals queue behind it (mpv's multi-argument
    // semantics).
    if (!cfg.initial_uri.empty()) playlist_.add(cfg.initial_uri);
    // Resume bookkeeping (docs/mvp.md §3): the source is already open and
    // playing (main.cpp opened it before the window exists), so a saved
    // position applies now — a few ms of from-zero playback precede the
    // jump, the honest cost of the synchronous open contract.
    applyResume(cfg.initial_uri, nowMs());
  } else {
    // Async P2P source (P4b-4): the magnet/.torrent URI is not openable
    // by the player until the metadata lands — handlePendingTorrent
    // records and queues the bridge URL instead. The raw URI never joins
    // Recent or the playlist, so nothing unopenable lingers in either.
    current_uri_.clear();
  }
  for (const std::string& uri : cfg.queued_uris) playlist_.add(uri);
  // Remote subtitle download (docs/mvp.md §6): endpoint and key arrive
  // from the environment — the core embeds no endpoint and no credential,
  // and with nothing configured the provider never touches the network.
  soar::HttpSubtitleConfig sub;
  if (const char* ep = std::getenv("SOAR_SUBTITLE_ENDPOINT")) {
    sub.endpoint = ep;
  }
  if (const char* key = std::getenv("SOAR_SUBTITLE_API_KEY")) {
    sub.api_key = key;
  }
  if (const char* t = std::getenv("SOAR_SUBTITLE_TIMEOUT_MS")) {
    const long ms = std::atol(t);
    if (ms > 0) sub.timeout = std::chrono::milliseconds(ms);
  }
  subtitle_env_cfg_ = sub;
  configureHttpSubtitles(st_.subtitle_autodl);
  // Subtitle text translation (docs/mvp.md §6), same env-injection
  // contract as the download provider above: no configured endpoint and
  // model means the menu never offers a [translate] row and nothing ever
  // dials out. The target language is also env-configured — the window
  // has no language picker of its own (assumption: one language per
  // installation is enough for the entry slice).
  soar::SubtitleTranslateConfig tr_cfg;
  if (const char* ep = std::getenv("SOAR_TRANSLATE_ENDPOINT")) {
    tr_cfg.endpoint = ep;
  }
  if (const char* key = std::getenv("SOAR_TRANSLATE_API_KEY")) {
    tr_cfg.api_key = key;
  }
  if (const char* m = std::getenv("SOAR_TRANSLATE_MODEL")) {
    tr_cfg.model = m;
  }
  if (const char* t = std::getenv("SOAR_TRANSLATE_TIMEOUT_MS")) {
    const long ms = std::atol(t);
    if (ms > 0) tr_cfg.timeout = std::chrono::milliseconds(ms);
  }
  if (const char* l = std::getenv("SOAR_TRANSLATE_LANGUAGE")) {
    if (*l != '\0') target_language_ = l;
  }
  translate_ready_ = !tr_cfg.endpoint.empty() && !tr_cfg.model.empty();
  translator_.configure(std::move(tr_cfg));
  // Library network scraping (docs/mvp.md §3), same env-injection
  // contract as the two subtitle providers: endpoint and key arrive from
  // the environment, the core embeds neither, and with nothing configured
  // the library never touches the network. The window holds the scraper;
  // the library holds a non-owning pointer to the adapter below.
  soar::HttpMetadataConfig meta;
  if (const char* ep = std::getenv("SOAR_META_ENDPOINT")) {
    meta.endpoint = ep;
  }
  if (const char* key = std::getenv("SOAR_META_API_KEY")) {
    meta.api_key = key;
  }
  if (const char* t = std::getenv("SOAR_META_TIMEOUT_MS")) {
    const long ms = std::atol(t);
    if (ms > 0) meta.timeout = std::chrono::milliseconds(ms);
  }
  metadata_.configure(std::move(meta));
  meta_ready_ = metadata_.online();
  if (library_ != nullptr) library_->setMetadataSource(&metadata_source_);
#ifdef SOAR_WITH_FFMPEG
  // Seek-hover thumbnail (docs/ui-design.md §1.6-5): the worker owns the
  // reopen-per-call grab (grabVideoFrame), so the UI only queues hover
  // positions and polls the LRU — a decode never runs on the UI thread.
  // The grab runs on the worker thread; the backend contract guarantees
  // it is safe alongside playback. Null backend → no preview, silently.
  if (cfg_.ffmpeg != nullptr && renderer_ != nullptr) {
    thumb_service_ = std::make_unique<ThumbnailService>(
        [this](std::chrono::milliseconds pos, ThumbnailFrame& f) {
          DecodedVideoFrame d;
          if (!cfg_.ffmpeg->grabVideoFrame(pos, d)) return false;
          f.width = d.width;
          f.height = d.height;
          f.stride_y = d.stride_y;
          f.stride_u = d.stride_u;
          f.stride_v = d.stride_v;
          f.y = std::move(d.y);
          f.u = std::move(d.u);
          f.v = std::move(d.v);
          return true;
        });
    // The initial source was recorded before the service existed (ctor
    // order); tag it now so the first hover's frames belong to it.
    thumb_source_ = current_uri_;
  }
#endif
}

PlayerHud::~PlayerHud() {
  // The seek-thumb texture dies with the window (the service's worker is
  // joined at member teardown; it may finish one in-flight grab first).
  if (thumb_texture_ != nullptr) {
    SDL_DestroyTexture(thumb_texture_);
    thumb_texture_ = nullptr;
  }
  // The cast session dies first: its stop() restores the bridge to
  // 127.0.0.1 and joins the poller, and the discovery thread (a bounded
  // 3s window) must be joined before the members go away.
  endCastSearch();
  if (cast_active_) cast_active_->stop();
  if (library_ == nullptr) return;
  if (!current_uri_.empty() && !isTorrentBridge(current_uri_)) {
    switch (player_.state()) {
      case PlaybackState::Playing:
      case PlaybackState::Paused:
        library_->recordProgress(current_uri_, player_.position().count(),
                                 player_.mediaInfo().duration.count());
        break;
      case PlaybackState::Ended:
        library_->clearPosition(current_uri_);
        break;
      default:
        break;  // Stopped/Error: nothing honest to write
    }
  }
  library_->save();
}

void PlayerHud::drawFocusRing() {
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  ImGui::GetWindowDrawList()->AddRect(
      ImVec2(lo.x - kFocusRingExpand, lo.y - kFocusRingExpand),
      ImVec2(hi.x + kFocusRingExpand, hi.y + kFocusRingExpand),
      ImGui::GetColorU32(theme::kAccent), 0.0f, 0, kFocusRingWidth);
}

bool PlayerHud::menuRowEnter() {
  return ui_focus_.takesActivation(UiFocus::Face::MenuItems, menu_rows_);
}

void PlayerHud::menuRowRing() {
  if (ui_focus_.holds(UiFocus::Face::MenuItems, menu_rows_)) drawFocusRing();
  ++menu_rows_;
}

void PlayerHud::focusMenuKey(SDL_Keycode key, bool shift) {
  if (menu_count_ <= 0) return;  // popup is drawing its first frame
  if (key == SDLK_TAB) {
    ui_focus_.advance(UiFocus::Face::MenuItems, shift ? -1 : 1, menu_count_);
  } else if (key == SDLK_DOWN) {
    ui_focus_.advance(UiFocus::Face::MenuItems, 1, menu_count_);
  } else if (key == SDLK_UP) {
    ui_focus_.advance(UiFocus::Face::MenuItems, -1, menu_count_);
  } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
    ui_focus_.armActivate();
  }
}

bool PlayerHud::focusOverlayKey(SDL_Keycode key, bool shift) {
  UiFocus::Face face = UiFocus::Face::None;
  if (st_.overlay == Overlay::Playlist) face = UiFocus::Face::PlaylistRows;
  else if (st_.overlay == Overlay::Library) face = UiFocus::Face::LibraryRows;
  else return false;
  if (key == SDLK_TAB) {
    const int count =
        face == UiFocus::Face::PlaylistRows ? playlist_rows_ : library_rows_;
    ui_focus_.advance(face, shift ? -1 : 1, count);
    // The caret (if any) leaves the search box: the ring is moving
    // onto the rows, and they must be able to take the next key.
    if (face == UiFocus::Face::LibraryRows) blur_text_next_ = true;
    return ui_focus_.active();  // a zero-row face leaves the key alone
  }
  if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
    // While the filter box has the caret it also owns Enter (typing a
    // needle must not fire a row); Tab above is the way out.
    if (ImGui::GetIO().WantCaptureKeyboard) return false;
    ui_focus_.armActivate();
    return true;
  }
  return false;
}

void PlayerHud::handleEvent(const SDL_Event& e, bool* quit) {
  const auto now = nowMs();
  switch (e.type) {
    case SDL_QUIT:
      *quit = true;
      return;
    case SDL_KEYDOWN: {
      if (e.key.repeat != 0) return;  // held key: no repeat actions in v0.1
      // SDL reports uppercase letter syms under Shift/CapsLock and defines
      // no SDLK_Q-style constants — normalize so one case label covers both.
      SDL_Keycode key = e.key.keysym.sym;
      if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
      // An open combo popup owns the keyboard (arrows drive the app-drawn
      // menu focus, docs §5.2); app shortcuts must not fire through it.
      if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                           ImGuiPopupFlags_AnyPopupLevel)) {
        focusMenuKey(key, (e.key.keysym.mod & KMOD_SHIFT) != 0);
        return;
      }
      // Tab/Enter are bound to nothing in §3.2: they move and fire the
      // app-drawn overlay focus (V7b, docs §5.2).
      if (focusOverlayKey(key, (e.key.keysym.mod & KMOD_SHIFT) != 0)) return;
      st_.hud.noteActivity(now);
      // A focused text field owns the keyboard (the library search box):
      // typing a "v" must not toggle subtitles. WantCaptureKeyboard is
      // ImGui's own verdict for the frame, so the shortcut map simply
      // stands aside; noteActivity above still counts the keypress as
      // the user being present, so the OSC stays awake while typing.
      // (Tab already slipped past via focusOverlayKey — it is how focus
      // escapes the box onto the rows.)
      if (ImGui::GetIO().WantCaptureKeyboard) return;
      const ShortcutKey pressed{
          static_cast<std::uint32_t>(key),
          (e.key.keysym.mod & KMOD_SHIFT) != 0,
          (e.key.keysym.mod & KMOD_CTRL) != 0,
          (e.key.keysym.mod & KMOD_ALT) != 0};
      // The rebindable map (docs/ui-design.md §3.2) decides which action
      // a keydown fires; its defaults reproduce the switch that used to
      // live here. Plain bindings ignore held modifiers (shift+q still
      // quits); a flagged binding (shift+left, ctrl+o) outranks the
      // plain one for the same key.
      ShortcutAction action = ShortcutAction::None;
      const int binding = shortcuts_.bindingFor(pressed, &action);
      switch (action) {
        case ShortcutAction::None:
          return;
        case ShortcutAction::Escape:
          // Fullscreen → window first; otherwise quit (docs §3.2, which
          // also keeps the CLI's Escape-exit contract the tests pin). The
          // Settings panel's "ESC exits fullscreen" switch gates the
          // leave-fullscreen half: off means Escape quits even in
          // fullscreen (the switch is the only caller of that state).
          if (st_.fullscreen && st_.esc_exits_fullscreen) {
            setFullscreen(false);
          } else {
            *quit = true;
          }
          return;
        case ShortcutAction::Quit:
          *quit = true;
          return;
        case ShortcutAction::PlayPause:
          togglePlayPause();
          return;
        case ShortcutAction::SeekBack5:
          nudgeSeek(-5000, now);
          return;
        case ShortcutAction::SeekFwd5:
          nudgeSeek(5000, now);
          return;
        case ShortcutAction::SeekBack1:
          nudgeSeek(-1000, now);
          return;
        case ShortcutAction::SeekFwd1:
          nudgeSeek(1000, now);
          return;
        case ShortcutAction::SeekBack60:
          nudgeSeek(-60000, now);
          return;
        case ShortcutAction::SeekFwd60:
          nudgeSeek(60000, now);
          return;
        case ShortcutAction::SeekStart:
          seekTo(milliseconds(0), now);
          return;
        case ShortcutAction::PercentSeek:
          // Jump in 10% steps: the Nth binding of the action goes to
          // N*10% (the shipped 0-9 table gives the digit semantics; a
          // rebound set maps ordinally; the cap bounds custom lists).
          if (binding >= 0) {
            percentSeek(std::min(binding * 10, 100), now);
          }
          return;
        case ShortcutAction::VolumeUp:
          nudgeVolume(+0.05, now);
          return;
        case ShortcutAction::VolumeDown:
          nudgeVolume(-0.05, now);
          return;
        case ShortcutAction::Mute:
          toggleMute(now);
          return;
        case ShortcutAction::RateDown:
          nudgeRate(-1, now);
          return;
        case ShortcutAction::RateUp:
          nudgeRate(+1, now);
          return;
        case ShortcutAction::Fullscreen:
          setFullscreen(!st_.fullscreen);
          return;
        case ShortcutAction::LoopAB:
          cycleLoopAB(now);
          return;
        case ShortcutAction::CycleAudio:
          cycleTrack(TrackType::Audio, now);
          return;
        case ShortcutAction::CycleSubtitle:
          cycleTrack(TrackType::Subtitle, now);
          return;
        // Subtitle keys (v0.2), back on since the X11 drive scripts
        // pin every coordinate they used to disturb (subsdrive presses
        // [ ] v with the overlay measured). `s` follows mpv and takes a
        // screenshot instead of toggling visibility: V does that now.
        case ShortcutAction::SubtitleOffsetBack:
          nudgeSubtitleOffset(-500, now);
          return;
        case ShortcutAction::SubtitleOffsetFwd:
          nudgeSubtitleOffset(+500, now);
          return;
        case ShortcutAction::SubtitleVisibility:
          toggleSubtitleVisibility(now);
          return;
        case ShortcutAction::Screenshot:
          takeScreenshot(now);
          return;
        case ShortcutAction::OpenFile:
          // Open-a-file (BUGS.md #3): the Windows common dialog;
          // elsewhere the attempt degrades to a toast saying what works.
          openFilePicker();
          return;
        case ShortcutAction::OpenFolder:
          // The open-a-folder entry (v0.2.1): the in-app picker with the
          // recursive toggle and the recent-dirs list.
          openDirPicker();
          return;
        case ShortcutAction::Playlist:
          toggleOverlay(Overlay::Playlist);
          return;
        case ShortcutAction::Library:
          openLibrary();  // media library (docs/mvp.md §3)
          return;
        case ShortcutAction::SearchLibrary:
          // The search half of the browse split (docs/todo.md 原型对齐
          // 批): open the overlay when needed, then put the caret in
          // the box — the flag is consumed by the next frame's draw.
          if (st_.overlay != Overlay::Library) openLibrary();
          if (st_.overlay == Overlay::Library) {
            st_.library_search_focus = true;
          }
          return;
        case ShortcutAction::Cast:
          openCast();  // DLNA cast overlay (docs/mvp.md §3)
          return;
        case ShortcutAction::QueueNext:
          playlistStep(false, now);
          return;
        case ShortcutAction::QueuePrev:
          playlistStep(true, now);
          return;
        case ShortcutAction::Info:
          toggleOverlay(Overlay::Info);
          return;
        case ShortcutAction::Recent:
          toggleOverlay(Overlay::Recent);
          return;
        case ShortcutAction::Help:
          toggleOverlay(Overlay::Help);
          return;
        case ShortcutAction::ToggleTheme: {
          // Light/dark palette (proto.css [data-theme]). Swap the palette
          // tokens, re-apply the ImGui style, and refresh the SDL clear
          // colour; the choice persists to <state>/soar/theme.txt.
          theme::applyPalette(!theme::g_light);
          applyTheme();
          saveThemePref();
          return;
        }
        case ShortcutAction::OpenSettings:
          toggleOverlay(Overlay::Settings);  // the settings panel (proto)
          return;
      }
    }
    case SDL_MOUSEMOTION:
      // Pointer movement is user presence too (the prototype's idle
      // wording is "no input", not "no keys or clicks"; mpv behaves the
      // same): motion over or off the widgets alike wakes a hidden bar
      // and restarts its idle window. Visibility itself is decided
      // per-frame from the pins, so no filtering is wanted here.
      st_.hud.noteActivity(now);
      return;
    case SDL_MOUSEBUTTONDOWN: {
      if (e.button.button != SDL_BUTTON_LEFT) return;
      // On a widget, or on the bar itself: docs §1.1 scopes the toggle to
      // the video area, and a press that lands in the same frame as a
      // warp onto the bar must not toggle it either (see mouseOverHud).
      if (ImGui::GetIO().WantCaptureMouse ||
          (st_.hud_alpha > 0.01f && mouseOverHud())) {
        return;
      }
      // First-run window (BUGS.md #3): no source yet — a click is the
      // open-file entry, not an OSC toggle (nothing is playing to
      // toggle anything for).
      if (firstRunEmpty()) {
        openFilePicker();
        return;
      }
      // Double click (<=500ms) toggles fullscreen; a single click toggles
      // the OSC (IINA singleClickAction semantics, docs §1.1 — keeps the
      // second click of the fullscreen gesture from toggling pause).
      // Decide visibility BEFORE any noteActivity: activity resets the
      // manual force, so checking after would never see a hidden bar.
      if (now - st_.last_click < milliseconds(500)) {
        setFullscreen(!st_.fullscreen);
        st_.last_click = milliseconds(0);
        return;
      }
      // Widget events were skipped above, so the pointer is not over the
      // HUD and no seek drag is live; the remaining pins still count.
      const bool shown = st_.hud.visible(
          now, /*pointer_over_hud=*/false, /*seek_drag=*/false,
          st_.overlay != Overlay::None,
          player_.state() != PlaybackState::Playing);
      if (shown) {
        st_.hud.setForce(HudVisibility::Force::Hide);
      } else {
        // noteActivity clears the force and restarts the timer: the bar
        // reappears for a fresh idle window.
        st_.hud.noteActivity(now);
      }
      st_.last_click = now;
      return;
    }
    case SDL_MOUSEWHEEL: {
      if (ImGui::GetIO().WantCaptureMouse) return;
      st_.hud.noteActivity(now);
      // Vertical wheel: volume; horizontal wheel or Shift+wheel: seek
      // (IINA scroll-action defaults, docs §1.1).
      const bool seek_wheel = e.wheel.x != 0 || shiftHeld();
      if (seek_wheel) {
        const int dir =
            e.wheel.x != 0 ? (e.wheel.x > 0 ? 1 : -1) : (e.wheel.y > 0 ? 1 : -1);
        nudgeSeek(dir * 5000, now);
      } else {
        nudgeVolume(e.wheel.y > 0 ? +0.05 : -0.05, now);
      }
      return;
    }
    case SDL_DROPFILE: {
      // Drag-and-drop open (docs §2): backends take plain paths as-is.
      // Drop payloads come from the source's text/uri-list, so the same
      // decode the CLI positional gets applies here too (a space in the
      // name arrives percent-encoded).
      std::string uri(e.drop.file ? e.drop.file : "");
      if (e.drop.file) SDL_free(e.drop.file);
      uri = soar::app::decodeSourceUri(uri);
      if (!uri.empty()) openSource(uri);
      return;
    }
    default:
      return;
  }
}

void PlayerHud::handlePendingTorrent(milliseconds now) {
  if (!torrent_pending_) return;
  switch (torrent_->phase()) {
    case soar::p2p::TorrentPhase::Connecting:
      return;
    case soar::p2p::TorrentPhase::Serving: {
      torrent_pending_ = false;
      const std::string uri = torrent_->playbackUrl();
      source_label_ = torrent_->fileName();
      fmt::print(stderr, "torrent: {} ({} bytes) -> {}\n", source_label_,
                 torrent_->fileSize(), uri);
      if (player_.open(soar::MediaSource{uri, cfg_.cache_dir, cfg_.hwdec})) {
        if (!source_label_.empty()) {
          SDL_SetWindowTitle(window_, (cfg_.title + " - " + source_label_).c_str());
        }
        recordOpen(uri);
        playlist_.add(uri);
        playlist_.setCurrent(playlist_.size() - 1);
        st_.loop_a_set = false;
        st_.toast.show("Opened " + displayName(uri), now);
        player_.play();
      } else {
        st_.toast.show("Open failed: " + player_.lastError(), now);
      }
      break;
    }
    case soar::p2p::TorrentPhase::Failed: {
      torrent_pending_ = false;
      std::string err = torrent_->lastError();
      const size_t nl = err.find('\n');
      if (nl != std::string::npos) err.resize(nl);
      fmt::print(stderr, "{}\n", torrent_->lastError());
      st_.toast.show("Open failed: " + err, now);
      break;
    }
    case soar::p2p::TorrentPhase::Idle:
      break;  // not started (or already stopped): nothing pending anymore
  }
}

void PlayerHud::drawUi(milliseconds now, bool video_active) {
  handlePendingTorrent(now);
  handleCastTick(now);
  // The one-shot --open-dialog request: cleared before the call so a
  // modal stay inside the picker cannot re-summon it on the next frame.
  if (open_dialog_on_start_) {
    open_dialog_on_start_ = false;
    openFilePicker();
  }
  const ImGuiIO& io = ImGui::GetIO();
  // A minimized window reports a zero-sized drawable, and every OSC
  // dimension below is derived from it (bar_w would go negative). Skip
  // the frame's draws instead of handing ImGui a degenerate geometry.
  if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) return;
  // Pin while "not playing" covers Paused/Stopped/Ended/Error: the user
  // is between content and about to act (vidstack behavior, docs §3.1).
  const bool paused = player_.state() != PlaybackState::Playing;
  const bool hud_shown = st_.hud.visible(now, /*pointer_over_hud=*/io.WantCaptureMouse,
                                         st_.seek_dragging,
                                         st_.overlay != Overlay::None, paused);
  // Trail edge for the walkthrough tests: the bar's logical visibility
  // flip (the 0.16s alpha ramp rides along; the edge is the assertion
  // surface, not the fade).
  if (hud_shown != st_.hud_was_shown) {
    st_.hud_was_shown = hud_shown;
    fmt::print(stderr, "event: osc={}\n", hud_shown ? "shown" : "hidden");
  }
  maybeRecordProgress(now);
  maybeAdvancePlaylist(now);
  // "Auto-show first subtitle" (settings Subtitles tab): the first frame
  // a track becomes selected turns visibility on — no V press needed.
  // The -1 -> >=0 edge keeps it one-shot per selection; the detector
  // never selects anything itself (a fresh open still starts with no
  // track selected — the frozen core contract the walkthrough pins).
  if (st_.subtitle_autoshow) {
    const int sel = player_.mediaInfo().selected_subtitle;
    if (st_.prev_selected_subtitle < 0 && sel >= 0 && !st_.subtitle_visible) {
      st_.subtitle_visible = true;
      if (cfg_.subtitle_visible) {
        cfg_.subtitle_visible->store(true, std::memory_order_relaxed);
      }
      st_.toast.show("Subtitles on", now);
    }
    st_.prev_selected_subtitle = sel;
  }
  const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 0.016f;
  const float target = hud_shown ? 1.0f : 0.0f;
  st_.hud_alpha += (target - st_.hud_alpha) * std::min(1.0f, dt / 0.160f);
  st_.hud_alpha = std::clamp(st_.hud_alpha, 0.0f, 1.0f);

  if (!video_active) drawPoster();
  drawPauseOverlay();
  // The full-page Settings overlay replaces the HUD (the v1 settings page
  // is standalone; the top bar / chips / OSC would otherwise keep drawing
  // over it). The overlay is drawn after, so nothing of the page is hidden.
  if (st_.overlay != Overlay::Settings) {
    drawTopBar();
    if (st_.hud_alpha > 0.01f) drawHud(now);
    drawChips(now);
  }
  drawSubtitles(now);
  drawToast(now);
  drawOverlays();
}

void PlayerHud::recordOpen(const std::string& uri) {
  if (uri.empty()) return;
  current_uri_ = uri;
  // The seek-hover thumbnail cache is keyed by position only, so a new
  // source makes every cached frame meaningless (docs §1.6-5). Drop the
  // cache and the uploaded texture so a stale frame can never flash over
  // the new file.
  if (thumb_service_ != nullptr && uri != thumb_source_) {
    thumb_source_ = uri;
    thumb_service_->clear();
    thumb_requested_ = milliseconds(-1);
    thumb_shown_ = milliseconds(-1);
    thumb_tex_w_ = thumb_tex_h_ = 0;
    if (thumb_texture_ != nullptr) {
      SDL_DestroyTexture(thumb_texture_);
      thumb_texture_ = nullptr;
    }
  }
  library_end_cleared_ = false;  // the new source starts a fresh resume book
  if (recent_.add(uri)) recent_.save();
  if (library_ != nullptr && !isTorrentBridge(uri)) {
    // Torrent bridge URLs change port every session (P4b-4): recording
    // one would plant a history row that can never replay. They keep
    // their Recent entry — that contract predates the library.
    library_->recordOpen(uri, std::time(nullptr));
    library_->save();
  }
}

bool PlayerHud::applyResume(const std::string& uri, milliseconds now) {
  if (!cfg_.resume_playback || library_ == nullptr) return false;
  // The Settings panel's "Remember last media" switch gates the jump: off
  // means the session opens at the top regardless of a saved position.
  if (!st_.remember_last_media) return false;
  const LibraryEntry* entry = library_->find(uri);
  if (entry == nullptr) return false;
  const std::int64_t pos = resumePositionMs(*entry);
  if (pos <= 0) return false;
  // An unseekable source refuses the jump and stays at zero — the
  // policy function only knows the recorded numbers, the backend has
  // the final say (the same asymmetry as the seek bar).
  if (!player_.seek(std::chrono::milliseconds(pos))) return false;
  st_.toast.show("Resumed at " +
                     formatClock(std::chrono::milliseconds(pos)),
                 now);
  return true;
}

bool PlayerHud::isTorrentBridge(const std::string& uri) const {
  return torrent_ != nullptr && uri == torrent_->playbackUrl();
}

void PlayerHud::maybeRecordProgress(milliseconds now) {
  if (library_ == nullptr || current_uri_.empty()) return;
  if (isTorrentBridge(current_uri_)) return;
  if (player_.state() == PlaybackState::Ended) {
    if (!library_end_cleared_) {
      library_end_cleared_ = true;
      library_->clearPosition(current_uri_);
      library_->save();
    }
    return;
  }
  if (player_.state() != PlaybackState::Playing) return;
  if (now - last_progress_write_ < kProgressInterval) return;
  last_progress_write_ = now;
  library_->recordProgress(current_uri_, player_.position().count(),
                           player_.mediaInfo().duration.count());
  library_->save();
}

std::string PlayerHud::displayName(const std::string& uri) const {
  return (!source_label_.empty() && uri == current_uri_) ? source_label_
                                                         : baseName(uri);
}

std::string PlayerHud::fileFormatLabel(const std::string& uri) {
  const std::string name = baseName(uri);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return "";
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return ext;
}

std::string PlayerHud::parentFolderLabel(const std::string& uri) {
  const std::string name = baseName(uri);
  const std::size_t slash = name.find_last_of("/\\");
  if (slash == std::string::npos || slash == 0) return "";
  return name.substr(0, slash);
}

bool PlayerHud::firstRunEmpty() const {
  return current_uri_.empty() && !torrent_pending_;
}

bool PlayerHud::shiftHeld() const { return (SDL_GetModState() & KMOD_SHIFT) != 0; }

bool PlayerHud::mouseOverHud() const {
  const ImGuiIO& io = ImGui::GetIO();
  const float bar_w = std::min(io.DisplaySize.x - 24.0f, 920.0f);
  const float bar_h = 80.0f;
  const float x0 = (io.DisplaySize.x - bar_w) * 0.5f;
  const float y0 = io.DisplaySize.y - bar_h - 16.0f;
  return io.MousePos.x >= x0 && io.MousePos.x < x0 + bar_w &&
         io.MousePos.y >= y0 && io.MousePos.y < y0 + bar_h;
}

#endif  // SOAR_WITH_IMGUI

namespace {


// runPlayerWindow sits outside the unnamed namespace; the default-font
// wiring for external ASS documents stays file-local through this shim.
void applyAssDefaultFont(soar::FFmpegBackend* ffmpeg) {
#ifdef SOAR_WITH_FFMPEG
  if (ffmpeg == nullptr) {
    return;
  }
  if (soar::AssRenderer* ass = ffmpeg->assRenderer()) {
    for (const std::string& path : fontCandidates()) {
      if (fileExists(path)) {
        ass->setDefaultFont(path);
        break;
      }
    }
  }
#else
  (void)ffmpeg;
#endif
}

}  // namespace
#endif  // SOAR_WITH_SDL2

int runPlayerWindow(soar::Player& player, const WindowUiConfig& cfg) {
#ifndef SOAR_WITH_SDL2
  (void)player;
  (void)cfg;
  return 0;  // unreachable: main only calls here on SDL2 builds
#else
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fmt::print(stderr, "SDL_Init failed: {}\n", SDL_GetError());
    reportFatalStartupError(
        "soar", std::string("SDL_Init failed: ") + SDL_GetError());
    return 1;
  }
  // P2P sources name the window after the streamed file, not the bridge URL
  // (docs/mvp.md §5 P4b-3); ordinary sources keep the bare title.
  std::string window_title = cfg.title;
  if (!cfg.source_label.empty()) window_title += " - " + cfg.source_label;
  SDL_Window* window = SDL_CreateWindow(
      window_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 540,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    fmt::print(stderr, "SDL_CreateWindow failed: {}\n", SDL_GetError());
    reportFatalStartupError(
        "soar", std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fmt::print(stderr, "SDL_CreateRenderer failed: {}\n", SDL_GetError());
    reportFatalStartupError(
        "soar", std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_Texture* texture = nullptr;
  SDL_Texture* ass_texture = nullptr;  // ASS overlay (docs/mvp.md §6)
  bool video_active = false;
#ifdef SOAR_WITH_FFMPEG
  soar::DecodedVideoFrame video_frame{};
  // External .ass documents have no container to carry font attachments:
  // without a fallback font file libass's glyph lookup misses for every
  // family that was never addFont'ed and the document renders nothing.
  // Same candidates as the UI fonts; best-effort — a renderer left
  // without a default still shows documents whose families resolve from
  // registered fonts or attachments.
  applyAssDefaultFont(cfg.ffmpeg);
#endif

#ifdef SOAR_WITH_IMGUI
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  // Every window here is NoSavedSettings (the OSC is positioned and sized
  // per frame, the overlays are transient), so ImGui's ini would only
  // litter the user's working directory with a file nobody reads.
  ImGui::GetIO().IniFilename = nullptr;
  loadThemePref();  // restore the saved light/dark palette before styling
  applyTheme();
  loadOverlayFont();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);
  {
    PlayerHud hud(player, cfg, window, renderer);

    bool running = true;
    while (running) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        bool quit = false;
        hud.handleEvent(e, &quit);
        if (quit) running = false;
      }

#ifdef SOAR_WITH_FFMPEG
      if (cfg.ffmpeg && cfg.ffmpeg->tryGetVideoFrame(video_frame)) {
        video_active = true;  // has shown frames: poster off from now on
      }
#endif

      // UI first (records scrub/toast state), then video, then UI draw
      // data — one renderer, video under the UI.
      ImGui_ImplSDL2_NewFrame();
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui::NewFrame();
      hud.drawUi(nowMs(), video_active);
      ImGui::Render();

      SDL_SetRenderDrawColor(renderer,
                             (Uint8)(theme::kWindowBg.x * 255.0f),
                             (Uint8)(theme::kWindowBg.y * 255.0f),
                             (Uint8)(theme::kWindowBg.z * 255.0f), 255);
      SDL_RenderClear(renderer);
#ifdef SOAR_WITH_FFMPEG
      // Canvas subtitles obey the same sync offset and V-toggle the
      // plain-text ones do (batch 1c): both live in the config atomics.
      const std::int64_t sub_offset_ms =
          cfg.subtitle_offset_ms
              ? cfg.subtitle_offset_ms->load(std::memory_order_relaxed)
              : 0;
      presentVideoFrame(renderer, &texture,
                        cfg.ffmpeg && video_active ? &video_frame : nullptr,
                        &ass_texture,
                        cfg.ffmpeg ? cfg.ffmpeg->assRenderer() : nullptr,
                        cfg.ffmpeg
                            ? cfg.ffmpeg->position().count() + sub_offset_ms
                            : 0,
                        cfg.subtitle_visible
                            ? cfg.subtitle_visible->load(
                                  std::memory_order_relaxed)
                            : true);
#endif
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
      SDL_RenderPresent(renderer);
      SDL_Delay(4);  // pace the loop when vsync cannot
    }
  }
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
#else
  // Bare fallback (no ImGui): video + Esc/window-close, the pre-UI loop.
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
    }
#ifdef SOAR_WITH_FFMPEG
    if (cfg.ffmpeg && cfg.ffmpeg->tryGetVideoFrame(video_frame)) {
      video_active = true;
    }
#endif
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
#ifdef SOAR_WITH_FFMPEG
    if (presentVideoFrame(renderer, &texture,
                          cfg.ffmpeg && video_active ? &video_frame : nullptr,
                          &ass_texture,
                          cfg.ffmpeg ? cfg.ffmpeg->assRenderer() : nullptr,
                          cfg.ffmpeg
                              ? cfg.ffmpeg->position().count() +
                                    (cfg.subtitle_offset_ms
                                         ? cfg.subtitle_offset_ms->load(
                                               std::memory_order_relaxed)
                                         : 0)
                              : 0,
                          cfg.subtitle_visible
                              ? cfg.subtitle_visible->load(
                                    std::memory_order_relaxed)
                              : true)) {
      SDL_RenderPresent(renderer);
    }
#endif
    SDL_Delay(16);
  }
#endif  // SOAR_WITH_IMGUI

  // 退出前先停播放：后端的解码/音频线程必须先 join，SDL_Quit 在函数末尾，
  // 晚于 SDL 音频设备的销毁（BUGS.md #4 同族顺序问题，tsan 可见）。
  player.stop();

  if (texture) SDL_DestroyTexture(texture);
  if (ass_texture) SDL_DestroyTexture(ass_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
#endif  // SOAR_WITH_SDL2
}

}  // namespace soar::app
