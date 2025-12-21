#include "soar/core/player.h"

#include <fmt/core.h>

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#endif

#include <string>

static void print_usage(const char* argv0) {
  fmt::print("Usage:\n  {} <path-or-url>\n\n", argv0);
  fmt::print("Note: this is a project skeleton. Playback backend is not implemented yet.\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 2;
  }

  soar::Player player(soar::makeNullBackend());
  if (!player.open(soar::MediaSource{std::string(argv[1])})) {
    fmt::print(stderr, "Failed to open source: {}\n", argv[1]);
    return 1;
  }
  player.play();

  fmt::print("Opened: {}\n", argv[1]);
  fmt::print("State: Playing (null backend)\n");

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
    SDL_Delay(16);
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
#endif

  return 0;
}

