FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ctest puts the toolchain's DLLs (MinGW's libstdc++ and friends) on PATH for the tests, so
# they run from any terminal.
set(FB_TEST_DLL_PATHS "")
if(MINGW)
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH FB_TEST_DLL_PATHS)
endif()

# Compiler unit tests, plus the end-to-end tests that build and run tests/programs/*.fb.
file(GLOB_RECURSE FB_COMPILER_TEST_SOURCES CONFIGURE_DEPENDS compiler/*.cpp)
add_executable(fb_compiler_tests ${FB_COMPILER_TEST_SOURCES})
target_link_libraries(fb_compiler_tests PRIVATE fb_compiler Catch2::Catch2WithMain)
target_compile_definitions(fb_compiler_tests PRIVATE
  FB_TEST_PROGRAMS="${CMAKE_CURRENT_SOURCE_DIR}/programs"
  FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/programs")
add_dependencies(fb_compiler_tests fb_runtime) # programs link against it
fb_set_warnings(fb_compiler_tests)
fb_copy_llvm(fb_compiler_tests)
catch_discover_tests(fb_compiler_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})

# CLI tests, which run the real fb.
file(GLOB_RECURSE FB_CLI_TEST_SOURCES CONFIGURE_DEPENDS cli/*.cpp)
add_executable(fb_cli_tests ${FB_CLI_TEST_SOURCES})
target_link_libraries(fb_cli_tests PRIVATE fb_compiler Catch2::Catch2WithMain)
target_compile_definitions(fb_cli_tests PRIVATE
  FB_CLI="$<TARGET_FILE:fb_cli>"
  FB_TEST_PROGRAMS="${CMAKE_CURRENT_SOURCE_DIR}/programs"
  FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/cli")
add_dependencies(fb_cli_tests fb_cli)
fb_set_warnings(fb_cli_tests)
fb_copy_llvm(fb_cli_tests)
catch_discover_tests(fb_cli_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})

# Runtime unit tests.
file(GLOB_RECURSE FB_RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
add_executable(fb_runtime_tests ${FB_RUNTIME_TEST_SOURCES})
target_link_libraries(fb_runtime_tests PRIVATE fb_runtime Catch2::Catch2WithMain)
fb_set_warnings(fb_runtime_tests)
catch_discover_tests(fb_runtime_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})
