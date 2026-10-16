function(fb_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wnon-virtual-dtor -Wold-style-cast -Wnull-dereference
      # Leaving a field out of a designated initializer is deliberate.
      -Wno-missing-field-initializers
    )
  endif()
endfunction()
