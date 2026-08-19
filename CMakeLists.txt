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

foreach(t itch_dump itch_census itch_slice)
  target_link_libraries(${t} PRIVATE itchbook)
endforeach()

# ---- Tests ----
enable_testing()
add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE itchbook)
add_test(NAME test_parser COMMAND test_parser)
