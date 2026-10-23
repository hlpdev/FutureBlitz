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
