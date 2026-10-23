# The C libraries std is built on, each fetched and built by the C compiler that builds the
# runtime, for the build's target, as an archive of its own, so a program links only what it
# uses. docs/stdlib.md lists them, and fb names each one a build links, with its license.

# libuv: the runtime's I/O thread runs its loop, for std.io, std.os, std.net, std.time's timers,
# and every Async twin of a blocking call.
set(FB_LIBUV_VERSION 1.52.1)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG v${FB_LIBUV_VERSION}
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(libuv)
# Its warnings are its own. GCC 14 and later make one an error, a const mismatch in uv_cpu_info's
# Windows code, which is harmless.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(uv_a PRIVATE -w -Wno-error=incompatible-pointer-types)
endif()
# Programs link MinGW's UCRT, not the msvcrt it defaults to, so what they link is compiled for it.
if(MINGW)
  target_compile_options(uv_a PRIVATE -mcrtdll=ucrt)
endif()
set(FB_LIBUV_VERSION ${FB_LIBUV_VERSION} PARENT_SCOPE)
# Its notices, in one file: libuv's license, then those of the code it has from Joyent's libuv,
# FreeBSD's tree.h, and ISC's inet.c, whose texts are at the top of those files.
file(READ "${libuv_SOURCE_DIR}/LICENSE" notices)
file(READ "${libuv_SOURCE_DIR}/LICENSE-extra" extra)
string(APPEND notices "\n${extra}")
foreach(part include/uv/tree.h src/inet.c)
  file(READ "${libuv_SOURCE_DIR}/${part}" text)
  string(FIND "${text}" "*/" end)
  math(EXPR end "${end} + 2")
  string(SUBSTRING "${text}" 0 ${end} license)
  string(APPEND notices "\n${part}:\n\n${license}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/notices/libuv.txt" "${notices}")
set(FB_LIBUV_NOTICE "${CMAKE_CURRENT_BINARY_DIR}/notices/libuv.txt" PARENT_SCOPE)
set(FB_LIBUV_LICENSES "the MIT, BSD-2-Clause, and ISC licenses" PARENT_SCOPE)

# The libraries below keep their warnings to themselves too, and are built for UCRT on MinGW.
# They're optimized in every build, since a debug build of a hash or a key derivation is too slow
# to test with, and their code isn't the program's to step through. One built by its own CMake
# gets the options for its whole folder, as its parts may be targets of their own.
set(FB_BUNDLED_OPTIONS "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  list(APPEND FB_BUNDLED_OPTIONS -w -O2)
endif()
if(MINGW)
  list(APPEND FB_BUNDLED_OPTIONS -mcrtdll=ucrt)
endif()

function(fb_bundled_library target)
  target_compile_options(${target} PRIVATE ${FB_BUNDLED_OPTIONS})
endfunction()

# FetchContent_MakeAvailable(name), with FB_BUNDLED_OPTIONS for every C file in its folder. A
# macro, so its <name>_SOURCE_DIR is here, with the C flags put back after.
macro(fb_make_available name)
  set(fb_saved_c_flags "${CMAKE_C_FLAGS}")
  list(JOIN FB_BUNDLED_OPTIONS " " fb_options)
  string(APPEND CMAKE_C_FLAGS " ${fb_options}")
  FetchContent_MakeAvailable(${name})
  set(CMAKE_C_FLAGS "${fb_saved_c_flags}")
endmacro()

# Adds `options` after the own options of each library target in `folder` and the folders in it.
function(fb_options_after folder options)
  get_property(targets DIRECTORY "${folder}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target ${targets})
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "OBJECT_LIBRARY")
      target_compile_options(${target} PRIVATE ${options})
    endif()
  endforeach()
  get_property(folders DIRECTORY "${folder}" PROPERTY SUBDIRECTORIES)
  foreach(inner ${folders})
    fb_options_after("${inner}" "${options}")
  endforeach()
endfunction()

# The file `name`.txt in notices/, of `text`, whose path is FB_<NAME>_NOTICE.
function(fb_notice name text)
  string(TOUPPER "${name}" upper)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/notices/${name}.txt" "${text}")
  set(FB_${upper}_NOTICE "${CMAKE_CURRENT_BINARY_DIR}/notices/${name}.txt" PARENT_SCOPE)
endfunction()

# mbedTLS: std.crypto's hashes, HMAC, HKDF, PBKDF2, ciphers, signatures, key exchange, keys and
# certificates, and secure random, through TF-PSA-Crypto's PSA API, and std.net.tls later. Only
# its crypto library (tfpsacrypto) is built. mbedtls_config.h turns its locks on, so any thread
# can use it.
set(FB_MBEDTLS_VERSION 4.1.1)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(LINK_WITH_PTHREAD ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_config.h" CACHE FILEPATH
    "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${FB_MBEDTLS_VERSION}/mbedtls-${FB_MBEDTLS_VERSION}.tar.bz2
  URL_HASH SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
  SYSTEM
  EXCLUDE_FROM_ALL
)
fb_make_available(mbedtls)
# Its CMake gives its libraries -O0 and -g3 of their own in a Debug build, after the options
# above, so they get -O2 and -g2 again after those.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  fb_options_after("${mbedtls_SOURCE_DIR}" "$<$<CONFIG:Debug>:-O2>;$<$<CONFIG:Debug>:-g2>")
endif()
# PSA takes RSA keys of up to 4096 bits, which crypto_sizes.h sets with no way to change it, so
# it's raised to 8192 there, the most mbedTLS's bignums hold, as it was fetched.
set(sizes "${mbedtls_SOURCE_DIR}/tf-psa-crypto/include/psa/crypto_sizes.h")
file(READ "${sizes}" text)
string(REPLACE "#define PSA_VENDOR_RSA_MAX_KEY_BITS 4096u" "#define PSA_VENDOR_RSA_MAX_KEY_BITS 8192u"
       raised "${text}")
if(NOT raised STREQUAL text)
  file(WRITE "${sizes}" "${raised}")
elseif(NOT text MATCHES "#define PSA_VENDOR_RSA_MAX_KEY_BITS 8192u")
  message(FATAL_ERROR "mbedTLS's crypto_sizes.h doesn't set PSA_VENDOR_RSA_MAX_KEY_BITS to 4096u")
endif()
# Its RSA module's headers, which aren't its public ones: std.crypto verifies RSA through it,
# since PSA takes no key of an odd number of bits.
set(FB_MBEDTLS_RSA_INCLUDES "${mbedtls_SOURCE_DIR}/tf-psa-crypto/drivers/builtin/include"
    "${mbedtls_SOURCE_DIR}/tf-psa-crypto/drivers/builtin/src" PARENT_SCOPE)
set(FB_MBEDTLS_VERSION ${FB_MBEDTLS_VERSION} PARENT_SCOPE)
# It's under Apache-2.0 or GPL-2.0-or-later, and std takes it under Apache-2.0. Its LICENSE has
# both texts.
file(READ "${mbedtls_SOURCE_DIR}/LICENSE" notices)
fb_notice(mbedtls "${notices}")
set(FB_MBEDTLS_NOTICE "${FB_MBEDTLS_NOTICE}" PARENT_SCOPE)
set(FB_MBEDTLS_LICENSES "the Apache-2.0 license" PARENT_SCOPE)

# xxHash: std.crypto's XxHash, a fast hash that isn't cryptographic, built from its one C file.
set(FB_XXHASH_VERSION 0.8.4)
FetchContent_Declare(
  xxhash
  URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v${FB_XXHASH_VERSION}.tar.gz
  URL_HASH SHA256=5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32
  SYSTEM
)
FetchContent_MakeAvailable(xxhash)
add_library(xxhash STATIC EXCLUDE_FROM_ALL "${xxhash_SOURCE_DIR}/xxhash.c")
target_include_directories(xxhash SYSTEM PUBLIC "${xxhash_SOURCE_DIR}")
fb_bundled_library(xxhash)
set(FB_XXHASH_VERSION ${FB_XXHASH_VERSION} PARENT_SCOPE)
file(READ "${xxhash_SOURCE_DIR}/LICENSE" notices)
fb_notice(xxhash "${notices}")
set(FB_XXHASH_NOTICE "${FB_XXHASH_NOTICE}" PARENT_SCOPE)
set(FB_XXHASH_LICENSES "the BSD-2-Clause license" PARENT_SCOPE)

# BLAKE3: std.crypto's Blake3, through its own CMake, which picks the target's SIMD code.
set(FB_BLAKE3_VERSION 1.8.7)
FetchContent_Declare(
  blake3
  URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/${FB_BLAKE3_VERSION}.tar.gz
  URL_HASH SHA256=c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c
  SOURCE_SUBDIR c
  SYSTEM
  EXCLUDE_FROM_ALL
)
fb_make_available(blake3)
set(FB_BLAKE3_VERSION ${FB_BLAKE3_VERSION} PARENT_SCOPE)
# It's under CC0-1.0, Apache-2.0, or Apache-2.0 with LLVM's exceptions, and std takes it under
# CC0-1.0.
file(READ "${blake3_SOURCE_DIR}/LICENSE_CC0" notices)
fb_notice(blake3 "${notices}")
set(FB_BLAKE3_NOTICE "${FB_BLAKE3_NOTICE}" PARENT_SCOPE)
set(FB_BLAKE3_LICENSES "the CC0-1.0 license" PARENT_SCOPE)

# Argon2: std.crypto's Argon2id, the reference code, without threads of its own, so its lanes
# run one after another.
set(FB_ARGON2_VERSION 20190702)
FetchContent_Declare(
  argon2
  URL https://github.com/P-H-C/phc-winner-argon2/archive/refs/tags/${FB_ARGON2_VERSION}.tar.gz
  URL_HASH SHA256=daf972a89577f8772602bf2eb38b6a3dd3d922bf5724d45e7f9589b5e830442c
  SYSTEM
)
FetchContent_MakeAvailable(argon2)
add_library(argon2 STATIC EXCLUDE_FROM_ALL)
target_sources(argon2 PRIVATE
  "${argon2_SOURCE_DIR}/src/argon2.c"
  "${argon2_SOURCE_DIR}/src/core.c"
  "${argon2_SOURCE_DIR}/src/encoding.c"
  "${argon2_SOURCE_DIR}/src/ref.c"
  "${argon2_SOURCE_DIR}/src/thread.c"
  "${argon2_SOURCE_DIR}/src/blake2/blake2b.c")
target_include_directories(argon2 SYSTEM PUBLIC "${argon2_SOURCE_DIR}/include")
target_compile_definitions(argon2 PRIVATE ARGON2_NO_THREADS)
fb_bundled_library(argon2)
set(FB_ARGON2_VERSION ${FB_ARGON2_VERSION} PARENT_SCOPE)
# It's under CC0-1.0 or Apache-2.0, as its LICENSE says, and std takes it under CC0-1.0.
file(READ "${argon2_SOURCE_DIR}/LICENSE" notices)
fb_notice(argon2 "${notices}")
set(FB_ARGON2_NOTICE "${FB_ARGON2_NOTICE}" PARENT_SCOPE)
set(FB_ARGON2_LICENSES "the CC0-1.0 license" PARENT_SCOPE)

# crypt_blowfish: std.crypto's Bcrypt, Solar Designer's, whose crypt_blowfish.c has both what
# hashes and what makes a setting from a salt.
set(FB_CRYPT_BLOWFISH_VERSION 1.3)
FetchContent_Declare(
  crypt_blowfish
  URL https://www.openwall.com/crypt/crypt_blowfish-${FB_CRYPT_BLOWFISH_VERSION}.tar.gz
  URL_HASH SHA256=83fa01fca6996fe8d882b7f8e9ba0305a5664936100b01481ea3c6a8ce8d72fd
  SYSTEM
)
FetchContent_MakeAvailable(crypt_blowfish)
add_library(crypt_blowfish STATIC EXCLUDE_FROM_ALL "${crypt_blowfish_SOURCE_DIR}/crypt_blowfish.c")
target_include_directories(crypt_blowfish SYSTEM PUBLIC "${crypt_blowfish_SOURCE_DIR}")
fb_bundled_library(crypt_blowfish)
set(FB_CRYPT_BLOWFISH_VERSION ${FB_CRYPT_BLOWFISH_VERSION} PARENT_SCOPE)
# Its notice is the comment at the top of crypt_blowfish.c.
file(READ "${crypt_blowfish_SOURCE_DIR}/crypt_blowfish.c" text)
string(FIND "${text}" "*/" end)
math(EXPR end "${end} + 2")
string(SUBSTRING "${text}" 0 ${end} notices)
fb_notice(crypt_blowfish "${notices}\n")
set(FB_CRYPT_BLOWFISH_NOTICE "${FB_CRYPT_BLOWFISH_NOTICE}" PARENT_SCOPE)
set(FB_CRYPT_BLOWFISH_LICENSES "a public-domain dedication, or else its permissive license"
    PARENT_SCOPE)

# Monocypher: std.crypto's Ed25519, from its optional monocypher-ed25519.c over monocypher.c, since
# no release of mbedTLS has EdDSA yet; mbedTLS does X25519 and the rest.
set(FB_MONOCYPHER_VERSION 4.0.3)
FetchContent_Declare(
  monocypher
  URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/${FB_MONOCYPHER_VERSION}.tar.gz
  URL_HASH SHA256=a7cbae546fbdc489bca632c3747e1ceb8ca3d4bd39e2706a0916f28ccd280e50
  SYSTEM
)
FetchContent_MakeAvailable(monocypher)
add_library(monocypher STATIC EXCLUDE_FROM_ALL)
target_sources(monocypher PRIVATE
  "${monocypher_SOURCE_DIR}/src/monocypher.c"
  "${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
target_include_directories(monocypher SYSTEM PUBLIC "${monocypher_SOURCE_DIR}/src"
  "${monocypher_SOURCE_DIR}/src/optional")
fb_bundled_library(monocypher)
set(FB_MONOCYPHER_VERSION ${FB_MONOCYPHER_VERSION} PARENT_SCOPE)
# It's under BSD-2-Clause or CC0-1.0, as its LICENCE.md says, and std takes it under CC0-1.0.
file(READ "${monocypher_SOURCE_DIR}/LICENCE.md" notices)
fb_notice(monocypher "${notices}")
set(FB_MONOCYPHER_NOTICE "${FB_MONOCYPHER_NOTICE}" PARENT_SCOPE)
set(FB_MONOCYPHER_LICENSES "the CC0-1.0 license" PARENT_SCOPE)
