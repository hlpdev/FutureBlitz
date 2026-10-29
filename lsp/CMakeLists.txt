# nlohmann/json reads and writes the LSP's JSON-RPC messages, header-only.
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# The language server, as a library its tests drive, and fb-lsp, which an editor starts and talks
# to over stdin and stdout.
file(GLOB_RECURSE FB_LSP_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM FB_LSP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(fb_lsp_server STATIC ${FB_LSP_SOURCES})
target_include_directories(fb_lsp_server PUBLIC include)
target_link_libraries(fb_lsp_server PUBLIC fb_compiler nlohmann_json::nlohmann_json)
fb_set_warnings(fb_lsp_server)

add_executable(fb_lsp src/main.cpp)
set_target_properties(fb_lsp PROPERTIES OUTPUT_NAME fb-lsp)
target_link_libraries(fb_lsp PRIVATE fb_lsp_server)
fb_set_warnings(fb_lsp)
fb_set_stack(fb_lsp)
fb_copy_llvm(fb_lsp)
