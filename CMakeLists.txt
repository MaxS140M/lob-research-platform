cmake_minimum_required(VERSION 3.25)
project(lob VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(LOB_BUILD_TOOLS "Build the command-line tools" ON)

set(LOB_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

# ---- Core library ----------------------------------------------------------
add_library(lob
  cpp/src/book_builder.cpp
  cpp/src/mapped_file.cpp
  cpp/src/order_book.cpp
)
target_include_directories(lob PUBLIC cpp/include)
target_compile_options(lob PRIVATE ${LOB_WARNINGS})

# ---- Tools -----------------------------------------------------------------
if(LOB_BUILD_TOOLS)
  add_executable(itch_stats cpp/tools/itch_stats.cpp)
  target_link_libraries(itch_stats PRIVATE lob)
  target_compile_options(itch_stats PRIVATE ${LOB_WARNINGS})

  add_executable(book_replay cpp/tools/book_replay.cpp)
  target_link_libraries(book_replay PRIVATE lob)
  target_compile_options(book_replay PRIVATE ${LOB_WARNINGS})
endif()

# ---- Tests -----------------------------------------------------------------
if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    cpp/tests/test_book_builder.cpp
    cpp/tests/test_itch_parser.cpp
    cpp/tests/test_mapped_file.cpp
    cpp/tests/test_matching_engine.cpp
    cpp/tests/test_order_book.cpp
  )
  target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
  target_compile_options(lob_tests PRIVATE ${LOB_WARNINGS})

  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()
