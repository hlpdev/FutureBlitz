# Installs the build in FB_BUILD as a release, into FB_OUTPUT/futureblitz, then, with PATH an empty
# folder's, so nothing but the release is there to run, makes `fb new`'s package with the
# installed fb, runs it, and has fb-bindgen read a C header, where it's built.

file(REMOVE_RECURSE "${FB_OUTPUT}")
file(MAKE_DIRECTORY "${FB_OUTPUT}/empty")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${FB_BUILD}" --prefix "${FB_OUTPUT}/futureblitz"
  OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)

# The release's folders, and nothing the libraries it's built from install of their own.
file(GLOB folders RELATIVE "${FB_OUTPUT}/futureblitz" "${FB_OUTPUT}/futureblitz/*")
file(GLOB libraries RELATIVE "${FB_OUTPUT}/futureblitz/lib" "${FB_OUTPUT}/futureblitz/lib/*")
list(SORT folders)
list(REMOVE_ITEM libraries clang)
if(NOT folders STREQUAL "bin;lib;lldb;notices;std" OR NOT libraries MATCHES "^[a-z]+-[a-z0-9]+$")
  message(FATAL_ERROR "The release has ${folders}, and in lib/, ${libraries}")
endif()

# What every machine has stays out of bin/: the C runtime, its loader, and the system's own.
file(GLOB system RELATIVE "${FB_OUTPUT}/futureblitz/bin" "${FB_OUTPUT}/futureblitz/bin/ld-*"
  "${FB_OUTPUT}/futureblitz/bin/libc.*" "${FB_OUTPUT}/futureblitz/bin/libm.*"
  "${FB_OUTPUT}/futureblitz/bin/libstdc++*" "${FB_OUTPUT}/futureblitz/bin/libgcc_s*"
  "${FB_OUTPUT}/futureblitz/bin/libSystem*" "${FB_OUTPUT}/futureblitz/bin/libc++*"
  "${FB_OUTPUT}/futureblitz/bin/kernel32.dll" "${FB_OUTPUT}/futureblitz/bin/ucrtbase.dll")
if(system)
  message(FATAL_ERROR "The release's bin/ has the system's ${system}")
endif()

# The notices of what's in the release besides std's bundled libraries: LLVM, toml++ in fb,
# nlohmann/json in fb-lsp, and the C runtime's pieces, with winpthreads and the libxml2 in lld.exe
# on Windows.
set(notices llvm tomlplusplus nlohmann_json)
if(CMAKE_HOST_WIN32)
  list(APPEND notices mingw-w64 winpthreads gcc libxml2)
elseif(NOT CMAKE_HOST_APPLE)
  list(APPEND notices gcc glibc linux-headers)
endif()
foreach(notice IN LISTS notices)
  if(NOT EXISTS "${FB_OUTPUT}/futureblitz/notices/${notice}.txt")
    message(FATAL_ERROR "The release has no notices/${notice}.txt")
  endif()
endforeach()

# A staged install, under DESTDIR, is the same release, its bundled libraries too.
if(NOT CMAKE_HOST_WIN32)
  set(ENV{DESTDIR} "${FB_OUTPUT}/stage")
  execute_process(COMMAND "${CMAKE_COMMAND}" --install "${FB_BUILD}" --prefix /futureblitz
    OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
  unset(ENV{DESTDIR})
  execute_process(COMMAND "${FB_OUTPUT}/stage/futureblitz/bin/fb" --version
    OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
  file(GLOB staged RELATIVE "${FB_OUTPUT}/futureblitz/bin" "${FB_OUTPUT}/futureblitz/bin/*")
  foreach(file IN LISTS staged)
    if(NOT EXISTS "${FB_OUTPUT}/stage/futureblitz/bin/${file}")
      message(FATAL_ERROR "The staged release has no bin/${file}")
    endif()
  endforeach()
endif()

set(fb "${FB_OUTPUT}/futureblitz/bin/fb${FB_EXECUTABLE_SUFFIX}")
set(ENV{PATH} "${FB_OUTPUT}/empty")

# Runs fb with the arguments after `folder`, which it runs in, and fails unless it exits with 0;
# its output goes in `out`.
function(run_fb folder out)
  execute_process(COMMAND "${fb}" ${ARGN} WORKING_DIRECTORY "${folder}"
    OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "fb ${ARGN} exited with ${result}:\n${output}${errors}")
  endif()
  set(${out} "${output}" PARENT_SCOPE)
endfunction()

run_fb("${FB_OUTPUT}" version --version)
if(NOT version MATCHES "^fb [0-9]+\\.[0-9]+\\.[0-9]+ \\(([0-9a-f]+|unknown), [a-z]+-[a-z0-9]+\\)\n$")
  message(FATAL_ERROR "fb --version printed '${version}'")
endif()

run_fb("${FB_OUTPUT}" made new hello)
run_fb("${FB_OUTPUT}/hello" ran run)
if(NOT ran STREQUAL "Hello, world!\n")
  message(FATAL_ERROR "fb new's package printed '${ran}'")
endif()

# fb-bindgen reads a header that includes the C library's, with the release's headers.
if(EXISTS "${FB_OUTPUT}/futureblitz/bin/fb-bindgen${FB_EXECUTABLE_SUFFIX}")
  file(WRITE "${FB_OUTPUT}/hello/point.h"
    "#include <stddef.h>\n#include <stdint.h>\n#include <stdlib.h>\n"
    "typedef struct point { int32_t x; int32_t y; } point;\n"
    "size_t point_count(const point* points);\n")
  run_fb("${FB_OUTPUT}/hello" bound bindgen point.h)
  file(READ "${FB_OUTPUT}/hello/point.fb" module)
  if(NOT module MATCHES "point_count")
    message(FATAL_ERROR "fb-bindgen wrote:\n${module}")
  endif()
endif()
