cmake_minimum_required(VERSION 3.14)
project(datetime LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# datetime.cc parses numbers out of date/time strings with the strict strto*
# wrappers (strict_stoi / strict_stoul / strict_stod) from the already-extracted
# strict-stox library. That is the pure core's only third-party dependency:
# FetchContent pulls it in at the SHA datetime was extracted against and puts
# strict_stox.hh on the include path so `#include "strict_stox.hh"` resolves.
#
# Note: the hashes / perfect-hash (phf) machinery used by the original Xapiand
# datetime.cc lived entirely in the MsgPack structured-object path, which is NOT
# part of this library (it stays in Xapiand as glue). So the pure core needs only
# strict-stox, not hashes.
FetchContent_Declare(
  strict_stox
  GIT_REPOSITORY https://github.com/Kronuz/strict-stox.git
  GIT_TAG        21b7941c71064b5a4e52ffb50a2701ee4aae4c6c
)
FetchContent_MakeAvailable(strict_stox)

# --- Library target ---
# datetime is a compiled STATIC library: the ISO 8601 + Elasticsearch-style
# date-math parser/formatter (datetime.cc) plus the time / timedelta parsers and
# the tm_t / clk_t / Format types and the DatetimeError exception hierarchy in
# datetime.h. The header keeps its original Xapiand name so a consumer that
# already `#include "datetime.h"` just needs this repo on its include path.
add_library(datetime STATIC datetime.cc)
target_include_directories(datetime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(datetime PUBLIC cxx_std_20)
target_link_libraries(datetime PUBLIC strict_stox::strict_stox)
# Alias so consumers can link `datetime::datetime` whether they add this via
# add_subdirectory or FetchContent.
add_library(datetime::datetime ALIAS datetime)

# Only build the test when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(datetime_test test/test.cc)
  target_link_libraries(datetime_test PRIVATE datetime)
  add_test(NAME datetime COMMAND datetime_test)

  # Differential test against the frozen original implementation (test/old,
  # namespace DatetimeOld): random, mutated and exhaustive inputs, valid and
  # invalid, compared field by field. Argument: iterations per random family.
  add_executable(datetime_diff_test test/diff_test.cc test/old/datetime_old.cc)
  target_include_directories(datetime_diff_test PRIVATE test)
  target_link_libraries(datetime_diff_test PRIVATE datetime)
  add_test(NAME datetime_diff COMMAND datetime_diff_test 20000)

  # Exhaustive years 1..9999 and huge date-math terms against independent
  # references (brute-force day counter, days-from-civil in 128-bit).
  add_executable(datetime_datemath_test test/datemath_test.cc)
  target_link_libraries(datetime_datemath_test PRIVATE datetime)
  add_test(NAME datetime_datemath COMMAND datetime_datemath_test 20000)

  # Benchmark against the frozen original (no external comparators needed):
  #   cmake -B build -DDATETIME_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
  option(DATETIME_BUILD_BENCH "Build the old-vs-new benchmark" OFF)
  if(DATETIME_BUILD_BENCH)
    add_executable(datetime_bench bench/bench.cc test/old/datetime_old.cc)
    target_include_directories(datetime_bench PRIVATE test)
    target_link_libraries(datetime_bench PRIVATE datetime)
  endif()

  # A runnable tour of the library (not a test): build & run ./build/datetime_demo
  add_executable(datetime_demo examples/demo.cc)
  target_link_libraries(datetime_demo PRIVATE datetime)
endif()
