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

# SQLite: std.db, from its amalgamation, one C file. It leaves out loading extensions, which
# would need the OS's dynamic loader, and what's deprecated, turns on foreign keys, which every
# other database has on, and has FTS5 and its math functions.
set(FB_SQLITE_VERSION 3.54.1)
FetchContent_Declare(
  sqlite
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
  URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
  SYSTEM
)
FetchContent_MakeAvailable(sqlite)
add_library(sqlite STATIC EXCLUDE_FROM_ALL "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(sqlite PRIVATE
  SQLITE_DQS=0
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_ENABLE_FTS5
  SQLITE_ENABLE_MATH_FUNCTIONS)
fb_bundled_library(sqlite)
set(FB_SQLITE_VERSION ${FB_SQLITE_VERSION} PARENT_SCOPE)
# It's in the public domain, which the top of sqlite3.h's first comment says with a blessing, up
# to the line of asterisks after them.
file(READ "${sqlite_SOURCE_DIR}/sqlite3.h" text)
string(FIND "${text}" "\n*****" end)
string(SUBSTRING "${text}" 0 ${end} notices)
fb_notice(sqlite "${notices}\n*/\n")
set(FB_SQLITE_NOTICE "${FB_SQLITE_NOTICE}" PARENT_SCOPE)
set(FB_SQLITE_LICENSES "a public-domain dedication" PARENT_SCOPE)

# libtommath: std.math.big's BigInt. Its CMake packages and installs it too, so it's built here
# instead, from every C file in its top folder.
set(FB_LIBTOMMATH_VERSION 1.3.0)
FetchContent_Declare(
  libtommath
  URL https://github.com/libtom/libtommath/archive/refs/tags/v${FB_LIBTOMMATH_VERSION}.tar.gz
  URL_HASH SHA256=6d099e93ff00fa9b18346f4bcd97dcc48c3e91286f7e16c4ac5515a7171c3149
  SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(libtommath)
file(GLOB FB_LIBTOMMATH_SOURCES "${libtommath_SOURCE_DIR}/*.c")
add_library(tommath STATIC EXCLUDE_FROM_ALL ${FB_LIBTOMMATH_SOURCES})
target_include_directories(tommath SYSTEM PUBLIC "${libtommath_SOURCE_DIR}")
fb_bundled_library(tommath)
set(FB_LIBTOMMATH_VERSION ${FB_LIBTOMMATH_VERSION} PARENT_SCOPE)
file(READ "${libtommath_SOURCE_DIR}/LICENSE" notices)
fb_notice(libtommath "${notices}")
set(FB_LIBTOMMATH_NOTICE "${FB_LIBTOMMATH_NOTICE}" PARENT_SCOPE)
set(FB_LIBTOMMATH_LICENSES "the Unlicense" PARENT_SCOPE)

# mpdecimal: std.math.big's Decimal. Its mpdecimal.h is made from mpdecimal.h.in as its configure
# makes it for a 64-bit target, and it's built as configure's portable 64-bit choice, with
# __uint128_t, which GCC and Clang have on every 64-bit target.
set(FB_MPDECIMAL_VERSION 4.0.1)
FetchContent_Declare(
  mpdecimal
  URL https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-${FB_MPDECIMAL_VERSION}.tar.gz
  URL_HASH SHA256=96d33abb4bb0070c7be0fed4246cd38416188325f820468214471938545b1ac8
)
FetchContent_MakeAvailable(mpdecimal)
file(READ "${mpdecimal_SOURCE_DIR}/libmpdec/mpdecimal.h.in" header)
string(REPLACE "@MPD_HEADER_CONFIG@" "/* ABI: 64-bit */
#define MPD_CONFIG_64 1

#ifdef MPD_CONFIG_32
  #error \"cannot use MPD_CONFIG_32 with 64-bit header.\"
#endif

#ifdef CONFIG_32
  #error \"cannot use CONFIG_32 with 64-bit header.\"
#endif" header "${header}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mpdecimal/mpdecimal.h" "${header}")
add_library(mpdecimal STATIC EXCLUDE_FROM_ALL)
foreach(part basearith constants context convolute crt difradix2 fnt fourstep io mpalloc
             mpdecimal numbertheory sixstep transpose)
  target_sources(mpdecimal PRIVATE "${mpdecimal_SOURCE_DIR}/libmpdec/${part}.c")
endforeach()
target_include_directories(mpdecimal SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/mpdecimal")
target_include_directories(mpdecimal PRIVATE "${mpdecimal_SOURCE_DIR}/libmpdec")
target_compile_definitions(mpdecimal PRIVATE CONFIG_64 ANSI HAVE_UINT128_T)
fb_bundled_library(mpdecimal)
set(FB_MPDECIMAL_VERSION ${FB_MPDECIMAL_VERSION} PARENT_SCOPE)
file(READ "${mpdecimal_SOURCE_DIR}/COPYRIGHT.txt" notices)
fb_notice(mpdecimal "${notices}")
set(FB_MPDECIMAL_NOTICE "${FB_MPDECIMAL_NOTICE}" PARENT_SCOPE)
set(FB_MPDECIMAL_LICENSES "the BSD-2-Clause license" PARENT_SCOPE)

# The IANA tz database: std.time's TimeZone. Its zic, built for the machine that builds fb,
# compiles its zones to TZif, slim, for the runtime to read, and embed_tz.cmake writes them into
# fb_tzdata, with every zone's and link's name. Its code and data are in the public domain.
set(FB_TZDATA_VERSION 2026e)
FetchContent_Declare(
  tzcode
  URL https://data.iana.org/time-zones/releases/tzcode${FB_TZDATA_VERSION}.tar.gz
  URL_HASH SHA256=cc3d27ca2a0d8399504551b920970d80af83bfb9c216e8082a15491921935d54
)
FetchContent_Declare(
  tzdata
  URL https://data.iana.org/time-zones/releases/tzdata${FB_TZDATA_VERSION}.tar.gz
  URL_HASH SHA256=b26882805f26aac59d5b222978e6580484b834ccdc98be89df2f05a6dc53a652
)
FetchContent_MakeAvailable(tzcode tzdata)
set(tz "${CMAKE_CURRENT_BINARY_DIR}/tz")
# The headers its Makefile would make.
file(WRITE "${tz}/version.h" "static char const PKGVERSION[] = \"(tzcode) \";
static char const TZVERSION[] = \"${FB_TZDATA_VERSION}\";
static char const REPORT_BUGS_TO[] = \"tz@iana.org\";
")
file(WRITE "${tz}/tzdir.h" "#define TZDEFAULT \"/etc/localtime\"
#define TZDIR \"/usr/share/zoneinfo\"
")
add_executable(fb_zic EXCLUDE_FROM_ALL "${tzcode_SOURCE_DIR}/zic.c")
target_include_directories(fb_zic PRIVATE "${tz}")
target_compile_definitions(fb_zic PRIVATE HAVE_GETTEXT=0)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fb_zic PRIVATE -w -O2)
endif()
# Windows has no POSIX owners, and its mkdir takes no mode; zic only needs neither.
if(WIN32)
  file(WRITE "${tz}/windows.h.in" "#include <direct.h>
#include <io.h>
#include <sys/types.h>
typedef int uid_t;
typedef int gid_t;
#define fchmod(fd, mode) 0
#define fchown(fd, uid, gid) 0
#define mkdir(path, mode) _mkdir(path)
")
  configure_file("${tz}/windows.h.in" "${tz}/posix.h" COPYONLY)
  target_compile_definitions(fb_zic PRIVATE HAVE_LINK=0 HAVE_SYMLINK=0 HAVE_MEMPCPY=1)
  target_compile_options(fb_zic PRIVATE -include "${tz}/posix.h")
endif()

set(FB_TZ_ZONES africa antarctica asia australasia europe northamerica southamerica etcetera
    backward)
list(TRANSFORM FB_TZ_ZONES PREPEND "${tzdata_SOURCE_DIR}/")
add_custom_command(
  OUTPUT "${tz}/tz_data.cpp"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${tz}/zoneinfo"
  COMMAND fb_zic -b slim -d "${tz}/zoneinfo" ${FB_TZ_ZONES}
  COMMAND "${CMAKE_COMMAND}" "-DDATA=${tzdata_SOURCE_DIR}" "-DZONEINFO=${tz}/zoneinfo"
          "-DOUTPUT=${tz}/tz_data.cpp" "-DVERSION=${FB_TZDATA_VERSION}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_tz.cmake"
  DEPENDS fb_zic "${CMAKE_CURRENT_SOURCE_DIR}/embed_tz.cmake" ${FB_TZ_ZONES}
  VERBATIM
)
add_library(fb_tzdata STATIC EXCLUDE_FROM_ALL "${tz}/tz_data.cpp")
target_include_directories(fb_tzdata PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
fb_bundled_library(fb_tzdata)
set(FB_TZDATA_VERSION ${FB_TZDATA_VERSION} PARENT_SCOPE)
file(READ "${tzdata_SOURCE_DIR}/LICENSE" notices)
fb_notice(tzdata "${notices}")
set(FB_TZDATA_NOTICE "${FB_TZDATA_NOTICE}" PARENT_SCOPE)
set(FB_TZDATA_LICENSES "the public domain" PARENT_SCOPE)

# zlib-ng, through its zlib API: std.compress's Deflate, Zlib, and Gzip, minizip-ng's Deflate, and
# curl's gzip and deflate. Its own CMake picks the target's SIMD code, at run time. Only its static
# library is built, without its gzFile functions, since std reads files itself. Its target and
# liblzma's are static ones as BUILD_SHARED_LIBS is set, which liblzma's CMake would set later.
set(FB_ZLIB_NG_VERSION 2.3.3)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
# On Windows its zconf.h includes no <unistd.h>, as when MSVC builds it: only gzFile's functions,
# which aren't built, need what it has, and clang-tidy reads with MSVC's headers, which have none.
if(WIN32)
  set(HAVE_UNISTD_H "" CACHE INTERNAL "")
endif()
FetchContent_Declare(
  zlib_ng
  URL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/${FB_ZLIB_NG_VERSION}.tar.gz
  URL_HASH SHA256=f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
  SYSTEM
  EXCLUDE_FROM_ALL
)
fb_make_available(zlib_ng)
set(FB_ZLIB_NG_VERSION ${FB_ZLIB_NG_VERSION} PARENT_SCOPE)
file(READ "${zlib_ng_SOURCE_DIR}/LICENSE.md" zlib_ng_notice)
fb_notice(zlib_ng "${zlib_ng_notice}")
set(FB_ZLIB_NG_NOTICE "${FB_ZLIB_NG_NOTICE}" PARENT_SCOPE)
set(FB_ZLIB_NG_LICENSES "the zlib license" PARENT_SCOPE)

# zstd: std.compress's Zstd, minizip-ng's, and curl's, from its library's C files: compression and
# decompression, without the dictionary builder, the formats before 1.0, threads, or the x86-64
# assembly, which isn't built the same everywhere.
set(FB_ZSTD_VERSION 1.5.7)
FetchContent_Declare(
  zstd
  URL https://github.com/facebook/zstd/releases/download/v${FB_ZSTD_VERSION}/zstd-${FB_ZSTD_VERSION}.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(zstd)
file(GLOB zstd_sources "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c"
     "${zstd_SOURCE_DIR}/lib/decompress/*.c")
add_library(zstd STATIC EXCLUDE_FROM_ALL ${zstd_sources})
target_include_directories(zstd SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
fb_bundled_library(zstd)
set(FB_ZSTD_VERSION ${FB_ZSTD_VERSION} PARENT_SCOPE)
# It's under BSD-3-Clause or GPL-2.0, and std takes it under BSD-3-Clause, which LICENSE has.
file(READ "${zstd_SOURCE_DIR}/LICENSE" zstd_notice)
fb_notice(zstd "${zstd_notice}")
set(FB_ZSTD_NOTICE "${FB_ZSTD_NOTICE}" PARENT_SCOPE)
set(FB_ZSTD_LICENSES "the BSD-3-Clause license" PARENT_SCOPE)

# LZ4: std.compress's Lz4, in LZ4's frame format, from its library's C files. Its copy of xxHash
# gets names of its own, apart from the xxHash library's.
set(FB_LZ4_VERSION 1.10.0)
FetchContent_Declare(
  lz4
  URL https://github.com/lz4/lz4/archive/refs/tags/v${FB_LZ4_VERSION}.tar.gz
  URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(lz4)
add_library(lz4 STATIC EXCLUDE_FROM_ALL)
target_sources(lz4 PRIVATE
  "${lz4_SOURCE_DIR}/lib/lz4.c"
  "${lz4_SOURCE_DIR}/lib/lz4hc.c"
  "${lz4_SOURCE_DIR}/lib/lz4frame.c"
  "${lz4_SOURCE_DIR}/lib/xxhash.c")
target_include_directories(lz4 SYSTEM PUBLIC "${lz4_SOURCE_DIR}/lib")
target_compile_definitions(lz4 PRIVATE XXH_NAMESPACE=LZ4_)
fb_bundled_library(lz4)
set(FB_LZ4_VERSION ${FB_LZ4_VERSION} PARENT_SCOPE)
# The library is under BSD-2-Clause, as lib/LICENSE says; LZ4's programs are GPL, and aren't built.
file(READ "${lz4_SOURCE_DIR}/lib/LICENSE" lz4_notice)
fb_notice(lz4 "${lz4_notice}")
set(FB_LZ4_NOTICE "${FB_LZ4_NOTICE}" PARENT_SCOPE)
set(FB_LZ4_LICENSES "the BSD-2-Clause license" PARENT_SCOPE)

# XZ Utils' liblzma: std.compress's XZ, 7z reading, and minizip-ng's LZMA and XZ, through its own
# CMake, without threads, translations, or XZ Utils' programs.
set(FB_LIBLZMA_VERSION 5.8.4)
set(XZ_THREADS no CACHE STRING "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
foreach(tool XZ_TOOL_XZ XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS)
  set(${tool} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(
  liblzma
  URL https://github.com/tukaani-project/xz/releases/download/v${FB_LIBLZMA_VERSION}/xz-${FB_LIBLZMA_VERSION}.tar.gz
  URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
  SYSTEM
  EXCLUDE_FROM_ALL
)
fb_make_available(liblzma)
# Its CMake gives lzma.h's folder only to what it installs.
target_include_directories(liblzma SYSTEM INTERFACE
  "$<BUILD_INTERFACE:${liblzma_SOURCE_DIR}/src/liblzma/api>")
set(FB_LIBLZMA_VERSION ${FB_LIBLZMA_VERSION} PARENT_SCOPE)
# liblzma is under 0BSD, as COPYING says, before COPYING.0BSD's text; XZ Utils' other licenses are
# for parts that aren't built.
file(READ "${liblzma_SOURCE_DIR}/COPYING" liblzma_notice)
file(READ "${liblzma_SOURCE_DIR}/COPYING.0BSD" liblzma_license)
string(APPEND liblzma_notice "\nCOPYING.0BSD:\n\n${liblzma_license}")
fb_notice(liblzma "${liblzma_notice}")
set(FB_LIBLZMA_NOTICE "${FB_LIBLZMA_NOTICE}" PARENT_SCOPE)
set(FB_LIBLZMA_LICENSES "the 0BSD license" PARENT_SCOPE)

# minizip-ng: std.compress's Zip, reading and writing zips in memory, stored or by Deflate,
# LZMA, XZ, or Zstd, over the libraries above, from its C files, since its own CMake fetches what
# it doesn't find. A zip's encryption isn't built.
set(FB_MINIZIP_NG_VERSION 4.2.2)
FetchContent_Declare(
  minizip_ng
  URL https://github.com/zlib-ng/minizip-ng/archive/refs/tags/${FB_MINIZIP_NG_VERSION}.tar.gz
  URL_HASH SHA256=71af7b9799856d8b03619df3949e9c1be9703f8de0795af71399ba283cb27aac
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(minizip_ng)
add_library(minizip_ng STATIC EXCLUDE_FROM_ALL)
target_sources(minizip_ng PRIVATE
  "${minizip_ng_SOURCE_DIR}/mz_crypt.c"
  "${minizip_ng_SOURCE_DIR}/mz_strm.c"
  "${minizip_ng_SOURCE_DIR}/mz_strm_mem.c"
  "${minizip_ng_SOURCE_DIR}/mz_strm_zlib.c"
  "${minizip_ng_SOURCE_DIR}/mz_strm_lzma.c"
  "${minizip_ng_SOURCE_DIR}/mz_strm_zstd.c"
  "${minizip_ng_SOURCE_DIR}/mz_zip.c")
# Its mz_config.h says what the C library has, which for the files built here is only <stdint.h>
# and <inttypes.h>, on every target.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/minizip_ng/mz_config.h"
  "#ifndef MZ_CONFIG_H\n#define MZ_CONFIG_H\n#define HAVE_INTTYPES_H 1\n#define HAVE_STDINT_H 1\n"
  "#endif\n")
target_include_directories(minizip_ng SYSTEM PUBLIC "${minizip_ng_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/minizip_ng")
target_compile_definitions(minizip_ng PRIVATE HAVE_ZLIB ZLIB_COMPAT HAVE_LZMA HAVE_ZSTD
  MZ_ZIP_NO_CRYPTO)
target_link_libraries(minizip_ng PRIVATE zlib-ng zstd liblzma)
fb_bundled_library(minizip_ng)
set(FB_MINIZIP_NG_VERSION ${FB_MINIZIP_NG_VERSION} PARENT_SCOPE)
# Its LICENSE has zlib's terms, and its files' comments have whose they are: mz_zip.c's and
# mz_strm_mem.c's name everyone the files built here have. A program that reads or writes zips links
# zlib-ng, zstd, and liblzma with it, so its notices have theirs after its own.
file(READ "${minizip_ng_SOURCE_DIR}/LICENSE" minizip_ng_notice)
foreach(part mz_zip.c mz_strm_mem.c)
  file(READ "${minizip_ng_SOURCE_DIR}/${part}" text)
  string(FIND "${text}" "*/" end)
  math(EXPR end "${end} + 2")
  string(SUBSTRING "${text}" 0 ${end} comment)
  string(APPEND minizip_ng_notice "\n${part}:\n\n${comment}\n")
endforeach()
string(APPEND minizip_ng_notice "\nzlib-ng:\n\n${zlib_ng_notice}\nzstd:\n\n${zstd_notice}")
string(APPEND minizip_ng_notice "\nliblzma:\n\n${liblzma_notice}")
fb_notice(minizip_ng "${minizip_ng_notice}")
set(FB_MINIZIP_NG_NOTICE "${FB_MINIZIP_NG_NOTICE}" PARENT_SCOPE)
set(FB_MINIZIP_NG_LICENSES "the zlib, BSD-3-Clause, and 0BSD licenses" PARENT_SCOPE)

# nghttp2: curl's HTTP/2, from its library's C files, since its own CMake looks for what its
# programs use. The nghttp2ver.h its CMake would write is written beside a copy of nghttp2.h, as
# curl's search looks for them together, and the rest of what its CMake would find is the OS's:
# Windows' GetTickCount64, or POSIX's monotonic clock and byte order.
set(FB_NGHTTP2_VERSION 1.70.0)
FetchContent_Declare(
  nghttp2
  URL https://github.com/nghttp2/nghttp2/releases/download/v${FB_NGHTTP2_VERSION}/nghttp2-${FB_NGHTTP2_VERSION}.tar.xz
  URL_HASH SHA256=e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(nghttp2)
set(nghttp2_includes "${CMAKE_CURRENT_BINARY_DIR}/nghttp2")
file(COPY "${nghttp2_SOURCE_DIR}/lib/includes/nghttp2/nghttp2.h"
     DESTINATION "${nghttp2_includes}/nghttp2")
set(PACKAGE_VERSION ${FB_NGHTTP2_VERSION})
string(REPLACE "." ";" parts ${FB_NGHTTP2_VERSION})
list(GET parts 0 major)
list(GET parts 1 minor)
list(GET parts 2 patch)
math(EXPR PACKAGE_VERSION_NUM "(${major} << 16) | (${minor} << 8) | ${patch}"
     OUTPUT_FORMAT HEXADECIMAL)
configure_file("${nghttp2_SOURCE_DIR}/lib/includes/nghttp2/nghttp2ver.h.in"
               "${nghttp2_includes}/nghttp2/nghttp2ver.h" @ONLY)
file(GLOB nghttp2_sources "${nghttp2_SOURCE_DIR}/lib/*.c")
add_library(nghttp2 STATIC EXCLUDE_FROM_ALL ${nghttp2_sources})
target_include_directories(nghttp2 SYSTEM PUBLIC "${nghttp2_includes}")
target_compile_definitions(nghttp2 PUBLIC NGHTTP2_STATICLIB PRIVATE BUILDING_NGHTTP2)
if(WIN32)
  target_compile_definitions(nghttp2 PRIVATE HAVE_WINDOWS_H HAVE_GETTICKCOUNT64)
else()
  target_compile_definitions(nghttp2 PRIVATE HAVE_ARPA_INET_H HAVE_NETINET_IN_H HAVE_CLOCK_GETTIME
                             HAVE_DECL_CLOCK_MONOTONIC=1)
endif()
fb_bundled_library(nghttp2)
set(FB_NGHTTP2_VERSION ${FB_NGHTTP2_VERSION} PARENT_SCOPE)
# Its notices: its COPYING, then the comments at the top of the files others have copyrights in
# too, ngtcp2's, nghttp3's, and sfparse's contributors, and that of sfparse.c's UTF-8 decoder,
# Bjoern Hoehrmann's, each under the MIT license.
file(READ "${nghttp2_SOURCE_DIR}/COPYING" notices)
foreach(part nghttp2_map.c nghttp2_extpri.c sfparse.c)
  file(READ "${nghttp2_SOURCE_DIR}/lib/${part}" text)
  string(FIND "${text}" "*/" end)
  math(EXPR end "${end} + 2")
  string(SUBSTRING "${text}" 0 ${end} license)
  string(APPEND notices "\nlib/${part}:\n\n${license}\n")
endforeach()
file(READ "${nghttp2_SOURCE_DIR}/lib/sfparse.c" text)
string(FIND "${text}" "/* Copyright (c) 2008-2010 Bjoern Hoehrmann" start)
if(start EQUAL -1)
  message(FATAL_ERROR "nghttp2's sfparse.c has no comment of Bjoern Hoehrmann's copyright")
endif()
string(SUBSTRING "${text}" ${start} -1 text)
string(FIND "${text}" "*/" end)
math(EXPR end "${end} + 2")
string(SUBSTRING "${text}" 0 ${end} license)
string(APPEND notices "\nlib/sfparse.c's UTF-8 decoder:\n\n${license}\n")
fb_notice(nghttp2 "${notices}")
set(FB_NGHTTP2_NOTICE "${FB_NGHTTP2_NOTICE}" PARENT_SCOPE)
set(FB_NGHTTP2_LICENSES "the MIT license" PARENT_SCOPE)

# libcurl: std.net.http's client, over mbedTLS's TLS (its mbedtls and mbedx509 libraries), with
# the OS's certificate roots, which the runtime gives it, so it has no bundle of its own, over
# zlib-ng and zstd, which decode gzip, deflate, and zstd, and over nghttp2, for HTTP/2 where TLS's
# ALPN agrees on it. Only HTTP and HTTPS are built, without the curl tool, its docs, or any
# library it would find on the machine.
set(FB_CURL_VERSION 8.23.0)
foreach(option BUILD_CURL_EXE BUILD_SHARED_LIBS BUILD_LIBCURL_DOCS BUILD_MISC_DOCS
               ENABLE_CURL_MANUAL CURL_ENABLE_EXPORT_TARGET CURL_USE_PKGCONFIG CURL_USE_CMAKECONFIG
               PICKY_COMPILER CURL_USE_OPENSSL CURL_USE_SCHANNEL CURL_WINDOWS_SSPI USE_LIBIDN2 USE_WIN32_IDN USE_APPLE_IDN CURL_USE_LIBPSL CURL_USE_LIBSSH2
               CURL_USE_GSSAPI ENABLE_UNIX_SOCKETS)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(option BUILD_STATIC_LIBS CURL_DISABLE_INSTALL CURL_USE_MBEDTLS USE_NGHTTP2
               NGHTTP2_USE_STATIC_LIBS CURL_DISABLE_ALTSVC
               CURL_DISABLE_AWS CURL_DISABLE_DICT CURL_DISABLE_DOH CURL_DISABLE_FILE
               CURL_DISABLE_FTP CURL_DISABLE_GOPHER CURL_DISABLE_HSTS CURL_DISABLE_IMAP
               CURL_DISABLE_IPFS CURL_DISABLE_KERBEROS_AUTH CURL_DISABLE_LDAP CURL_DISABLE_LDAPS
               CURL_DISABLE_LIBCURL_OPTION CURL_DISABLE_MIME CURL_DISABLE_FORM_API
               CURL_DISABLE_MQTT CURL_DISABLE_NEGOTIATE_AUTH CURL_DISABLE_NETRC CURL_DISABLE_POP3
               CURL_DISABLE_PROGRESS_METER CURL_DISABLE_RTSP CURL_DISABLE_SMTP CURL_DISABLE_TELNET
               CURL_DISABLE_TFTP CURL_DISABLE_BINDLOCAL CURL_DISABLE_GETOPTIONS)
  set(${option} ON CACHE BOOL "" FORCE)
endforeach()
set(CURL_ZLIB ON CACHE STRING "" FORCE)
set(CURL_ZSTD ON CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_CA_BUNDLE none CACHE STRING "" FORCE)
set(CURL_CA_PATH none CACHE STRING "" FORCE)
# Its mbedTLS is the one above, whose targets stand for the libraries its search would find.
set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY tfpsacrypto CACHE STRING "" FORCE)
# So are its zlib, zlib-ng's zlib API, whose target CMake's search for zlib takes as it finds it
# there, its zstd, and its nghttp2, whose searches link the targets they're named.
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlib-ng)
endif()
set(ZLIB_INCLUDE_DIR "${zlib_ng_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY zlib-ng CACHE STRING "" FORCE)
set(ZSTD_INCLUDE_DIR "${zstd_SOURCE_DIR}/lib" CACHE PATH "" FORCE)
set(ZSTD_LIBRARY zstd CACHE STRING "" FORCE)
set(NGHTTP2_INCLUDE_DIR "${nghttp2_includes}" CACHE PATH "" FORCE)
set(NGHTTP2_LIBRARY nghttp2 CACHE STRING "" FORCE)
string(REPLACE "." "_" tag "${FB_CURL_VERSION}")
FetchContent_Declare(
  curl
  URL https://github.com/curl/curl/releases/download/curl-${tag}/curl-${FB_CURL_VERSION}.tar.xz
  URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
  SYSTEM
  EXCLUDE_FROM_ALL
)
fb_make_available(curl)
set(FB_CURL_VERSION ${FB_CURL_VERSION} PARENT_SCOPE)
file(READ "${curl_SOURCE_DIR}/COPYING" notices)
fb_notice(curl "${notices}")
set(FB_CURL_NOTICE "${FB_CURL_NOTICE}" PARENT_SCOPE)
set(FB_CURL_LICENSES "the curl license" PARENT_SCOPE)
