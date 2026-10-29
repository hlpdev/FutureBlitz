# Run when a release is installed (cmake/install.cmake), on Linux and macOS: copies into bin/ the
# shared libraries that the executables there load, and those that they load, but the system's
# own, and has each binary find them beside it. On Linux that's a RUNPATH of $ORIGIN, which
# patchelf sets; on macOS, @rpath names and an @loader_path rpath, which install_name_tool sets,
# after which each binary is signed again, as arm64 Macs run only signed code. Each library's
# license goes in notices/, from the package it came in.
#
# FB_BUNDLE_BIN is bin/, FB_BUNDLE_EXECUTABLES the executables' names there, lld among them,
# FB_BUNDLE_LLD the lld it was copied from, FB_BUNDLE_FOLDERS where else to look for what they
# load, and FB_BUNDLE_NOTICES notices/.

# An install script has no project's policies, but this one's.
cmake_policy(VERSION 3.28)

set(fb_executables "")
foreach(name IN LISTS FB_BUNDLE_EXECUTABLES)
  list(APPEND fb_executables "${FB_BUNDLE_BIN}/${name}")
endforeach()
# lld is read where it was installed from, where its own rpaths find what it loads.
set(fb_scanned ${fb_executables})
list(REMOVE_ITEM fb_scanned "${FB_BUNDLE_BIN}/lld")
list(APPEND fb_scanned "${FB_BUNDLE_LLD}")

# What every machine fb runs on has: on macOS, Apple's; on Linux, glibc's, GCC's, and the
# libraries that every Ubuntu has, which its package manager needs.
if(APPLE)
  set(fb_system_names "^$")
  set(fb_system_paths "^/usr/lib/" "^/System/Library/")
else()
  set(fb_system_names
    "^(ld-linux|linux-vdso|libc|libm|libdl|libpthread|librt|libutil|libresolv|libgcc_s|libstdc\\+\\+|libz|libzstd|liblzma|libtinfo)\\.so")
  set(fb_system_paths "^$")
endif()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${fb_scanned}
  RESOLVED_DEPENDENCIES_VAR fb_resolved
  UNRESOLVED_DEPENDENCIES_VAR fb_unresolved
  CONFLICTING_DEPENDENCIES_PREFIX fb_conflicting
  DIRECTORIES ${FB_BUNDLE_FOLDERS}
  PRE_EXCLUDE_REGEXES ${fb_system_names}
  POST_EXCLUDE_REGEXES ${fb_system_paths})
if(fb_unresolved)
  message(FATAL_ERROR "fb's executables load libraries that weren't found: ${fb_unresolved}")
endif()

find_program(FB_PATCHELF patchelf)
if(NOT APPLE AND fb_resolved AND NOT FB_PATCHELF)
  message(FATAL_ERROR "Installing fb on Linux needs patchelf, to have ${fb_resolved} found beside it")
endif()

file(MAKE_DIRECTORY "${FB_BUNDLE_NOTICES}")
set(fb_bundled "")
foreach(library IN LISTS fb_resolved)
  cmake_path(GET library FILENAME name)
  if(name IN_LIST fb_bundled)
    continue()
  endif()

  file(REAL_PATH "${library}" real)
  message(STATUS "Bundling: ${FB_BUNDLE_BIN}/${name}, from ${real}")
  file(COPY_FILE "${real}" "${FB_BUNDLE_BIN}/${name}")
  file(CHMOD "${FB_BUNDLE_BIN}/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
    GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  list(APPEND fb_bundled "${name}")

  # Its license: the Debian package's copyright file, or the Homebrew keg's license.
  set(package "")
  set(license "")
  if(APPLE)
    if(real MATCHES "/Cellar/([^/]+)/[^/]+/")
      set(package "${CMAKE_MATCH_1}")
      string(REGEX REPLACE "(/Cellar/[^/]+/[^/]+)/.*" "\\1" keg "${real}")
      file(GLOB license "${keg}/LICENSE*" "${keg}/COPYING*")
    endif()
  else()
    execute_process(COMMAND dpkg-query -S "${real}" OUTPUT_VARIABLE owner ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(owner MATCHES "^([^:,]+)")
      set(package "${CMAKE_MATCH_1}")
      file(GLOB license "/usr/share/doc/${package}/copyright")
    endif()
  endif()
  if(license)
    list(GET license 0 license)
    file(COPY_FILE "${license}" "${FB_BUNDLE_NOTICES}/${package}.txt")
  else()
    message(WARNING "No license was found for ${real}, which ${FB_BUNDLE_BIN} ships")
  endif()
endforeach()

# Each binary finds the bundled libraries beside it.
set(fb_binaries ${fb_executables})
foreach(name IN LISTS fb_bundled)
  list(APPEND fb_binaries "${FB_BUNDLE_BIN}/${name}")
endforeach()

foreach(binary IN LISTS fb_binaries)
  cmake_path(GET binary FILENAME own)
  if(NOT APPLE)
    # fb's own already have it.
    if(own IN_LIST fb_bundled OR own STREQUAL "lld")
      execute_process(COMMAND "${FB_PATCHELF}" --set-rpath "$ORIGIN" "${binary}"
        COMMAND_ERROR_IS_FATAL ANY)
    endif()
    continue()
  endif()

  execute_process(COMMAND otool -L "${binary}" OUTPUT_VARIABLE loads COMMAND_ERROR_IS_FATAL ANY)
  string(REPLACE "\n" ";" loads "${loads}")
  set(changes "")
  foreach(line IN LISTS loads)
    if(line MATCHES "^[ \t]+([^ \t]+) \\(")
      set(load "${CMAKE_MATCH_1}")
      cmake_path(GET load FILENAME name)
      if(name IN_LIST fb_bundled AND NOT load STREQUAL "@rpath/${name}")
        list(APPEND changes -change "${load}" "@rpath/${name}")
      endif()
    endif()
  endforeach()
  if(own IN_LIST fb_bundled)
    list(APPEND changes -id "@rpath/${own}")
  endif()

  # Its rpaths are @loader_path alone, so what's beside it is what it loads.
  execute_process(COMMAND otool -l "${binary}" OUTPUT_VARIABLE commands COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n[ \t]*path [^\n]*" rpaths "${commands}")
  set(has_loader_path FALSE)
  foreach(rpath IN LISTS rpaths)
    if(rpath MATCHES "path ([^\n]+) \\(offset")
      if(CMAKE_MATCH_1 STREQUAL "@loader_path")
        set(has_loader_path TRUE)
      else()
        list(APPEND changes -delete_rpath "${CMAKE_MATCH_1}")
      endif()
    endif()
  endforeach()
  if(NOT has_loader_path)
    list(APPEND changes -add_rpath "@loader_path")
  endif()

  execute_process(COMMAND install_name_tool ${changes} "${binary}" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND codesign --force --sign - "${binary}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
