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

add_test(NAME soar_backend_media_tests COMMAND soar_backend_media_tests)

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

  add_test(NAME soar_cli_tests COMMAND soar_cli_tests)

# A subprocess that hangs (e.g. an SDL window loop on a runner where
# video init unexpectedly succeeds) would otherwise pin the whole CI
# job until its two-digit-minute timeout. CTest kills the test at the
# 5-minute mark instead, so one wedged case costs minutes, not the job.
set_tests_properties(soar_core_tests soar_backend_tests soar_backend_media_tests soar_cli_tests
  PROPERTIES TIMEOUT 300)
endif()
