file(GLOB_RECURSE FB_LSP_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(fb_lsp ${FB_LSP_SOURCES})
target_link_libraries(fb_lsp PRIVATE fb_compiler)
fb_set_warnings(fb_lsp)
