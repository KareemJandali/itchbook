cmake_minimum_required(VERSION 3.20)
project(itchbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
  # -march=native is great locally but breaks portable CI; opt in with -DNATIVE=ON
  if(NATIVE)
    add_compile_options(-march=native)
  endif()
endif()

find_package(ZLIB REQUIRED)

# The library is header-only for now (Phase 1). This INTERFACE target carries
# the public include path and the zlib dependency to everything that links it.
add_library(itchbook INTERFACE)
target_include_directories(itchbook INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(itchbook INTERFACE ZLIB::ZLIB)

# ---- Tools (Phase 1 deliverables) ----
add_executable(itch_dump   tools/itch_dump.cpp)
add_executable(itch_census tools/itch_census.cpp)
add_executable(itch_slice  tools/itch_slice.cpp)

# ---- Main driver (Phase 3) ----
add_executable(book_replay tools/book_replay.cpp)

# ---- Benchmark (Phase 4) ----
# Always built with optimisation: measuring a sanitiser build tells you about
# the sanitiser, not the book.
add_executable(book_bench tools/book_bench.cpp)
target_compile_options(book_bench PRIVATE -O3 -fno-omit-frame-pointer)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(book_bench PRIVATE -fno-sanitize=all)
  target_link_options(book_bench PRIVATE -fno-sanitize=all)
endif()

foreach(t itch_dump itch_census itch_slice book_replay book_bench)
  target_link_libraries(${t} PRIVATE itchbook)
endforeach()

# ---- Tests ----
enable_testing()
foreach(t test_parser test_book test_matcher)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE itchbook)
  # tests include "tests/check.hpp"
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR})
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# ---- Property fuzzer (Phase 5) ----
# Runs a bounded number of sequences under ctest so CI stays quick; run the
# binary directly with --iterations for a real soak. Builds optimised even in
# Debug: a sanitiser build manages roughly a tenth the sequences per second,
# and coverage of random sequences is the point.
add_executable(fuzz_matcher tests/fuzz/fuzz_matcher.cpp)
target_link_libraries(fuzz_matcher PRIVATE itchbook)
target_include_directories(fuzz_matcher PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fuzz_matcher PRIVATE -O2)
# Built without sanitizers even in Debug. Under ASan the engine's allocations
# cost roughly ten times as much wall clock, and what this test buys is
# coverage of random sequences — the unit tests already exercise the same code
# under sanitizers. For a sanitized soak, drop these two lines and run fewer
# iterations.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(fuzz_matcher PRIVATE -fno-sanitize=all)
  target_link_options(fuzz_matcher PRIVATE -fno-sanitize=all)
endif()
add_test(NAME fuzz_matcher COMMAND fuzz_matcher --iterations 200000)
