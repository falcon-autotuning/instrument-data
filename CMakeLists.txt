cmake_minimum_required(VERSION 3.20)
project(
  instrument-data
  VERSION 1.1.3
  DESCRIPTION "An cross OS shared memory platform for collecting scientific data from instruments"
  HOMEPAGE_URL "https://github.com/falcon-autotuning/instrument-data"
  LANGUAGES C
)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
option(BUILD_TESTS "Enable building tests" ON)
option(USE_CCACHE "Enable ccache or sccache if available" ON)
option(ENABLE_PCH
       "Enable target_precompile_headers to speed up builds (opt-in)" ON)

# Windows-specific definitions
if(WIN32)
  add_compile_definitions(NOMINMAX)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Try to auto-detect a compiler launcher (prefer sccache if present)
if(USE_CCACHE)
  find_program(SCCACHE_PROGRAM sccache)
  if(SCCACHE_PROGRAM)
    message(STATUS "Using sccache: ${SCCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
  else()
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
      message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
      set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
      set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    else()
      message(
        STATUS "No ccache/sccache found (fine, builds will just be normal).")
    endif()
  endif()
endif()

find_path(UTHASH_INCLUDE_DIRS "utarray.h")
find_package(Threads REQUIRED)

add_library(instrument-data SHARED
  src/buffer.c
  src/manager.c
  src/shm.c
  src/process.c
  src/util.c
)

target_link_libraries(instrument-data
  PRIVATE
    Threads::Threads
)
if(UNIX AND NOT APPLE)
  target_link_libraries(instrument-data PRIVATE rt)
endif()

target_include_directories(instrument-data
  PRIVATE
    ${UTHASH_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

include(GenerateExportHeader)
set(GENERATED_INCLUDE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/include/)
file(MAKE_DIRECTORY ${GENERATED_INCLUDE_DIR})
generate_export_header(instrument-data
  EXPORT_FILE_NAME
    ${GENERATED_INCLUDE_DIR}/instrument-data-export.h
)
target_include_directories(instrument-data PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
  ${GENERATED_INCLUDE_DIR}/instrument-data-export.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/instrument-data
)
install(TARGETS instrument-data
  EXPORT instrument-data-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT instrument-data-targets
  FILE instrument-data-targets.cmake
  NAMESPACE instrument-data::
  DESTINATION ${CMAKE_INSTALL_DATADIR}/instrument-data
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/instrument-data-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)
configure_package_config_file(
  cmake/instrument-data-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/instrument-data-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_DATADIR}/instrument-data
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/instrument-data-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/instrument-data-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/instrument-data
)
configure_file(
  cmake/instrument-data.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/instrument-data.pc
  @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/instrument-data.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

if(CMAKE_GENERATOR STREQUAL "Ninja" AND WIN32)
  file(TOUCH "${CMAKE_BINARY_DIR}/build.ninja")
endif()
