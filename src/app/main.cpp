#include "soar/core/player.h"

#include <fmt/core.h>

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#endif

#include <chrono>
#include <cstdio>
#include <string>

static void print_usage(const char* argv0) {
  fmt::print("Usage:\n  {} <path-or-url>\n\n", argv0);
  fmt::print("  {} --headless <path-or-url>\n\n", argv0);
  fmt::print("Note: this is a project skeleton. Playback backend is not implemented yet.\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 2;
  }

  bool headless = false;
  int uri_index = 1;
  if (std::string(argv[1]) == "--headless") {
    headless = true;
    uri_index = 2;
  }
  if (argc <= uri_index) {
    print_usage(argv[0]);
    return 2;
  }

  soar::Player player(soar::makeNullBackend());
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

  fmt::print(stderr, "Opened: {}\n", uri);
  fmt::print(stderr, "State: Playing (null backend)\n");

  if (headless) {
    (void)player.seek(std::chrono::seconds(1));
    (void)player.selectTrack(soar::TrackType::Subtitle, 2);
    (void)player.disableSubtitles();
    (void)player.stop();
    return 0;
  }

#ifdef SOAR_WITH_SDL2
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
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

  bool running = true;
  auto last_tick = std::chrono::steady_clock::now();
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

    auto now = std::chrono::steady_clock::now();
    if (now - last_tick > std::chrono::milliseconds(1000)) {
      last_tick = now;
      (void)player.seek(player.position() + std::chrono::milliseconds(1000));
    }
    SDL_Delay(16);
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
#endif

  return 0;
}
