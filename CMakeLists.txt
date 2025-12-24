cmake_minimum_required(VERSION 3.20)

project(
  soar
  VERSION 0.1.0
  LANGUAGES CXX
)

option(SOAR_BUILD_APP "Build the sample app executable" ON)
option(SOAR_ENABLE_SDL2 "Enable SDL2 window/event loop in the app (if available)" ON)
option(SOAR_ENABLE_FFMPEG "Enable FFmpeg backend (if available)" ON)

add_library(soar_core)
add_library(soar::core ALIAS soar_core)

target_sources(soar_core PRIVATE
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

if(SOAR_BUILD_APP)
  add_executable(soar src/app/main.cpp)
  target_compile_features(soar PRIVATE cxx_std_17)
  target_link_libraries(soar PRIVATE soar::core)

  if(SOAR_ENABLE_SDL2)
    find_package(SDL2 CONFIG QUIET)
    if(SDL2_FOUND)
      target_compile_definitions(soar PRIVATE SOAR_WITH_SDL2=1)
      target_link_libraries(soar PRIVATE
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
      )
    else()
      message(STATUS "SDL2 not found; building CLI-only app. (Install via vcpkg: sdl2)")
    endif()
  endif()
endif()
