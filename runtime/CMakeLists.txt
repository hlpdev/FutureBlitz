file(GLOB_RECURSE FB_RUNTIME_SOURCES CONFIGURE_DEPENDS src/*.cpp)
# The parts over bundled C libraries are archives of their own.
set(FB_RUNTIME_PARTS io crypto crypto_pem xxhash blake3 argon2 bcrypt ed25519)
foreach(part ${FB_RUNTIME_PARTS})
  list(REMOVE_ITEM FB_RUNTIME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/${part}.cpp)
endforeach()

add_library(fb_runtime STATIC ${FB_RUNTIME_SOURCES})
target_include_directories(fb_runtime PUBLIC include)
fb_set_warnings(fb_runtime)

# The runtime's part over libuv, the I/O thread, is an archive of its own, which a program links
# with libuv only when it calls the fb_io_ functions. In one archive with the rest, it could be
# linked for a template it shares with them.
add_library(fb_runtime_io STATIC src/io.cpp)
target_include_directories(fb_runtime_io PUBLIC include)
target_include_directories(fb_runtime_io SYSTEM PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(fb_runtime_io PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_COMPILE_DEFINITIONS>)
add_dependencies(fb_runtime_io uv_a)
fb_set_warnings(fb_runtime_io)

# So are its parts over std.crypto's libraries, each linked with its library only when a program
# calls its functions: the fb_crypto_ ones over mbedTLS, and the fb_xxhash_, fb_blake3_,
# fb_argon2_, fb_bcrypt_, and fb_ed25519_ ones over xxHash, BLAKE3, Argon2, crypt_blowfish, and
# Monocypher. Linking each to its library here only gives it the library's headers and
# settings.
foreach(part crypto xxhash blake3 argon2 bcrypt ed25519)
  add_library(fb_runtime_${part} STATIC src/${part}.cpp)
  target_include_directories(fb_runtime_${part} PUBLIC include)
  fb_set_warnings(fb_runtime_${part})
endforeach()
# PEM keys and certificates are an object file of their own, so only a program that reads them
# links mbedTLS's X.509.
target_sources(fb_runtime_crypto PRIVATE src/crypto_pem.cpp)
target_link_libraries(fb_runtime_crypto PRIVATE tfpsacrypto mbedx509)
target_link_libraries(fb_runtime_xxhash PRIVATE xxhash)
target_link_libraries(fb_runtime_blake3 PRIVATE blake3)
target_link_libraries(fb_runtime_argon2 PRIVATE argon2)
target_link_libraries(fb_runtime_bcrypt PRIVATE crypt_blowfish)
target_link_libraries(fb_runtime_ed25519 PRIVATE monocypher)

# What a program can link: each part and its library, which fb and the tests that build programs
# depend on.
set(FB_RUNTIME_ARCHIVES fb_runtime fb_runtime_io uv_a fb_runtime_crypto tfpsacrypto mbedx509
    fb_runtime_xxhash xxhash fb_runtime_blake3 blake3 fb_runtime_argon2 argon2 fb_runtime_bcrypt
    crypt_blowfish fb_runtime_ed25519 monocypher PARENT_SCOPE)

# Nothing in the runtime throws: exceptions can't cross into compiled programs.
foreach(part fb_runtime fb_runtime_io fb_runtime_crypto fb_runtime_xxhash fb_runtime_blake3
             fb_runtime_argon2 fb_runtime_bcrypt fb_runtime_ed25519)
  if(MSVC)
    target_compile_options(${part} PRIVATE /EHs-c-)
  else()
    target_compile_options(${part} PRIVATE -fno-exceptions)
  endif()
endforeach()
