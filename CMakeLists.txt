cmake_minimum_required(VERSION 3.20)

project(
  soar
  VERSION 0.1.0
  LANGUAGES CXX
)

option(SOAR_BUILD_APP "Build the sample app executable" ON)
option(SOAR_BUILD_TESTS "Build the unit tests" ON)
option(SOAR_ENABLE_SDL2 "Enable SDL2 window/event loop in the app (if available)" ON)
option(SOAR_ENABLE_FFMPEG "Enable FFmpeg backend (if available)" ON)
option(SOAR_ENABLE_IMGUI "Enable the Dear ImGui player UI overlay (fetched at configure time)" ON)
option(SOAR_ENABLE_LIBASS "Enable libass ASS/SSA subtitle rendering (if available)" ON)
option(SOAR_ENABLE_TORRENT_ENCRYPTION "Enable BitTorrent protocol encryption (MSE) in the P2P download kernel" ON)

add_library(soar_core)
add_library(soar::core ALIAS soar_core)

target_sources(soar_core PRIVATE
  src/core/ass_dialogue.cpp
  src/core/http_cache.cpp
  src/core/null_backend.cpp
  src/core/player.cpp
  src/core/subtitle_provider.cpp
  src/core/subtitle_text.cpp
)

target_include_directories(soar_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(soar_core PUBLIC cxx_std_17)

find_package(fmt CONFIG REQUIRED)
target_link_libraries(soar_core PUBLIC fmt::fmt)

# Winsock lives in its own import library on Windows; MSVC does not
# auto-link it. http_cache.cpp and subtitle_provider.cpp both speak
# sockets, so the dependency belongs to the core target (PUBLIC: the
# test binaries link the same objects).
if(WIN32)
  target_link_libraries(soar_core PUBLIC ws2_32)
endif()

# FFmpeg support (optional)
if(SOAR_ENABLE_FFMPEG)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
      libavformat
      libavcodec
      libavutil
      libswresample
      libswscale
    )
  endif()

  if(FFMPEG_FOUND)
    target_compile_definitions(soar_core PUBLIC SOAR_WITH_FFMPEG=1)
    target_sources(soar_core PRIVATE
      src/core/ffmpeg_backend.cpp
    )
    target_link_libraries(soar_core PUBLIC
      PkgConfig::FFMPEG
    )
    message(STATUS "FFmpeg backend enabled")
  else()
    message(STATUS "FFmpeg not found; building without FFmpeg backend")
  endif()
endif()

# ASS subtitle rendering (docs/mvp.md §6, style-faithful playback).
# Optional capability, same probe-and-degrade contract as FFmpeg above:
# libass via system pkg-config (Linux CI/apt) or the vcpkg port
# (mac/win); neither found (or the option is OFF) → SOAR_WITH_LIBASS
# stays undefined and the wrapper compiles to its inert stub branch
# while the plain-text subtitle path keeps running. The wrapper TU is
# unconditionally part of the target — the FFmpegBackend references
# AssRenderer regardless, so the symbols must exist in every
# configuration. The library itself never enters the gcovr filter set;
# only the wrapper carries coverage.
target_sources(soar_core PRIVATE src/core/ass_renderer.cpp)
if(SOAR_ENABLE_LIBASS)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ASS IMPORTED_TARGET libass)
  endif()
  if(NOT ASS_FOUND)
    find_package(unofficial-libass CONFIG QUIET)
    if(TARGET unofficial::libass::libass)
      set(ASS_FOUND TRUE)
      set(ASS_LINK unofficial::libass::libass)
    endif()
  else()
    set(ASS_LINK PkgConfig::ASS)
  endif()
  if(ASS_FOUND)
    target_compile_definitions(soar_core PUBLIC SOAR_WITH_LIBASS=1)
    target_link_libraries(soar_core PUBLIC ${ASS_LINK})
    message(STATUS "libass subtitle rendering enabled")
  else()
    message(STATUS "libass not found; ASS scripts render as plain text. (Install via apt: libass-dev; via vcpkg: libass)")
  endif()
endif()

# P2P streaming bridge (docs/mvp.md §5 P4): the vendored libtorrent is the
# download kernel only; soar's TorrentStream (src/p2p) presents the chosen
# file as a plain local-http URL with Range support, so the player opens it
# through the normal path and no backend learns about torrents. The vendored
# tree is pinned to upstream v2.0.15 — see third_party/libtorrent/VENDOR.md.
# Options must be set before add_subdirectory: feature_option() calls
# option(), and a normal variable shadows the cache entry in this scope.
set(BUILD_SHARED_LIBS OFF)
set(static_runtime OFF)
# BitTorrent protocol encryption (MSE, docs/mvp.md §5 P4b-5). In 2.0.15 the
# handshake is self-contained — DH over boost::multiprecision plus the
# bundled RC4/SHA-1 (src/pe_crypto.cpp) — so switching it on costs one extra
# TU and *no* new external dependency; the OpenSSL lookup further down in
# the vendored CMakeLists is independent of this flag and already runs
# either way. With it off, peers that require encryption (pe_forced on their
# side) drop the connection outright, so the default is on; the option is
# the escape hatch if a platform ever misbehaves.
if(SOAR_ENABLE_TORRENT_ENCRYPTION)
  set(encryption ON)
else()
  set(encryption OFF)
  message(STATUS "Torrent protocol encryption disabled; require-encryption peers stay unreachable")
endif()
set(gnutls OFF)
set(dht ON)          # upstream defaults, spelled out for review
set(streaming ON)    # piece deadlines — required for stream-on-demand
set(logging ON)
set(deprecated-functions OFF)
set(build_tests OFF)
set(build_examples OFF)
set(build_tools OFF)
set(python-bindings OFF)
add_subdirectory(third_party/libtorrent)

add_library(soar_p2p STATIC src/p2p/torrent_stream.cpp)
target_include_directories(soar_p2p PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p)
target_compile_features(soar_p2p PUBLIC cxx_std_17)
target_link_libraries(soar_p2p PUBLIC torrent-rasterbar)

# Pure UI logic (auto-hide state machine, recent store, toast, time format:
# docs/ui-design.md §5.3) — no SDL/ImGui dependency, shared by the app and
# the unit-test suite.
add_library(soar_app_ui STATIC src/app/ui/ui_state.cpp)
target_compile_features(soar_app_ui PUBLIC cxx_std_17)
target_include_directories(soar_app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)

if(SOAR_BUILD_APP)
  add_executable(soar
    src/app/main.cpp
    src/app/ui/player_window.cpp
  )
  target_compile_features(soar PRIVATE cxx_std_17)
  # soar --version prints this; install scripts use it as the post-install
  # smoke check (daily-build's BUILD_INFO greps the same project VERSION).
  target_compile_definitions(soar PRIVATE SOAR_APP_VERSION="${PROJECT_VERSION}")
  target_link_libraries(soar PRIVATE soar::core soar_app_ui soar_p2p)

  # Dev tool for the P2P walkthrough (docs/mvp.md §5 P4): seeds a local
  # swarm from a file so the TorrentStream path can be exercised without
  # external trackers. Not referenced by tests; not shipped.
  add_executable(seed_torrent tools/seed_torrent.cpp)
  target_compile_features(seed_torrent PRIVATE cxx_std_17)
  target_link_libraries(seed_torrent PRIVATE torrent-rasterbar)

  if(SOAR_ENABLE_SDL2)
    # 2.0.18 is the floor: the ImGui sdlrenderer2 backend needs 2.0.17
    # (SDL_RenderGeometry) and the window layer's key-repeat suppression
    # uses SDL_KeyboardEvent::repeat, added in 2.0.18. Asking for it here
    # turns "too old" into the same degrade-to-CLI path as "not found"
    # instead of a compile error deep inside a vendored backend.
    find_package(SDL2 2.0.18 CONFIG QUIET)
    if(SDL2_FOUND)
      set(SOAR_SDL2_LIBS
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
      )
      target_compile_definitions(soar PRIVATE SOAR_WITH_SDL2=1)
      target_compile_definitions(soar_core PRIVATE SOAR_WITH_SDL2=1)
      target_link_libraries(soar_core PUBLIC ${SOAR_SDL2_LIBS})
      target_link_libraries(soar PRIVATE ${SOAR_SDL2_LIBS})

      # Player UI overlay (docs/ui-design.md §5): Dear ImGui drawn over the
      # video texture through imgui_impl_sdlrenderer2. The sources land in
      # the build tree, never in the repo, and never enter the gcovr filter
      # set (src/include).
      #
      # Cloned explicitly instead of via FetchContent so that a blocked,
      # offline, or git-less network degrades to the bare video window
      # rather than failing the configure step — the same "optional
      # capability" contract as SDL2 and FFmpeg above. An already-populated
      # directory (earlier configure, or a hand-placed tree) is reused.
      if(SOAR_ENABLE_IMGUI)
        set(soar_imgui_tag "v1.91.9b")
        set(soar_imgui_dir "${CMAKE_BINARY_DIR}/_deps/imgui-src")
        set(soar_imgui_ok FALSE)
        if(EXISTS "${soar_imgui_dir}/imgui.cpp")
          set(soar_imgui_ok TRUE)
        else()
          find_package(Git QUIET)
          if(GIT_EXECUTABLE)
            # A half-finished clone would make `git clone` refuse the
            # non-empty destination; start from a clean directory.
            file(REMOVE_RECURSE "${soar_imgui_dir}")
            message(STATUS "Fetching Dear ImGui ${soar_imgui_tag} into ${soar_imgui_dir}")
            execute_process(
              COMMAND "${GIT_EXECUTABLE}" clone --depth 1 --branch ${soar_imgui_tag}
                      https://github.com/ocornut/imgui.git "${soar_imgui_dir}"
              RESULT_VARIABLE soar_imgui_clone
              OUTPUT_QUIET ERROR_QUIET
            )
            if(soar_imgui_clone EQUAL 0 AND EXISTS "${soar_imgui_dir}/imgui.cpp")
              set(soar_imgui_ok TRUE)
            endif()
          endif()
        endif()
        if(soar_imgui_ok)
          add_library(imgui STATIC
            ${soar_imgui_dir}/imgui.cpp
            ${soar_imgui_dir}/imgui_draw.cpp
            ${soar_imgui_dir}/imgui_tables.cpp
            ${soar_imgui_dir}/imgui_widgets.cpp
            ${soar_imgui_dir}/backends/imgui_impl_sdl2.cpp
            ${soar_imgui_dir}/backends/imgui_impl_sdlrenderer2.cpp
          )
          target_include_directories(imgui PUBLIC
            ${soar_imgui_dir}
            ${soar_imgui_dir}/backends
          )
          target_link_libraries(imgui PUBLIC ${SOAR_SDL2_LIBS})
          target_compile_definitions(soar PRIVATE SOAR_WITH_IMGUI=1)
          target_link_libraries(soar PRIVATE imgui)
          message(STATUS "Player UI overlay: Dear ImGui ${soar_imgui_tag} (imgui_impl_sdlrenderer2)")
        else()
          message(STATUS "Dear ImGui unavailable (no git, or no network); building the bare video window. (-DSOAR_ENABLE_IMGUI=OFF to skip the attempt)")
        endif()
      endif()
    else()
      message(STATUS "SDL2 >= 2.0.18 not found; building CLI-only app. (Install via vcpkg: sdl2)")
    endif()
  endif()
endif()

# Tests come last: the CLI integration tests need the soar app target to
# exist so they can invoke the freshly built executable.
if(SOAR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
