file(GLOB_RECURSE FB_CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(fb_cli ${FB_CLI_SOURCES})
target_link_libraries(fb_cli PRIVATE fb_compiler)
fb_set_warnings(fb_cli)
