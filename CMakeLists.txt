cmake_minimum_required(VERSION 3.28)
project(FutureBlitz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules here; scanning adds GCC-only flags that break clang-tidy and clangd.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(FB_BUILD_TESTS "Build the test suite" ON)

# MSVC otherwise reads source files in the ANSI code page.
if(MSVC)
  add_compile_options(/utf-8)
endif()

# The Windows C runtime's headers call getenv and friends unsafe.
if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

include(FetchContent)
include(cmake/warnings.cmake)
include(cmake/stack.cmake)
include(cmake/llvm.cmake)
include(cmake/libclang.cmake)

add_subdirectory(third_party)
add_subdirectory(compiler)
add_subdirectory(runtime)
add_subdirectory(cli)
add_subdirectory(lsp)
add_subdirectory(tools/bindgen)

if(FB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
