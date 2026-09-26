// The windowed player UI (docs/ui-design.md): SDL2 window + YUV video
// texture + a Dear ImGui overlay drawn with the same renderer (the
// imgui_impl_sdlrenderer2 backend), driven entirely through the Player
// facade so the UI layer stays swappable (§5.2).

#ifndef SOAR_APP_PLAYER_WINDOW_H_
#define SOAR_APP_PLAYER_WINDOW_H_

#include <atomic>
#include <cstdint>
#include <string>

namespace soar {
class Player;
class FFmpegBackend;  // opaque here; only player_window.cpp dereferences it
}

namespace soar::app {

struct WindowUiConfig {
  std::string title = "soar";
  std::string initial_uri;     // the CLI-opened source; recorded + shown
  std::string backend_label;   // "ffmpeg" / "null", shown in the info overlay
  std::string cache_dir;       // shown in the info overlay when set
  std::string recent_path;     // RecentStore file (docs/ui-design.md §2)
  // Video frames come from the FFmpeg backend (null when it is not in use).
  soar::FFmpegBackend* ffmpeg = nullptr;
  // Set by the Player event callback (backend thread) on
  // BufferingStarted/Ended; the UI polls it each frame.
  std::atomic<bool>* buffering = nullptr;
  // Mirrors the DownloadProgress event payload (P3c): bytes cached out of
  // total source bytes while playing through the disk cache. total == 0
  // means no active download; the chip hides once bytes == total.
  std::atomic<std::uint64_t>* download_bytes = nullptr;
  std::atomic<std::uint64_t>* download_total = nullptr;

  // Subtitle rendering configuration (v0.2 basics: font/size/offset).
  // These are owned by the caller and live for the window lifetime.
  std::atomic<float>* subtitle_font_size = nullptr;      // default 24.0
  std::atomic<int>* subtitle_offset_ms = nullptr;        // sync offset in ms
  std::atomic<bool>* subtitle_visible = nullptr;         // show/hide toggle
};

// Runs the window loop until the user quits (Esc outside fullscreen /
// window close / Q). Returns the process exit code. Only called when the
// SDL2 window layer is compiled in; without ImGui the loop degrades to the
// bare video + Esc window.
int runPlayerWindow(soar::Player& player, const WindowUiConfig& cfg);

}  // namespace soar::app

#endif  // SOAR_APP_PLAYER_WINDOW_H_
