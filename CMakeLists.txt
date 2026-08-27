cmake_minimum_required(VERSION 3.20)
project(itchbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# ThreadSanitizer instead of ASan/UBSan, because they cannot share a binary.
# Phase 10 is the first code here with two threads in it, and a lock-free
# structure that has never been under a race detector is an assertion rather
# than a result -- so this is a third CI job, not a flag nobody sets.
option(ITCHBOOK_TSAN "Build with ThreadSanitizer instead of ASan/UBSan" OFF)

if(ITCHBOOK_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=thread)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # -fno-sanitize-recover is the flag that makes UBSan mean something. Without
  # it UBSan DIAGNOSES and CONTINUES: it prints "runtime error: signed integer
  # overflow" to stderr, the process exits 0, and CI goes green with undefined
  # behaviour in the build it just blessed. ASan aborts on its own; UBSan has
  # to be told to. A sanitizer nobody's exit code listens to is decoration.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
endif()

add_executable(spsc_stress tests/stress/spsc_stress.cpp)
target_link_libraries(spsc_stress PRIVATE itchbook)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
  # -march=native is great locally but breaks portable CI; opt in with -DNATIVE=ON
  if(NATIVE)
    add_compile_options(-march=native)
  endif()
endif()

find_package(ZLIB REQUIRED)

# Phase 10 is the first thing here with a thread in it. Threads::Threads rather
# than -pthread by hand: the flag differs between platforms and toolchains, and
# a race that only appears in one of them is not a race anyone wants to debug.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# The library is header-only for now (Phase 1). This INTERFACE target carries
# the public include path and the zlib dependency to everything that links it.
add_library(itchbook INTERFACE)
target_include_directories(itchbook INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(itchbook INTERFACE ZLIB::ZLIB Threads::Threads)

# ---- Tools (Phase 1 deliverables) ----
add_executable(itch_dump   tools/itch_dump.cpp)
add_executable(itch_census tools/itch_census.cpp)
add_executable(itch_slice  tools/itch_slice.cpp)

# ---- Main driver (Phase 3) ----
add_executable(book_replay tools/book_replay.cpp)
# Phase 12.1: runs the phase-9 path and the split replayer over one feed in
# one process and compares the book each message touched, after every message.
add_executable(split_replay_gate tools/split_replay_gate.cpp)

# ---- Benchmark (Phase 4) ----
# Always built with optimisation: measuring a sanitiser build tells you about
# the sanitiser, not the book.
add_executable(queue_sim tools/queue_sim.cpp)
add_executable(queue_backtest tools/queue_backtest.cpp)
add_executable(calibrate_intensity tools/calibrate_intensity.cpp)
add_executable(as_experiment tools/as_experiment.cpp)
add_executable(latency_sweep tools/latency_sweep.cpp)
add_executable(mold_wrap   tools/mold_wrap.cpp)
add_executable(mold_replay tools/mold_replay.cpp)
add_executable(mold_damage tools/mold_damage.cpp)
add_executable(restart_check tools/restart_check.cpp)

# ---- Phase 10 ----
# These two follow the build type, unlike book_bench and tsc_offset above, and
# the difference is deliberate. A sanitiser's instrumentation is far larger than
# the handoff being measured, so a MEASUREMENT run is -DCMAKE_BUILD_TYPE=Release
# and gets no sanitiser at all. But these are the first programs here with two
# threads and a socket in them, and Debug is where their correctness is checked:
# under ASan/UBSan, and under ThreadSanitizer via -DITCHBOOK_TSAN=ON. Excusing
# them from sanitisers to protect numbers that are never taken in Debug would
# buy nothing and cost the only race detection they have.
add_executable(mold_replay_udp tools/mold_replay_udp.cpp)
add_executable(wire_to_book tools/wire_to_book.cpp)

# Phase 12.7's two halves. They are separate PROCESSES on purpose -- the whole
# claim is that the feed leaves one address space and is understood in another
# -- so they are two targets rather than two modes of one binary.
add_executable(exchange tools/exchange.cpp)
add_executable(strategy tools/strategy.cpp)

add_executable(tsc_offset tools/tsc_offset.cpp)
target_compile_options(tsc_offset PRIVATE -O2)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(tsc_offset PRIVATE -fno-sanitize=all)
  target_link_options(tsc_offset PRIVATE -fno-sanitize=all)
endif()

# Same treatment as tsc_offset and for the same reason: this measures how long a
# thread goes without running, so a sanitiser's instrumentation would be part of
# the measurement rather than beside it.
add_executable(cpu_jitter tools/cpu_jitter.cpp)
target_compile_options(cpu_jitter PRIVATE -O2)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(cpu_jitter PRIVATE -fno-sanitize=all)
  target_link_options(cpu_jitter PRIVATE -fno-sanitize=all)
endif()
find_package(Threads REQUIRED)
target_link_libraries(cpu_jitter PRIVATE Threads::Threads)

add_executable(book_bench tools/book_bench.cpp)
target_compile_options(book_bench PRIVATE -O3 -fno-omit-frame-pointer)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(book_bench PRIVATE -fno-sanitize=all)
  target_link_options(book_bench PRIVATE -fno-sanitize=all)
endif()

foreach(t itch_dump itch_census itch_slice book_replay book_bench queue_sim queue_backtest
          split_replay_gate
          calibrate_intensity as_experiment
          tsc_offset mold_replay_udp wire_to_book exchange strategy
          latency_sweep mold_wrap mold_replay mold_damage restart_check)
  target_link_libraries(${t} PRIVATE itchbook)
endforeach()

# ---- Tests ----
enable_testing()
foreach(t test_parser test_book test_book_set test_live_orders test_spsc_ring test_reader_thread
          test_matcher test_fees test_queue test_ledger test_split_replay test_ouch
          test_soupbin test_gateway
          test_sim test_kill_switch test_mold test_recover test_restart test_closed_loop test_as_maker test_intensity)
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

# ---- Gateway fuzz + cross-protocol differential (Phase 12.6) ----
# Built WITH sanitizers, unlike fuzz_matcher above. That opt-out is a
# throughput decision for a 200,000-sequence run; the cost here was measured
# rather than inherited -- ~110k ops/s under ASan/UBSan, so the phase's
# million-operation done-condition takes about two minutes. The ctest run is
# smaller; the CI job does the full million.
add_executable(fuzz_gateway tests/fuzz/fuzz_gateway.cpp)
target_link_libraries(fuzz_gateway PRIVATE itchbook)
target_include_directories(fuzz_gateway PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fuzz_gateway PRIVATE -O2)
add_test(NAME fuzz_gateway COMMAND fuzz_gateway --iterations 3000)

add_executable(fuzz_queue tests/fuzz/fuzz_queue.cpp)
target_link_libraries(fuzz_queue PRIVATE itchbook)
target_include_directories(fuzz_queue PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fuzz_queue PRIVATE -O2)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(fuzz_queue PRIVATE -fno-sanitize=all)
  target_link_options(fuzz_queue PRIVATE -fno-sanitize=all)
endif()
add_test(NAME fuzz_queue COMMAND fuzz_queue --iterations 50000)
