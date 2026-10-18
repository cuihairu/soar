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
#include "ui_tokens.h"
#include "ui_state.h"

#include <fmt/format.h>

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
enum class Overlay { None, Info, Recent, Help, Subtitle, Playlist, DirPick, Library,
                     Cast, Settings };

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

// System font candidates, in order: Linux Noto/DejaVu, macOS, Windows.
// Shared by the ImGui overlay fonts and the libass default font (both
// compilations of this TU — with and without ImGui — reach the latter).
std::vector<std::string> fontCandidates() {
  return {
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/System/Library/Fonts/PingFang.ttc",
      "/System/Library/Fonts/Helvetica.ttc",
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\segoeui.ttf",
  };
}

bool fileExists(const std::string& path) {
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

#ifdef SOAR_WITH_IMGUI

// Theme tokens (prototypes/redesign/assets/proto.css — the v2 design
// language the user reviews against). Single signal orange is the only
// interactive accent; sky-blue is reserved for network/P2P semantics. Every
// colour and radius the UI draws is named here — the draw code reads tokens,
// never bare literals. Hex comments are the prototype token values.
namespace theme {
// Palette tokens. These are runtime-mutable so the light/dark toggle
// (proto.css :root[data-theme]) can swap them; the draw code reads token
// names, never literals, so switching is a single applyPalette() call.
// Defaults below are the DARK palette; applyPalette(false) reloads them.
ImVec4 kText{0.933f, 0.945f, 0.961f, 1.0f};             // #EEF1F5 text-1
ImVec4 kTextDim{0.725f, 0.753f, 0.796f, 1.0f};          // #B9C0CB text-2
ImVec4 kTextMuted{0.545f, 0.580f, 0.639f, 1.0f};        // #8B94A3 text-3
// Single accent lock (proto.css --orange-500 + --ink-on-accent): the play /
// interactive accent. Hover is --accent-hover; the ink on accent buttons is
// the warm near-black, not the neutral one. Accent + its ink are identical in
// both themes (the orange is the same orange on light and dark).
ImVec4 kAccent{1.0f, 0.420f, 0.208f, 1.0f};             // #FF6B35 orange-500
ImVec4 kAccentHover{1.0f, 0.490f, 0.302f, 1.0f};        // #FF7D4D accent-hover
ImVec4 kInkOnAccent{0.110f, 0.051f, 0.016f, 1.0f};      // #1C0D04 ink-on-accent
ImVec4 kDanger{1.0f, 0.361f, 0.400f, 1.0f};             // #FF5C66 red-500
ImVec4 kOk{0.243f, 0.812f, 0.431f, 1.0f};               // #3ECF6E ok-500
ImVec4 kWarn{1.0f, 0.698f, 0.141f, 1.0f};               // #FFB224 warn-500
// Network / P2P semantic (proto.css --sky-net): the one place sky-blue stays.
ImVec4 kNet{0.424f, 0.694f, 1.0f, 1.0f};                // #6CB1FF sky-net
// The opaque page background behind the video (theme::kPanelBg is the
// translucent OSC face over the video; this is the SDL clear colour, i.e.
// surface-0). Dark #0A0B0E, light #EEF0F4.
ImVec4 kWindowBg{0.039f, 0.043f, 0.055f, 1.0f};         // #0A0B0E surface-0
ImVec4 kPanelBg{0.043f, 0.047f, 0.059f, 0.88f};         // #0A0B0E oscbg over video
ImVec4 kPopupBg{0.10f, 0.11f, 0.15f, 0.98f};
ImVec4 kBorder{1.0f, 1.0f, 1.0f, 0.10f};                // --oline rgba(255,255,255,.10)
ImVec4 kFrameBg{1.0f, 1.0f, 1.0f, 0.10f};
ImVec4 kFrameBgHovered{1.0f, 1.0f, 1.0f, 0.16f};
ImVec4 kFrameBgActive{1.0f, 1.0f, 1.0f, 0.20f};
ImVec4 kButton{1.0f, 1.0f, 1.0f, 0.10f};
ImVec4 kButtonHovered{1.0f, 1.0f, 1.0f, 0.18f};
ImVec4 kHeader{1.0f, 1.0f, 1.0f, 0.08f};
ImVec4 kHeaderHovered{1.0f, 1.0f, 1.0f, 0.14f};
// Card surfaces (proto.css --surface-2 card bg + --line-strong hover edge):
// the library grid's tiles sit on surface-2 and light their border to
// line-strong on hover. surface-2 is opaque (it fully covers the panel).
ImVec4 kSurface2{0.086f, 0.098f, 0.125f, 1.0f};         // #161920 surface-2 (dark)
ImVec4 kLineStrong{0.212f, 0.243f, 0.294f, 1.0f};       // #363E4B line-strong (dark)

// Which palette is loaded. false = dark (the default the app ships with).
inline bool g_light = false;

// Load one of the two proto.css palettes into the tokens above. Callers then
// re-run applyTheme() (ImGui style) and refresh the SDL clear colour. Values
// are the prototype's CSS custom properties for each [data-theme] block.
inline void applyPalette(bool light) {
  g_light = light;
  if (!light) {
    // :root (dark) — the palette the tokens above already default to.
    kText = {0.933f, 0.945f, 0.961f, 1.0f};         // #EEF1F5 text-1
    kTextDim = {0.725f, 0.753f, 0.796f, 1.0f};      // #B9C0CB text-2
    kTextMuted = {0.545f, 0.580f, 0.639f, 1.0f};    // #8B94A3 text-3
    kDanger = {1.0f, 0.361f, 0.400f, 1.0f};         // #FF5C66 red-500
    kOk = {0.243f, 0.812f, 0.431f, 1.0f};           // #3ECF6E ok-500
    kWarn = {1.0f, 0.698f, 0.141f, 1.0f};           // #FFB224 warn-500
    kNet = {0.424f, 0.694f, 1.0f, 1.0f};            // #6CB1FF sky-net
    kWindowBg = {0.043f, 0.047f, 0.059f, 1.0f};     // #0B0C0F surface-0
    kPanelBg = {0.039f, 0.043f, 0.055f, 0.88f};     // #0A0B0E oscbg over video
    kPopupBg = {0.10f, 0.11f, 0.15f, 0.98f};
    kBorder = {1.0f, 1.0f, 1.0f, 0.10f};            // rgba(255,255,255,.10)
    kFrameBg = {1.0f, 1.0f, 1.0f, 0.10f};
    kFrameBgHovered = {1.0f, 1.0f, 1.0f, 0.16f};
    kFrameBgActive = {1.0f, 1.0f, 1.0f, 0.20f};
    kButton = {1.0f, 1.0f, 1.0f, 0.10f};
    kButtonHovered = {1.0f, 1.0f, 1.0f, 0.18f};
    kHeader = {1.0f, 1.0f, 1.0f, 0.08f};
    kHeaderHovered = {1.0f, 1.0f, 1.0f, 0.14f};
    kSurface2 = {0.086f, 0.098f, 0.125f, 1.0f};   // #161920 surface-2
    kLineStrong = {0.212f, 0.243f, 0.294f, 1.0f}; // #363E4B line-strong
    return;
  }
  // :root[data-theme="light"] — the light palette. Accent + ink-on-accent
  // stay the same orange/near-black; the semantic inks switch to their
  // "-ink" variants (readable on a light ground) and every surface flips to
  // the light ladder (#eef0f4 / #f7f8fa / #ffffff) with dark-alpha controls.
  kText = {0.090f, 0.102f, 0.122f, 1.0f};           // #171A1F text-1
  kTextDim = {0.271f, 0.306f, 0.361f, 1.0f};        // #454E5C text-2
  kTextMuted = {0.384f, 0.424f, 0.490f, 1.0f};      // #626C7D text-3
  kDanger = {0.788f, 0.208f, 0.251f, 1.0f};         // --red-ink #C93540
  kOk = {0.118f, 0.557f, 0.306f, 1.0f};             // --ok-ink #1E8E4E
  kWarn = {0.604f, 0.404f, 0.0f, 1.0f};             // --warn-ink #9A6700
  kNet = {0.169f, 0.424f, 0.690f, 1.0f};            // --sky-net-ink #2B6CB0
  kWindowBg = {0.933f, 0.941f, 0.957f, 1.0f};       // #EEF0F4 surface-0
  kPanelBg = {1.0f, 1.0f, 1.0f, 0.94f};             // #FFFFFF oscbg over video
  kPopupBg = {1.0f, 1.0f, 1.0f, 0.98f};             // #FFFFFF surface-2
  kBorder = {0.875f, 0.890f, 0.918f, 1.0f};         // --line #DFE3EA
  kFrameBg = {0.906f, 0.918f, 0.941f, 1.0f};        // --surface-3 #E7EAF0
  kFrameBgHovered = {0.831f, 0.855f, 0.898f, 1.0f}; // --line-strong #C3CAD5
  kFrameBgActive = {0.765f, 0.792f, 0.847f, 1.0f};  // a shade darker
  kButton = {0.906f, 0.918f, 0.941f, 1.0f};         // --surface-3 #E7EAF0
  kButtonHovered = {0.831f, 0.855f, 0.898f, 1.0f};  // --line-strong #C3CAD5
  kHeader = {0.906f, 0.918f, 0.941f, 1.0f};         // --surface-3 #E7EAF0
  kHeaderHovered = {0.831f, 0.855f, 0.898f, 1.0f};  // --line-strong #C3CAD5
  kSurface2 = {1.0f, 1.0f, 1.0f, 1.0f};             // --surface-2 #FFFFFF
  kLineStrong = {0.765f, 0.792f, 0.835f, 1.0f};     // --line-strong #C3CAD5
}
// Radii live in ui_tokens.h (no ImGui dependency, so the suite can pin
// them): the redesign v2 system is panel 10 / control 7 / badge pill.
using soar::app::kPanelRounding;
using soar::app::kControlRounding;
using soar::app::kPillRounding;
using soar::app::kUiFontSize;
}  // namespace theme

// Poster placeholder (proto.css .thumb / .ph-glyph :215-224). This is the
// CONTENT layer, which stays deep in both themes — so it is a pair of fixed
// literals, deliberately NOT theme tokens. #14171e ground, #3a4250 glyph.
static const ImVec4 kThumbBg{0.078f, 0.090f, 0.118f, 1.0f};    // #14171e
static const ImVec4 kThumbGlyph{0.227f, 0.259f, 0.314f, 1.0f}; // #3a4250

void applyTheme() {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = theme::kPanelRounding;
  st.PopupRounding = theme::kPanelRounding;
  st.FrameRounding = theme::kControlRounding;
  st.GrabRounding = theme::kControlRounding;
  st.WindowBorderSize = 1.0f;
  st.WindowPadding = ImVec2(12, 8);
  st.FramePadding = ImVec2(8, 6);
  st.ItemSpacing = ImVec2(8, 6);

  using theme::kAccent;
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = theme::kText;
  c[ImGuiCol_TextDisabled] = theme::kTextDim;
  c[ImGuiCol_WindowBg] = theme::kPanelBg;
  c[ImGuiCol_PopupBg] = theme::kPopupBg;
  c[ImGuiCol_Border] = theme::kBorder;
  c[ImGuiCol_FrameBg] = theme::kFrameBg;
  c[ImGuiCol_FrameBgHovered] = theme::kFrameBgHovered;
  c[ImGuiCol_FrameBgActive] = theme::kFrameBgActive;
  c[ImGuiCol_TitleBg] = c[ImGuiCol_WindowBg];
  c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
  c[ImGuiCol_Button] = theme::kButton;
  c[ImGuiCol_ButtonHovered] = theme::kButtonHovered;
  c[ImGuiCol_ButtonActive] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = kAccent;
  c[ImGuiCol_Header] = theme::kHeader;
  c[ImGuiCol_HeaderHovered] = theme::kHeaderHovered;
  c[ImGuiCol_HeaderActive] = kAccent;
  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_Separator] = theme::kBorder;
  c[ImGuiCol_TextSelectedBg] = kAccent;
}

// <state>/soar/theme.txt — one line, "light" or "dark". Same XDG ladder the
// recent/library/shortcut stores use; absent or unrecognised means dark (the
// default the app ships with).
std::string themePrefPath() {
  const char* dir = nullptr;
#ifdef _WIN32
  dir = std::getenv("APPDATA");
  if (dir && *dir) return std::string(dir) + "\\soar\\theme.txt";
  return "soar-theme.txt";
#else
  dir = std::getenv("XDG_STATE_HOME");
  if (dir && *dir) return std::string(dir) + "/soar/theme.txt";
  const char* home = std::getenv("HOME");
  if (home && *home) return std::string(home) + "/.local/state/soar/theme.txt";
  return "soar-theme.txt";
#endif
}

// Load the saved palette before the first applyTheme(). Called once at init.
void loadThemePref() {
  std::ifstream in(themePrefPath());
  std::string word;
  if (in >> word) theme::applyPalette(word == "light");
}

// Persist the current palette (called after a ToggleTheme).
void saveThemePref() {
  const std::string path = themePrefPath();
  // Best-effort: mirror the parent dir the other stores create before write.
  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  out << (theme::g_light ? "light" : "dark") << "\n";
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

// Whether an embedded subtitle track's codec carries text the exporter can
// demux out (decoder names, exactly as mediaInfo() spells them via
// codecNameFromCodecId). Bitmap subtitles (pgssub/dvdsub/dvbsub) and exotic
// codecs get no [translate] row — exportSubtitleText would only ever fail.
static bool embeddedTextSubtitleCodec(const std::string& codec) {
  static const char* const kTextCodecs[] = {"subrip", "srt",  "ass",
                                            "ssa",    "movtext", "mov_text",
                                            "webvtt", "text"};
  for (const char* name : kTextCodecs) {
    if (codec == name) {
      return true;
    }
  }
  return false;
}

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
  for (const std::string& path : fontCandidates()) {
    if (fileExists(path)) {
      fonts.size_16 = io.Fonts->AddFontFromFileTTF(path.c_str(),
                                                   theme::kUiFontSize);
      fonts.size_20 = io.Fonts->AddFontFromFileTTF(path.c_str(), 20.0f);
      fonts.size_24 = io.Fonts->AddFontFromFileTTF(path.c_str(), 24.0f);
      fonts.size_28 = io.Fonts->AddFontFromFileTTF(path.c_str(), 28.0f);
      fonts.size_32 = io.Fonts->AddFontFromFileTTF(path.c_str(), 32.0f);
      fonts.size_36 = io.Fonts->AddFontFromFileTTF(path.c_str(), 36.0f);
      fonts.size_48 = io.Fonts->AddFontFromFileTTF(path.c_str(), 48.0f);
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
// Both paths clear the v2 font floor (ui_tokens.h): the UI loads
// theme::kUiFontSize, and ImGui's embedded default is 13px — above
// soar::app::kMinFontSize.
void loadOverlayFont() {
  ImGuiIO& io = ImGui::GetIO();
  if (std::getenv("SOAR_UI_BITMAP_FONT") != nullptr) {
    io.Fonts->AddFontDefault();
    return;
  }
  for (const std::string& path : fontCandidates()) {
    if (fileExists(path)) {
      if (io.Fonts->AddFontFromFileTTF(path.c_str(), theme::kUiFontSize) !=
          nullptr)
        return;
    }
  }
  io.Fonts->AddFontDefault();
}

// The library's MetadataSource (ui/library.h) over the core scraper: derives
// the query the scraper wants from the media path, and folds the answer into
// the library's scrape shape. It lives here, not in the core, because picking
// a metadata service is an app choice - the library only sees the interface.
class LibraryMetadataSource : public MetadataSource {
 public:
  explicit LibraryMetadataSource(const soar::HttpMetadataScraper& scraper)
      : scraper_(scraper) {}

  bool scrape(const std::string& path, NfoInfo& out) const override {
    const std::string query = scrapeQueryForPath(path);
    if (query.empty()) return false;  // nothing to search on
    soar::ScrapedMetadata answer;
    if (!scraper_.scrape(query, &answer, nullptr)) return false;
    out.title = std::move(answer.title);
    out.plot = std::move(answer.plot);
    return !out.title.empty() || !out.plot.empty();
  }

 private:
  const soar::HttpMetadataScraper& scraper_;
};

// The player HUD: OSC, overlays, toast, and app-level input routing. Every
// action goes through the Player facade — no backend knowledge (docs §5.2).
class PlayerHud {
 public:
  PlayerHud(soar::Player& player, const WindowUiConfig& cfg, SDL_Window* window)
      : player_(player), cfg_(cfg), window_(window), recent_(cfg.recent_path),
        current_uri_(cfg.initial_uri), source_label_(cfg.source_label),
        torrent_(cfg.torrent), torrent_pending_(cfg.torrent != nullptr),
        cast_location_(cfg.cast_location),
        cast_transcode_(cfg.cast_transcode),
        recent_dirs_(defaultRecentDirsPath(), 8),
        library_(cfg.library),
        subtitle_fonts_(loadSubtitleFonts()) {
    // Every toast lands on the CLI event trail (main.cpp prints the
    // player-side events; the UI side mirrors its own here) so the
    // walkthrough tests assert OSD feedback from the teed stderr alone.
    st_.toast.mirror = [](const std::string& text) {
      fmt::print(stderr, "event: toast={}\n", text);
    };
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
    external_.configure(std::move(sub));
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
  }

  // Watched-to-end bookkeeping: the final record before the window goes
  // away. The player is still open here (runPlayerWindow stops it after
  // the HUD block ends), so position()/state() are live.
  ~PlayerHud() {
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

  // ---- App-drawn keyboard focus (V7b, docs/ui-design.md §5.2「焦点可见」).
  // ImGui nav stays off — §3.2's rebindable shortcut map keeps every key —
  // so the ring is plain state moved by keys nobody else owns: Tab
  // (+Shift backward) on the playlist/library overlays (unbound in §3.2)
  // and arrows inside a popup menu, where the map already stands aside
  // (handleEvent returns above while a popup is open). Enter replays the
  // focused row through its own click path on the next draw. Row counts
  // are re-published every frame by the drawing side, so focus can never
  // point past a list that shrank between frames (clamp in drawOverlays).
  UiFocus ui_focus_;
  int playlist_rows_ = 0;        // queue rows the last playlist draw saw
  int library_rows_ = 0;         // source rows the last library draw saw
  int menu_rows_ = 0;            // rows drawn so far in the open popup
  int menu_count_ = 0;           // rows the open popup had last frame
  bool blur_text_next_ = false;  // Tab left the library search box's caret

  // The ring: 2px accent drawn 4px outside the focused row (the
  // prototype's :focus-visible; geometry pinned in ui_tokens.h). Call
  // right after the row's widget so GetItemRect* still reads that row.
  void drawFocusRing() {
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(
        ImVec2(lo.x - kFocusRingExpand, lo.y - kFocusRingExpand),
        ImVec2(hi.x + kFocusRingExpand, hi.y + kFocusRingExpand),
        ImGui::GetColorU32(theme::kAccent), 0.0f, 0, kFocusRingWidth);
  }

  // Per-row protocol for the popup menus: read before the widget (did
  // Enter pick this row?), ring+count after it. One pair per focusable
  // row site keeps the index order identical to the visual order.
  bool menuRowEnter() {
    return ui_focus_.takesActivation(UiFocus::Face::MenuItems, menu_rows_);
  }
  void menuRowRing() {
    if (ui_focus_.holds(UiFocus::Face::MenuItems, menu_rows_)) drawFocusRing();
    ++menu_rows_;
  }

  // Arrows/Tab/Enter inside an open combo popup. handleEvent stands aside
  // there, so these keys are nobody else's — the arrows are not dual-role
  // (outside a popup they keep their §3.2 seek/volume meaning).
  void focusMenuKey(SDL_Keycode key, bool shift) {
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

  // Tab/Enter on the playlist/library overlays. Returns true when the key
  // was consumed.
  bool focusOverlayKey(SDL_Keycode key, bool shift) {
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
        std::string uri(e.drop.file ? e.drop.file : "");
        if (e.drop.file) SDL_free(e.drop.file);
        if (!uri.empty()) openSource(uri);
        return;
      }
      default:
        return;
    }
  }

  // P4b-4 async torrent open: while the stream is Connecting the poster
  // (below) shows the wait; on Serving the window opens the bridge URL —
  // the same path a synchronous open would have taken, now from the UI
  // thread — and on Failed the error becomes a toast (first line only:
  // lastError can carry a bounded file table). The raw magnet/.torrent URI
  // never opens here; only the bridge URL does.
  void handlePendingTorrent(milliseconds now) {
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

  // Per frame: animate the HUD alpha, then emit the UI draws. Must be
  // called between ImGui::NewFrame and ImGui::Render.
  void drawUi(milliseconds now, bool video_active) {
    handlePendingTorrent(now);
    handleCastTick(now);
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
    const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 0.016f;
    const float target = hud_shown ? 1.0f : 0.0f;
    st_.hud_alpha += (target - st_.hud_alpha) * std::min(1.0f, dt / 0.160f);
    st_.hud_alpha = std::clamp(st_.hud_alpha, 0.0f, 1.0f);

    if (!video_active) drawPoster();
    drawPauseOverlay();
    if (st_.hud_alpha > 0.01f) drawHud(now);
    drawChips();
    drawSubtitles(now);
    drawToast(now);
    drawOverlays();
  }

  void recordOpen(const std::string& uri) {
    if (uri.empty()) return;
    current_uri_ = uri;
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

  // Jumps back to the saved position of `uri` when the resume policy
  // (resumePositionMs in library.h) says it is worth it. True when the
  // jump happened — the caller lets a "Resumed at m:ss" toast replace its
  // own, the jump is the more informative story.
  bool applyResume(const std::string& uri, milliseconds now) {
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

  // Torrent bridge URLs are session-scoped: display-labeled, never
  // history-keyed.
  bool isTorrentBridge(const std::string& uri) const {
    return torrent_ != nullptr && uri == torrent_->playbackUrl();
  }

  // The library's write half of the frame loop (the read half is
  // applyResume at open time): a throttled progress write while playing,
  // and the watched-to-end clear when a source runs into Ended. Ordered
  // before the playlist advance in drawUi so the clearing frame still
  // knows the finished source as current.
  void maybeRecordProgress(milliseconds now) {
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

  // Source name as displayed (poster, toasts, playlist): P2P sources stream
  // from a 127.0.0.1 bridge whose URL reads badly, so cfg_.source_label —
  // the streamed torrent file's name — replaces it for the current entry;
  // everything else keeps the plain base name.
  std::string displayName(const std::string& uri) const {
    return (!source_label_.empty() && uri == current_uri_) ? source_label_
                                                           : baseName(uri);
  }

  // The card grid's source chip: the path's extension uppercased (MKV, MP4,
  // WEBM...), or "" for a path with no usable extension (a bare host URL).
  static std::string fileFormatLabel(const std::string& uri) {
    const std::string name = baseName(uri);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
  }

  // The card grid's sub line when nothing is resumable: the entry's parent
  // folder leaf (the watched root it came from), or "" if it has no parent.
  static std::string parentFolderLabel(const std::string& uri) {
    const std::string name = baseName(uri);
    const std::size_t slash = name.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0) return "";
    return name.substr(0, slash);
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

  // The remote catalog for the current media, searched at most once per
  // uri while a menu is open (an open menu redraws every frame; the answer
  // must not re-query the server each time). Unconfigured means the search
  // returns empty without any network activity.
  const std::vector<SubtitleCandidate>& remoteCandidates() {
    if (!remote_searched_ || searched_uri_ != current_uri_) {
      remote_searched_ = true;
      searched_uri_ = current_uri_;
      remote_candidates_ = external_.findCandidates(MediaSource{current_uri_, {}});
    }
    return remote_candidates_;
  }

  // Sidecar subtitle candidates (docs/mvp.md §6): what a SubtitleProvider
  // finds next to the media. Shared by the subtitle combo and the Subtitle
  // Settings overlay; both call it only while their menu is open, because
  // enumerating means reading a directory. Picking one loads it as a real
  // subtitle track (its id sits past the container's stream range, so it
  // shows up in the track list like any other) and selects it.
  //
  // Already-loaded sidecars are not offered again — the track above carries
  // them, and loading is idempotent by path anyway. When a remote endpoint
  // is configured, its candidates follow as a "[download]" section: picking
  // one downloads, stores under the system temp and loads the stored file
  // through the same pipeline as a sidecar pick.
  // Loaded external tracks a "[translate]" row can work on (docs/mvp.md §6
  // "字幕文本翻译", the window wiring): the window knows their source paths
  // because it loaded them, each has not been translated this session, and
  // an endpoint is configured. Embedded text streams follow as the
  // "内嵌轨先导出" slice: their row carries no path, and picking one demuxes
  // the stream out to plain SubRip first (exportSubtitleText). Bitmap and
  // unknown-codec streams get no row that can only fail. A subtitle track
  // whose title is not a loaded external path is embedded by construction —
  // loadExternalSubtitle titles every external track after its file and the
  // window records exactly those titles.
  std::vector<std::pair<const TrackInfo*, const std::string*>> translatableTracks(
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

  void drawSidecarEntries(const MediaInfo& info, milliseconds now) {
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

  // The translation path behind a "[translate]" pick: read the track's
  // source file, batch-translate it, store and load the result like any
  // sidecar. Every failure degrades to a toast — the media keeps playing
  // and the original track is untouched (the translator is all-or-nothing,
  // so no half-translated track can appear). Like downloadAndLoad this
  // runs on the UI thread: the loopback fixture answers in milliseconds,
  // and the timeout env bounds a slow endpoint.
  void translateAndLoad(const std::string& title, const std::string& path,
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

  // The embedded-track variant (docs/mvp.md §6 "内嵌轨先导出"): demux the
  // stream out to plain SubRip first, then run the same translate-and-load
  // tail. The export pass reopens the source independently, so the playing
  // decode path is never touched; it is synchronous (quick for local and
  // loopback sources), which is why the pick accepts the pause.
  void exportTranslateAndLoad(const TrackInfo& track, milliseconds now) {
    std::string srt;
    if (!player_.exportSubtitleText(track.id, srt) || srt.empty()) {
      st_.toast.show("Translation failed", now);
      return;
    }
    if (translateTextAndLoad(track.title, srt, SubtitleFormat::SubRip, now)) {
      translated_ids_.push_back(track.id);
    }
  }

  // Shared tail of both paths: batch-translate, store under the system
  // temp, load and select. Records the stored path so the new track itself
  // becomes translatable-external, and reports the result — false means a
  // toast was shown and nothing changed.
  bool translateTextAndLoad(const std::string& title, const std::string& text,
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

  // The title a loaded external track carries in the track list: the
  // backend names it after the file it loaded (ffmpeg_backend's
  // loadExternalSubtitle). Keying loaded_external_paths_ the same way
  // makes a menu row resolvable back to its source path by title alone.
  static std::string externalTrackTitle(const std::string& path) {
    return std::filesystem::path(path).filename().string();
  }

  // The download path behind a "[download]" pick: fetch the text, persist
  // it under the system temp and load it like any sidecar. Every failure
  // degrades to a toast — the media keeps playing (SubtitleProvider
  // contract, docs/mvp.md §6).
  void downloadAndLoad(const SubtitleCandidate& c, milliseconds now) {
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

  // First-run window (BUGS.md #3): the GUI entry can start with no media
  // at all (--gui, the installed shortcuts, a double-clicked soarw.exe).
  // The poster is then the open entry rather than a playback state. The
  // async torrent wait also runs source-less, but it has its own poster
  // state (connecting...), so it stays out of this predicate.
  bool firstRunEmpty() const {
    return current_uri_.empty() && !torrent_pending_;
  }

  // Open-a-file entry: the Windows common dialog picks a path and rides
  // the same openSource() path as a drop; elsewhere (SDL2 ships no dialog
  // API) the attempt degrades to a toast that names what does work.
  void openFilePicker() {
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

  // Open-a-folder entry (v0.2.1): the in-app directory picker. Starts at
  // the most recent folder, falling back to $HOME (then the cwd) so the
  // first use still lands somewhere browsable.
  void openDirPicker() {
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

  // Same picker, library mode: the action button becomes "Watch this
  // folder" (docs/mvp.md §3) and the pick lands back in the Library
  // overlay instead of the queue.
  void openDirPickerWatch() {
    openDirPicker();
    dirpick_for_library_ = true;
  }

  // Same picker, file mode (todo.md 原型对齐批): the rows gain file
  // checkboxes and the action button adds the checked set to the queue —
  // the picker for when a folder import is more than the user wanted.
  void openDirPickerFiles() {
    openDirPicker();
    dirpick_files_ = true;
    dirpick_checked_.clear();
  }

  // The file-mode action: queue the checked files in the order they were
  // ticked (a pick across directories keeps the user's order, not an
  // alphabetical one), then hand the user back to the Playlist overlay
  // that sent them here — the same shape importDirectory and
  // watchDirectory end on.
  void addCheckedFiles(milliseconds now) {
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

  // Watch action from the picker's library mode: the folder joins the
  // watched set, an immediate scan lands its files in the store, and the
  // user is handed back to the Library overlay that sent them here.
  void watchDirectory(const std::string& dir, milliseconds now) {
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

  // Media library browse (docs/mvp.md §3): the first open of a session
  // walks the watched folders once — a big tree must not be re-scanned on
  // every toggle; after that the Rescan button owns the refresh.
  void openLibrary() {
    if (library_ != nullptr && !library_scanned_) {
      library_->scan();
      library_->save();
      library_scanned_ = true;
    }
    toggleOverlay(Overlay::Library);
  }

  // ---- Casting (docs/mvp.md §3): the DLNA control-point entry ----
  //
  // Ownership: every start attempt constructs a fresh CastSession (the
  // component's start() demands the Idle phase, so a failed attempt can
  // never restart in place — a new object is the retry). Discovery runs on
  // a plain background thread so the 3s M-SEARCH window never freezes the
  // UI; the session itself is always started from the UI thread with a
  // direct location, which keeps CastSession's owner-thread contract.

  void openCast() { toggleOverlay(Overlay::Cast); }

  // One discovery burst in the background; results land under cast_mu_.
  void beginCastSearch() {
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
      soar::cast::discoverRenderers(3000, &found, &err, nullptr);
      std::lock_guard<std::mutex> lock(cast_mu_);
      cast_found_ = std::move(found);
      cast_searching_.store(false, std::memory_order_release);
    });
  }

  // Joins the discovery thread (bounded by the component's 3s window).
  void endCastSearch() {
    if (cast_discover_thread_.joinable()) cast_discover_thread_.join();
  }

  // Starts a cast toward the renderer description at `location`. One
  // session at a time: a fresh attempt stops and replaces the previous one.
  void startCastTo(const std::string& location, milliseconds now) {
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

  void stopCast(milliseconds now) {
    if (!cast_active_) return;
    cast_active_->stop();
    st_.toast.show("Cast stopped", now);
  }

  // Pause/Resume at the renderer. A rejected action is surfaced: the user
  // asked for something the receiver refused, and the button must not read
  // as if it had worked.
  void pauseCast(milliseconds now) {
    if (!cast_active_) return;
    if (cast_active_->pause()) st_.toast.show("Cast paused", now);
    else st_.toast.show("Pause rejected: " + cast_active_->lastError(), now);
  }

  void resumeCast(milliseconds now) {
    if (!cast_active_) return;
    if (cast_active_->resume()) st_.toast.show("Cast resumed", now);
    else st_.toast.show("Resume rejected: " + cast_active_->lastError(), now);
  }

  // Per-frame: the pre-armed --cast request and the auto-pick handoff.
  // The auto request waits for the discovery burst to finish, then casts
  // to the first answer (or toasts the silence).
  void handleCastTick(milliseconds now) {
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

  void drawCastOverlay() {
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

  // Settings panel (docs/public/prototypes/redesign/settings.html, the「通用」
  // page — one of the four prototype pages). Each row is a proto `.set`: a
  // label block (title + description) with a control on the right. The
  // switches edit live session state (esc_exits_fullscreen / remember_last_
  // media) rather than being decorative; the theme select drives the same
  // applyPalette path the y toggle uses.
  void drawSettingsOverlay() {
    bool open = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 340), ImGuiCond_Appearing);
    if (ImGui::Begin("Settings", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::TextUnformatted("General");  // chead h1 通用
      ImGui::Separator();
      ImGui::TextDisabled("%s", "Startup & interface");  // sect h2 启动与界面

      // Row: 记住上次媒体 — resume on open. A switch (proto .switch).
      ImGui::Checkbox("Remember last media", &st_.remember_last_media);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", "Restore the last file and position on startup");
      }

      // Row: ESC 退出全屏 — gate the Escape action's leave-fullscreen half.
      ImGui::Checkbox("ESC exits fullscreen", &st_.esc_exits_fullscreen);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          "Only in fullscreen; in-window ESC closes settings");
      }

      // Row: 界面语言 — a select. ASCII-only build: the shipped label set is
      // English (the bitmap font has no CJK), so this reflects the runtime
      // palette in use and lets the user flip it here too.
      ImGui::TextUnformatted("Theme:");
      ImGui::SameLine();
      const char* theme_items[] = {"Dark", "Light"};
      int theme_idx = theme::g_light ? 1 : 0;
      if (ImGui::Combo("##theme", &theme_idx, theme_items, 2)) {
        theme::applyPalette(theme_idx == 1);
        applyTheme();
        saveThemePref();
      }
    }
    ImGui::End();
    if (!open) st_.overlay = Overlay::None;
  }


  // Folder import: every media file under `dir` (recursive per the picker
  // toggle) joins the queue; an empty queue starts playing the first one —
  // importing into a fresh window should leave something on screen, while
  // importing into a running session is append-only. The folder joins the
  // recent-dirs MRU either way (it was picked, even if it held no media).
  void importDirectory(const std::string& dir, milliseconds now) {
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

  // Reopen path for Recent/DnD; Player::open closes existing media first
  // (the FFmpeg backend re-enters cleanly — verified in its open()). Both
  // are user-initiated opens, so the source also joins the playlist as the
  // new current entry (mpv semantics: a dropped/queued file plays now).
  void openSource(const std::string& uri) {
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

  // Playlist navigation (docs/mvp.md §2). Opens entry `index` from the
  // queue without re-adding it — add() is for user-initiated opens.
  // Returns whether the entry actually opened (the import flow needs to
  // know so its success toast does not bury an "Open failed").
  bool openPlaylistEntry(std::size_t index, milliseconds now) {
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

  void playlistStep(bool backward, milliseconds now) {
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

  // Fires once per Ended: the queue's next entry opens (Loop::One replays
  // the finished one, Loop::Off stops when the round is exhausted). The
  // edge flag resets as soon as playback leaves Ended, so a later manual
  // replay (space at the end) does not double-advance.
  void maybeAdvancePlaylist(milliseconds now) {
    if (player_.state() != PlaybackState::Ended) {
      st_.playlist_end_handled = false;
      return;
    }
    if (st_.playlist_end_handled) return;
    st_.playlist_end_handled = true;
    const std::size_t index = playlist_.advance();
    if (index != PlaylistStore::kNone) openPlaylistEntry(index, now);
  }

  void setFullscreen(bool on) {
    st_.fullscreen = on;
    SDL_SetWindowFullscreen(window_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  }

  void toggleOverlay(Overlay which) {
    st_.overlay = st_.overlay == which ? Overlay::None : which;
  }

  // A-B loop (v0.2): L cycles arm-A -> arm-B -> clear. Point A is held in
  // the UI state because the backend only knows armed pairs; the B press
  // validates the pair through the player (so A >= B or B > duration land
  // as a toast instead of a silent no-op).
  void cycleLoopAB(milliseconds now) {
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

  // Screenshot (v0.2, mpv's `s`): PNG of the last presented frame in the
  // working directory. The dereference needs the full backend type, which
  // only exists in FFmpeg builds (player_window.h keeps it opaque), and
  // the null backend has no frames to capture at all.
  void takeScreenshot(milliseconds now) {
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

  bool shiftHeld() const { return (SDL_GetModState() & KMOD_SHIFT) != 0; }

  // ---- Drawing ----

  // The bar's rect, mirrored from drawHud below: bar_w = min(w - 24, 920),
  // bar_h = 80, centered, 16px above the bottom edge. The click handler
  // needs this instead of ImGui::GetIO().WantCaptureMouse: that flag is
  // decided at the previous frame's end from the mouse position then in
  // effect, so a pointer that warped onto the bar in the same frame as
  // the press still reports "not over a widget" for the new position and
  // the click both fires the button and toggles the OSC. The geometry is
  // exact; it only counts while the bar is actually drawn.
  bool mouseOverHud() const {
    const ImGuiIO& io = ImGui::GetIO();
    const float bar_w = std::min(io.DisplaySize.x - 24.0f, 920.0f);
    const float bar_h = 80.0f;
    const float x0 = (io.DisplaySize.x - bar_w) * 0.5f;
    const float y0 = io.DisplaySize.y - bar_h - 16.0f;
    return io.MousePos.x >= x0 && io.MousePos.x < x0 + bar_w &&
           io.MousePos.y >= y0 && io.MousePos.y < y0 + bar_h;
  }

  // Paused: dim the video and center a round 64px accent play button with
  // a resume hint (the prototype's paused overlay, redesign/desktop-main.html).
  // This replaces the old Paused chip — the pill carries the state name. The
  // dim is input-transparent; only the button takes clicks (click = resume,
  // same as Space). Drawn before the HUD so the bar, chips and canvas
  // subtitles stay crisp above the dim.
  //
  // Window order matters here and is counterintuitive: ImGui 1.91 pushes a
  // new NoBringToFrontOnFocus window to the FRONT of g.Windows, and g.Windows
  // is drawn front-to-back — so the LAST of these three Begin() calls lands at
  // the BACK of the stack. The dim is therefore begun last even though it is
  // the first thing seen; reordering these blocks puts the button under the
  // dim (verified by pixel probe: a 38%-black rect over the accent reads a
  // dimmed accent, not the full-strength face).
  void drawPauseOverlay() {
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

  void drawHud(milliseconds now) {
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
      if (ImGui::Button("Info")) toggleOverlay(Overlay::Info);
      ImGui::SameLine();
      if (ImGui::Button(st_.fullscreen ? "Window" : "Full")) {
        setFullscreen(!st_.fullscreen);
      }
      ImGui::SameLine();
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

  void drawRateCombo(milliseconds now) {
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

  void drawChips() {
    // Buffering chip while the backend reports a stall (P1 events);
    // download chip while the disk cache is still filling (P3c events).
    // (The Paused chip is gone: the paused overlay's pill carries the
    // state name — one indicator, not two.) All fade with the theme,
    // never block input.
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
    // Cast chip while a DLNA session is live (docs/mvp.md §3): the TV
    // plays even when this window is minimized, so the state stays visible.
    if (cast_active_ &&
        cast_active_->phase() == soar::cast::CastSession::Phase::Casting) {
      chip("##cast", "Casting", 56.0f);
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

  // Subtitle state
  std::string current_subtitle;
  std::chrono::milliseconds subtitle_pts{0};
  std::chrono::milliseconds subtitle_duration{0};

  void chip(const char* name, const char* text, float y) {
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
      bg.w = 0.6f;  // the alpha SetNextWindowBgAlpha used to carry
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

  // The open-folder picker (v0.2.1). In-app on purpose: it carries the
  // recursive-import toggle and the recent-dirs shortcuts, which native
  // dialogs cannot, and it renders identically on every platform. A click
  // on a row descends into that subdirectory, ".." climbs out, and the
  // import button acts on the browsed directory itself. The media count
  // under the browsed directory is cached per (path, recursive) pair — a
  // deep tree must not be re-walked on every frame while the dialog is up.
  void drawDirPickOverlay() {
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

  // The play queue (docs/mvp.md §2). Rows show the playing entry with a
  // ">" marker; a row click jumps to it (the overlay stays open so the
  // list can be stepped through), the trailing "x" removes the entry —
  // removing the playing one only re-points the queue, playback runs on.
  // Header buttons: the two pickers (files, then a whole folder) and the
  // loop/shuffle toggles; no keys are spent on them (L is the A-B loop,
  // and mpv leaves shuffle unbound too). "Clear queue" wipes the list in
  // two clicks: the first arms it, the second (inside the arm window)
  // executes — one click at one button's width away from a destroyed
  // queue is too little intent for a store with no undo.
  void drawPlaylistOverlay() {
    bool open = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 340), ImGuiCond_Appearing);
    if (ImGui::Begin("Playlist", &open, ImGuiWindowFlags_NoSavedSettings)) {
      // The arm expires: a user who armed it and walked away must not
      // destroy the queue with their next visit's stray click.
      if (playlist_clear_armed_ &&
          nowMs() - playlist_clear_armed_at_ > std::chrono::seconds(4)) {
        playlist_clear_armed_ = false;
      }
      if (ImGui::SmallButton("Add files...")) {
        openDirPickerFiles();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Open folder...")) {
        openDirPicker();
      }
      ImGui::SameLine();
      const char* loop_label = playlist_.loop() == PlaylistStore::Loop::Off
                                   ? "Loop: off"
                                   : playlist_.loop() == PlaylistStore::Loop::All
                                       ? "Loop: all"
                                       : "Loop: one";
      if (ImGui::SmallButton(loop_label)) {
        playlist_.cycleLoop();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton(playlist_.shuffle() ? "Shuffle: on"
                                                 : "Shuffle: off")) {
        playlist_.setShuffle(!playlist_.shuffle());
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(playlist_.empty());
      if (ImGui::SmallButton(playlist_clear_armed_ ? "Clear: sure?"
                                                   : "Clear queue")) {
        if (playlist_clear_armed_) {
          playlist_.clear();
          playlist_clear_armed_ = false;
          st_.toast.show("Playlist cleared", nowMs());
        } else {
          playlist_clear_armed_ = true;
          playlist_clear_armed_at_ = nowMs();
        }
      }
      ImGui::EndDisabled();
      ImGui::Separator();
      if (playlist_.empty()) {
        ImGui::TextDisabled("%s", "Playlist is empty — opened files queue here.");
      }
      // Snapshot before iterating: a pick or a remove rewrites the store's
      // vectors, and iterating the very vectors those calls mutate leaves
      // the loop's index dangling (the Recent overlay learned this first).
      const std::vector<std::string> entries = playlist_.entries();
      const std::vector<DirGroup> groups = groupByDirectory(entries);
      // V7b: the focus face indexes every queue entry exactly once (tree
      // leaves included), so the cycle covers the same rows a mouse sees.
      playlist_rows_ = static_cast<int>(entries.size());
      if (groups.size() > 1) {
        // Folder tree (v0.2.1): one node per parent directory, first
        // occurrence order, expanded on first sight (Cond_Once — the user
        // can collapse nodes and the choice sticks for the session). A
        // node title shows the folder name with its queue count; the full
        // path is the tooltip. "Play group" starts at the group's first
        // queue entry, which walks the rest of the group in order.
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
        // Zero or one folder: a tree node would only add a click between
        // the user and the single run of entries, so keep the flat list.
        for (std::size_t i = 0; i < entries.size(); ++i) {
          drawPlaylistRow(entries, i);
        }
      }
    }
    ImGui::End();
    if (!open) st_.overlay = Overlay::None;
  }

  // One queue row: current marker, display name, full-source tooltip, and
  // the trailing remove button. A row click jumps to the entry (the
  // overlay stays open so the list can be stepped through).
  void drawPlaylistRow(const std::vector<std::string>& entries, std::size_t i) {
    std::string label = i == playlist_.current() ? "> " : "   ";
    label += displayName(entries[i]);
    // AllowOverlap: the Selectable fills the row width and would otherwise
    // hold the hover over the SameLine "x" button, whose clicks would never
    // land (hover goes to the first claimant).
    // Enter replays on the focused row (V7b); the ring reads the row's
    // rect before the tooltip can steal the last-item pointer.
    const bool enter = ui_focus_.takesActivation(
        UiFocus::Face::PlaylistRows, static_cast<int>(i));
    if (ImGui::Selectable(label.c_str(), i == playlist_.current(),
                          ImGuiSelectableFlags_AllowOverlap) ||
        enter) {
      openPlaylistEntry(i, nowMs());
    }
    if (ui_focus_.holds(UiFocus::Face::PlaylistRows, static_cast<int>(i))) {
      drawFocusRing();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entries[i].c_str());
    ImGui::SameLine();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      if (playlist_.remove(i)) {
        st_.toast.show("Removed " + displayName(entries[i]), nowMs());
      }
    }
    ImGui::PopID();
  }

  // The media library browse (docs/mvp.md §3): watched folders on top,
  // then the category buckets beside the rows they select. A row click
  // plays the source (openSource applies the resume jump); the trailing
  // "x" forgets a source / unwatches a folder. Snapshots before
  // iterating: a click rewrites the store (recordOpen re-sorts entries_,
  // an unwatch mutates folders_) and the loop must not walk a vector
  // being mutated — the Recent overlay learned this first.
  void drawLibraryOverlay() {
    bool open = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
    if (ImGui::Begin("Library", &open, ImGuiWindowFlags_NoSavedSettings)) {
      if (library_ == nullptr) {
        ImGui::TextDisabled("%s", "No library in this session.");
      } else {
        if (ImGui::SmallButton("Add folder...")) openDirPickerWatch();
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) {
          library_->scan();
          library_->save();
        }
        // The network half of the scrape (docs/mvp.md §3): only offered
        // with an endpoint configured, so an offline session never shows a
        // button that could only fail. Each click is capped (the scrape
        // budget) and every attempt costs up to the configured timeout, same
        // UI-thread precedent as the subtitle download. The SameLine rides
        // inside the gate: an unconditional one would pull the "Watched
        // folders" caption onto the button row and lift every row below it
        // one pitch (the walkthrough's bucket pins mis-clicked Resume for
        // Recent that way — BUGS.md #7).
        if (meta_ready_) {
          ImGui::SameLine();
          if (ImGui::SmallButton("Scrape")) {
            const std::size_t n = library_->scrapeEntries(MediaLibrary::kScrapeBudget);
            st_.toast.show(
                n == 1 ? "Scraped 1 entry" : "Scraped " + std::to_string(n) + " entries",
                nowMs());
          }
        }
        const std::vector<std::string> folders = library_->folders();
        if (folders.empty()) {
          ImGui::TextDisabled("%s", "No folders watched yet - add one to scan it.");
        } else {
          ImGui::TextDisabled("Watched folders (%zu)", folders.size());
          for (std::size_t i = 0; i < folders.size(); ++i) {
            ImGui::PushID(static_cast<int>(1000 + i));
            // The row itself is not a pick target — only the "x" acts on a
            // folder row (unwatch); the full path is the tooltip.
            ImGui::Selectable(folders[i].c_str(), false,
                              ImGuiSelectableFlags_AllowOverlap);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", folders[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
              library_->removeFolder(folders[i]);
              library_->save();
            }
            ImGui::PopID();
          }
          ImGui::Separator();
        }
        // The browse split (docs/todo.md 原型对齐批): the buckets read the
        // entries' state, the rows obey. The bucket is session state, so
        // reopening the overlay lands where the user left off.
        const std::vector<LibraryEntry> entries = library_->entries();
        drawLibraryBuckets(entries);
        ImGui::SameLine();
        drawLibraryEntries(entries, !folders.empty());
      }
    }
    ImGui::End();
    if (!open) st_.overlay = Overlay::None;
  }

  // The bucket column: one row per category, each showing its live count
  // over the same snapshot the row side renders. All is the default and
  // the unfiltered view — with it selected the overlay reads exactly like
  // the pre-sidebar layout.
  void drawLibraryBuckets(const std::vector<LibraryEntry>& entries) {
    static const char* const kLabels[] = {"All", "Recent", "Resume", "Finished"};
    ImGui::BeginChild("library-buckets", ImVec2(110, 0));
    for (int i = 0; i < 4; ++i) {
      const LibraryCategory cat = static_cast<LibraryCategory>(i);
      std::size_t count = 0;
      for (const LibraryEntry& e : entries) {
        if (matchesCategory(e, cat)) ++count;
      }
      const std::string label =
          std::string(kLabels[i]) + " (" + std::to_string(count) + ")";
      if (ImGui::Selectable(label.c_str(), st_.library_category == cat)) {
        st_.library_category = cat;
      }
    }
    ImGui::EndChild();
  }

  // The row side of the split: the selected bucket's entries. Only Recent
  // reorders (compareRecent — newest content first); the rest keep the
  // store's path order. The remove button right-aligns to the column's
  // edge, so one x fits every row regardless of the label's length (the
  // X11 drive pins a single coordinate).
  void drawLibraryEntries(const std::vector<LibraryEntry>& entries, bool watching) {
    std::vector<const LibraryEntry*> shown;
    shown.reserve(entries.size());
    for (const LibraryEntry& e : entries) {
      if (!matchesCategory(e, st_.library_category)) continue;
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
    // The search box (docs/todo.md 原型对齐批): it narrows the bucket's
    // rows, so the two halves compose — the sidebar filters by state, the
    // box by text. It lives INSIDE this child on purpose: it shares the
    // buckets' line, and a line's height is its tallest item (the buckets
    // child takes all of it), so a box drawn next to that child would
    // push everything after it past the window edge. SetKeyboardFocusHere
    // has to precede the item it targets, so the / action's one-shot flag
    // is consumed on the frame the box is drawn.
    ImGui::BeginChild("library-entries", ImVec2(0, 0));
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
    ImGui::InputTextWithHint("##library-filter", "Filter title or path ( / )",
                             st_.library_filter, sizeof(st_.library_filter));
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
    // Card grid (docs/public/prototypes/redesign/library.html .grid/.card
    // :63-92, the v0.3 redesign set). Auto-fill columns of ~196px cards, a
    // 16px gutter, each card a 16:9 poster over a meta block. The poster is
    // the placeholder thumb (kThumbBg/kThumbGlyph — the content layer stays
    // deep in both themes) with a source-format pill top-left and a centre
    // play badge on hover. The whole card is one hit target (open), with the
    // remove affordance as a small x in the poster's top-right corner.
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
      // Card frame: surface-2 fill, 1px border (line → line-strong on hover),
      // panel radius. (proto.css .card :69-74)
      const float rad = static_cast<float>(theme::kPanelRounding);
      dl->AddRectFilled(rmin, rmax, u_card, rad);
      dl->AddRect(rmin, rmax, hovered ? u_edge : u_line, rad, 0, 1.0f);
      // Poster: the 16:9 placeholder thumb + a centred media glyph
      // (proto.css .thumb/.ph-glyph :215-224; the glyph mirrors the
      // prototype's rounded-rect-with-play-triangle SVG).
      const ImVec2 pmin(rmin.x + 1.0f, rmin.y + 1.0f);
      const ImVec2 pmax(rmax.x - 1.0f, rmin.y + poster_h);
      dl->AddRectFilled(pmin, pmax, ImGui::ColorConvertFloat4ToU32(kThumbBg),
                        rad, ImDrawFlags_RoundCornersTop);
      const ImVec2 gc((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
      const ImU32 u_glyph = ImGui::ColorConvertFloat4ToU32(kThumbGlyph);
      dl->AddRect(ImVec2(gc.x - 13.0f, gc.y - 9.0f),
                  ImVec2(gc.x + 13.0f, gc.y + 9.0f), u_glyph, 2.5f, 0, 1.4f);
      dl->AddTriangleFilled(ImVec2(gc.x - 3.0f, gc.y - 4.0f),
                            ImVec2(gc.x - 3.0f, gc.y + 4.0f),
                            ImVec2(gc.x + 5.0f, gc.y), u_glyph);
      // Source-format pill, top-left (proto.css .card .poster .kind :76 —
      // a .pill.net: net-soft ground, net ink). The format is the card's
      // source chip.
      const std::string fmt = fileFormatLabel(e.path);
      if (!fmt.empty()) {
        const float pill_h = 18.0f;
        const ImVec2 tsize = ImGui::CalcTextSize(fmt.c_str());
        const ImVec2 ppmin(pmin.x + 8.0f, pmin.y + 8.0f);
        const ImVec2 ppmax(ppmin.x + tsize.x + 12.0f, ppmin.y + pill_h);
        dl->AddRectFilled(ppmin, ppmax, IM_COL32((u_net & 0xFF),
                                                 ((u_net >> 8) & 0xFF),
                                                 ((u_net >> 16) & 0xFF), 31),
                          pill_h * 0.5f);
        dl->AddText(ImVec2(ppmin.x + 6.0f, ppmin.y + (pill_h - tsize.y) * 0.5f),
                    u_net, fmt.c_str());
      }
      // Remove affordance, top-right of the poster: a small x that reads as
      // part of the poster chrome (click zones are resolved below).
      dl->AddText(ImVec2(pmax.x - 18.0f, pmin.y + 6.0f), u_text3, "x");
      // Centre play badge on hover (proto.css .card .play-badge :86-92).
      if (hovered) {
        const ImVec2 bc(gc.x, gc.y);
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

  void drawHelpOverlay() {
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
    // Trail edge latch for the OSC's logical visibility.
    bool hud_was_shown = true;
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

    // A-B loop arming state (v0.2): the backend only knows armed pairs,
    // so point A waits here until the L press that supplies B.
    bool loop_a_set = false;
    std::chrono::milliseconds loop_a_ms{0};

    // Playlist continuation edge flag: set when an Ended frame has been
    // serviced, cleared on the first frame that is not Ended.
    bool playlist_end_handled = false;

    // Library browse bucket (docs/todo.md 原型对齐批): the sidebar keeps
    // its selection across closing the overlay, so the list lands where
    // the user left off.
    LibraryCategory library_category = LibraryCategory::All;
    // The search box's needle, which survives closing the overlay too —
    // the user's narrowed view comes back as they left it. A char buffer
    // instead of a std::string on purpose: ImGui's string overload lives
    // in imgui_stdlib.cpp, a translation unit this app does not build.
    char library_filter[64] = {};
    // The one-shot caret request the / action leaves behind; the draw
    // consumes it on the frame the box is drawn.
    bool library_search_focus = false;
    // Settings panel (proto redesign/settings.html「通用」): live toggles the
    // Settings overlay edits. esc_exits_fullscreen gates the Escape action's
    // leave-fullscreen half; remember_last_media gates the resume-on-open jump.
    bool esc_exits_fullscreen = true;
    bool remember_last_media = true;
  };

  static constexpr milliseconds kProgressInterval{5000};

  soar::Player& player_;
  const WindowUiConfig& cfg_;
  SDL_Window* window_ = nullptr;
  RecentStore recent_;
  // Play queue (docs/mvp.md §2): CLI positionals seed it, user opens
  // append to it, and Ended walks it. Pure model in ui_state.h.
  PlaylistStore playlist_;
  std::string current_uri_;
  std::string source_label_;  // display-only override (P2P bridge sources)
  // Async P2P (P4b-4): when WindowUiConfig::torrent is non-null, the HUD
  // polls phase() each frame and opens the bridge URL on Serving.
  soar::p2p::TorrentStream* torrent_ = nullptr;
  bool torrent_pending_ = false;
  // Casting (docs/mvp.md §3): the pre-armed --cast request, one attempt at
  // a time (a fresh CastSession per start — the component cannot restart a
  // failed session), the background discovery burst, and the position
  // mirror the session's poll thread writes for the overlay.
  std::string cast_location_;
  bool cast_transcode_ = false;  // the cast overlay's transcode switch
  bool cast_auto_done_ = false;
  bool cast_discover_started_ = false;
  std::unique_ptr<soar::cast::CastSession> cast_active_;
  std::mutex cast_mu_;
  std::vector<soar::cast::DiscoveredRenderer> cast_found_;
  std::atomic<bool> cast_searching_{false};
  std::atomic<std::int64_t> cast_pos_ms_{-1};
  std::atomic<std::int64_t> cast_dur_ms_{-1};
  std::thread cast_discover_thread_;
  // Open-folder flow (v0.2.1): recent directories MRU (smaller cap than
  // files), the picker's browse position, and the recursive-import choice
  // (kept for the session so repeated imports share one setting).
  RecentStore recent_dirs_;
  // Rebindable keyboard shortcuts (docs/ui-design.md §3.2): defaults in
  // the ctor, then the config file / CLI flags via WindowUiConfig.
  ShortcutMap shortcuts_;
  std::string dirpick_path_;
  bool dirpick_recursive_ = false;
  // Picker mode: false imports the picked folder into the queue, true
  // watches it into the media library (the "Add folder..." entry).
  bool dirpick_for_library_ = false;
  // Third mode (todo.md 原型对齐批): file picking. Media rows gain
  // checkboxes and the action button queues the checked set — click
  // order kept, across directory changes too.
  bool dirpick_files_ = false;
  std::vector<std::string> dirpick_checked_;
  // The clear button's two-click confirm (see drawPlaylistOverlay): armed
  // on the first click, executed on the second, disarmed by the arm's
  // own expiry.
  bool playlist_clear_armed_ = false;
  milliseconds playlist_clear_armed_at_{0};
  // The library's once-per-session auto-scan (first overlay open).
  bool library_scanned_ = false;
  // Cached "how many media files are under the browsed folder" — re-walked
  // only when the picker's path or recursive toggle changes, never per frame.
  std::size_t dirpick_count_ = 0;
  bool dirpick_count_valid_ = false;
  std::string dirpick_counted_path_;
  bool dirpick_counted_recursive_ = false;
  // Media library (docs/mvp.md §3): the resume-point store, the throttle
  // clock for its progress writes, and the watched-to-end edge flag.
  MediaLibrary* library_ = nullptr;
  milliseconds last_progress_write_{-kProgressInterval};
  bool library_end_cleared_ = false;
  SubtitleFonts subtitle_fonts_;
  // Enumerates sidecar subtitle files next to the media (§6). Stateless and
  // cheap, so it lives with the HUD rather than in the backend: the directory
  // is only walked while a menu that offers sidecars is open.
  SidecarSubtitleProvider sidecar_;
  // Remote subtitle download (§6), env-configured; empty endpoint means it
  // stays offline. The catalog is searched once per media item and cached,
  // and downloaded titles are remembered so the menu stops offering them.
  ExternalSubtitleProvider external_;
  std::string searched_uri_;
  std::vector<SubtitleCandidate> remote_candidates_;
  bool remote_searched_ = false;
  std::vector<std::string> downloaded_titles_;
  // Subtitle text translation (§6). loaded_external_paths_ carries the
  // source path of every external track this window loaded (keyed by the
  // track title the menu shows), so a [translate] row can read the text
  // without the track list having to carry paths; translated_titles_
  // keeps the menu from re-offering an external track it already
  // translated, and translated_ids_ does the same for embedded tracks
  // (their container titles can repeat, so they key by track id).
  SubtitleTranslator translator_;
  // Library network scraping (§3). The scraper is env-configured like the two
  // subtitle providers; meta_ready_ false means no endpoint was configured and
  // the library stays offline. metadata_source_ is the adapter the library
  // holds - non-owning, pointing at the scraper above.
  soar::HttpMetadataScraper metadata_;
  LibraryMetadataSource metadata_source_{metadata_};
  bool meta_ready_ = false;
  std::map<std::string, std::string> loaded_external_paths_;
  std::vector<std::string> translated_titles_;
  std::vector<TrackId> translated_ids_;
  std::string target_language_ = "English";
  bool translate_ready_ = false;
  State st_;
};

#endif  // SOAR_WITH_IMGUI

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
