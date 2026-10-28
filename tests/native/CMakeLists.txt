# The C library the FFI tests link as a package's [native] library, built with the C compiler
# that builds the runtime. Programs link MinGW's UCRT, not the msvcrt it defaults to, so the
# library is compiled for UCRT too.
add_library(fb_vecmath STATIC vecmath/vecmath.c)
set_target_properties(fb_vecmath PROPERTIES OUTPUT_NAME vecmath C_STANDARD 11)
if(MSVC)
  target_compile_options(fb_vecmath PRIVATE /W4)
else()
  target_compile_options(fb_vecmath PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(MINGW)
  target_compile_options(fb_vecmath PRIVATE -mcrtdll=ucrt)
endif()

# The C library the tests load as a package's [native] dynamic library, which the build copies
# beside the program: dyncalc.dll and its import library, libdyncalc.dll.a, on Windows,
# libdyncalc.dylib on macOS, whose install name is @rpath/libdyncalc.dylib, and on Linux
# libdyncalc.so.1, its SONAME, which the build copies it as. Like vecmath, it's built for UCRT on
# MinGW, and with its C runtime support inside it, so it needs no DLL of MinGW's.
add_library(fb_dyncalc SHARED dyncalc/dyncalc.c)
set_target_properties(fb_dyncalc PROPERTIES OUTPUT_NAME dyncalc C_STANDARD 11
                      C_VISIBILITY_PRESET hidden)
if(WIN32)
  set_target_properties(fb_dyncalc PROPERTIES PREFIX "")
elseif(NOT APPLE)
  set_target_properties(fb_dyncalc PROPERTIES SOVERSION 1)
endif()

if(MSVC)
  target_compile_options(fb_dyncalc PRIVATE /W4)
else()
  target_compile_options(fb_dyncalc PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(MINGW)
  target_compile_options(fb_dyncalc PRIVATE -mcrtdll=ucrt)
  target_link_options(fb_dyncalc PRIVATE -mcrtdll=ucrt -static-libgcc)
endif()

# The stand-in engine, a C++ library in a DLL of its own with a C API, as the real engine will be,
# which tests/packages/engine wraps: engine.dll and libengine.dll.a on Windows, libengine.so on
# Linux, and libengine.dylib, as @rpath/libengine.dylib, on macOS. Its C++ runtime is linked
# into it, so it needs nothing but the OS's own libraries.
add_library(fb_engine SHARED engine/engine.cpp)
set_target_properties(fb_engine PROPERTIES OUTPUT_NAME engine CXX_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(fb_engine PRIVATE ENGINE_BUILDING)
fb_set_warnings(fb_engine)
if(WIN32)
  set_target_properties(fb_engine PROPERTIES PREFIX "")
endif()

# MinGW's static winpthread needs UCRT, as programs' does. On Linux it exports only its C API,
# not the C++ runtime linked into it, which a program could otherwise take over. MSVC's and
# macOS's C++ runtimes are the system's.
if(MINGW)
  target_compile_options(fb_engine PRIVATE -mcrtdll=ucrt)
  target_link_options(fb_engine PRIVATE -static -mcrtdll=ucrt)
elseif(UNIX AND NOT APPLE)
  target_link_options(fb_engine PRIVATE -static-libstdc++ -static-libgcc
                      "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/engine/engine.map")
  set_target_properties(fb_engine PROPERTIES
                        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/engine/engine.map")
endif()

# On Windows, a program that prints the console's output code page, for the tests that check fb
# and the programs it builds give the console back as they found it.
if(WIN32)
  add_executable(fb_console_probe console_probe/console_probe.c)
  set_target_properties(fb_console_probe PROPERTIES C_STANDARD 11)
endif()

# For std.term's runtime tests, a program that reads its terminal's keys as std.term does, which
# they run in a pseudoconsole on Windows and a pty elsewhere. It finds MinGW's DLLs as the tests
# do, on the PATH ctest gives them.
add_executable(fb_term_probe term_probe/term_probe.cpp)
target_link_libraries(fb_term_probe PRIVATE fb_runtime)
fb_set_warnings(fb_term_probe)

# The ACME tests' certificate authority, as Pebble is for Let's Encrypt's, which a test server
# hosts, over mbedTLS's X.509 and crypto.
add_library(fb_acme_ca STATIC acme_ca/acme_ca.cpp)
target_include_directories(fb_acme_ca PUBLIC acme_ca)
target_link_libraries(fb_acme_ca PUBLIC mbedx509 tfpsacrypto)
fb_set_warnings(fb_acme_ca)

# The HTTP and HTTPS server std.net.http's program tests run against (an http_ program runs as
# its child), with the test certificates make_certificates.sh made, over mbedTLS's TLS and
# nghttp2's HTTP/2, and compressing by zlib-ng and zstd. It hosts the ACME tests' CA too.
add_executable(fb_http_test_server http_server/http_server.cpp)
target_link_libraries(fb_http_test_server PRIVATE fb_acme_ca mbedtls mbedx509 tfpsacrypto
                      nghttp2 zlib-ng zstd)
# It uses only nghttp2's functions of nghttp2_ssize, so it needs no ssize_t, which MSVC's headers,
# which clang-tidy reads, don't have.
target_compile_definitions(fb_http_test_server PRIVATE
  NGHTTP2_NO_SSIZE_T
  FB_HTTP_TEST_CERTIFICATE="${CMAKE_CURRENT_SOURCE_DIR}/http_server/server.pem"
  FB_HTTP_TEST_KEY="${CMAKE_CURRENT_SOURCE_DIR}/http_server/server_key.pem"
  FB_HTTP_TEST_CA="${CMAKE_CURRENT_SOURCE_DIR}/http_server/test_ca.pem")
if(WIN32)
  target_link_libraries(fb_http_test_server PRIVATE ws2_32)
endif()
