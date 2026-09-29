find_package(doctest CONFIG REQUIRED)

add_executable(soar_core_tests
  test_player.cpp
)

target_compile_features(soar_core_tests PRIVATE cxx_std_17)
target_link_libraries(soar_core_tests PRIVATE soar::core doctest::doctest)

add_test(NAME soar_core_tests COMMAND soar_core_tests)

# Concurrency tests for the FFmpeg backend. Without SOAR_WITH_FFMPEG the
# binary builds as an empty shell that reports a skip message, so the test
# target (and its pass status) exists on every platform.
add_executable(soar_backend_tests
  test_backend_concurrency.cpp
)

target_compile_features(soar_backend_tests PRIVATE cxx_std_17)
target_link_libraries(soar_backend_tests PRIVATE soar::core doctest::doctest)

add_test(NAME soar_backend_tests COMMAND soar_backend_tests)

# Functional (semantic) tests for the FFmpeg backend public surface:
# event delivery, unopened-state control calls, media lifecycle. Builds
# as an empty shell without SOAR_WITH_FFMPEG so the test always exists.
add_executable(soar_backend_media_tests
  test_backend_media.cpp
)

target_compile_features(soar_backend_media_tests PRIVATE cxx_std_17)
target_link_libraries(soar_backend_media_tests PRIVATE soar::core doctest::doctest)
# Same fixture font the ass unit tests use: external ASS documents have no
# container attachments, so the renderer needs the checked-in face for
# deterministic glyph resolution (ASS_FONTPROVIDER_NONE).
target_compile_definitions(soar_backend_media_tests PRIVATE
  SOAR_TEST_FONT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/NotoMono-Regular.ttf")

add_test(NAME soar_backend_media_tests COMMAND soar_backend_media_tests)

# Disk-cache component tests (P3a). The component itself is backend-
# agnostic (soar_core); the FFmpeg integration case is gated inside the
# file on SOAR_WITH_FFMPEG + a fixture env var, like the media tests.
add_executable(soar_http_cache_tests
  test_http_cache.cpp
)

target_compile_features(soar_http_cache_tests PRIVATE cxx_std_17)
target_link_libraries(soar_http_cache_tests PRIVATE soar::core doctest::doctest)

add_test(NAME soar_http_cache_tests COMMAND soar_http_cache_tests)

# External-subtitle core unit tests (docs/mvp.md §6): the SRT/WebVTT text
# parser and the sidecar SubtitleProvider. Pure logic over strings and one
# temp directory — no display, no media fixture, no network.
add_executable(soar_subtitle_tests
  test_subtitle.cpp
)

target_compile_features(soar_subtitle_tests PRIVATE cxx_std_17)
target_link_libraries(soar_subtitle_tests PRIVATE soar::core doctest::doctest)

add_test(NAME soar_subtitle_tests COMMAND soar_subtitle_tests)

# ASS renderer unit tests (docs/mvp.md §6, style-faithful ASS/SSA): the
# libass wrapper, its straight-alpha compositor, and the streaming feed
# for embedded tracks. Pure logic over in-memory scripts + the checked-in
# fixture font — no display, no media fixture, no network. Builds as an
# empty shell without SOAR_WITH_LIBASS so the test always exists.
add_executable(soar_ass_tests
  test_ass_renderer.cpp
)

target_compile_features(soar_ass_tests PRIVATE cxx_std_17)
target_link_libraries(soar_ass_tests PRIVATE soar::core doctest::doctest)
# libass runs with ASS_FONTPROVIDER_NONE and resolves glyphs from this
# file (or bytes registered via addFont), so rendering is deterministic
# on any machine. See tests/fixtures/FONT-LICENSE.txt.
target_compile_definitions(soar_ass_tests PRIVATE
  SOAR_TEST_FONT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/NotoMono-Regular.ttf")

add_test(NAME soar_ass_tests COMMAND soar_ass_tests)

# Pure UI logic unit tests (docs/ui-design.md §5.3): auto-hide state
# machine, recent store, toast, time formatting. No display needed.
add_executable(soar_ui_tests
  test_ui_state.cpp
)

target_compile_features(soar_ui_tests PRIVATE cxx_std_17)
target_link_libraries(soar_ui_tests PRIVATE soar_app_ui doctest::doctest)

add_test(NAME soar_ui_tests COMMAND soar_ui_tests)

# CLI integration tests run the real soar executable as a subprocess and
# check exit codes / stderr reports for the headless and error paths.
# They need the app target, so they only exist when the app is built;
# the FFmpeg availability macro must match the app's own build config.
if(TARGET soar)
  add_executable(soar_cli_tests
    test_cli.cpp
  )

  target_compile_features(soar_cli_tests PRIVATE cxx_std_17)
  target_link_libraries(soar_cli_tests PRIVATE doctest::doctest)
  target_compile_definitions(soar_cli_tests PRIVATE
    SOAR_CLI_EXECUTABLE="$<TARGET_FILE:soar>"
  )
  if(FFMPEG_FOUND)
    target_compile_definitions(soar_cli_tests PRIVATE SOAR_WITH_FFMPEG=1)
  endif()

  # The scripted UI sessions (the OSC widget tour, the Recent overlay, the
  # buffering chip) need the ImGui overlay the app was built with. Report
  # that to the test binary so those cases skip — with a message — in the
  # -DSOAR_ENABLE_IMGUI=OFF configuration instead of failing on a HUD that
  # is not there. The bare-window cases (Escape, WM_DELETE_WINDOW) have no
  # such dependency and keep running there.
  get_target_property(soar_app_defs soar COMPILE_DEFINITIONS)
  if("SOAR_WITH_IMGUI=1" IN_LIST soar_app_defs)
    target_compile_definitions(soar_cli_tests PRIVATE SOAR_CLI_HAS_IMGUI=1)
  endif()

  add_dependencies(soar_cli_tests soar)
  add_test(NAME soar_cli_tests COMMAND soar_cli_tests)

# A subprocess that hangs (e.g. an SDL window loop on a runner where
# video init unexpectedly succeeds) would otherwise pin the whole CI
# job until its two-digit-minute timeout. CTest kills the test at the
# 10-minute mark instead, so one wedged case costs minutes, not the job.
set_tests_properties(soar_core_tests soar_backend_tests soar_ui_tests
  soar_subtitle_tests soar_ass_tests PROPERTIES TIMEOUT 600)
# soar_cli_tests needs its own, bigger budget. Under coverage
# instrumentation (--coverage -O0 -g) the scripted window cases crawl: the
# subtitle-settings tour alone takes ~10 min on a quiet CI runner (noted in
# 09c1da9; CI runs 36298838808 / 36299389988 / 36301040326 still died at the
# shared 600 s mark). Two children are capped at 1500 s inside runCli
# (subsdrive, and the stalled-source case — the cap saved it on a shared
# loaded box where 600 s ran out), so the ceiling clears two such worst
# cases plus the other ~25 (~50 s each when instrumented, measured locally).
set_tests_properties(soar_cli_tests PROPERTIES TIMEOUT 4500)
# http_cache_tests contains the offline-replay integration case: two
# playback passes with their own 240 s deadlines, so a runner where the
# clock crawls can legitimately exceed the shared 300 s budget (local run
# hit the mark when the first pass stalled). Its own budget, media-suite
# style.
set_tests_properties(soar_http_cache_tests
  PROPERTIES TIMEOUT 900)
# media_tests is structurally slower than the rest: the network-stall case
# serves a file with a deliberate 40 s pause and polls with a 240 s deadline,
# and sanitizers (TSAN ~15x) inflate every wait. A full deadline plus the
# rest of the suite reaches 300 s+ — CI run 36078113614 died as a Timeout at
# exactly 300.06 s. Give it its own budget instead of betting on the happy
# path (73 s) repeating every run.
set_tests_properties(soar_backend_media_tests
  PROPERTIES TIMEOUT 1200)
endif()
