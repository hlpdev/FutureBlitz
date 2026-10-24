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

# The C library that packages in tests/packages link as a [native] library.
add_subdirectory(native)

# Compiler unit tests, plus the end-to-end tests that build and run tests/programs/*.fb.
file(GLOB_RECURSE FB_COMPILER_TEST_SOURCES CONFIGURE_DEPENDS compiler/*.cpp)
add_executable(fb_compiler_tests ${FB_COMPILER_TEST_SOURCES})
target_link_libraries(fb_compiler_tests PRIVATE fb_compiler Catch2::Catch2WithMain)
target_compile_definitions(fb_compiler_tests PRIVATE
  FB_TEST_PROGRAMS="${CMAKE_CURRENT_SOURCE_DIR}/programs"
  FB_TEST_PACKAGES="${CMAKE_CURRENT_SOURCE_DIR}/packages"
  FB_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
  FB_TEST_VECMATH="$<TARGET_FILE:fb_vecmath>"
  FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/programs"
  FB_TEST_COMPILE_COMMANDS="${CMAKE_BINARY_DIR}/compile_commands.json")
add_dependencies(fb_compiler_tests ${FB_RUNTIME_ARCHIVES} fb_vecmath) # programs link them
fb_set_warnings(fb_compiler_tests)
fb_set_stack(fb_compiler_tests)
fb_copy_llvm(fb_compiler_tests)
catch_discover_tests(fb_compiler_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})

# CLI tests, which run the real fb.
file(GLOB_RECURSE FB_CLI_TEST_SOURCES CONFIGURE_DEPENDS cli/*.cpp)
add_executable(fb_cli_tests ${FB_CLI_TEST_SOURCES})
target_link_libraries(fb_cli_tests PRIVATE fb_compiler Catch2::Catch2WithMain)
target_compile_definitions(fb_cli_tests PRIVATE
  FB_CLI="$<TARGET_FILE:fb_cli>"
  FB_TEST_PROGRAMS="${CMAKE_CURRENT_SOURCE_DIR}/programs"
  FB_TEST_PACKAGES="${CMAKE_CURRENT_SOURCE_DIR}/packages"
  FB_TEST_VECMATH="$<TARGET_FILE:fb_vecmath>"
  FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/cli")
add_dependencies(fb_cli_tests fb_cli fb_vecmath)
if(TARGET fb_console_probe)
  add_dependencies(fb_cli_tests fb_console_probe)
  target_compile_definitions(fb_cli_tests PRIVATE
    FB_TEST_CONSOLE_PROBE="$<TARGET_FILE:fb_console_probe>")
endif()
# `fb bindgen` runs fb-bindgen, which is next to fb where it's built.
if(TARGET fb_bindgen_cli)
  add_dependencies(fb_cli_tests fb_bindgen_cli)
  target_compile_definitions(fb_cli_tests PRIVATE FB_TEST_BINDGEN=1)
endif()
fb_set_warnings(fb_cli_tests)
fb_copy_llvm(fb_cli_tests)
catch_discover_tests(fb_cli_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})

# fb-bindgen's tests, where it's built: each header becomes the module it should, which
# compiles.
if(TARGET fb_bindgen)
  file(GLOB_RECURSE FB_BINDGEN_TEST_SOURCES CONFIGURE_DEPENDS bindgen/*.cpp)
  add_executable(fb_bindgen_tests ${FB_BINDGEN_TEST_SOURCES})
  target_link_libraries(fb_bindgen_tests PRIVATE fb_bindgen Catch2::Catch2WithMain)
  target_compile_definitions(fb_bindgen_tests PRIVATE
    FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/bindgen"
    FB_TEST_PACKAGES="${CMAKE_CURRENT_SOURCE_DIR}/packages"
    FB_TEST_VECMATH_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/native/vecmath/vecmath.h")
  fb_set_warnings(fb_bindgen_tests)
  fb_copy_llvm(fb_bindgen_tests)
  fb_copy_libclang(fb_bindgen_tests)
  catch_discover_tests(fb_bindgen_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})
endif()

# Runtime unit tests.
file(GLOB_RECURSE FB_RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
add_executable(fb_runtime_tests ${FB_RUNTIME_TEST_SOURCES})
target_link_libraries(fb_runtime_tests PRIVATE fb_runtime fb_runtime_tz fb_tzdata Catch2::Catch2WithMain)
target_compile_definitions(fb_runtime_tests PRIVATE
  FB_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/programs/image_text")
fb_set_warnings(fb_runtime_tests)
catch_discover_tests(fb_runtime_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})
