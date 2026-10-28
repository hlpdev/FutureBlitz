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
  FB_TEST_DYNCALC="$<TARGET_FILE:fb_dyncalc>"
  FB_TEST_DYNCALC_LINKED="$<TARGET_LINKER_FILE:fb_dyncalc>"
  FB_TEST_ENGINE="$<TARGET_FILE:fb_engine>"
  FB_TEST_ENGINE_LINKED="$<TARGET_LINKER_FILE:fb_engine>"
  FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/programs"
  FB_TEST_COMPILE_COMMANDS="${CMAKE_BINARY_DIR}/compile_commands.json"
  FB_TEST_HTTP_SERVER="$<TARGET_FILE:fb_http_test_server>")
# Programs link the archives; http_ programs run under the HTTP test server.
add_dependencies(fb_compiler_tests ${FB_RUNTIME_ARCHIVES} fb_vecmath fb_dyncalc fb_engine fb_http_test_server)
fb_set_warnings(fb_compiler_tests)
fb_set_stack(fb_compiler_tests)
fb_copy_llvm(fb_compiler_tests)
catch_discover_tests(fb_compiler_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})
# The test programs, over a thousand, are built and run in shards that ctest runs side by side:
# shard i of n checks every n-th program from the i-th.
set(FB_PROGRAM_SHARDS 32)
math(EXPR FB_LAST_PROGRAM_SHARD "${FB_PROGRAM_SHARDS} - 1")
foreach(shard RANGE ${FB_LAST_PROGRAM_SHARD})
  set(test "programs print what they should ${shard}/${FB_PROGRAM_SHARDS}")
  add_test(NAME "${test}" COMMAND fb_compiler_tests "programs print what they should")
  # A shard takes about two minutes, 32 at once, so a hung program fails it in 20, not never.
  set_tests_properties("${test}" PROPERTIES
    ENVIRONMENT "FB_PROGRAM_SHARD=${shard}/${FB_PROGRAM_SHARDS}" TIMEOUT 1200)
  if(FB_TEST_DLL_PATHS)
    set_tests_properties("${test}" PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FB_TEST_DLL_PATHS}")
  endif()
endforeach()

# tools/verify.py's own tests, in Python, which ask ctest about the shards above.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME "verify.py's own tests" COMMAND Python3::Interpreter -m unittest -v verify_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools)
  set_tests_properties("verify.py's own tests" PROPERTIES ENVIRONMENT
    "FB_TEST_BUILD=${CMAKE_BINARY_DIR};PYTHONDONTWRITEBYTECODE=1")
endif()

# CLI tests, which run the real fb.
file(GLOB_RECURSE FB_CLI_TEST_SOURCES CONFIGURE_DEPENDS cli/*.cpp)
add_executable(fb_cli_tests ${FB_CLI_TEST_SOURCES})
target_link_libraries(fb_cli_tests PRIVATE fb_compiler Catch2::Catch2WithMain)
target_compile_definitions(fb_cli_tests PRIVATE
  FB_CLI="$<TARGET_FILE:fb_cli>"
  FB_TEST_PROGRAMS="${CMAKE_CURRENT_SOURCE_DIR}/programs"
  FB_TEST_PACKAGES="${CMAKE_CURRENT_SOURCE_DIR}/packages"
  FB_TEST_VECMATH="$<TARGET_FILE:fb_vecmath>"
  FB_TEST_DYNCALC="$<TARGET_FILE:fb_dyncalc>"
  FB_TEST_DYNCALC_LINKED="$<TARGET_LINKER_FILE:fb_dyncalc>"
  FB_TEST_ENGINE="$<TARGET_FILE:fb_engine>"
  FB_TEST_ENGINE_LINKED="$<TARGET_LINKER_FILE:fb_engine>"
  FB_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/cli")
add_dependencies(fb_cli_tests fb_cli fb_vecmath fb_dyncalc fb_engine)
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
    FB_TEST_VECMATH_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/native/vecmath/vecmath.h"
    FB_TEST_ENGINE_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/native/engine/engine.h")
  fb_set_warnings(fb_bindgen_tests)
  fb_copy_llvm(fb_bindgen_tests)
  fb_copy_libclang(fb_bindgen_tests)
  catch_discover_tests(fb_bindgen_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})
endif()

# Runtime unit tests, with the runtime's sockets over libuv, and its internal header.
file(GLOB_RECURSE FB_RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
add_executable(fb_runtime_tests ${FB_RUNTIME_TEST_SOURCES})
target_link_libraries(fb_runtime_tests PRIVATE fb_runtime_markdown md4c fb_runtime_zip fb_runtime_io
                      uv_a fb_runtime fb_runtime_tz fb_tzdata Catch2::Catch2WithMain)
target_include_directories(fb_runtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
# CommonMark's spec examples, which std.text.markdown's HTML passes.
target_compile_definitions(fb_runtime_tests PRIVATE
  FB_TEST_COMMONMARK_SPEC="${CMAKE_CURRENT_SOURCE_DIR}/runtime/data/commonmark/spec.json")
# std.term's tests run the term probe in a terminal of their own: a pty, which forkpty makes.
add_dependencies(fb_runtime_tests fb_term_probe)
target_compile_definitions(fb_runtime_tests PRIVATE FB_TEST_TERM_PROBE="$<TARGET_FILE:fb_term_probe>")
if(UNIX AND NOT APPLE)
  target_link_libraries(fb_runtime_tests PRIVATE util)
endif()
# image_text's fonts, which std.image's font tests break.
target_compile_definitions(fb_runtime_tests PRIVATE
  FB_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/programs/image_text")
# The OS's roots for TLS, inside the runtime's HTTP part, and mbedTLS's X.509, which reads them;
# and the runtime's TLS, with the HTTP test server's certificates.
target_link_libraries(fb_runtime_tests PRIVATE fb_runtime_http fb_runtime_tls mbedx509)
target_compile_definitions(fb_runtime_tests PRIVATE
  FB_TEST_TLS_CERTIFICATE="${CMAKE_CURRENT_SOURCE_DIR}/native/http_server/server.pem"
  FB_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/native/http_server/server_key.pem"
  FB_TEST_TLS_CA="${CMAKE_CURRENT_SOURCE_DIR}/native/http_server/test_ca.pem")
target_include_directories(fb_runtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
fb_set_warnings(fb_runtime_tests)
catch_discover_tests(fb_runtime_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})

# The ACME tests' certificate authority's own tests, which a client of their own drives.
add_executable(fb_acme_ca_tests native/acme_ca/acme_ca_tests.cpp)
target_link_libraries(fb_acme_ca_tests PRIVATE fb_acme_ca mbedx509 tfpsacrypto
                      Catch2::Catch2WithMain)
fb_set_warnings(fb_acme_ca_tests)
catch_discover_tests(fb_acme_ca_tests DISCOVERY_MODE PRE_TEST DL_PATHS ${FB_TEST_DLL_PATHS})
