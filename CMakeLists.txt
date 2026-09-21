cmake_minimum_required(VERSION 3.20)
project(tapewatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Core: the detection engine. No I/O beyond files, no third-party code.
# ---------------------------------------------------------------------------
add_library(tapewatch_core STATIC
  src/engine/event.cpp
  src/engine/json.cpp
  src/engine/alert.cpp
  src/engine/config.cpp
  src/engine/book_state.cpp
  src/engine/pipeline.cpp
  src/detectors/spoofing.cpp
  src/detectors/layering.cpp
  src/detectors/wash_trading.cpp
  src/detectors/momentum_ignition.cpp
  src/validation/feed_integrity.cpp
)
target_include_directories(tapewatch_core PUBLIC include PRIVATE src)

add_executable(tapewatch-detect src/engine/detect_main.cpp)
target_link_libraries(tapewatch-detect PRIVATE tapewatch_core)

# ---------------------------------------------------------------------------
# Tests. The harness is tests/test_harness.hpp -- thirty lines, no dependency.
# Catch2 would add a network fetch to the container build and about forty
# seconds of compile time per translation unit, for macros this suite does not
# use.
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB TAPEWATCH_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(tapewatch-tests ${TAPEWATCH_TEST_SOURCES})
target_link_libraries(tapewatch-tests PRIVATE tapewatch_core)
target_include_directories(tapewatch-tests PRIVATE tests src)
add_test(NAME unit COMMAND tapewatch-tests)

# ---------------------------------------------------------------------------
# API server. Optional: it needs SQLite and cpp-httplib, and the detection
# engine and its tests must build without either. A bare `g++` checkout with
# no package manager still gets `make build` and `make test`; the console's
# back end is a container concern.
# ---------------------------------------------------------------------------
option(TAPEWATCH_BUILD_API "Build the REST API server (needs SQLite3)" ON)

if(TAPEWATCH_BUILD_API)
  find_package(SQLite3 QUIET)
  if(NOT SQLite3_FOUND)
    message(WARNING "SQLite3 not found -- skipping tapewatch-api. "
                    "Install libsqlite3-dev or configure with -DTAPEWATCH_BUILD_API=OFF.")
  else()
    include(FetchContent)
    FetchContent_Declare(httplib
      GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
      GIT_TAG v0.15.3
      GIT_SHALLOW TRUE
    )
    # Header-only: populate rather than add_subdirectory, so none of its own
    # options, tests or install rules leak into this project.
    FetchContent_GetProperties(httplib)
    if(NOT httplib_POPULATED)
      FetchContent_Populate(httplib)
    endif()

    add_executable(tapewatch-api
      src/api/api_main.cpp
      src/api/store.cpp
      src/api/replay.cpp
    )
    target_include_directories(tapewatch-api PRIVATE src ${httplib_SOURCE_DIR})
    target_link_libraries(tapewatch-api PRIVATE tapewatch_core SQLite::SQLite3)
    find_package(Threads REQUIRED)
    target_link_libraries(tapewatch-api PRIVATE Threads::Threads)
  endif()
endif()
