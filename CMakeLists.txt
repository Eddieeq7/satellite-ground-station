cmake_minimum_required(VERSION 3.20)
project(ground_station CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# -DGS_SANITIZE=thread  or  -DGS_SANITIZE=address
set(GS_SANITIZE "" CACHE STRING "Sanitizer to enable (thread|address)")
if(GS_SANITIZE)
  add_compile_options(-fsanitize=${GS_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${GS_SANITIZE})
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(gs_core src/protocol.cpp src/sequence_tracker.cpp src/poller.cpp src/station.cpp)
target_include_directories(gs_core PUBLIC include)
target_link_libraries(gs_core PUBLIC Threads::Threads)

add_executable(gs_station src/station_main.cpp)
target_link_libraries(gs_station gs_core)

add_executable(gs_sim src/sim_main.cpp)
target_link_libraries(gs_sim gs_core)

add_executable(gs_bench bench/bench_pipeline.cpp)
target_link_libraries(gs_bench gs_core)

option(GS_BUILD_TESTS "Build GoogleTest suite" ON)
if(GS_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_executable(gs_tests tests/test_protocol.cpp tests/test_sequence.cpp tests/test_ring.cpp tests/test_station.cpp)
  target_link_libraries(gs_tests gs_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(gs_tests)
endif()
