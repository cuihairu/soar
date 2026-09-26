#include "soar/core/player.h"

// fmt/format.h (not core.h): fmt::print lives here across the fmt 9.x
// (system packages) and 11.x (vcpkg) range we build against.
#include <fmt/format.h>

#ifdef SOAR_WITH_FFMPEG
#  include "soar/core/ffmpeg_backend.h"
#endif

#ifdef SOAR_WITH_SDL2
#  include "ui/player_window.h"
#  include "ui/ui_state.h"
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <string>

static void print_usage(const char* argv0) {
  fmt::print("Usage:\n");
  fmt::print("  {} [--backend=ffmpeg|null] <path-or-url>\n\n", argv0);
  fmt::print("  {} --headless [--backend=ffmpeg|null] <path-or-url>\n\n", argv0);
  fmt::print("Options:\n");
  fmt::print("  --headless    Run without GUI\n");
  fmt::print("  --backend=    Select backend (ffmpeg, null)\n");
  fmt::print("  --cache-dir=  Cache http:// downloads here for offline replay\n");
#ifdef SOAR_WITH_FFMPEG
  fmt::print("\nFFmpeg backend is available.\n");
#else
  fmt::print("\nFFmpeg backend not available (compiled without FFmpeg support).\n");
#endif
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 2;
  }

  bool headless = false;
  std::string backend_type = "null";  // default to null backend
  std::string cache_dir;
  int uri_index = -1;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--headless") {
      headless = true;
    } else if (arg.rfind("--backend=", 0) == 0) {
      backend_type = arg.substr(10);  // after "--backend="
    } else if (arg.rfind("--cache-dir=", 0) == 0) {
      cache_dir = arg.substr(12);  // after "--cache-dir="
    } else if (arg.rfind('-', 0) == 0) {
      fmt::print(stderr, "Unknown option: {}\n", arg);
      print_usage(argv[0]);
      return 2;
    } else {
      // This should be the URI
      uri_index = i;
      break;
    }
  }

  if (uri_index < 0) {
    print_usage(argv[0]);
    return 2;
  }

  // Create backend based on selection
  std::unique_ptr<soar::IBackend> backend;
#ifdef SOAR_WITH_FFMPEG
  soar::FFmpegBackend* ffmpeg_backend = nullptr;
#endif

  if (backend_type == "ffmpeg") {
#ifdef SOAR_WITH_FFMPEG
    backend = soar::makeFFmpegBackend();
    ffmpeg_backend = dynamic_cast<soar::FFmpegBackend*>(backend.get());
    fmt::print(stderr, "Using FFmpeg backend\n");
#else
    fmt::print(stderr, "FFmpeg backend requested but not available\n");
    fmt::print(stderr, "Falling back to null backend\n");
    backend = soar::makeNullBackend();
#endif
  } else if (backend_type == "null") {
    backend = soar::makeNullBackend();
    fmt::print(stderr, "Using null backend\n");
  } else {
    fmt::print(stderr, "Unknown backend type: {}\n", backend_type);
    fmt::print(stderr, "Available backends: null");
#ifdef SOAR_WITH_FFMPEG
    fmt::print(stderr, ", ffmpeg");
#endif
    fmt::print(stderr, "\n");
    return 2;
  }

  soar::Player player(std::move(backend));
  // Mirrors the BufferingStarted/Ended event pair for the window UI's
  // buffering chip (backend threads set it; the window loop polls it).
  std::atomic<bool> buffering{false};
  // Mirrors the DownloadProgress payload for the download chip (P3c):
  // total == 0 means no active download; the chip hides at bytes == total.
  std::atomic<std::uint64_t> download_bytes{0};
  std::atomic<std::uint64_t> download_total{0};
  // Subtitle rendering configuration (v0.2 basics)
  std::atomic<float> subtitle_font_size{24.0f};
  std::atomic<int> subtitle_offset_ms{0};
  std::atomic<bool> subtitle_visible{true};
  player.setEventCallback([&player, &buffering, &download_bytes,
                           &download_total](const soar::Event& e) {
    if (e.type == soar::EventType::StateChanged) {
      fmt::print(stderr, "event: state={}\n", static_cast<int>(e.state));
      if (e.state == soar::PlaybackState::Error ||
          e.state == soar::PlaybackState::Ended) {
        buffering.store(false, std::memory_order_relaxed);
        download_total.store(0, std::memory_order_relaxed);
      }
    } else if (e.type == soar::EventType::MediaInfoChanged) {
      const auto info = player.mediaInfo();
      fmt::print(
        stderr,
        "event: media-info duration={}ms seekable={} tracks={} selected(video={},audio={},sub={})\n",
        info.duration.count(),
        info.seekable,
        info.tracks.size(),
        info.selected_video,
        info.selected_audio,
        info.selected_subtitle
      );
    } else if (e.type == soar::EventType::PositionChanged) {
      fmt::print(stderr, "event: position={}ms\n", e.position.count());
    } else if (e.type == soar::EventType::BufferingStarted) {
      fmt::print(stderr, "event: buffering started\n");
      buffering.store(true, std::memory_order_relaxed);
    } else if (e.type == soar::EventType::BufferingEnded) {
      fmt::print(stderr, "event: buffering ended\n");
      buffering.store(false, std::memory_order_relaxed);
    } else if (e.type == soar::EventType::DownloadProgress) {
      // Payload rides in message as "downloaded/total" decimal bytes
      // (Event must not grow fields — backend.h / coverage-notes §3.8).
      unsigned long long have = 0, total = 0;
      if (std::sscanf(e.message.c_str(), "%llu/%llu", &have, &total) == 2) {
        fmt::print(stderr, "event: download {}% ({}/{})\n",
                   total > 0 ? have * 100 / total : 0, have, total);
        download_bytes.store(have, std::memory_order_relaxed);
        download_total.store(total, std::memory_order_relaxed);
      }
    } else if (e.type == soar::EventType::Error) {
      fmt::print(stderr, "event: error={}\n", e.message);
    }
  });

  const std::string uri(argv[uri_index]);
  if (!player.open(soar::MediaSource{uri, cache_dir})) {
    fmt::print(stderr, "Failed to open source: {}\n", uri);
    fmt::print(stderr, "Error: {}\n", player.lastError());
    return 1;
  }
  player.play();

  const auto info = player.mediaInfo();
  fmt::print(stderr, "\n=== Media Info ===\n");
  fmt::print(stderr, "Duration: {}s\n", info.duration.count() / 1000.0);
  fmt::print(stderr, "Seekable: {}\n", info.seekable ? "yes" : "no");
  fmt::print(stderr, "Tracks: {}\n", info.tracks.size());
  for (const auto& track : info.tracks) {
    fmt::print(stderr, "  [{}] {} - {} ({})\n",
      static_cast<int>(track.type),
      track.id,
      track.codec,
      track.title
    );
  }
  fmt::print(stderr, "\n");

  if (headless) {
    (void)player.seek(std::chrono::seconds(1));
    (void)player.pause();
    (void)player.stop();

    const auto info2 = player.mediaInfo();
    auto first_subtitle = std::find_if(
      info2.tracks.begin(),
      info2.tracks.end(),
      [](const soar::TrackInfo& t) { return t.type == soar::TrackType::Subtitle; }
    );
    if (first_subtitle != info2.tracks.end()) {
      (void)player.selectTrack(soar::TrackType::Subtitle, first_subtitle->id);
      (void)player.disableSubtitles();
    }
    return 0;
  }

#ifdef SOAR_WITH_SDL2
  // The interactive window UI (docs/ui-design.md): SDL2 + ImGui overlay.
  soar::app::WindowUiConfig ui_cfg;
  ui_cfg.title = "soar";
  ui_cfg.initial_uri = uri;
  ui_cfg.backend_label = backend_type;
  ui_cfg.cache_dir = cache_dir;
  ui_cfg.recent_path = soar::app::defaultRecentPath();
#  ifdef SOAR_WITH_FFMPEG
  ui_cfg.ffmpeg = ffmpeg_backend;
#  endif
  ui_cfg.buffering = &buffering;
  ui_cfg.download_bytes = &download_bytes;
  ui_cfg.download_total = &download_total;
  ui_cfg.subtitle_font_size = &subtitle_font_size;
  ui_cfg.subtitle_offset_ms = &subtitle_offset_ms;
  ui_cfg.subtitle_visible = &subtitle_visible;
  return soar::app::runPlayerWindow(player, ui_cfg);
#else
  fmt::print(stderr, "Window support not compiled in; use --headless.\n");
  return 0;
#endif
}
