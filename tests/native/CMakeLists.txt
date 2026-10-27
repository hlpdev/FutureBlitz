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
