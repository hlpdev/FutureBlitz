# LLVM's C API, as the imported target fb_llvm.
#
# Windows: the official LLVM installer has LLVM-C.dll and its import library but not the llvm-c
# headers, so the headers are downloaded from the matching release tag. Linux and macOS: the
# system's LLVM package (apt's llvm-dev, Homebrew's llvm).
#
# FB_LLD_DIRECTORY is where ld.lld lives; programs are linked with it.

set(FB_LLVM_VERSION 22.1.8)

if(WIN32)
  file(TO_CMAKE_PATH "$ENV{ProgramFiles}/LLVM" fb_default_llvm_root)
  set(FB_LLVM_ROOT "${fb_default_llvm_root}" CACHE PATH "Where the LLVM installer put LLVM")

  find_file(FB_LLVM_C_DLL LLVM-C.dll PATHS "${FB_LLVM_ROOT}/bin" NO_DEFAULT_PATH)
  find_file(FB_LLVM_C_IMPLIB LLVM-C.lib PATHS "${FB_LLVM_ROOT}/lib" NO_DEFAULT_PATH)
  if(NOT FB_LLVM_C_DLL OR NOT FB_LLVM_C_IMPLIB)
    message(FATAL_ERROR
      "LLVM ${FB_LLVM_VERSION} wasn't found in ${FB_LLVM_ROOT}. Install it from "
      "https://github.com/llvm/llvm-project/releases/tag/llvmorg-${FB_LLVM_VERSION} "
      "(LLVM-${FB_LLVM_VERSION}-win64.exe), or set FB_LLVM_ROOT.")
  endif()

  # The headers are only right for the matching DLL.
  execute_process(COMMAND "${FB_LLVM_ROOT}/bin/clang.exe" --version
    OUTPUT_VARIABLE fb_clang_version ERROR_QUIET)
  string(REGEX MATCH "version ([0-9.]+)" fb_clang_version "${fb_clang_version}")
  if(NOT CMAKE_MATCH_1 VERSION_EQUAL FB_LLVM_VERSION)
    message(WARNING "LLVM in ${FB_LLVM_ROOT} is ${CMAKE_MATCH_1}, not ${FB_LLVM_VERSION}")
  endif()

  set(fb_llvm_include "${CMAKE_BINARY_DIR}/_deps/llvm-c-${FB_LLVM_VERSION}/include")
  set(fb_llvm_headers
    Analysis Core DataTypes Deprecated Error ErrorHandling ExternC Target TargetMachine Types
    Visibility Transforms/PassBuilder BitReader BitWriter Linker)
  set(fb_llvm_hashes
    b822af9c7ceb7efc92fa5a467e526c18e7a3fce427ec190d1eae906f85a22b50
    1dc17fcb91c751ee2fc2211d0b53add643ab7e840707401127afe4a35a7572b2
    443a1142ba0ec4bf930b07b593ad95208ae954b7eb936894300ead8729770be7
    dc0131614ca423510877f1605ae79cc33e3c02bed067e021cf7f46576643c7ae
    b8025e3dff1375fade4507d14dc3076abcfb162134a305ec84f93902d19d761b
    a056bf231ea9df04500fe17e16c2c273f120e5bca0b52e81fcf02fe6b23b6a66
    2fdcf3619c57d9cd7916ae1530045e4c85866613daf44759807dada94449f857
    d88db9be749a73266e9a2936335f2f512bd50bef08f3bcd13a0d68f6a577c199
    3cef6abafa88b536b4fd2698d085548c61e38ff97b3be336082b12190db43a84
    c84f1c5043bb828f121a1914fbbea57cf8ff50b0353b553a835606ba41052bf0
    98b41b3b378f68fd345b9de6ff730c068e78a4880fe9c6a5b293b8ea5dce019e
    42bc1d65b43ce602866a7b6b5445cd19c31bf5be9e48d8828987277280de8d91
    fefdeb693257ed85e1971048f6526ee30c934e9a92c8b4db87b644499de195dc
    d51d5b87e3bb5e95d4f399f7c3cc6077e3fe7b88e7e8ab1cc36ea104d5fc494a
    671876029cb735fc9dca8eaa325c1f548844291acc11bfa8a9af522df018a24e)
  set(fb_llvm_url
    "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-${FB_LLVM_VERSION}/llvm/include")
  foreach(header hash IN ZIP_LISTS fb_llvm_headers fb_llvm_hashes)
    set(destination "${fb_llvm_include}/llvm-c/${header}.h")
    if(NOT EXISTS "${destination}")
      file(DOWNLOAD "${fb_llvm_url}/llvm-c/${header}.h" "${destination}"
        EXPECTED_HASH SHA256=${hash} STATUS status)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        file(REMOVE "${destination}")
        message(FATAL_ERROR "Couldn't download llvm-c/${header}.h: ${status}")
      endif()
    endif()
  endforeach()

  # The release headers include files that LLVM's own build generates. These stand in for them,
  # with the native target, and the targets FutureBlitz has: x86-64 and AArch64.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(fb_llvm_native X86)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64|arm64)$")
    set(fb_llvm_native AArch64)
  else()
    message(FATAL_ERROR "No LLVM target for ${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  set(fb_llvm_config "${fb_llvm_include}/llvm/Config")
  file(WRITE "${fb_llvm_config}/llvm-config.h"
    "#pragma once\n"
    "#define LLVM_NATIVE_ARCH ${fb_llvm_native}\n"
    "#define LLVM_NATIVE_TARGETINFO LLVMInitialize${fb_llvm_native}TargetInfo\n"
    "#define LLVM_NATIVE_TARGET LLVMInitialize${fb_llvm_native}Target\n"
    "#define LLVM_NATIVE_TARGETMC LLVMInitialize${fb_llvm_native}TargetMC\n"
    "#define LLVM_NATIVE_ASMPRINTER LLVMInitialize${fb_llvm_native}AsmPrinter\n"
    "#define LLVM_NATIVE_ASMPARSER LLVMInitialize${fb_llvm_native}AsmParser\n"
    "#define LLVM_NATIVE_DISASSEMBLER LLVMInitialize${fb_llvm_native}Disassembler\n")
  file(WRITE "${fb_llvm_config}/Targets.def" "LLVM_TARGET(X86)\nLLVM_TARGET(AArch64)\n")
  file(WRITE "${fb_llvm_config}/AsmPrinters.def"
    "LLVM_ASM_PRINTER(X86)\nLLVM_ASM_PRINTER(AArch64)\n")
  file(WRITE "${fb_llvm_config}/AsmParsers.def" "LLVM_ASM_PARSER(${fb_llvm_native})\n")
  file(WRITE "${fb_llvm_config}/Disassemblers.def" "LLVM_DISASSEMBLER(${fb_llvm_native})\n")

  add_library(fb_llvm SHARED IMPORTED GLOBAL)
  set_target_properties(fb_llvm PROPERTIES
    IMPORTED_LOCATION "${FB_LLVM_C_DLL}"
    IMPORTED_IMPLIB "${FB_LLVM_C_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${fb_llvm_include}")
  set(FB_LLD_DIRECTORY "${FB_LLVM_ROOT}/bin")
else()
  find_package(LLVM REQUIRED CONFIG)
  add_library(fb_llvm INTERFACE)
  target_include_directories(fb_llvm SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
  if(TARGET LLVM)
    target_link_libraries(fb_llvm INTERFACE LLVM)
  else()
    llvm_map_components_to_libnames(fb_llvm_libraries core analysis passes target bitreader
      bitwriter linker X86 AArch64)
    target_link_libraries(fb_llvm INTERFACE ${fb_llvm_libraries})
  endif()

  find_program(FB_LLD ld.lld HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
  cmake_path(GET FB_LLD PARENT_PATH FB_LLD_DIRECTORY)
endif()

# Puts LLVM-C.dll next to an executable that uses the compiler, so it runs without PATH changes.
function(fb_copy_llvm target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FB_LLVM_C_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()
