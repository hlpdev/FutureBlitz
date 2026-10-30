# The compiler's passes recurse as deep as code nests, up to the parser's limit, which can take
# more than a main thread's default stack in a debug build: Windows' 1 MiB, and macOS' 8 MiB with
# Apple clang's frames, which unoptimized are bigger than GCC's. The executables that run the
# compiler get 64 MiB, which only takes memory as it's used. Linux gives 8 MiB, which is enough.
function(fb_set_stack target)
  if(MINGW)
    target_link_options(${target} PRIVATE "-Wl,--stack,67108864")
  elseif(MSVC)
    target_link_options(${target} PRIVATE "/STACK:67108864")
  elseif(APPLE)
    target_link_options(${target} PRIVATE "LINKER:-stack_size,0x4000000")
  endif()
endfunction()
