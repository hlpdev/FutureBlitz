file(GLOB_RECURSE FB_RUNTIME_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM FB_RUNTIME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/io.cpp)

add_library(fb_runtime STATIC ${FB_RUNTIME_SOURCES})
target_include_directories(fb_runtime PUBLIC include)
fb_set_warnings(fb_runtime)

# The runtime's part over libuv, the I/O thread, is an archive of its own, which a program links
# with libuv only when it calls the fb_io_ functions. In one archive with the rest, it could be
# linked for a template it shares with them.
add_library(fb_runtime_io STATIC src/io.cpp)
target_include_directories(fb_runtime_io PUBLIC include)
target_include_directories(fb_runtime_io SYSTEM PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(fb_runtime_io PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_COMPILE_DEFINITIONS>)
add_dependencies(fb_runtime_io uv_a)
fb_set_warnings(fb_runtime_io)

# Nothing in the runtime throws: exceptions can't cross into compiled programs.
foreach(part fb_runtime fb_runtime_io)
  if(MSVC)
    target_compile_options(${part} PRIVATE /EHs-c-)
  else()
    target_compile_options(${part} PRIVATE -fno-exceptions)
  endif()
endforeach()
