cmake_minimum_required(VERSION 3.20)
project(lob VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build the GoogleTest unit tests" ON)

# ---- Core library ----------------------------------------------------------
add_library(lob
  cpp/src/itch_parser.cpp
  cpp/src/order_book.cpp
)
target_include_directories(lob PUBLIC cpp/include)
target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic)

# ---- Tests -----------------------------------------------------------------
if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests cpp/tests/test_order_book.cpp)
  target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
  target_compile_options(lob_tests PRIVATE -Wall -Wextra -Wpedantic)

  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()
