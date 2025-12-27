#include "soar/core/player.h"

#include <fmt/core.h>

#ifdef SOAR_WITH_FFMPEG
#  include "soar/core/ffmpeg_backend.h"
#endif

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#endif

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <string>

static void print_usage(const char* argv0) {
  fmt::print("Usage:\n");
  fmt::print("  {} [--backend=ffmpeg|null] <path-or-url>\n\n", argv0);
  fmt::print("  {} --headless [--backend=ffmpeg|null] <path-or-url>\n\n", argv0);
  fmt::print("Options:\n");
  fmt::print("  --headless    Run without GUI\n");
  fmt::print("  --backend=    Select backend (ffmpeg, null)\n");
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
  int uri_index = 1;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--headless") {
      headless = true;
    } else if (arg.rfind("--backend=", 0) == 0) {
      backend_type = arg.substr(10);  // after "--backend="
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

  if (argc <= uri_index) {
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
  player.setEventCallback([&player](const soar::Event& e) {
    if (e.type == soar::EventType::StateChanged) {
      fmt::print(stderr, "event: state={}\n", static_cast<int>(e.state));
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
    } else if (e.type == soar::EventType::Error) {
      fmt::print(stderr, "event: error={}\n", e.message);
    }
  });

  const std::string uri(argv[uri_index]);
  if (!player.open(soar::MediaSource{uri})) {
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
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fmt::print(stderr, "SDL_Init failed: {}\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
    "soar (skeleton)",
    SDL_WINDOWPOS_CENTERED,
    SDL_WINDOWPOS_CENTERED,
    960,
    540,
    SDL_WINDOW_SHOWN
  );
  if (!window) {
    fmt::print(stderr, "SDL_CreateWindow failed: {}\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fmt::print(stderr, "SDL_CreateRenderer failed: {}\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_Texture* texture = nullptr;
#ifdef SOAR_WITH_FFMPEG
  soar::FFmpegBackend::DecodedVideoFrame video_frame{};
#endif

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = false;
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        running = false;
      }
    }

#ifdef SOAR_WITH_FFMPEG
    if (ffmpeg_backend && ffmpeg_backend->tryGetVideoFrame(video_frame)) {
      if (video_frame.width <= 0 || video_frame.height <= 0) {
        // ignore
      } else {
        int tw = 0;
        int th = 0;
        if (!texture || SDL_QueryTexture(texture, nullptr, nullptr, &tw, &th) != 0 ||
            tw != video_frame.width || th != video_frame.height) {
          if (texture) {
            SDL_DestroyTexture(texture);
          }
          texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            video_frame.width,
            video_frame.height
          );
          if (!texture) {
            fmt::print(stderr, "SDL_CreateTexture failed: {}\n", SDL_GetError());
          }
        }
      }

      if (texture) {
        SDL_UpdateYUVTexture(
          texture,
          nullptr,
          video_frame.y.data(),
          video_frame.stride_y,
          video_frame.u.data(),
          video_frame.stride_u,
          video_frame.v.data(),
          video_frame.stride_v
        );
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
      }
    }
#endif
    SDL_Delay(16);
  }

  if (texture) {
    SDL_DestroyTexture(texture);
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
#endif

  return 0;
}
