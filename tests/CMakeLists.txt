cmake_minimum_required(VERSION 3.20)

project(instrument-data-tests LANGUAGES C)

find_package(cmocka CONFIG REQUIRED)
find_package(Threads REQUIRED)

file(GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_config.h
  CONTENT "#pragma once\n#define TEST_BINARY_PATH \"$<TARGET_FILE:instrument-data-tests>\"\n"
)

# ============================================================
# Test executable (single binary, supports child modes)
# ============================================================

add_executable(instrument-data-tests
  test_main.c
  test_basic.c
  test_registry.c
  test_multiprocess.c
)

target_include_directories(instrument-data-tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/internal
  ${CMAKE_CURRENT_BINARY_DIR}
)


target_link_libraries(instrument-data-tests 
  PRIVATE
    instrument-data
    cmocka::cmocka
    Threads::Threads
)
target_compile_definitions(instrument-data-tests PRIVATE USE_TSAN)

if(UNIX AND NOT APPLE)
  target_link_libraries(instrument-data-tests PRIVATE rt)
endif()

# ============================================================
# Register test with CTest
# ============================================================

add_test(
  NAME instrument-data-test-suite
  COMMAND instrument-data-tests
)

# ensure correct runtime working directory
set_tests_properties(instrument-data-test-suite PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)


add_executable(instrument-data-stress-deterministic
    stress_deterministic.c)

add_executable(instrument-data-stress-chaos
    stress_chaos.c)

target_link_libraries(instrument-data-stress-deterministic 
  PRIVATE
    instrument-data
    cmocka::cmocka
    Threads::Threads
)
target_include_directories(instrument-data-stress-deterministic PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(instrument-data-stress-chaos
  PRIVATE
    instrument-data
    cmocka::cmocka
    Threads::Threads
)
target_include_directories(instrument-data-stress-chaos PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
)
add_test(NAME stress_deterministic
         COMMAND instrument-data-stress-deterministic)

foreach(i RANGE 1 15)
  add_test(NAME stress_chaos_${i}
           COMMAND instrument-data-stress-chaos)
endforeach()
