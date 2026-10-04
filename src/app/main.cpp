#include "app_main.h"
#include "soar/core/player.h"
#include "startup_report.h"

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

#include "torrent_stream.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#ifndef SOAR_APP_VERSION
#  define SOAR_APP_VERSION "dev"
#endif

using soar::app::reportFatalStartupError;
using soar::app::showStartupNotice;

static void print_usage(const char* argv0) {
  fmt::print("Usage:\n");
  fmt::print("  {} [--backend=ffmpeg|null] <path-or-url> [more-paths...]\n\n", argv0);
  fmt::print("  {} --headless [--backend=ffmpeg|null] <path-or-url>\n\n", argv0);
  fmt::print("Options:\n");
  fmt::print("  --headless    Run without GUI\n");
  fmt::print("  --gui         Open the window with no media source (empty start)\n");
  fmt::print("  --version     Print the version and exit\n");
  fmt::print("  --help        Print this help and exit\n");
  fmt::print("  --backend=    Select backend (ffmpeg, null; default: ffmpeg when built in)\n");
  fmt::print("  --cache-dir=  Cache http:// downloads here for offline replay\n");
  fmt::print("  --torrent-store=  Where torrent data lands (default: <tmp>/soar-torrent)\n");
  fmt::print("  --torrent-peer=   Seed endpoint host:port to connect to directly (repeatable)\n");
  fmt::print("  --torrent-index=  Which file of a multi-file torrent to stream (default 0)\n");
  fmt::print("  --torrent-list    Print the torrent's file table and exit (magnet:\n");
  fmt::print("                    contacts the swarm for metadata first)\n");
  fmt::print("Sources: local paths, http(s)://, .torrent files and magnet: URIs\n");
  fmt::print("(served over a local-http bridge, docs/mvp.md §5 P4).\n");
#ifdef SOAR_WITH_FFMPEG
  fmt::print("\nFFmpeg backend is available.\n");
#else
  fmt::print("\nFFmpeg backend not available (compiled without FFmpeg support).\n");
#endif
}

static std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// Dialog-facing variants of the usage exits: the full print_usage stays
// on stdout/stderr for terminal users; a double-clicked binary (no
// console that outlives it) gets the one-line story in a dialog instead
// (startup_report.h).
static void notice_usage(const char* argv0) {
  showStartupNotice(
      "soar",
      std::string("soar ") + SOAR_APP_VERSION +
          "\n\nNo media source given.\n\nUsage: " + argv0 +
          " [--backend=ffmpeg|null] <path-or-url> [more-paths...]\n\n"
          "Run soar from a terminal for the full usage text.");
}

int soarAppMain(int argc, char** argv, bool gui_entry) {
  // No up-front argc check: the no-source exit below covers it (no
  // arguments means no positional, and nothing in between can have parse
  // side effects), and the GUI entry must not take it at all — the
  // installed shortcuts launch with no arguments and expect the empty
  // window, not the CLI usage exit (BUGS.md #3).

  bool headless = false;
  std::string backend_type;  // empty until --backend=; resolved below
  bool backend_given = false;
  std::string cache_dir;
  std::string torrent_store;
  std::vector<std::string> torrent_peers;
  int torrent_index = 0;
  bool torrent_list = false;
  int uri_index = -1;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--headless") {
      headless = true;
    } else if (arg == "--version") {
      fmt::print("soar {}\n", SOAR_APP_VERSION);
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--gui") {
      // Explicit window entry with no media source (the GUI front-end
      // sets the same flag): the empty player window is the start state,
      // not a usage error (BUGS.md #3).
      gui_entry = true;
    } else if (arg.rfind("--backend=", 0) == 0) {
      backend_type = arg.substr(10);  // after "--backend="
      backend_given = true;
    } else if (arg.rfind("--cache-dir=", 0) == 0) {
      cache_dir = arg.substr(12);  // after "--cache-dir="
    } else if (arg.rfind("--torrent-store=", 0) == 0) {
      torrent_store = arg.substr(16);
    } else if (arg.rfind("--torrent-peer=", 0) == 0) {
      torrent_peers.push_back(arg.substr(15));
    } else if (arg.rfind("--torrent-index=", 0) == 0) {
      torrent_index = std::atoi(arg.c_str() + 16);
    } else if (arg == "--torrent-list") {
      torrent_list = true;
    } else if (arg.rfind('-', 0) == 0) {
      fmt::print(stderr, "Unknown option: {}\n", arg);
      print_usage(argv[0]);
      showStartupNotice("soar", "Unknown option: " + arg);
      return 2;
    } else {
      // First positional is the playing source; the rest queue up
      // (docs/mvp.md §2 playlist, mpv's multi-argument semantics).
      if (uri_index < 0) uri_index = i;
    }
  }

  if (uri_index < 0) {
    // The no-source exit keeps the console contract (tests pin usage text
    // + exit 2) and covers --headless too — there is nothing for a
    // headless run to do without a source. A GUI entry (--gui, or the
    // Windows GUI front-end) falls through to the window with no media:
    // the poster is the open-file entry there (BUGS.md #3).
    if (!gui_entry || headless) {
      print_usage(argv[0]);
      notice_usage(argv[0]);
      return 2;
    }
  }

  // Create backend based on selection. Default: FFmpeg when built in —
  // window sessions open more sources later (file dialog, drag-and-drop)
  // and the null backend only fakes a decodable media, so a null default
  // would black-screen everything opened from the UI (BUGS.md #3). An
  // explicit --backend always wins.
  if (!backend_given) {
#ifdef SOAR_WITH_FFMPEG
    backend_type = "ffmpeg";
#else
    backend_type = "null";
#endif
  }
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
    showStartupNotice("soar", "Unknown backend type: " + backend_type +
                                  "\nAvailable backends: null"
#ifdef SOAR_WITH_FFMPEG
                                  ", ffmpeg"
#endif
    );
    return 2;
  }

  // Download chip mirrors (P3c events; the torrent monitor thread for P2P
  // sources, docs/mvp.md §5 P4). Declared before torrent_stream so the
  // monitor's on_progress callback — which runs until stop() — captures
  // live objects. total == 0 means no active download; the chip hides at
  // bytes == total.
  std::atomic<std::uint64_t> download_bytes{0};
  std::atomic<std::uint64_t> download_total{0};
  // P2P sources (docs/mvp.md §5 P4): a `.torrent` positional or a magnet:
  // URI is streamed by the local-http bridge in src/p2p; the player sees an
  // ordinary 127.0.0.1 URL with Range support and no backend learns about
  // torrents. Declared before `player` so the player (which holds the
  // bridge's HTTP connection) is torn down first.
  soar::p2p::TorrentStream torrent_stream;
  // Empty until a positional arrives (a --gui start has none); the torrent
  // probe and the upfront open below both guard on uri_index.
  std::string uri;
  if (uri_index >= 0) uri = argv[uri_index];
  // True when the stream was started with startAsync() for the window (the
  // bridge is opened by the window's pending-torrent poll, not here).
  bool torrent_async = false;
  if (uri_index >= 0) {
    const std::string lower_uri = lower_copy(uri);
    const bool is_magnet = lower_uri.rfind("magnet:", 0) == 0;
    const bool is_torrent = !is_magnet &&
        lower_uri.size() > 8 && lower_uri.compare(lower_uri.size() - 8, 8, ".torrent") == 0;
    if (is_magnet || is_torrent) {
      if (torrent_store.empty()) {
        torrent_store =
            (std::filesystem::temp_directory_path() / "soar-torrent").string();
      }
      std::error_code fs_err;
      std::filesystem::create_directories(torrent_store, fs_err);

      soar::p2p::TorrentStream::Params tp;
      if (is_magnet) {
        tp.magnet_uri = uri;
      } else {
        tp.torrent_path = uri;
      }
      tp.store_dir = torrent_store;
      tp.file_index = torrent_index;
      tp.peers = torrent_peers;
      // Also drives the window download chip (same atomics the P3c event
      // callback writes); during the pre-open metadata wait start() calls
      // it too, which surfaces the wait as terminal ticks.
      tp.on_progress = [&download_bytes, &download_total](
                           const soar::p2p::TorrentStatus& s) {
        fmt::print(stderr, "torrent: {:.1f}/{:.1f} MiB peers={}{}\n",
                   s.downloaded / 1048576.0, s.total / 1048576.0, s.peers,
                   s.metadata ? "" : " (metadata pending)");
        download_bytes.store(s.downloaded, std::memory_order_relaxed);
        download_total.store(s.total, std::memory_order_relaxed);
      };
      // Multi-file listing (docs/mvp.md §5 P4b-3): fires once from start()
      // with the metadata just arrived, before the file selection is
      // applied. During playback the table goes to stderr so --torrent-index
      // counts are visible; with --torrent-list it is the whole output and
      // goes to stdout.
      tp.on_files = [&torrent_list](
                        const std::vector<soar::p2p::TorrentFile>& files) {
        if (!torrent_list && files.size() <= 1) return;
        if (torrent_list) {
          fmt::print("torrent: {} files\n", files.size());
          for (const auto& f : files) {
            fmt::print("  [{}] {:>14}  {}\n", f.index, f.size, f.path);
          }
        } else {
          fmt::print(stderr, "torrent: {} files (pick with --torrent-index=N)\n",
                     files.size());
          // Bounded like the component's error table: a huge torrent must
          // not flood stderr on every playback start.
          const std::size_t shown = std::min<std::size_t>(files.size(), 32);
          for (std::size_t i = 0; i < shown; ++i) {
            fmt::print(stderr, "  [{}] {}  {}\n", files[i].index, files[i].size,
                       files[i].path);
          }
          if (files.size() > shown) fmt::print(stderr, "  ...\n");
        }
      };
      // P4b-4: in window mode the torrent starts asynchronously — the
      // window opens right away and its pending-torrent poll opens the
      // bridge once the metadata lands (poster shows the wait). Headless
      // and --torrent-list keep start()'s synchronous contract.
#ifdef SOAR_WITH_SDL2
      const bool window_mode = !headless;
#else
      const bool window_mode = false;
#endif
      if (window_mode && !torrent_list) {
        if (!torrent_stream.startAsync(tp)) {
          fmt::print(stderr, "{}\n", torrent_stream.lastError());
          reportFatalStartupError("soar", torrent_stream.lastError());
          return 1;
        }
        torrent_async = true;
      } else {
        if (!torrent_stream.start(tp)) {
          fmt::print(stderr, "{}\n", torrent_stream.lastError());
          reportFatalStartupError("soar", torrent_stream.lastError());
          return 1;
        }
        if (torrent_list) {
          // Metadata acquired, table printed; exit without opening a player.
          return 0;
        }
        fmt::print(stderr, "torrent: {} ({} bytes) -> {}\n", torrent_stream.fileName(),
                   torrent_stream.fileSize(), torrent_stream.playbackUrl());
        uri = torrent_stream.playbackUrl();
      }
    }
  }

  soar::Player player(std::move(backend));
  // Mirrors the BufferingStarted/Ended event pair for the window UI's
  // buffering chip (backend threads set it; the window loop polls it).
  std::atomic<bool> buffering{false};
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

  // Async torrent sources open from the window once the metadata lands;
  // everything here (open, play, media info print) runs for the sources
  // that were already openable at this point. A --gui start has no source
  // at all: the player stays closed until the UI opens one.
  if (uri_index >= 0 && !torrent_async) {
    if (!player.open(soar::MediaSource{uri, cache_dir})) {
      fmt::print(stderr, "Failed to open source: {}\n", uri);
      fmt::print(stderr, "Error: {}\n", player.lastError());
      reportFatalStartupError(
          "soar", "Failed to open source: " + uri +
                      "\nError: " + player.lastError());
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
  }

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
  // Remaining positionals join the play queue behind `uri` (docs/mvp.md
  // §2 playlist). Headless keeps the single-source contract. The start
  // index is floored at 1 so a --gui launch (uri_index == -1) never
  // walks argv[0].
  for (int i = std::max(uri_index + 1, 1); i < argc; ++i) {
    std::string extra(argv[i]);
    if (!extra.empty() && extra[0] != '-') ui_cfg.queued_uris.push_back(extra);
  }
  ui_cfg.backend_label = backend_type;
  ui_cfg.cache_dir = cache_dir;
  ui_cfg.recent_path = soar::app::defaultRecentPath();
  // P2P sources: show the streamed file's name instead of the bridge URL
  // (empty for ordinary sources — the window falls back to the URI).
  ui_cfg.source_label = torrent_stream.fileName();
  // P4b-4 async P2P source: the window polls the stream and opens the
  // bridge URL itself once Serving lands (poster shows the wait; Failed is
  // a toast). Null for ordinary and synchronous sources.
  ui_cfg.torrent = torrent_async ? &torrent_stream : nullptr;
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

int main(int argc, char** argv) {
  return soarAppMain(argc, argv);
}
