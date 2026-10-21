# The compiler's passes recurse as deep as code nests, up to the parser's limit, which can take
# more than Windows' default main thread stack in a debug build. The executables that run the
# compiler get 64 MiB, which only takes memory as it's used. Linux and macOS give 8 MiB already.
function(fb_set_stack target)
  if(MINGW)
    target_link_options(${target} PRIVATE "-Wl,--stack,67108864")
  elseif(MSVC)
    target_link_options(${target} PRIVATE "/STACK:67108864")
  endif()
endfunction()
