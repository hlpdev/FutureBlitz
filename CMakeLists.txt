cmake_minimum_required(VERSION 3.28)
project(FutureBlitz VERSION 1.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules here; scanning adds GCC-only flags that break clang-tidy and clangd.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The version fb says it is: a release's, from its v<version> tag (release.yml), else this file's.
set(FB_VERSION "" CACHE STRING "The version fb says it is, like 1.0.0; empty is CMakeLists.txt's")
if(FB_VERSION STREQUAL "")
  set(fb_version "${PROJECT_VERSION}")
else()
  set(fb_version "${FB_VERSION}")
endif()
if(NOT fb_version MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
  message(FATAL_ERROR "FB_VERSION is three numbers, like 1.0.0, not \"${fb_version}\"")
endif()
set(fb_version_major "${CMAKE_MATCH_1}")
set(fb_version_minor "${CMAKE_MATCH_2}")
set(fb_version_patch "${CMAKE_MATCH_3}")

# std.core's FB_VERSION and its numbers are the version fb is built as, which a release writes
# in std/core/version.fb when it sets this file's.
set(fb_version_file "${PROJECT_SOURCE_DIR}/std/core/version.fb")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fb_version_file}")
file(READ "${fb_version_file}" fb_version_text)
foreach(fb_name_and_value IN ITEMS "FB_VERSION=\"${fb_version}\"" "FB_VERSION_MAJOR=${fb_version_major}"
                                    "FB_VERSION_MINOR=${fb_version_minor}"
                                    "FB_VERSION_PATCH=${fb_version_patch}")
  string(FIND "${fb_name_and_value}" "=" fb_at)
  string(SUBSTRING "${fb_name_and_value}" 0 ${fb_at} fb_name)
  math(EXPR fb_at "${fb_at} + 1")
  string(SUBSTRING "${fb_name_and_value}" ${fb_at} -1 fb_wanted)
  if(NOT fb_version_text MATCHES "Public Const ${fb_name} = ([^\r\n]*)"
     OR NOT CMAKE_MATCH_1 STREQUAL fb_wanted)
    message(FATAL_ERROR "std/core/version.fb's ${fb_name} is to be ${fb_wanted}, as fb is built "
                        "as ${fb_version}")
  endif()
endforeach()

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
