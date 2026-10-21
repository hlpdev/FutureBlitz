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
