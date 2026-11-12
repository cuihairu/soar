// PlayerHud: the windowed player UI class plus the shared preamble
// helpers (state names, theme, preference IO, subtitle fonts) it and
// player_window.cpp use. Split out of player_window.cpp; included by
// player_window.cpp and the player_hud_*.cpp translation units only.

#ifndef SOAR_APP_PLAYER_HUD_H_
#define SOAR_APP_PLAYER_HUD_H_

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
#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#  include "torrent_stream.h"
#endif
#ifdef SOAR_WITH_FFMPEG
#  include "soar/core/ffmpeg_backend.h"
#endif
#ifdef SOAR_WITH_IMGUI
#  include "cast_session.h"
#  include "renderer.h"
#  include "imgui.h"
#  include "imgui_internal.h"
#  include "backends/imgui_impl_sdl2.h"
#  include "backends/imgui_impl_sdlrenderer2.h"
#endif

namespace soar::app {

using milliseconds = std::chrono::milliseconds;


inline milliseconds nowMs() {
  return std::chrono::duration_cast<milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
}

inline const char* stateName(PlaybackState s) {
  switch (s) {
    case PlaybackState::Stopped: return "Stopped";
    case PlaybackState::Paused: return "Paused";
    case PlaybackState::Playing: return "Playing";
    case PlaybackState::Ended: return "Ended";
    case PlaybackState::Error: return "Error";
  }
  return "?";
}

inline const char* trackTypeName(TrackType t) {
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

// The settings page's nav tabs (proto settings.html): General, Playback and
// Subtitles form the first group, Network and About their own (the .grp
// separators). Playback/About have no content in the v1 prototype, so they
// render a minimal placeholder.
enum class SettingsTab { General, Playback, Subtitles, Network, About };

// Letterbox destination: the frame scaled to fit, centered (docs §4 — the
// video area is always aspect-correct on the theme background).


// System font candidates, in order: Linux Noto/DejaVu, macOS, Windows.
// Shared by the ImGui overlay fonts and the libass default font (both
// compilations of this TU — with and without ImGui — reach the latter).
inline std::vector<std::string> fontCandidates() {
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

inline bool fileExists(const std::string& path) {
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}


// Everything below needs ImGui types (theme colors, the HUD class).
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
inline ImVec4 kText{0.933f, 0.945f, 0.961f, 1.0f};             // #EEF1F5 text-1
inline ImVec4 kTextDim{0.725f, 0.753f, 0.796f, 1.0f};          // #B9C0CB text-2
inline ImVec4 kTextMuted{0.545f, 0.580f, 0.639f, 1.0f};        // #8B94A3 text-3
// Single accent lock (proto.css --orange-500 + --ink-on-accent): the play /
// interactive accent. Hover is --accent-hover; the ink on accent buttons is
// the warm near-black, not the neutral one. Accent + its ink are identical in
// both themes (the orange is the same orange on light and dark).
inline ImVec4 kAccent{1.0f, 0.420f, 0.208f, 1.0f};             // #FF6B35 orange-500
inline ImVec4 kAccentHover{1.0f, 0.490f, 0.302f, 1.0f};        // #FF7D4D accent-hover
inline ImVec4 kInkOnAccent{0.110f, 0.051f, 0.016f, 1.0f};      // #1C0D04 ink-on-accent
inline ImVec4 kDanger{1.0f, 0.361f, 0.400f, 1.0f};             // #FF5C66 red-500
inline ImVec4 kOk{0.243f, 0.812f, 0.431f, 1.0f};               // #3ECF6E ok-500
inline ImVec4 kWarn{1.0f, 0.698f, 0.141f, 1.0f};               // #FFB224 warn-500
// Network / P2P semantic (proto.css --sky-net): the one place sky-blue stays.
inline ImVec4 kNet{0.424f, 0.694f, 1.0f, 1.0f};                // #6CB1FF sky-net
// The opaque page background behind the video (theme::kPanelBg is the
// translucent OSC face over the video; this is the SDL clear colour, i.e.
// surface-0). Dark #0A0B0E, light #EEF0F4.
inline ImVec4 kWindowBg{0.039f, 0.043f, 0.055f, 1.0f};         // #0A0B0E surface-0
inline ImVec4 kPanelBg{0.043f, 0.047f, 0.059f, 0.88f};         // #0A0B0E oscbg over video
inline ImVec4 kPopupBg{0.10f, 0.11f, 0.15f, 0.98f};
inline ImVec4 kBorder{1.0f, 1.0f, 1.0f, 0.10f};                // --oline rgba(255,255,255,.10)
inline ImVec4 kFrameBg{1.0f, 1.0f, 1.0f, 0.10f};
inline ImVec4 kFrameBgHovered{1.0f, 1.0f, 1.0f, 0.16f};
inline ImVec4 kFrameBgActive{1.0f, 1.0f, 1.0f, 0.20f};
inline ImVec4 kButton{1.0f, 1.0f, 1.0f, 0.10f};
inline ImVec4 kButtonHovered{1.0f, 1.0f, 1.0f, 0.18f};
inline ImVec4 kHeader{1.0f, 1.0f, 1.0f, 0.08f};
inline ImVec4 kHeaderHovered{1.0f, 1.0f, 1.0f, 0.14f};
// Card surfaces (proto.css --surface-2 card bg + --line-strong hover edge):
// the library grid's tiles sit on surface-2 and light their border to
// line-strong on hover. surface-2 is opaque (it fully covers the panel).
inline ImVec4 kSurface2{0.086f, 0.098f, 0.125f, 1.0f};         // #161920 surface-2 (dark)
inline ImVec4 kLineStrong{0.212f, 0.243f, 0.294f, 1.0f};       // #363E4B line-strong (dark)
// The v1 prototype pages (docs/public/prototypes/*.html, proto.css :root) —
// the single visual baseline for the settings/library/playlist/translate
// pages and the main-window recheck. v1 palette differs from the redesign
// v2 surface tokens above in the page/card/text tiers, so the page draws
// use these; the OSC keeps the v2 tokens it was pixel-probed against.
inline ImVec4 kPageBg{0.055f, 0.063f, 0.075f, 1.0f};           // #0E1013 v1 --bg1
inline ImVec4 kNavBg{0.031f, 0.035f, 0.043f, 1.0f};            // #08090B v1 --bg0
inline ImVec4 kCardBg{0.075f, 0.086f, 0.106f, 1.0f};           // #13161B v1 --bg2
inline ImVec4 kCardLine{0.141f, 0.169f, 0.208f, 1.0f};         // #242B35 v1 --line
inline ImVec4 kText2{0.635f, 0.671f, 0.722f, 1.0f};            // #A2ABB8 v1 --t2
inline ImVec4 kText3{0.431f, 0.471f, 0.525f, 1.0f};            // #6E7886 v1 --t3
inline ImVec4 kText4{0.302f, 0.333f, 0.376f, 1.0f};            // #4D5560 v1 --t4
inline ImVec4 kAccentHi{1.0f, 0.522f, 0.329f, 1.0f};           // #FF8554 v1 --accent-hi
// Field faces (v1 --bg3 / --bg4): the select boxes, stepper buttons, the
// nav hover fill and the switch's off state all sit one tier above the
// card ground.
inline ImVec4 kFieldBg{0.102f, 0.118f, 0.145f, 1.0f};          // #1A1E25 v1 --bg3
inline ImVec4 kFieldBgHi{0.133f, 0.153f, 0.184f, 1.0f};        // #22272F v1 --bg4

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
  // The v1 page tokens flip onto the same light ladder the v2 tokens use
  // (#EEF0F4 / #F7F8FA / #FFFFFF); accents and accent-hi are theme-invariant.
  kPageBg = {0.969f, 0.973f, 0.980f, 1.0f};         // #F7F8FA v1 --bg1 (light)
  kNavBg = {0.933f, 0.941f, 0.957f, 1.0f};          // #EEF0F4 v1 --bg0 (light)
  kCardBg = {1.0f, 1.0f, 1.0f, 1.0f};               // #FFFFFF v1 --bg2 (light)
  kCardLine = {0.875f, 0.890f, 0.918f, 1.0f};       // #DFE3EA v1 --line (light)
  kText2 = {0.271f, 0.306f, 0.361f, 1.0f};          // #454E5C v1 --t2 (light)
  kText3 = {0.384f, 0.424f, 0.490f, 1.0f};          // #626C7D v1 --t3 (light)
  kText4 = {0.596f, 0.631f, 0.686f, 1.0f};          // #98A1AF v1 --t4 (light)
  kAccentHi = {1.0f, 0.522f, 0.329f, 1.0f};         // #FF8554 accent-hi (same)
  kFieldBg = {0.906f, 0.918f, 0.941f, 1.0f};        // #E7EAF0 v1 --bg3 (light)
  kFieldBgHi = {0.765f, 0.792f, 0.835f, 1.0f};      // #C3CAD5 v1 --bg4 (light)
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
inline const ImVec4 kThumbBg{0.078f, 0.090f, 0.118f, 1.0f};    // #14171e
inline const ImVec4 kThumbGlyph{0.227f, 0.259f, 0.314f, 1.0f}; // #3a4250

inline void applyTheme() {
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
inline std::string themePrefPath() {
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
inline void loadThemePref() {
  std::ifstream in(themePrefPath());
  std::string word;
  if (in >> word) theme::applyPalette(word == "light");
}

// Persist the current palette (called after a ToggleTheme).
inline void saveThemePref() {
  const std::string path = themePrefPath();
  // Best-effort: mirror the parent dir the other stores create before write.
  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  out << (theme::g_light ? "light" : "dark") << "\n";
}

// <state>/soar/settings.txt — the settings page's persisted rows beyond the
// palette. Same XDG ladder as theme.txt. Absent or unparsable falls back to
// the shipped defaults (recent cap 15, remember-last-media on, ESC-exits
// fullscreen on), so an old install never loses history over a missing row.
inline std::string settingsPrefPath() {
  const char* dir = nullptr;
#ifdef _WIN32
  dir = std::getenv("APPDATA");
  if (dir && *dir) return std::string(dir) + "\\soar\\settings.txt";
  return "soar-settings.txt";
#else
  dir = std::getenv("XDG_STATE_HOME");
  if (dir && *dir) return std::string(dir) + "/soar/settings.txt";
  const char* home = std::getenv("HOME");
  if (home && *home) return std::string(home) + "/.local/state/soar/settings.txt";
  return "soar-settings.txt";
#endif
}

// Reads one persisted "key=value" row. Missing file, unparsable value or a
// key the caller did not ask for all report `fallback` — never a partial
// value that would silently reset the setting to 0.
inline int loadSettingsInt(const char* key, int fallback) {
  std::ifstream in(settingsPrefPath());
  if (!in) return fallback;
  std::string line;
  const std::string prefix = std::string(key) + "=";
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) != 0) continue;
    const char* v = line.c_str() + prefix.size();
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    // No digits, or trailing garbage ("recent_cap=20abc"), is not a value
    // the row holds: report the fallback instead of a partial read that
    // would silently change the setting to a number the row never wrote.
    if (end == v || *end != '\0') return fallback;
    return static_cast<int>(n);
  }
  return fallback;
}

inline void saveSettingsInt(const char* key, int value) {
  // Read-modify-write one key at a time: the settings page edits rows
  // independently, and a stale overwrite of the other rows is worse than
  // dropping the file. Rewrite the whole file only on a successful read.
  std::map<std::string, std::string> rows;
  {
    std::ifstream in(settingsPrefPath());
    std::string line;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      rows[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  rows[key] = std::to_string(value);
  const std::string path = settingsPrefPath();
  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  // Atomic write (tmp + rename), the same convention as RecentStore::save
  // and HttpCache's meta: a crash or power loss between the truncate and
  // the final flush leaves the *old* file intact, never an empty or
  // half-written settings.txt that would silently drop the persisted rows.
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& [k, v] : rows) out << k << "=" << v << "\n";
    if (!out) return;
  }
#ifdef _WIN32
  std::remove(path.c_str());  // rename() fails when the target exists
#endif
  std::rename(tmp.c_str(), path.c_str());
}

// The MRU cap the settings stepper edits (RecentStore::kMaxEntries default).
// Persisted so the cap survives a restart. The stepper only admits [5, 50]
// (settingsStepper's lo/hi), so a hand-edited or legacy row outside that
// range is clamped to the nearest bound rather than becoming a live cap the
// stepper cannot display.
inline int loadRecentCap() {
  const int n = loadSettingsInt("recent_cap", RecentStore::kMaxEntries);
  constexpr int kMinCap = 5;
  constexpr int kMaxCap = 50;
  if (n < kMinCap) return kMinCap;
  if (n > kMaxCap) return kMaxCap;
  return n;
}

inline void saveRecentCap(int n) {
  saveSettingsInt("recent_cap", n >= 1 ? n : RecentStore::kMaxEntries);
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
inline bool embeddedTextSubtitleCodec(const std::string& codec) {
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

inline SubtitleFonts loadSubtitleFonts() {
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
inline void loadOverlayFont() {
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
  PlayerHud(soar::Player& player, const WindowUiConfig& cfg, SDL_Window* window,
            SDL_Renderer* renderer);


  // Watched-to-end bookkeeping: the final record before the window goes
  // away. The player is still open here (runPlayerWindow stops it after
  // the HUD block ends), so position()/state() are live.
  ~PlayerHud();


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
  void drawFocusRing();


  // Per-row protocol for the popup menus: read before the widget (did
  // Enter pick this row?), ring+count after it. One pair per focusable
  // row site keeps the index order identical to the visual order.
  bool menuRowEnter();

  void menuRowRing();


  // Arrows/Tab/Enter inside an open combo popup. handleEvent stands aside
  // there, so these keys are nobody else's — the arrows are not dual-role
  // (outside a popup they keep their §3.2 seek/volume meaning).
  void focusMenuKey(SDL_Keycode key, bool shift);


  // Tab/Enter on the playlist/library overlays. Returns true when the key
  // was consumed.
  bool focusOverlayKey(SDL_Keycode key, bool shift);


  // App-level input (docs §3). `quit` is set on the exit paths. The UI's
  // widgets have already seen the event (ImGui_ImplSDL2_ProcessEvent runs
  // first in the caller); events the UI claims are skipped here.
  void handleEvent(const SDL_Event& e, bool* quit);


  // P4b-4 async torrent open: while the stream is Connecting the poster
  // (below) shows the wait; on Serving the window opens the bridge URL —
  // the same path a synchronous open would have taken, now from the UI
  // thread — and on Failed the error becomes a toast (first line only:
  // lastError can carry a bounded file table). The raw magnet/.torrent URI
  // never opens here; only the bridge URL does.
  void handlePendingTorrent(milliseconds now);


  // Per frame: animate the HUD alpha, then emit the UI draws. Must be
  // called between ImGui::NewFrame and ImGui::Render.
  void drawUi(milliseconds now, bool video_active);


  void recordOpen(const std::string& uri);


  // Jumps back to the saved position of `uri` when the resume policy
  // (resumePositionMs in library.h) says it is worth it. True when the
  // jump happened — the caller lets a "Resumed at m:ss" toast replace its
  // own, the jump is the more informative story.
  bool applyResume(const std::string& uri, milliseconds now);


  // Torrent bridge URLs are session-scoped: display-labeled, never
  // history-keyed.
  bool isTorrentBridge(const std::string& uri) const;


  // The library's write half of the frame loop (the read half is
  // applyResume at open time): a throttled progress write while playing,
  // and the watched-to-end clear when a source runs into Ended. Ordered
  // before the playlist advance in drawUi so the clearing frame still
  // knows the finished source as current.
  void maybeRecordProgress(milliseconds now);


  // Source name as displayed (poster, toasts, playlist): P2P sources stream
  // from a 127.0.0.1 bridge whose URL reads badly, so cfg_.source_label —
  // the streamed torrent file's name — replaces it for the current entry;
  // everything else keeps the plain base name.
  std::string displayName(const std::string& uri) const;


  // The card grid's source chip: the path's extension uppercased (MKV, MP4,
  // WEBM...), or "" for a path with no usable extension (a bare host URL).
  static std::string fileFormatLabel(const std::string& uri);


  // The card grid's sub line when nothing is resumable: the entry's parent
  // folder leaf (the watched root it came from), or "" if it has no parent.
  static std::string parentFolderLabel(const std::string& uri);


 private:
  // ---- Actions (all keyboard-reachable; toasts acknowledge, docs §3.2) ----

  void togglePlayPause();


  void seekTo(milliseconds target, milliseconds now);


  void nudgeSeek(long long delta_ms, milliseconds now);


  void percentSeek(int percent, milliseconds now);


  void nudgeVolume(double delta, milliseconds now);


  void toggleMute(milliseconds now);


  // Rate steps through the same ladder the combo offers (vidstack </>).
  void nudgeRate(int step, milliseconds now);


  // The remote catalog for the current media, searched at most once per
  // uri while a menu is open (an open menu redraws every frame; the answer
  // must not re-query the server each time). Unconfigured means the search
  // returns empty without any network activity.
  const std::vector<SubtitleCandidate>& remoteCandidates();


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
      const MediaInfo& info) const;


  void drawSidecarEntries(const MediaInfo& info, milliseconds now);


  // The translation path behind a "[translate]" pick: read the track's
  // source file, batch-translate it, store and load the result like any
  // sidecar. Every failure degrades to a toast — the media keeps playing
  // and the original track is untouched (the translator is all-or-nothing,
  // so no half-translated track can appear). Like downloadAndLoad this
  // runs on the UI thread: the loopback fixture answers in milliseconds,
  // and the timeout env bounds a slow endpoint.
  void translateAndLoad(const std::string& title, const std::string& path,
                        milliseconds now);


  // The embedded-track variant (docs/mvp.md §6 "内嵌轨先导出"): demux the
  // stream out to plain SubRip first, then run the same translate-and-load
  // tail. The export pass reopens the source independently, so the playing
  // decode path is never touched; it is synchronous (quick for local and
  // loopback sources), which is why the pick accepts the pause.
  void exportTranslateAndLoad(const TrackInfo& track, milliseconds now);


  // Shared tail of both paths: batch-translate, store under the system
  // temp, load and select. Records the stored path so the new track itself
  // becomes translatable-external, and reports the result — false means a
  // toast was shown and nothing changed.
  bool translateTextAndLoad(const std::string& title, const std::string& text,
                            SubtitleFormat format, milliseconds now);


  // The title a loaded external track carries in the track list: the
  // backend names it after the file it loaded (ffmpeg_backend's
  // loadExternalSubtitle). Keying loaded_external_paths_ the same way
  // makes a menu row resolvable back to its source path by title alone.
  static std::string externalTrackTitle(const std::string& path);


  // The download path behind a "[download]" pick: fetch the text, persist
  // it under the system temp and load it like any sidecar. Every failure
  // degrades to a toast — the media keeps playing (SubtitleProvider
  // contract, docs/mvp.md §6).
  void downloadAndLoad(const SubtitleCandidate& c, milliseconds now);


  void cycleTrack(TrackType type, milliseconds now);


  // First-run window (BUGS.md #3): the GUI entry can start with no media
  // at all (--gui, the installed shortcuts, a double-clicked soarw.exe).
  // The poster is then the open entry rather than a playback state. The
  // async torrent wait also runs source-less, but it has its own poster
  // state (connecting...), so it stays out of this predicate.
  bool firstRunEmpty() const;


  // Open-a-file entry: the Windows common dialog picks a path and rides
  // the same openSource() path as a drop; elsewhere (SDL2 ships no dialog
  // API) the attempt degrades to a toast that names what does work.
  void openFilePicker();


  // Open-a-folder entry (v0.2.1): the in-app directory picker. Starts at
  // the most recent folder, falling back to $HOME (then the cwd) so the
  // first use still lands somewhere browsable.
  void openDirPicker();


  // Same picker, library mode: the action button becomes "Watch this
  // folder" (docs/mvp.md §3) and the pick lands back in the Library
  // overlay instead of the queue.
  void openDirPickerWatch();


  // Same picker, file mode (todo.md 原型对齐批): the rows gain file
  // checkboxes and the action button adds the checked set to the queue —
  // the picker for when a folder import is more than the user wanted.
  void openDirPickerFiles();


  // The file-mode action: queue the checked files in the order they were
  // ticked (a pick across directories keeps the user's order, not an
  // alphabetical one), then hand the user back to the Playlist overlay
  // that sent them here — the same shape importDirectory and
  // watchDirectory end on.
  void addCheckedFiles(milliseconds now);


  // Watch action from the picker's library mode: the folder joins the
  // watched set, an immediate scan lands its files in the store, and the
  // user is handed back to the Library overlay that sent them here.
  void watchDirectory(const std::string& dir, milliseconds now);


  // Media library browse (docs/mvp.md §3): the first open of a session
  // walks the watched folders once — a big tree must not be re-scanned on
  // every toggle; after that the Rescan button owns the refresh.
  void openLibrary();


  // ---- Casting (docs/mvp.md §3): the DLNA control-point entry ----
  //
  // Ownership: every start attempt constructs a fresh CastSession (the
  // component's start() demands the Idle phase, so a failed attempt can
  // never restart in place — a new object is the retry). Discovery runs on
  // a plain background thread so the 3s M-SEARCH window never freezes the
  // UI; the session itself is always started from the UI thread with a
  // direct location, which keeps CastSession's owner-thread contract.

  void openCast();


  // One discovery burst in the background; results land under cast_mu_.
  void beginCastSearch();


  // Joins the discovery thread (bounded by the component's 3s window).
  void endCastSearch();


  // Starts a cast toward the renderer description at `location`. One
  // session at a time: a fresh attempt stops and replaces the previous one.
  void startCastTo(const std::string& location, milliseconds now);


  void stopCast(milliseconds now);


  // Pause/Resume at the renderer. A rejected action is surfaced: the user
  // asked for something the receiver refused, and the button must not read
  // as if it had worked.
  void pauseCast(milliseconds now);


  void resumeCast(milliseconds now);


  // Per-frame: the pre-armed --cast request and the auto-pick handoff.
  // The auto request waits for the discovery burst to finish, then casts
  // to the first answer (or toasts the silence).
  void handleCastTick(milliseconds now);


  void drawCastOverlay();


  // The prototype's .switch (settings.html): a 34x20 pill toggled by a
  // click. Off = a --bg4 face with a line-strong border and a --t3 knob
  // parked left; on = accent fill, no border, the warm ink knob parked
  // right. Drawn on the draw list; the hit area is an InvisibleButton.
  bool settingsSwitch(const char* id, bool* on);


  // The prototype's .stepper: a -/+ pair around a 44px value field (the
  // .val min-width keeps the control's width stable). Buttons carry the
  // --bg3 face with a --line border and --t2 glyphs; the value prints in
  // --t1. Returns true when the value moved. Width is 24+2+44+2+24 = 96.
  bool settingsStepper(const char* id, int* val, int delta, int lo, int hi,
                       const char* fmt);


  // Runtime switch for the remote download provider (the settings page's
  // "Auto download" row): on = the env-configured endpoint (an empty one
  // stays offline — the provider never dials without a URL); off =
  // reconfigured with an empty endpoint so findCandidates()/fetch() go
  // inert immediately (docs/mvp.md §6 zero-network default).
  void configureHttpSubtitles(bool enabled);


  // The chead's Reset defaults: every live session setting returns to its
  // factory value; the persisted ones (recent cap, theme) rewrite their
  // files too, so the choice survives a restart.
  void resetSettingsDefaults();


  // The nav's 16px line icons (settings.html inlines the same five SVGs,
  // stroke-width 1.6 on a 24px grid — rescaled here to 16px). Drawn with
  // the draw list: the bitmap font carries no icon set.
  void settingsNavIcon(ImDrawList* dl, const ImVec2 c, SettingsTab tab,
                       ImU32 col);


  // The prototype's orange rounded square with the "S" initial
  // (settings.html .nav .wing and .about .logo). The face is the accent
  // (the prototype's 140deg #FF8F5F->#FF5A2A gradient reads as one orange
  // at these sizes); the bitmap font's "S" is the ink.
  void drawGradientMark(ImDrawList* dl, const ImVec2& p, float size, float r,
                        ImU32);


  // A nav tab row: a 32px strip with the tab's line icon + label, an
  // accent-soft face when active, a --bg3 fill on hover. Ink follows the
  // prototype tiering: --t3 idle, --t2 hover, --accent-hi active.
  // Returns true on a click.
  bool settingsNavRow(const char* id, SettingsTab tab, const char* label,
                      bool active);


  // The nav's .grp separator: a muted uppercase group label above the tabs
  // it heads (the prototype's 10px letter-spaced label, rendered at the
  // bitmap font's single size).
  void settingsGrp(const char* label);


  // A prototype .set row: a bordered rounded card (--bg2 face, --line
  // border, r-md 10) with the label block (title + description) on the
  // left and a fixed-width control on the right, vertically centred.
  void settingCard(const char* id, const char* title, const char* desc,
                   float control_w, float control_h,
                   const std::function<void()>& control);


  // The prototype's .ghostbtn: a 24px bordered neutral button (field
  // ground, hairline border, normal text, hover raises the face).
  bool settingsGhostButton(const char* id, const char* label, float w);


  // The General tab's appearance row: a two-option segmented control
  // (Dark / Light) bound to the theme palette — the in-app half of the
  // prototype's `?theme=` switch.
  void themeSegmented();

  // The prototype's .select: a 26px box with a line-strong border and a
  // --bg3 face. Live = clicking cycles through the caller's options (the
  // bitmap font cannot unfold a dropdown cleanly, so the box shows the
  // current value); disabled = an honest static box for the options the
  // app does not implement yet.
  bool settingsSelectBox(const char* id, const char* label, float w,
                         bool disabled);


  // The prototype's .pill: a rounded badge with a soft tinted face and the
  // matching ink (settings.html uses sky for the tracker count and ok for
  // the offline summary).
  void settingsPill(const char* label, const ImVec4& ink);


  // The prototype's .textinput: a 26px mono box (line-strong border, --bg3
  // face). Rendered read-only — the sidecar directory comes from the state
  // layout, not a per-run edit.
  void settingsTextInput(const char* value, float w);


  // The .sect h2: a muted uppercase tier heading (--t4).
  void settingsSect(const char* label);


  // The prototype's .version-row: a bordered k/v strip (r-md, --line).
  void versionRow(const char* id, const char* k, const char* v);


  // The prototype's .ep endpoint row: a tag column, a dim ellipsised value
  // and an Edit button (the settings page's honest unconfigured state).
  void endpointRow(const char* id, const char* tag, const char* value,
                   milliseconds now);


  // The full settings page (docs/public/prototypes/settings.html, v1): a
  // full-window layout — a 196px nav (--bg0, General / Playback /
  // Subtitles, then .grp NETWORK / ABOUT) beside a content column (--bg1)
  // with its chead (title + Reset defaults / Done) and a per-tab body of
  // .set rows. Every tab renders real content: the General startup rows,
  // Playback's rate/loop/shuffle, the Subtitles render/timing/fetch grid
  // + endpoints, Network's local-first rows + zero-external summary, and
  // About's mark + version rows. Controls the app does not implement yet
  // render disabled — visible per the prototype, honestly inert.
  void drawSettingsOverlay();



  // Folder import: every media file under `dir` (recursive per the picker
  // toggle) joins the queue; an empty queue starts playing the first one —
  // importing into a fresh window should leave something on screen, while
  // importing into a running session is append-only. The folder joins the
  // recent-dirs MRU either way (it was picked, even if it held no media).
  void importDirectory(const std::string& dir, milliseconds now);


  // Reopen path for Recent/DnD; Player::open closes existing media first
  // (the FFmpeg backend re-enters cleanly — verified in its open()). Both
  // are user-initiated opens, so the source also joins the playlist as the
  // new current entry (mpv semantics: a dropped/queued file plays now).
  void openSource(const std::string& uri);


  // Playlist navigation (docs/mvp.md §2). Opens entry `index` from the
  // queue without re-adding it — add() is for user-initiated opens.
  // Returns whether the entry actually opened (the import flow needs to
  // know so its success toast does not bury an "Open failed").
  bool openPlaylistEntry(std::size_t index, milliseconds now);


  void playlistStep(bool backward, milliseconds now);


  // Fires once per Ended: the queue's next entry opens (Loop::One replays
  // the finished one, Loop::Off stops when the round is exhausted). The
  // edge flag resets as soon as playback leaves Ended, so a later manual
  // replay (space at the end) does not double-advance.
  void maybeAdvancePlaylist(milliseconds now);


  void setFullscreen(bool on);


  void toggleOverlay(Overlay which);


  // A-B loop (v0.2): L cycles arm-A -> arm-B -> clear. Point A is held in
  // the UI state because the backend only knows armed pairs; the B press
  // validates the pair through the player (so A >= B or B > duration land
  // as a toast instead of a silent no-op).
  void cycleLoopAB(milliseconds now);


  void nudgeSubtitleOffset(int delta_ms, milliseconds now);


  void toggleSubtitleVisibility(milliseconds now);


  // Screenshot (v0.2, mpv's `s`): PNG of the last presented frame in the
  // working directory. The dereference needs the full backend type, which
  // only exists in FFmpeg builds (player_window.h keeps it opaque), and
  // the null backend has no frames to capture at all.
  void takeScreenshot(milliseconds now);


  bool shiftHeld() const;


  // ---- Drawing ----

  // The bar's rect, mirrored from drawHud below: bar_w = min(w - 24, 920),
  // bar_h = 80, centered, 16px above the bottom edge. The click handler
  // needs this instead of ImGui::GetIO().WantCaptureMouse: that flag is
  // decided at the previous frame's end from the mouse position then in
  // effect, so a pointer that warped onto the bar in the same frame as
  // the press still reports "not over a widget" for the new position and
  // the click both fires the button and toggles the OSC. The geometry is
  // exact; it only counts while the bar is actually drawn.
  bool mouseOverHud() const;


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
  void drawPauseOverlay();


  // Uploads a grabbed thumbnail into the dedicated SDL texture — the same
  // IYUV path presentVideoFrame uses, on its own texture so the video
  // texture and its mailbox are untouched. (Re)creates the texture when
  // the frame size changes (a different source can scale differently).
  void uploadThumbFrame(const ThumbnailFrame& f);


  // The seek-hover preview card (docs §1.6-5, the v1 thumbnail): a rounded
  // frame with the thumbnail and the hovered timecode strip, floating above
  // the rail and centered on the pointer (clamped to the window). NoInputs
  // keeps the mouse on the rail; the card is drawn on the window's draw list
  // with zero window padding so the geometry is exact.
  void drawThumbPreview(milliseconds pos, float frac, const ImVec2& rail_min,
                        const ImVec2& rail_max);


  void drawHud(milliseconds now);


  void drawRateCombo(milliseconds now);


  void drawTrackCombo(TrackType type, milliseconds now);


  // No video (audio-only / null backend): a centered "what is playing"
  // panel so the window carries content (docs §2).
  void drawPoster();


  void drawChips(milliseconds now);


  // Draw subtitle overlay: polls the backend for decoded subtitle frames
  // and renders them at the bottom of the video area.
  void drawSubtitles(milliseconds now);


  // Subtitle state
  std::string current_subtitle;
  std::chrono::milliseconds subtitle_pts{0};
  std::chrono::milliseconds subtitle_duration{0};

  void chip(const char* name, const char* text, float y);


  // The prototype's .statechip (desktop-main.html:166-169): a centered pill
  // under the top bar with a pulse dot, the state word, both clocks and —
  // while the disk cache fills — the P2P share. NoInputs: purely a status
  // readout, clicks fall through to the video beneath. Same draw-list pill
  // dance as chip() (a 999 WindowRounding would stretch the auto-resize
  // window — see chip()).
  void stateChip(const char* name, const std::string& text, const ImVec4& dot);


  // A .pill in the top bar, drawn on the draw list: 22px tall, 999 radius,
  // tinted text on a tinted ground (or a neutral white .08 ground), and the
  // small play triangle the .pill.ok carries. Returns the x after the pill
  // so the caller chains the next one.
  float topPill(ImDrawList* dl, float x, float cy, const char* text,
                const ImVec4& fg, bool tinted, bool play);


  // A 30x30 tool button with a letter glyph (the shortcut key) drawn on the
  // draw list; hover paints the prototype's rgba(255,255,255,.08) face.
  // Returns true on a click. `pos` is the absolute screen-space top-left.
  bool topTool(const char* id, char glyph, const char* tip, const ImVec2& pos);


  // The prototype's .topbar (desktop-main.html:138-164): a persistent strip
  // across the top — brand + state/media/P2P pills on the left, the four
  // tool buttons (Info/Recent/Help/Fullscreen) on the right — over a
  // black-to-transparent gradient. Info/Fullscreen moved here from the OSC
  // when the bar landed (M1): the prototype has the four tools up here, not
  // in the transport row. Always visible; buttons are mouse- and
  // keyboard-reachable (I/R/H/F).
  void drawTopBar();


  // Truncate a long label with a "..." tail so the top-bar media pill stays
  // a single short pill (the bitmap font has no ellipsis glyph).
  static std::string clipEllipsis(const std::string& s, std::size_t max_chars);


  void drawToast(milliseconds now);


  void drawOverlays();


  void drawInfoOverlay();


  void drawRecentOverlay();


  // The open-folder picker (v0.2.1). In-app on purpose: it carries the
  // recursive-import toggle and the recent-dirs shortcuts, which native
  // dialogs cannot, and it renders identically on every platform. A click
  // on a row descends into that subdirectory, ".." climbs out, and the
  // import button acts on the browsed directory itself. The media count
  // under the browsed directory is cached per (path, recursive) pair — a
  // deep tree must not be re-walked on every frame while the dialog is up.
  void drawDirPickOverlay();


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
  // The full-page playlist (docs/public/prototypes/playlist.html): a 74px
  // icon rail (logo + the four page links) over the queue column — the head
  // (title/count/note pill/Loop segmented/Shuffle/Add/Clear), the rows, and
  // the now-playing bar pinned to the bottom. The prototype's per-row
  // duration and resolution/size meta need per-entry probing the queue
  // store does not track, so rows carry the format (extension) only and no
  // duration column — documented, not a silent shortfall.
  void drawPlaylistOverlay();


  // The rail's page links (playlist.html .rail a): a 44x44 rounded-12 face,
  // --accent-soft when active, --bg3 on hover, 20px line icon centered.
  enum class RailIcon { Main, Playlist, Library, Settings };

  bool railButton(const char* id, RailIcon icon, bool active);


  void drawRailIcon(ImDrawList* dl, const ImVec2& c, RailIcon icon, ImU32 col);


  // The head (playlist.html .head): title, count, the note pill, a spacer,
  // then the Loop segmented / Shuffle / Add files / Clear cluster.
  void drawPlaylistHead();


  // The .head note pill (.pill, h22, rounded 999, --bg3, --line border).
  void drawHeadPill(const char* text);


  // A .head .btn (h30, rounded 7, --bg3 face, --line border). Returns a click.
  bool drawHeadBtn(const char* id, const char* label);


  // The .loop three-state segmented (playlist.html .head .loop): Off / All /
  // One on a 999-pill container, the active state an accent pill.
  void loopSegmented();


  // The Shuffle toggle (the prototype's .seg icon, accent when on).
  bool shuffleButton();


  // One queue row (playlist.html .row): the index (or an EQ for the current
  // entry), a 72x40 gradient thumb, the title + format meta, and the
  // hover-revealed next-play / remove actions. A row click jumps to the
  // entry (the overlay stays open so the list can be stepped through).
  void drawPlaylistRow(const std::vector<std::string>& entries, std::size_t i);


  // A 26x26 row action button (playlist.html .ra button): next-play (play
  // glyph) or remove (x glyph), --bg4 on hover.
  bool rowActionButton(ImDrawList* dl, const char* id, const ImVec2& p,
                       bool next);


  // The bottom now-playing bar (playlist.html .nowbar): thumb, title + meta,
  // the mini progress rail, the timecode, the volume and the play/pause.
  void drawPlaylistNowBar();


  // The row/now-bar meta: the format (upper-cased extension). The queue
  // store is URI-only, so this is the one honest meta it can render.
  static std::string rowMeta(const std::string& uri);


  // Ellipsize `s` to `maxw` px (the prototype's .tt text-overflow: ellipsis).
  static std::string ellipsize(const std::string& s, float maxw);


  // The media library browse (docs/mvp.md §3): watched folders on top,
  // then the category buckets beside the rows they select. A row click
  // plays the source (openSource applies the resume jump); the trailing
  // "x" forgets a source / unwatches a folder. Snapshots before
  // iterating: a click rewrites the store (recordOpen re-sorts entries_,
  // an unwatch mutates folders_) and the loop must not walk a vector
  // being mutated — the Recent overlay learned this first.
  // The sidebar row icons (library.html .side a svg, 16px, stroke ~1.6).
  enum class LibIcon { All, Recent, Resume, Finished, Folder, Globe };

  void drawLibIcon(ImDrawList* dl, const ImVec2& c, LibIcon icon, ImU32 col);


  // The full-page library (docs/public/prototypes/library.html): a 224px
  // sidebar (category + type groups with live counts) over a main column
  // whose head holds the title/count/search and whose body is the card
  // grid. The prototype's .side is 224px (wider than the settings nav's
  // 196px) and its active row is the Recent bucket — the screenshot state.
  void drawLibraryOverlay();


  // The .side .grp label (library.html .side .grp, margin 12px 8px 4px).
  void librarySideGrp(const char* label);


  // A .side a row: 32px, icon + label + right-aligned count (.cnt, mono,
  // --t4; --accent-hi when active). Returns true on a click.
  bool librarySideRow(const char* id, const char* label, std::size_t count,
                      bool active, LibIcon icon);


  // The .head: the active bucket's title, the count caption, the spacer,
  // then the search box (icon + input + "/" kbd tip) — library.html .head.
  void drawLibraryHead(const std::vector<LibraryEntry>& entries);


  // The row side of the split: the selected bucket's entries. Only Recent
  // reorders (compareRecent — newest content first); the rest keep the
  // store's path order. The remove button right-aligns to the column's
  // edge, so one x fits every row regardless of the label's length (the
  // X11 drive pins a single coordinate).
  void drawLibraryEntries(const std::vector<LibraryEntry>& entries, bool watching);


  void drawHelpOverlay();


  void drawSubtitleOverlay();


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
    // The sidebar's Type group (the prototype library page's second
    // column group): All shows both halves, so the default changes no
    // behaviour — the bucket view is exactly as before.
    LibrarySource library_source = LibrarySource::All;
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
    // Settings page (proto settings.html): the nav tab selection survives
    // closing the overlay, so the panel reopens where the user left it.
    SettingsTab settings_tab = SettingsTab::General;
    // The MRU cap the settings stepper edits (RecentStore::kMaxEntries default).
    int recent_cap = 15;
    // Settings page Subtitles tab (proto settings.html): the two live
    // fetch behaviours. autoshow turns the first selected track visible
    // without a V press; autodl gates the remote download provider (an
    // off switch reconfigures it offline — the zero-network default).
    bool subtitle_autoshow = true;
    bool subtitle_autodl = true;
    // The previous frame's selected subtitle id: the -1 -> >=0 edge is
    // the autoshow trigger (a track got selected by any path).
    int prev_selected_subtitle = -1;
  };


  static constexpr milliseconds kProgressInterval{5000};

  soar::Player& player_;
  const WindowUiConfig& cfg_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;  // thumb texture upload (docs §1.6-5)
  // Seek-hover thumbnail (docs/ui-design.md §1.6-5): the worker decodes on a
  // side thread (reopen-per-call grab), the UI uploads the latest ready
  // frame into a dedicated SDL texture. Null when no FFmpeg backend.
  std::unique_ptr<ThumbnailService> thumb_service_;
  SDL_Texture* thumb_texture_ = nullptr;
  int thumb_tex_w_ = 0;   // the texture's current size (recreated on change)
  int thumb_tex_h_ = 0;
  // The last quantized hover position requested, so a still pointer does not
  // re-ask the service every frame (its own dedupe also gates, but this
  // skips the lock round-trip).
  std::chrono::milliseconds thumb_requested_{-1};
  // The quantized position currently on screen (-1 = nothing shown yet):
  // the preview hides while a new position decodes and appears once its
  // frame is uploaded.
  std::chrono::milliseconds thumb_shown_{-1};
  // The source whose frames the cache holds (the URI recorded at the last
  // open). Every cached frame is keyed by position only, so a source switch
  // must clear the cache + texture — recordOpen does both.
  std::string thumb_source_;
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
  std::string cast_iface_;
  bool open_dialog_on_start_ = false;
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
  // The env-derived download config, kept so the settings page's "Auto
  // download" switch can re-apply (or blank) it at runtime.
  soar::HttpSubtitleConfig subtitle_env_cfg_;
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

}  // namespace soar::app

#endif  // SOAR_APP_PLAYER_HUD_H_
