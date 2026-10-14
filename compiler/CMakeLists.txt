file(GLOB_RECURSE FB_COMPILER_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(fb_compiler STATIC ${FB_COMPILER_SOURCES})
target_include_directories(fb_compiler PUBLIC include)
fb_set_warnings(fb_compiler)
