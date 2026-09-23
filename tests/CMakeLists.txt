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
