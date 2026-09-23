find_package(doctest CONFIG REQUIRED)

add_executable(soar_core_tests
  test_player.cpp
)

target_compile_features(soar_core_tests PRIVATE cxx_std_17)
target_link_libraries(soar_core_tests PRIVATE soar::core doctest::doctest)

add_test(NAME soar_core_tests COMMAND soar_core_tests)
