# libclang's C API, as the imported target fb_libclang, which only tools/bindgen uses. Where it
# isn't found, fb-bindgen isn't built, and the rest builds as before.
#
# Windows: the LLVM installer's libclang.dll and import library, next to LLVM-C's. Linux and
# macOS: the libclang of the system's LLVM package. Its clang-c headers are copied into the
# build folder, so nothing sees the other headers next to them.

if(WIN32)
  find_file(FB_LIBCLANG_DLL libclang.dll PATHS "${FB_LLVM_ROOT}/bin" NO_DEFAULT_PATH)
  find_file(FB_LIBCLANG_LIBRARY libclang.lib PATHS "${FB_LLVM_ROOT}/lib" NO_DEFAULT_PATH)
  set(fb_clang_c "${FB_LLVM_ROOT}/include/clang-c")
else()
  find_library(FB_LIBCLANG_LIBRARY NAMES clang libclang HINTS ${LLVM_LIBRARY_DIRS})
  find_path(FB_LIBCLANG_INCLUDE clang-c/Index.h HINTS ${LLVM_INCLUDE_DIRS})
  set(fb_clang_c "${FB_LIBCLANG_INCLUDE}/clang-c")
endif()

# libclang's own headers, like stddef.h and the intrinsics, which it reads a header with in
# place of the C compiler's own, from the folder next to its library or LLVM's.
set(FB_LIBCLANG_RESOURCE_DIR "")
if(FB_LIBCLANG_LIBRARY)
  get_filename_component(fb_libclang_folder "${FB_LIBCLANG_LIBRARY}" DIRECTORY)
  file(GLOB fb_clang_resources LIST_DIRECTORIES true "${fb_libclang_folder}/clang/*")
  foreach(folder IN LISTS LLVM_LIBRARY_DIRS)
    file(GLOB fb_llvm_resources LIST_DIRECTORIES true "${folder}/clang/*")
    list(APPEND fb_clang_resources ${fb_llvm_resources})
  endforeach()
  foreach(folder IN LISTS fb_clang_resources)
    if(EXISTS "${folder}/include/stddef.h")
      set(FB_LIBCLANG_RESOURCE_DIR "${folder}")
      break()
    endif()
  endforeach()
endif()

if(NOT FB_LIBCLANG_LIBRARY OR NOT EXISTS "${fb_clang_c}/Index.h" OR (WIN32 AND NOT FB_LIBCLANG_DLL))
  message(STATUS "libclang wasn't found, so fb-bindgen won't be built")
  return()
endif()

if(NOT FB_LIBCLANG_RESOURCE_DIR)
  message(STATUS "libclang's own headers weren't found next to it, so fb-bindgen won't be built")
  return()
endif()

set(fb_libclang_include "${CMAKE_BINARY_DIR}/_deps/libclang/include")
file(COPY "${fb_clang_c}" DESTINATION "${fb_libclang_include}")

if(WIN32)
  add_library(fb_libclang SHARED IMPORTED GLOBAL)
  set_target_properties(fb_libclang PROPERTIES
    IMPORTED_LOCATION "${FB_LIBCLANG_DLL}"
    IMPORTED_IMPLIB "${FB_LIBCLANG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${fb_libclang_include}")
else()
  add_library(fb_libclang UNKNOWN IMPORTED GLOBAL)
  set_target_properties(fb_libclang PROPERTIES
    IMPORTED_LOCATION "${FB_LIBCLANG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${fb_libclang_include}")
endif()

# Puts libclang.dll next to an executable that uses it, so it runs without PATH changes.
function(fb_copy_libclang target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FB_LIBCLANG_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()
