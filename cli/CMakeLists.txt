file(GLOB_RECURSE FB_CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(fb_cli ${FB_CLI_SOURCES})
set_target_properties(fb_cli PROPERTIES OUTPUT_NAME fb)
target_include_directories(fb_cli PRIVATE include)
target_link_libraries(fb_cli PRIVATE fb_compiler)
add_dependencies(fb_cli fb_runtime) # programs link against it
fb_set_warnings(fb_cli)
fb_copy_llvm(fb_cli)

# MinGW's C++ runtime goes next to fb too, so it runs from any terminal.
if(MINGW)
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH fb_mingw_bin)
  foreach(dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
    add_custom_command(TARGET fb_cli POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fb_mingw_bin}/${dll}" "$<TARGET_FILE_DIR:fb_cli>")
  endforeach()
endif()
