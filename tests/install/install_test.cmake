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
