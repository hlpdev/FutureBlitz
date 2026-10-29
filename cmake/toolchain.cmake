# fb's toolchain in the build folder, laid out as a release's: lib/<target>/ holds the runtime's
# archives, std's bundled libraries, and the pieces of the C runtime a program links, which are
# copied here from the C++ compiler that builds the runtime, so fb links with lld alone.
#
# FB_TOOLCHAIN_TARGET is the target's name, like windows-x64, and FB_TOOLCHAIN_LIBRARIES its
# folder.

if(WIN32)
  set(fb_os windows)
elseif(APPLE)
  set(fb_os macos)
else()
  set(fb_os linux)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$")
  set(fb_arch x64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64|arm64)$")
  set(fb_arch arm64)
else()
  message(FATAL_ERROR "No FutureBlitz target for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(FB_TOOLCHAIN_TARGET "${fb_os}-${fb_arch}")
set(FB_TOOLCHAIN_LIBRARIES "${CMAKE_BINARY_DIR}/lib/${FB_TOOLCHAIN_TARGET}")
file(MAKE_DIRECTORY "${FB_TOOLCHAIN_LIBRARIES}")

# Puts `file` in the target's folder as `name`, a hard link where it can be.
function(fb_toolchain_copy file name)
  file(REAL_PATH "${file}" real)
  file(CREATE_LINK "${real}" "${FB_TOOLCHAIN_LIBRARIES}/${name}" COPY_ON_ERROR)
endfunction()

# The path where the C++ compiler finds `name`, in `out`, or "" when it has none.
function(fb_compiler_file name out)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${name}
    OUTPUT_VARIABLE path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(IS_ABSOLUTE "${path}" AND EXISTS "${path}")
    set(${out} "${path}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

# Copies the C++ compiler's `pieces`, each of which it has to have.
function(fb_copy_compiler_files)
  foreach(piece IN LISTS ARGN)
    fb_compiler_file(${piece} path)
    if(NOT path)
      message(FATAL_ERROR "${CMAKE_CXX_COMPILER} has no ${piece}, which programs link")
    endif()
    fb_toolchain_copy("${path}" ${piece})
  endforeach()
endfunction()

if(fb_os STREQUAL "windows")
  # MinGW-w64's startup files and GCC's runtime, linked statically, with UCRT.
  fb_copy_compiler_files(crt2.o dllcrt2.o crtbegin.o crtend.o default-manifest.o libstdc++.a
    libgcc.a libgcc_eh.a libmingw32.a libmingwex.a libucrt.a libpthread.a)

  # The system's import libraries, beside kernel32's, which a package's [native] system
  # libraries name; not the other C runtimes, the umbrella libraries of API sets, or the
  # import libraries of the toolchain's own DLLs.
  fb_compiler_file(libkernel32.a kernel32)
  cmake_path(GET kernel32 PARENT_PATH fb_windows_libraries)
  file(GLOB fb_import_libraries "${fb_windows_libraries}/lib*.a")
  foreach(library IN LISTS fb_import_libraries)
    cmake_path(GET library FILENAME name)
    if(NOT name MATCHES "\\.dll\\.a$|^lib(msvcr|msvcp|ucrtbase|crtdll|onecore|windowsapp|mincore)|_apiset\\.a$")
      fb_toolchain_copy("${library}" ${name})
    endif()
  endforeach()
elseif(fb_os STREQUAL "linux")
  # glibc's and GCC's startup files, and GCC's runtime, which is linked statically.
  fb_copy_compiler_files(Scrt1.o crti.o crtn.o crtbeginS.o crtendS.o libstdc++.a libgcc.a
    libgcc_eh.a)

  # glibc's libraries, each a library, a linker script, which is written again naming the files
  # it groups beside it, or, where glibc has merged it into libc, an empty archive.
  foreach(library c m pthread dl rt)
    fb_compiler_file(lib${library}.so shared)
    if(NOT shared)
      fb_copy_compiler_files(lib${library}.a)
      continue()
    endif()

    file(READ "${shared}" magic LIMIT 4 HEX)
    if(magic STREQUAL "7f454c46")
      fb_toolchain_copy("${shared}" lib${library}.so)
      continue()
    endif()

    file(READ "${shared}" script)
    string(REGEX MATCHALL "/[^ ()]+" grouped "${script}")
    foreach(file IN LISTS grouped)
      cmake_path(GET file FILENAME name)
      fb_toolchain_copy("${file}" ${name})
      string(REPLACE "${file}" "${name}" script "${script}")
    endforeach()
    file(WRITE "${FB_TOOLCHAIN_LIBRARIES}/lib${library}.so" "${script}")
  endforeach()
elseif(fb_os STREQUAL "macos")
  # The compiler's builtins, which Apple's Clang links into every program; libSystem and
  # libc++ are the SDK's.
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE fb_builtins OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT EXISTS "${fb_builtins}")
    message(FATAL_ERROR "${CMAKE_CXX_COMPILER} has no compiler-rt builtins, which programs link")
  endif()
  fb_toolchain_copy("${fb_builtins}" libclang_rt.osx.a)
endif()
