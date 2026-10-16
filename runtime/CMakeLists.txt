file(GLOB_RECURSE FB_RUNTIME_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(fb_runtime STATIC ${FB_RUNTIME_SOURCES})
target_include_directories(fb_runtime PUBLIC include)
fb_set_warnings(fb_runtime)

# Nothing in the runtime throws: exceptions can't cross into compiled programs.
if(MSVC)
  target_compile_options(fb_runtime PRIVATE /EHs-c-)
else()
  target_compile_options(fb_runtime PRIVATE -fno-exceptions)
endif()
