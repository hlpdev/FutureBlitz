# A release's layout, which `cmake --install <build> --prefix <folder>` writes, and which
# fb/toolchain.hpp finds beside fb:
#
#   bin/            fb, fb-bindgen, fb's language server, lld, and the shared libraries they load
#   lib/<target>/   the runtime's archives, std's bundled libraries, the C runtime's pieces, and
#                   in include/ the target's C headers, which fb-bindgen reads a header with
#   lib/clang/      clang's own headers, which fb-bindgen reads one with too
#   notices/        the licenses of what a release ships, and of std's bundled libraries
#   std/            the standard library's source files
#   lldb/           futureblitz.py, which shows FutureBlitz values in lldb and CodeLLDB
#
# A runtime-only build installs lib/<target>/ alone, which a release built elsewhere takes as
# FB_TOOLCHAIN_PREBUILT: the Linux release's, built against glibc 2.28.
#
# fb's own executables run with nothing else installed: MinGW's runtime is linked into them on
# Windows, and libstdc++ and libgcc on Linux; the shared libraries they and lld load, but the
# system's, go beside them in bin/ (cmake/bundle.cmake).

set(FB_TOOLCHAIN_PREBUILT "" CACHE PATH
  "A runtime-only build's installed lib/<target>/, which the release ships in place of this one's")
# The target's C headers, which fb-bindgen reads a header's #includes in: a folder of them, or
# else the C compiler's own include folders.
set(FB_C_HEADERS "" CACHE PATH "The folder of the target's C headers a release ships")

set(fb_target_folder "lib/${FB_TOOLCHAIN_TARGET}")

# lib/<target>/: this build's, or the prebuilt one, which has its own include/.
if(FB_TOOLCHAIN_PREBUILT)
  install(DIRECTORY "${FB_TOOLCHAIN_PREBUILT}/" DESTINATION "${fb_target_folder}")
else()
  install(DIRECTORY "${FB_TOOLCHAIN_LIBRARIES}/" DESTINATION "${fb_target_folder}")
  if(FB_C_HEADERS)
    install(DIRECTORY "${FB_C_HEADERS}/" DESTINATION "${fb_target_folder}/include")
  elseif(NOT APPLE)
    foreach(folder IN LISTS FB_C_HEADER_DIRECTORIES)
      install(DIRECTORY "${folder}/" DESTINATION "${fb_target_folder}/include")
    endforeach()
  endif()
endif()

if(FB_RUNTIME_ONLY)
  return()
endif()

set(fb_executables fb_cli fb_lsp)
if(TARGET fb_bindgen_cli)
  list(APPEND fb_executables fb_bindgen_cli)
endif()

include(CheckCXXSourceCompiles)

# MinGW's winpthread links statically only where it's built for the C runtime the toolchain
# links by default, as MSYS2's is; CLion's MinGW has one built for UCRT, which needs UCRT's
# setjmp. There libstdc++ and libgcc are linked statically, and winpthread's DLL goes beside.
set(fb_winpthread_dll "")
if(MINGW)
  set(CMAKE_REQUIRED_LINK_OPTIONS -static)
  check_cxx_source_compiles("#include <thread>\nint main() { std::thread([] {}).join(); }"
    FB_MINGW_LINKS_STATICALLY)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(NOT FB_MINGW_LINKS_STATICALLY)
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH fb_mingw_bin)
    set(fb_winpthread_dll "${fb_mingw_bin}/libwinpthread-1.dll")
  endif()
endif()

foreach(target IN LISTS fb_executables)
  if(MINGW AND FB_MINGW_LINKS_STATICALLY)
    target_link_options(${target} PRIVATE -static)
  elseif(MINGW)
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    add_custom_target(${target}_winpthread
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fb_winpthread_dll}" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
    add_dependencies(${target} ${target}_winpthread)
  elseif(UNIX AND NOT APPLE)
    # A static LLVM names libraries, like z3, that its parts fb links don't use. Its symbols
    # stay the executable's own, so the libLLVM that libclang loads keeps its own LLVM.
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc LINKER:--as-needed
                        LINKER:--exclude-libs,ALL)
  endif()

  # The shared libraries they load are found beside them.
  if(APPLE)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
endforeach()

install(TARGETS ${fb_executables} RUNTIME DESTINATION bin)

# lld, by its own file, not a link to it.
file(REAL_PATH "${FB_LLD_DIRECTORY}/lld${CMAKE_EXECUTABLE_SUFFIX}" fb_lld)
install(PROGRAMS "${fb_lld}" DESTINATION bin RENAME "lld${CMAKE_EXECUTABLE_SUFFIX}")

# What they load. The LLVM installer's DLLs on Windows hold all of LLVM and clang, and need
# only the system's own; elsewhere cmake/bundle.cmake finds them.
if(WIN32)
  install(PROGRAMS "${FB_LLVM_C_DLL}" DESTINATION bin)
  if(TARGET fb_bindgen_cli)
    install(PROGRAMS "${FB_LIBCLANG_DLL}" DESTINATION bin)
  endif()
  if(fb_winpthread_dll)
    install(PROGRAMS "${fb_winpthread_dll}" DESTINATION bin)
  endif()
else()
  set(fb_bundled "$<TARGET_FILE_NAME:fb_cli>;$<TARGET_FILE_NAME:fb_lsp>;lld")
  set(fb_library_folders "")
  foreach(library IN ITEMS "${FB_LIBCLANG_LIBRARY}" "${fb_lld}")
    if(library)
      cmake_path(GET library PARENT_PATH folder)
      list(APPEND fb_library_folders "${folder}" "${folder}/../lib")
    endif()
  endforeach()
  list(APPEND fb_library_folders ${LLVM_LIBRARY_DIRS})
  if(TARGET fb_bindgen_cli)
    list(APPEND fb_bundled "$<TARGET_FILE_NAME:fb_bindgen_cli>")
  endif()
  install(CODE "
    set(FB_BUNDLE_BIN \"\${CMAKE_INSTALL_PREFIX}/bin\")
    set(FB_BUNDLE_EXECUTABLES \"${fb_bundled}\")
    set(FB_BUNDLE_FOLDERS \"${fb_library_folders}\")
    set(FB_BUNDLE_NOTICES \"\${CMAKE_INSTALL_PREFIX}/notices\")
    include(\"${PROJECT_SOURCE_DIR}/cmake/bundle.cmake\")
  ")
endif()

# clang's own headers, from its resource folder: include/ is all fb-bindgen reads.
if(FB_LIBCLANG_RESOURCE_DIR AND TARGET fb_bindgen_cli)
  install(DIRECTORY "${FB_LIBCLANG_RESOURCE_DIR}/include" DESTINATION lib/clang)
endif()

install(DIRECTORY "${PROJECT_SOURCE_DIR}/std/" DESTINATION std)
install(FILES "${PROJECT_SOURCE_DIR}/tools/lldb/futureblitz.py" DESTINATION lldb)

# The notices: std's bundled libraries', which programs link, LLVM's for LLVM, clang, and lld,
# and those of the C runtime's pieces in lib/<target>/: MinGW-w64's and GCC's on Windows, and
# GCC's, glibc's, and the Linux headers' on Linux.
cmake_path(GET FB_LIBUV_NOTICE PARENT_PATH fb_notices)
install(DIRECTORY "${fb_notices}/" DESTINATION notices)
set(fb_release_notices llvm)
if(WIN32)
  list(APPEND fb_release_notices mingw-w64 gcc)
elseif(NOT APPLE)
  list(APPEND fb_release_notices gcc glibc linux-headers)
endif()
foreach(notice IN LISTS fb_release_notices)
  install(FILES "${PROJECT_SOURCE_DIR}/cmake/notices/${notice}.txt" DESTINATION notices)
endforeach()
