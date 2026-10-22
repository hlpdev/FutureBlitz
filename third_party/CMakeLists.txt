# The C libraries std is built on, each fetched and built by the C compiler that builds the
# runtime, for the build's target, as an archive of its own, so a program links only what it
# uses. docs/stdlib.md lists them, and fb names each one a build links, with its license.

# libuv: the runtime's I/O thread runs its loop, for std.io, std.os, std.net, std.time's timers,
# and every Async twin of a blocking call.
set(FB_LIBUV_VERSION 1.52.1)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG v${FB_LIBUV_VERSION}
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(libuv)
# Its warnings are its own. GCC 14 and later make one an error, a const mismatch in uv_cpu_info's
# Windows code, which is harmless.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(uv_a PRIVATE -w -Wno-error=incompatible-pointer-types)
endif()
# Programs link MinGW's UCRT, not the msvcrt it defaults to, so what they link is compiled for it.
if(MINGW)
  target_compile_options(uv_a PRIVATE -mcrtdll=ucrt)
endif()
set(FB_LIBUV_VERSION ${FB_LIBUV_VERSION} PARENT_SCOPE)
# Its notices, in one file: libuv's license, then those of the code it has from Joyent's libuv,
# FreeBSD's tree.h, and ISC's inet.c, whose texts are at the top of those files.
file(READ "${libuv_SOURCE_DIR}/LICENSE" notices)
file(READ "${libuv_SOURCE_DIR}/LICENSE-extra" extra)
string(APPEND notices "\n${extra}")
foreach(part include/uv/tree.h src/inet.c)
  file(READ "${libuv_SOURCE_DIR}/${part}" text)
  string(FIND "${text}" "*/" end)
  math(EXPR end "${end} + 2")
  string(SUBSTRING "${text}" 0 ${end} license)
  string(APPEND notices "\n${part}:\n\n${license}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/notices/libuv.txt" "${notices}")
set(FB_LIBUV_NOTICE "${CMAKE_CURRENT_BINARY_DIR}/notices/libuv.txt" PARENT_SCOPE)
set(FB_LIBUV_LICENSES "the MIT, BSD-2-Clause, and ISC licenses" PARENT_SCOPE)
