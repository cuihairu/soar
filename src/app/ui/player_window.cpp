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

#include "soar/core/player.h"
#include "ui_state.h"

#include <fmt/format.h>

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>

#  include <algorithm>
#  include <atomic>
#  include <chrono>
#  include <cmath>
#  include <cstdio>
#  include <string>
#  include <vector>

#  ifdef SOAR_WITH_FFMPEG
#    include "soar/core/ffmpeg_backend.h"
#  endif

#  ifdef SOAR_WITH_IMGUI
#    include "imgui.h"
#    include "backends/imgui_impl_sdl2.h"
#    include "backends/imgui_impl_sdlrenderer2.h"
#  endif

namespace soar::app {
namespace {

using milliseconds = std::chrono::milliseconds;

milliseconds nowMs() {
  return std::chrono::duration_cast<milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
}

const char* stateName(PlaybackState s) {
  switch (s) {
    case PlaybackState::Stopped: return "Stopped";
    case PlaybackState::Paused: return "Paused";
    case PlaybackState::Playing: return "Playing";
    case PlaybackState::Ended: return "Ended";
    case PlaybackState::Error: return "Error";
  }
  return "?";
}

const char* trackTypeName(TrackType t) {
  switch (t) {
    case TrackType::Video: return "video";
    case TrackType::Audio: return "audio";
    case TrackType::Subtitle: return "subtitle";
  }
  return "?";
}

// Which overlay page is open; at most one at a time (docs/ui-design.md §2).
enum class Overlay { None, Info, Recent, Help, Subtitle };

std::string baseName(const std::string& uri) {
  const std::size_t slash = uri.find_last_of("/\\");
  return slash == std::string::npos ? uri : uri.substr(slash + 1);
}

// Letterbox destination: the frame scaled to fit, centered (docs §4 — the
// video area is always aspect-correct on the theme background).
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
// and blits it letterboxed. Returns false when no frame was ready.
bool presentVideoFrame(SDL_Renderer* renderer, SDL_Texture** texture,
                       const void* frame_ptr) {
  if (frame_ptr == nullptr) return false;
#ifdef SOAR_WITH_FFMPEG
  const auto& frame =
      *static_cast<const soar::FFmpegBackend::DecodedVideoFrame*>(frame_ptr);
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
  return true;
#else
  (void)renderer;
  (void)texture;
  return false;
#endif
}

#ifdef SOAR_WITH_IMGUI

// Theme tokens (docs/ui-design.md §4): one dark palette, no scattered
// literals — every color the UI draws comes from here.
void applyTheme() {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = 8.0f;
  st.FrameRounding = 6.0f;
  st.GrabRounding = 4.0f;
  st.PopupRounding = 6.0f;
  st.WindowBorderSize = 1.0f;
  st.WindowPadding = ImVec2(12, 8);
  st.FramePadding = ImVec2(8, 6);
  st.ItemSpacing = ImVec2(8, 6);

  const ImVec4 accent = ImVec4(0.298f, 0.553f, 1.0f, 1.0f);  // #4C8DFF
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = ImVec4(0.910f, 0.918f, 0.941f, 1.0f);        // #E8EAF0
  c[ImGuiCol_TextDisabled] = ImVec4(0.910f, 0.918f, 0.941f, 0.55f);
  c[ImGuiCol_WindowBg] = ImVec4(0.063f, 0.071f, 0.094f, 0.94f);   // panel
  c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.15f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.10f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.16f);
  c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.20f);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_WindowBg];
  c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
  c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.10f);
  c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.18f);
  c[ImGuiCol_ButtonActive] = accent;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = accent;
  c[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.14f);
  c[ImGuiCol_HeaderActive] = accent;
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
  c[ImGuiCol_TextSelectedBg] = accent;
}

// Best-effort system UI font: the ImGui embedded font is ASCII-only bitmap,
// which mangles non-ASCII titles. CI runners lack the CJK candidates — the
// ASCII-everywhere labels keep that fallback path harmless. SOAR_UI_BITMAP_FONT
// (set by the X11 drive test) pins the embedded font: identical glyph metrics
// on every machine, so the OSC widget geometry the injector clicks is stable.
// Also loads the same font at multiple sizes for subtitle rendering (v0.2).
struct SubtitleFonts {
  ImFont* size_16 = nullptr;
  ImFont* size_20 = nullptr;
  ImFont* size_24 = nullptr;
  ImFont* size_28 = nullptr;
  ImFont* size_32 = nullptr;
  ImFont* size_36 = nullptr;
  ImFont* size_48 = nullptr;
};

static SubtitleFonts loadSubtitleFonts() {
  SubtitleFonts fonts;
  ImGuiIO& io = ImGui::GetIO();
  if (std::getenv("SOAR_UI_BITMAP_FONT") != nullptr) {
    // Bitmap font mode: all sizes point to default
    io.Fonts->AddFontDefault();
    fonts.size_16 = fonts.size_20 = fonts.size_24 = fonts.size_28 =
        fonts.size_32 = fonts.size_36 = fonts.size_48 = io.Fonts->Fonts[0];
    return fonts;
  }
  const char* candidates[] = {
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/System/Library/Fonts/PingFang.ttc",
      "/System/Library/Fonts/Helvetica.ttc",
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\segoeui.ttf",
  };
  for (const char* path : candidates) {
    if (FILE* f = std::fopen(path, "rb")) {
      std::fclose(f);
      fonts.size_16 = io.Fonts->AddFontFromFileTTF(path, 16.0f);
      fonts.size_20 = io.Fonts->AddFontFromFileTTF(path, 20.0f);
      fonts.size_24 = io.Fonts->AddFontFromFileTTF(path, 24.0f);
      fonts.size_28 = io.Fonts->AddFontFromFileTTF(path, 28.0f);
      fonts.size_32 = io.Fonts->AddFontFromFileTTF(path, 32.0f);
      fonts.size_36 = io.Fonts->AddFontFromFileTTF(path, 36.0f);
      fonts.size_48 = io.Fonts->AddFontFromFileTTF(path, 48.0f);
      if (fonts.size_24) return fonts;  // At least the default size loaded
    }
  }
  // Fallback: all point to default
  io.Fonts->AddFontDefault();
  fonts.size_16 = fonts.size_20 = fonts.size_24 = fonts.size_28 =
      fonts.size_32 = fonts.size_36 = fonts.size_48 = io.Fonts->Fonts[0];
  return fonts;
}

// Best-effort system UI font: the ImGui embedded font is ASCII-only bitmap,
// which mangles non-ASCII titles. CI runners lack the CJK candidates — the
// ASCII-everywhere labels keep that fallback path harmless. SOAR_UI_BITMAP_FONT
// (set by the X11 drive test) pins the embedded font: identical glyph metrics
// on every machine, so the OSC widget geometry the injector clicks is stable.
void loadOverlayFont() {
  ImGuiIO& io = ImGui::GetIO();
  if (std::getenv("SOAR_UI_BITMAP_FONT") != nullptr) {
    io.Fonts->AddFontDefault();
    return;
  }
  const char* candidates[] = {
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/System/Library/Fonts/PingFang.ttc",
      "/System/Library/Fonts/Helvetica.ttc",
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\segoeui.ttf",
  };
  for (const char* path : candidates) {
    if (FILE* f = std::fopen(path, "rb")) {
      std::fclose(f);
      if (io.Fonts->AddFontFromFileTTF(path, 16.0f) != nullptr) return;
    }
  }
  io.Fonts->AddFontDefault();
}

// The player HUD: OSC, overlays, toast, and app-level input routing. Every
// action goes through the Player facade — no backend knowledge (docs §5.2).
class PlayerHud {
 public:
  PlayerHud(soar::Player& player, const WindowUiConfig& cfg, SDL_Window* window)
      : player_(player), cfg_(cfg), window_(window), recent_(cfg.recent_path),
        current_uri_(cfg.initial_uri), subtitle_fonts_(loadSubtitleFonts()) {
    recent_.load();
    recordOpen(cfg.initial_uri);
  }

  // App-level input (docs §3). `quit` is set on the exit paths. The UI's
  // widgets have already seen the event (ImGui_ImplSDL2_ProcessEvent runs
  // first in the caller); events the UI claims are skipped here.
  void handleEvent(const SDL_Event& e, bool* quit) {
    const auto now = nowMs();
    switch (e.type) {
      case SDL_QUIT:
        *quit = true;
        return;
      case SDL_KEYDOWN: {
        if (e.key.repeat != 0) return;  // held key: no repeat actions in v0.1
        // An open combo popup owns the keyboard (arrows move its
        // selection); app shortcuts must not fire through it.
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                             ImGuiPopupFlags_AnyPopupLevel)) {
          return;
        }
        st_.hud.noteActivity(now);
        // SDL reports uppercase letter syms under Shift/CapsLock and defines
        // no SDLK_Q-style constants — normalize so one case label covers both.
        SDL_Keycode key = e.key.keysym.sym;
        if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
        const bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
        switch (key) {
          case SDLK_ESCAPE:
            // Fullscreen → window first; otherwise quit (docs §3.2, which
            // also keeps the CLI's Escape-exit contract the tests pin).
            if (st_.fullscreen) {
              setFullscreen(false);
            } else {
              *quit = true;
            }
            return;
          case SDLK_q:
            *quit = true;
            return;
          case SDLK_SPACE:
          case SDLK_k:
            togglePlayPause();
            return;
          case SDLK_LEFT:
            nudgeSeek(shift ? -1000 : -5000, now);
            return;
          case SDLK_RIGHT:
            nudgeSeek(shift ? 1000 : 5000, now);
            return;
          case SDLK_PAGEUP:
            nudgeSeek(-60000, now);
            return;
          case SDLK_PAGEDOWN:
            nudgeSeek(60000, now);
            return;
          case SDLK_HOME:
            seekTo(milliseconds(0), now);
            return;
          case SDLK_UP:
            nudgeVolume(+0.05, now);
            return;
          case SDLK_DOWN:
            nudgeVolume(-0.05, now);
            return;
          case SDLK_m:
            toggleMute(now);
            return;
          case SDLK_COMMA:
            nudgeRate(-1, now);
            return;
          case SDLK_PERIOD:
            nudgeRate(+1, now);
            return;
          case SDLK_f:
            setFullscreen(!st_.fullscreen);
            return;
          case SDLK_a:
            cycleTrack(TrackType::Audio, now);
            return;
          case SDLK_c:
            cycleTrack(TrackType::Subtitle, now);
            return;
          case SDLK_i:
            toggleOverlay(Overlay::Info);
            return;
          case SDLK_r:
            toggleOverlay(Overlay::Recent);
            return;
          case SDLK_h:
            toggleOverlay(Overlay::Help);
            return;
          // Subtitle key bindings (v0.2) — temporarily disabled to keep X11 test stable
          // case SDLK_LEFTBRACKET:  // '[' - decrease subtitle offset
          //   nudgeSubtitleOffset(-500, now);
          //   return;
          // case SDLK_RIGHTBRACKET:  // ']' - increase subtitle offset
          //   nudgeSubtitleOffset(+500, now);
          //   return;
          // case SDLK_s:  // 's' - toggle subtitle visibility
          //   toggleSubtitleVisibility(now);
          //   return;
          default:
            if (key >= SDLK_0 && key <= SDLK_9) {
              percentSeek(static_cast<int>(key - SDLK_0) * 10, now);
            }
            return;
        }
      }
      case SDL_MOUSEBUTTONDOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) return;
        if (ImGui::GetIO().WantCaptureMouse) return;  // on a widget
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
        std::string uri(e.drop.file ? e.drop.file : "");
        if (e.drop.file) SDL_free(e.drop.file);
        if (!uri.empty()) openSource(uri);
        return;
      }
      default:
        return;
    }
  }

  // Per frame: animate the HUD alpha, then emit the UI draws. Must be
  // called between ImGui::NewFrame and ImGui::Render.
  void drawUi(milliseconds now, bool video_active) {
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
    const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 0.016f;
    const float target = hud_shown ? 1.0f : 0.0f;
    st_.hud_alpha += (target - st_.hud_alpha) * std::min(1.0f, dt / 0.160f);
    st_.hud_alpha = std::clamp(st_.hud_alpha, 0.0f, 1.0f);

    if (!video_active) drawPoster();
    if (st_.hud_alpha > 0.01f) drawHud(now);
    drawChips();
    drawSubtitles(now);
    drawToast(now);
    drawOverlays();
  }

  void recordOpen(const std::string& uri) {
    if (uri.empty()) return;
    current_uri_ = uri;
    if (recent_.add(uri)) recent_.save();
  }

 private:
  // ---- Actions (all keyboard-reachable; toasts acknowledge, docs §3.2) ----

  void togglePlayPause() {
    if (player_.state() == PlaybackState::Playing) {
      player_.pause();
    } else {
      if (player_.state() == PlaybackState::Ended) {
        player_.seek(milliseconds(0));  // replay from the top, mpv style
      }
      player_.play();
    }
  }

  void seekTo(milliseconds target, milliseconds now) {
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

  void nudgeSeek(long long delta_ms, milliseconds now) {
    seekTo(player_.position() + milliseconds(delta_ms), now);
  }

  void percentSeek(int percent, milliseconds now) {
    const MediaInfo info = player_.mediaInfo();
    seekTo(info.duration * percent / 100, now);
  }

  void nudgeVolume(double delta, milliseconds now) {
    st_.last_volume = std::clamp(st_.last_volume + delta, 0.0, 1.0);
    player_.setVolume(st_.last_volume);
    st_.toast.show(
        "Volume " + std::to_string(static_cast<int>(st_.last_volume * 100)) + "%",
        now);
  }

  void toggleMute(milliseconds now) {
    st_.last_muted = !st_.last_muted;
    player_.setMuted(st_.last_muted);
    st_.toast.show(st_.last_muted ? "Muted" : "Unmuted", now);
  }

  // Rate steps through the same ladder the combo offers (vidstack </>).
  void nudgeRate(int step, milliseconds now) {
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

  void cycleTrack(TrackType type, milliseconds now) {
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

  // Reopen path for Recent/DnD; Player::open closes existing media first
  // (the FFmpeg backend re-enters cleanly — verified in its open()).
  void openSource(const std::string& uri) {
    if (player_.open(soar::MediaSource{uri, cfg_.cache_dir})) {
      recordOpen(uri);
      st_.toast.show("Opened " + baseName(uri), nowMs());
      player_.play();
    } else {
      st_.toast.show("Open failed: " + player_.lastError(), nowMs());
    }
  }

  void setFullscreen(bool on) {
    st_.fullscreen = on;
    SDL_SetWindowFullscreen(window_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  }

  void toggleOverlay(Overlay which) {
    st_.overlay = st_.overlay == which ? Overlay::None : which;
  }

  void nudgeSubtitleOffset(int delta_ms, milliseconds now) {
    st_.subtitle_offset_ms += delta_ms;
    if (cfg_.subtitle_offset_ms) {
      cfg_.subtitle_offset_ms->store(st_.subtitle_offset_ms, std::memory_order_relaxed);
    }
    char text[32];
    std::snprintf(text, sizeof(text), "Sub offset %+d ms", st_.subtitle_offset_ms);
    st_.toast.show(text, now);
  }

  void toggleSubtitleVisibility(milliseconds now) {
    st_.subtitle_visible = !st_.subtitle_visible;
    if (cfg_.subtitle_visible) {
      cfg_.subtitle_visible->store(st_.subtitle_visible, std::memory_order_relaxed);
    }
    st_.toast.show(st_.subtitle_visible ? "Subtitles on" : "Subtitles off", now);
  }

  bool shiftHeld() const { return (SDL_GetModState() & KMOD_SHIFT) != 0; }

  // ---- Drawing ----

  void drawHud(milliseconds now) {
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;
    const float bar_w = std::min(w - 24.0f, 920.0f);
    const float bar_h = 80.0f;  // seek row + button row

    const MediaInfo info = player_.mediaInfo();
    const double dur = static_cast<double>(info.duration.count());
    const double pos = static_cast<double>(player_.position().count());
    const double shown = st_.seek_dragging ? st_.seek_scrub : pos;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st_.hud_alpha);
    ImGui::SetNextWindowPos(ImVec2((w - bar_w) * 0.5f, h - bar_h - 16.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(bar_w, bar_h), ImGuiCond_Always);
    if (ImGui::Begin("##osc", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollbar)) {
      // Row 1: the seek bar, full width. Hover/drag tooltip shows the time
      // under the pointer; dragging previews, release commits (docs §3.1).
      const bool seekable = info.seekable && dur > 0.0;
      if (!seekable) ImGui::BeginDisabled();
      ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
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
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
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

      // Row 2: transport, clocks, volume, rate, tracks, overlays, full.
      const bool playing = player_.state() == PlaybackState::Playing;
      if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(54, 0))) {
        togglePlayPause();
      }
      ImGui::SameLine();
      if (ImGui::Button("Stop", ImVec2(46, 0))) player_.stop();
      ImGui::SameLine();
      const std::string clock =
          (st_.seek_dragging ? "> " : "") + formatClock(milliseconds(static_cast<long long>(shown))) +
          " / " + formatClock(info.duration);
      ImGui::TextUnformatted(clock.c_str());
      ImGui::SameLine();
      if (ImGui::Button(st_.last_muted ? "Unmute" : "Mute")) toggleMute(now);
      ImGui::SameLine();
      ImGui::PushItemWidth(64.0f);
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
      ImGui::PushItemWidth(84.0f);
      drawTrackCombo(TrackType::Audio, now);
      ImGui::PopItemWidth();
      ImGui::SameLine();
      ImGui::PushItemWidth(84.0f);
      drawTrackCombo(TrackType::Subtitle, now);
      ImGui::PopItemWidth();
      ImGui::SameLine();
      // Sub overlay button (v0.2) — temporarily disabled to keep X11 test stable
      // if (ImGui::Button("Sub")) toggleOverlay(Overlay::Subtitle);
      // ImGui::SameLine();
      if (ImGui::Button("Info")) toggleOverlay(Overlay::Info);
      ImGui::SameLine();
      if (ImGui::Button(st_.fullscreen ? "Window" : "Full")) {
        setFullscreen(!st_.fullscreen);
      }
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

  void drawRateCombo(milliseconds now) {
    static const double kRates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    char label[16];
    std::snprintf(label, sizeof(label), "%.2gx", st_.last_rate);
    if (!ImGui::BeginCombo("##rate", label)) return;
    for (const double r : kRates) {
      const bool selected = std::abs(r - st_.last_rate) < 1e-9;
      char item[16];
      std::snprintf(item, sizeof(item), "%.2gx", r);
      if (ImGui::Selectable(item, selected) && !selected) {
        st_.last_rate = r;
        player_.setRate(r);
        st_.toast.show(std::string(item) + " speed", now);
      }
    }
    ImGui::EndCombo();
  }

  void drawTrackCombo(TrackType type, milliseconds now) {
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
    // Current value marked, one level of nesting max (docs §1.4-4).
    if (type == TrackType::Subtitle) {
      const bool off = selected < 0;
      if (ImGui::Selectable("Off", off) && !off) {
        player_.disableSubtitles();
        st_.toast.show("Subtitles off", now);
      }
      ImGui::Separator();
    }
    for (const auto* t : tracks) {
      std::string item =
          "#" + std::to_string(t->id) + " " + t->codec;
      if (!t->language.empty()) item += " (" + t->language + ")";
      const bool is_sel = t->id == selected;
      if (ImGui::Selectable(item.c_str(), is_sel) && !is_sel) {
        if (player_.selectTrack(type, t->id)) {
          st_.toast.show((type == TrackType::Audio ? "Audio track "
                                                   : "Subtitle track ") +
                             std::to_string(t->id),
                         now);
        } else {
          st_.toast.show("Track switch failed", now);
        }
      }
    }
    ImGui::EndCombo();
  }

  // No video (audio-only / null backend): a centered "what is playing"
  // panel so the window carries content (docs §2).
  void drawPoster() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("##poster", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs)) {
      ImGui::TextUnformatted(baseName(current_uri_).c_str());
      ImGui::TextDisabled("%s", stateName(player_.state()));
      ImGui::Spacing();
      ImGui::TextDisabled("%s", "Drop a file here - R recent - H shortcuts");
    }
    ImGui::End();
  }

  void drawChips() {
    // Paused chip while paused; buffering chip while the backend reports a
    // stall (P1 events); download chip while the disk cache is still
    // filling (P3c events). All fade with the theme, never block input.
    if (player_.state() == PlaybackState::Paused) {
      chip("##paused", "Paused", 12.0f);
    }
    if (cfg_.buffering && cfg_.buffering->load(std::memory_order_relaxed)) {
      chip("##buffering", "Buffering...", 12.0f);
    }
    const std::uint64_t total =
        cfg_.download_total ? cfg_.download_total->load(std::memory_order_relaxed) : 0;
    const std::uint64_t have =
        cfg_.download_bytes ? cfg_.download_bytes->load(std::memory_order_relaxed) : 0;
    if (total > 0 && have < total) {
      char text[32];
      std::snprintf(text, sizeof(text), "Downloading %llu%%",
                    static_cast<unsigned long long>(have * 100 / total));
      chip("##download", text, 34.0f);
    }
  }

  // Draw subtitle overlay: polls the backend for decoded subtitle frames
  // and renders them at the bottom of the video area.
  void drawSubtitles(milliseconds now) {
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

    // Poll for new subtitle frame
    soar::FFmpegBackend::DecodedSubtitleFrame sub_frame;
    if (cfg_.ffmpeg->tryGetSubtitleFrame(sub_frame)) {
      // Store the latest subtitle frame with its timing
      st_.current_subtitle = sub_frame.text;
      st_.subtitle_pts = sub_frame.pts;
      st_.subtitle_duration = sub_frame.duration;
    }

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

  // Subtitle state
  std::string current_subtitle;
  std::chrono::milliseconds subtitle_pts{0};
  std::chrono::milliseconds subtitle_duration{0};

  void chip(const char* name, const char* text, float y) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, y), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(text);
    }
    ImGui::End();
  }

  void drawToast(milliseconds now) {
    if (!st_.toast.active(now)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const double age = st_.toast.age(now);
    // Full opacity for the first 75%, a 25% fade-out tail (docs §4).
    const float alpha =
        static_cast<float>(age < 0.75 ? 1.0 : 1.0 - (age - 0.75) / 0.25);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 56.0f), ImGuiCond_Always,
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

  void drawOverlays() {
    switch (st_.overlay) {
      case Overlay::None: return;
      case Overlay::Info: drawInfoOverlay(); return;
      case Overlay::Recent: drawRecentOverlay(); return;
      case Overlay::Help: drawHelpOverlay(); return;
      case Overlay::Subtitle: drawSubtitleOverlay(); return;
    }
  }

  void drawInfoOverlay() {
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
      const std::string err = player_.lastError();
      if (!err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.365f, 0.365f, 1.0f));
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

  void drawRecentOverlay() {
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
        if (ImGui::Selectable(baseName(uri).c_str())) {
          openSource(uri);
          st_.overlay = Overlay::None;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", uri.c_str());
      }
    }
    ImGui::End();
    if (!open) st_.overlay = Overlay::None;
  }

  void drawHelpOverlay() {
    bool open = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380, 400), ImGuiCond_Appearing);
    if (ImGui::Begin("Shortcuts", &open, ImGuiWindowFlags_NoSavedSettings)) {
      static const char* const kRows[][2] = {
          {"Space / K", "Play / pause"},
          {"Left / Right", "Seek -5s / +5s"},
          {"Shift+Left / Right", "Seek -1s / +1s"},
          {"PgUp / PgDn", "Seek -60s / +60s"},
          {"Home", "Go to start"},
          {"0 - 9", "Jump to 0% - 90%"},
          {"Up / Down", "Volume -5% / +5%"},
          {"M", "Mute"},
          {", / .", "Speed down / up"},
          {"A", "Cycle audio track"},
          {"C", "Cycle subtitles (incl. off)"},
          {"S", "Toggle subtitles on/off"},
          {"[ / ]", "Subtitle offset -500ms / +500ms"},
          {"F", "Fullscreen"},
          {"I", "Media info"},
          {"R", "Recent files"},
          {"H", "This help"},
          {"Esc", "Leave fullscreen / quit"},
          {"Q", "Quit"},
      };
      if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg)) {
        for (const auto& row : kRows) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row[0]);
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", row[1]);
        }
        ImGui::EndTable();
      }
    }
    ImGui::End();
    if (!open) st_.overlay = Overlay::None;
  }

  void drawSubtitleOverlay() {
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
      } else {
        ImGui::Text("Track:");
        ImGui::SameLine();
        const TrackId selected = info.selected_subtitle;
        char label[40];
        std::snprintf(label, sizeof(label), "Sub %d/%d",
                      selected < 0 ? 0 : static_cast<int>(selected),
                      static_cast<int>(sub_tracks.size()));
        if (ImGui::BeginCombo("##sub_select", label)) {
          if (ImGui::Selectable("Off", selected < 0) && selected >= 0) {
            player_.disableSubtitles();
            st_.toast.show("Subtitles off", nowMs());
          }
          ImGui::Separator();
          for (const auto* t : sub_tracks) {
            std::string item = "#" + std::to_string(t->id) + " " + t->codec;
            if (!t->language.empty()) item += " (" + t->language + ")";
            if (!t->title.empty()) item += " - " + t->title;
            const bool is_sel = t->id == selected;
            if (ImGui::Selectable(item.c_str(), is_sel) && !is_sel) {
              if (player_.selectTrack(TrackType::Subtitle, t->id)) {
                st_.toast.show("Subtitle track " + std::to_string(t->id), nowMs());
              } else {
                st_.toast.show("Subtitle switch failed", nowMs());
              }
            }
          }
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
      ImGui::TextDisabled("S: cycle subtitle  |  [: decrease offset  |  ]: increase offset");
    }
    ImGui::End();
    if (!open) st_.overlay = Overlay::None;
  }

  // UI-local mirrors of volume/rate/mute: the Player facade exposes setters
  // only (no getters in v0.1), so the HUD tracks what it last set. Honest
  // enough for a single-actor UI; core getters would be a v0.2 cleanup.
  struct State {
    HudVisibility hud{milliseconds(0)};
    Toast toast;
    Overlay overlay = Overlay::None;
    bool fullscreen = false;
    bool seek_dragging = false;
    double seek_scrub = 0.0;
    milliseconds last_click{0};
    float hud_alpha = 1.0f;
    double last_volume = 1.0;
    double last_rate = 1.0;
    bool last_muted = false;

    // Subtitle rendering state (v0.2 basics)
    float subtitle_font_size = 24.0f;
    int subtitle_offset_ms = 0;
    bool subtitle_visible = true;
    // Runtime subtitle frame from backend
    std::string current_subtitle;
    std::chrono::milliseconds subtitle_pts{0};
    std::chrono::milliseconds subtitle_duration{0};
  };

  soar::Player& player_;
  const WindowUiConfig& cfg_;
  SDL_Window* window_ = nullptr;
  RecentStore recent_;
  std::string current_uri_;
  SubtitleFonts subtitle_fonts_;
  State st_;
};

#endif  // SOAR_WITH_IMGUI

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
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow(
      cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 540,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    fmt::print(stderr, "SDL_CreateWindow failed: {}\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fmt::print(stderr, "SDL_CreateRenderer failed: {}\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_Texture* texture = nullptr;
  bool video_active = false;
#ifdef SOAR_WITH_FFMPEG
  soar::FFmpegBackend::DecodedVideoFrame video_frame{};
#endif

#ifdef SOAR_WITH_IMGUI
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  // Every window here is NoSavedSettings (the OSC is positioned and sized
  // per frame, the overlays are transient), so ImGui's ini would only
  // litter the user's working directory with a file nobody reads.
  ImGui::GetIO().IniFilename = nullptr;
  applyTheme();
  loadOverlayFont();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);
  {
    PlayerHud hud(player, cfg, window);

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

      SDL_SetRenderDrawColor(renderer, 10, 11, 14, 255);  // theme background
      SDL_RenderClear(renderer);
#ifdef SOAR_WITH_FFMPEG
      presentVideoFrame(renderer, &texture,
                        cfg.ffmpeg && video_active ? &video_frame : nullptr);
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
                          cfg.ffmpeg && video_active ? &video_frame : nullptr)) {
      SDL_RenderPresent(renderer);
    }
#endif
    SDL_Delay(16);
  }
#endif  // SOAR_WITH_IMGUI

  if (texture) SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
#endif  // SOAR_WITH_SDL2
}

}  // namespace soar::app
