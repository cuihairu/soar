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

add_library(soar_core)
add_library(soar::core ALIAS soar_core)

target_sources(soar_core PRIVATE
  src/core/http_cache.cpp
  src/core/null_backend.cpp
  src/core/player.cpp
)

target_include_directories(soar_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(soar_core PUBLIC cxx_std_17)

find_package(fmt CONFIG REQUIRED)
target_link_libraries(soar_core PUBLIC fmt::fmt)

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
  target_link_libraries(soar PRIVATE soar::core soar_app_ui)

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
