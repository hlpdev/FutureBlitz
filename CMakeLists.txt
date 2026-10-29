cmake_minimum_required(VERSION 3.28)
project(FutureBlitz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules here; scanning adds GCC-only flags that break clang-tidy and clangd.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(FB_BUILD_TESTS "Build the test suite" ON)
# Only what programs link: the runtime, std's bundled libraries, and lib/<target>/, with no LLVM.
# The Linux release builds these against glibc 2.28, apart from fb itself.
option(FB_RUNTIME_ONLY "Build only the runtime's pieces, which programs link" OFF)

# MSVC otherwise reads source files in the ANSI code page.
if(MSVC)
  add_compile_options(/utf-8)
endif()

# Programs are position-independent executables, which link the runtime and std's bundled
# libraries, so those are too, where the compiler doesn't make them so by default, as
# AlmaLinux's doesn't.
if(NOT WIN32)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# The Windows C runtime's headers call getenv and friends unsafe.
if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

include(FetchContent)
include(cmake/warnings.cmake)
include(cmake/stack.cmake)
if(NOT FB_RUNTIME_ONLY)
  include(cmake/llvm.cmake)
  include(cmake/libclang.cmake)
endif()
include(cmake/toolchain.cmake)

add_subdirectory(third_party)
add_subdirectory(compiler)
add_subdirectory(runtime)
if(NOT FB_RUNTIME_ONLY)
  add_subdirectory(cli)
  add_subdirectory(lsp)
  add_subdirectory(tools/bindgen)
endif()
include(cmake/install.cmake)

if(FB_BUILD_TESTS AND NOT FB_RUNTIME_ONLY)
  enable_testing()
  add_subdirectory(tests)
endif()
