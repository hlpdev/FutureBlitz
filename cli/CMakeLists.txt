file(GLOB_RECURSE FB_CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(fb_cli ${FB_CLI_SOURCES})
set_target_properties(fb_cli PROPERTIES OUTPUT_NAME fb)
target_include_directories(fb_cli PRIVATE include)
target_link_libraries(fb_cli PRIVATE fb_compiler)
add_dependencies(fb_cli ${FB_RUNTIME_ARCHIVES}) # programs link them
fb_set_warnings(fb_cli)
fb_set_stack(fb_cli)
fb_copy_llvm(fb_cli)
