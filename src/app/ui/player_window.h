// The windowed player UI (docs/ui-design.md): SDL2 window + YUV video
// texture + a Dear ImGui overlay drawn with the same renderer (the
// imgui_impl_sdlrenderer2 backend), driven entirely through the Player
// facade so the UI layer stays swappable (§5.2).

#ifndef SOAR_APP_PLAYER_WINDOW_H_
#define SOAR_APP_PLAYER_WINDOW_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace soar {
class Player;
class FFmpegBackend;  // opaque here; only player_window.cpp dereferences it
}

namespace soar::p2p {
class TorrentStream;  // opaque here; only player_window.cpp polls it
}

namespace soar::app {

struct WindowUiConfig {
  std::string title = "soar";
  std::string initial_uri;     // the CLI-opened source; recorded + shown
  // Extra CLI positionals (mpv queue semantics): appended to the playlist
  // after initial_uri, which stays the playing entry.
  std::vector<std::string> queued_uris;
  std::string backend_label;   // "ffmpeg" / "null", shown in the info overlay
  std::string cache_dir;       // shown in the info overlay when set
  std::string recent_path;     // RecentStore file (docs/ui-design.md §2)
  // Display-only override for the source name shown in the poster and the
  // window title (P2P: the streamed file's name instead of the bridge URL).
  // Empty = derive from the URI as before.
  std::string source_label;
  // Async P2P source (docs/mvp.md §5 P4b-4): non-null when the torrent was
  // started with startAsync() and the window must poll phase() — it opens
  // the bridge URL itself on Serving and toasts the error on Failed.
  // Null for ordinary and synchronous sources; the caller owns the stream,
  // which outlives the window.
  soar::p2p::TorrentStream* torrent = nullptr;
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
