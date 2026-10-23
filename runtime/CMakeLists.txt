file(GLOB_RECURSE FB_RUNTIME_SOURCES CONFIGURE_DEPENDS src/*.cpp)
# The parts over bundled C libraries are archives of their own.
set(FB_RUNTIME_PARTS io dns crypto crypto_pem xxhash blake3 argon2 bcrypt ed25519 db bigint
    decimal tz zlib zstd lz4 xz seven_zip zip unicode regex markdown png stb apng webp svg http
    tls_roots)
foreach(part ${FB_RUNTIME_PARTS})
  list(REMOVE_ITEM FB_RUNTIME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/${part}.cpp)
endforeach()

add_library(fb_runtime STATIC ${FB_RUNTIME_SOURCES})
target_include_directories(fb_runtime PUBLIC include)
# fast_float reads floats, which Apple's C++ library's std::from_chars doesn't yet.
target_link_libraries(fb_runtime PRIVATE FastFloat::fast_float)
fb_set_warnings(fb_runtime)

# The runtime's part over libuv, the I/O thread and std.net's Dns, is an archive of its own,
# which a program links with libuv only when it calls the fb_io_ functions. In one archive with
# the rest, it could be linked for a template it shares with them.
add_library(fb_runtime_io STATIC src/io.cpp src/dns.cpp)
target_include_directories(fb_runtime_io PUBLIC include)
target_include_directories(fb_runtime_io SYSTEM PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(fb_runtime_io PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_COMPILE_DEFINITIONS>)
add_dependencies(fb_runtime_io uv_a)
fb_set_warnings(fb_runtime_io)

# So are its parts over std.crypto's and std.text's libraries, each linked with its library only
# when a program calls its functions: the fb_crypto_ ones over mbedTLS, the fb_xxhash_,
# fb_blake3_, fb_argon2_, fb_bcrypt_, and fb_ed25519_ ones over xxHash, BLAKE3, Argon2,
# crypt_blowfish, and Monocypher, and the fb_markdown_ ones over md4c. Linking each to its
# library here only gives it the library's headers and settings.
foreach(part crypto xxhash blake3 argon2 bcrypt ed25519 markdown)
  add_library(fb_runtime_${part} STATIC src/${part}.cpp)
  target_include_directories(fb_runtime_${part} PUBLIC include)
  fb_set_warnings(fb_runtime_${part})
endforeach()
# PEM keys and certificates are an object file of their own, so only a program that reads them
# links that code.
target_sources(fb_runtime_crypto PRIVATE src/crypto_pem.cpp)
target_link_libraries(fb_runtime_crypto PRIVATE tfpsacrypto)
target_include_directories(fb_runtime_crypto SYSTEM PRIVATE ${FB_MBEDTLS_RSA_INCLUDES})
target_link_libraries(fb_runtime_xxhash PRIVATE xxhash)
target_link_libraries(fb_runtime_blake3 PRIVATE blake3)
target_link_libraries(fb_runtime_argon2 PRIVATE argon2)
target_link_libraries(fb_runtime_bcrypt PRIVATE crypt_blowfish)
target_link_libraries(fb_runtime_ed25519 PRIVATE monocypher)
target_link_libraries(fb_runtime_markdown PRIVATE md4c)

# std.db's fb_db_ functions over SQLite are one more.
add_library(fb_runtime_db STATIC src/db.cpp)
target_include_directories(fb_runtime_db PUBLIC include)
target_link_libraries(fb_runtime_db PRIVATE sqlite)
fb_set_warnings(fb_runtime_db)

# And its parts over std.math.big's: the fb_bigint_ ones over libtommath, and the fb_decimal_ ones
# over mpdecimal.
foreach(part bigint decimal)
  add_library(fb_runtime_${part} STATIC src/${part}.cpp)
  target_include_directories(fb_runtime_${part} PUBLIC include)
  fb_set_warnings(fb_runtime_${part})
endforeach()
target_link_libraries(fb_runtime_bigint PRIVATE tommath)
# fast_float, which the compiler's CMake fetches, reads the Doubles Decimals become.
target_link_libraries(fb_runtime_decimal PRIVATE mpdecimal FastFloat::fast_float)

# And its part over the tz database, the fb_tz_ functions, which a program links with the zones
# only when it calls one.
add_library(fb_runtime_tz STATIC src/tz.cpp)
target_include_directories(fb_runtime_tz PUBLIC include)
fb_set_warnings(fb_runtime_tz)

# And its parts over std.compress's: the fb_zlib_, fb_zstd_, fb_lz4_, fb_xz_, and fb_zip_
# functions over zlib-ng, zstd, LZ4, liblzma, and minizip-ng. 7z's archives, read over liblzma, are
# an object file of their own in liblzma's part, so only a program that reads them links that code.
foreach(part zlib zstd lz4 xz zip)
  add_library(fb_runtime_${part} STATIC src/${part}.cpp)
  target_include_directories(fb_runtime_${part} PUBLIC include)
  fb_set_warnings(fb_runtime_${part})
endforeach()
target_link_libraries(fb_runtime_zlib PRIVATE zlib-ng)
target_link_libraries(fb_runtime_zstd PRIVATE zstd)
target_link_libraries(fb_runtime_lz4 PRIVATE lz4)
target_sources(fb_runtime_xz PRIVATE src/seven_zip.cpp)
target_link_libraries(fb_runtime_xz PRIVATE liblzma)
target_link_libraries(fb_runtime_zip PRIVATE minizip_ng liblzma)
# std.image's PNG and APNG encoders are an object file of their own in zlib-ng's part.
target_sources(fb_runtime_zlib PRIVATE src/png.cpp)

# And its parts over std.image's: the fb_stb_, fb_webp_, and fb_svg_ functions over stb_image and
# stb_image_write, libwebp, and nanosvg. APNG's decoder is an object file of its own in stb's part.
# QOI's codec, GIF's encoder, and what every decoder gives need no library, and are fb_runtime's.
foreach(part stb webp svg)
  add_library(fb_runtime_${part} STATIC src/${part}.cpp)
  target_include_directories(fb_runtime_${part} PUBLIC include)
  fb_set_warnings(fb_runtime_${part})
endforeach()
target_sources(fb_runtime_stb PRIVATE src/apng.cpp)
target_link_libraries(fb_runtime_stb PRIVATE stb)
target_link_libraries(fb_runtime_webp PRIVATE webp)
target_link_libraries(fb_runtime_svg PRIVATE nanosvg)

# Its part over utf8proc, the fb_unicode_ functions, which std.text's case, categories,
# normalization, and grapheme clusters call.
add_library(fb_runtime_unicode STATIC src/unicode.cpp)
target_include_directories(fb_runtime_unicode PUBLIC include)
target_link_libraries(fb_runtime_unicode PRIVATE utf8proc)
fb_set_warnings(fb_runtime_unicode)

# Its part over PCRE2, the fb_regex_ functions, which std.text.regex calls.
add_library(fb_runtime_regex STATIC src/regex.cpp)
target_include_directories(fb_runtime_regex PUBLIC include)
target_link_libraries(fb_runtime_regex PRIVATE pcre2-8-static)
fb_set_warnings(fb_runtime_regex)

# std.image's resizing and drawing compute in doubles, so they're built as std.image's libraries
# are, without fused multiply-adds, and give the same pixels on every target.
set_source_files_properties(src/transform.cpp src/draw.cpp src/font.cpp src/cff.cpp PROPERTIES
                            COMPILE_OPTIONS "${FB_SAME_FLOATS}")

# The fb_http_ functions, std.net.http's client, over libcurl and its TLS over mbedTLS, with the
# OS's roots (tls_roots.cpp); its Async transfers run on the I/O thread, so a program that calls
# them links fb_runtime_io and libuv too.
add_library(fb_runtime_http STATIC src/http.cpp src/tls_roots.cpp)
target_include_directories(fb_runtime_http PUBLIC include)
target_link_libraries(fb_runtime_http PRIVATE libcurl_static)
target_include_directories(fb_runtime_http SYSTEM PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(fb_runtime_http PRIVATE
  $<TARGET_PROPERTY:uv_a,INTERFACE_COMPILE_DEFINITIONS>)
add_dependencies(fb_runtime_http uv_a)
fb_set_warnings(fb_runtime_http)
# What reads the OS's roots: Windows' store, and macOS's Security framework.
if(WIN32)
  target_link_libraries(fb_runtime_http INTERFACE crypt32)
elseif(APPLE)
  target_link_libraries(fb_runtime_http INTERFACE "-framework Security" "-framework CoreFoundation")
endif()

# What a program can link: each part and its library, which fb and the tests that build programs
# depend on.
set(FB_RUNTIME_ARCHIVES fb_runtime fb_runtime_io uv_a fb_runtime_crypto tfpsacrypto
    fb_runtime_xxhash xxhash fb_runtime_blake3 blake3 fb_runtime_argon2 argon2 fb_runtime_bcrypt
    crypt_blowfish fb_runtime_ed25519 monocypher fb_runtime_db sqlite fb_runtime_bigint tommath
    fb_runtime_decimal mpdecimal fb_runtime_tz fb_tzdata fb_runtime_zlib zlib-ng fb_runtime_zstd
    zstd fb_runtime_lz4 lz4 fb_runtime_xz liblzma fb_runtime_zip minizip_ng fb_runtime_unicode
    utf8proc fb_runtime_regex pcre2-8-static fb_runtime_markdown md4c fb_runtime_stb stb
    fb_runtime_webp webp sharpyuv fb_runtime_svg nanosvg fb_runtime_http libcurl_static mbedtls
    mbedx509 PARENT_SCOPE)

# Nothing in the runtime throws: exceptions can't cross into compiled programs.
foreach(part fb_runtime fb_runtime_io fb_runtime_crypto fb_runtime_xxhash fb_runtime_blake3
             fb_runtime_argon2 fb_runtime_bcrypt fb_runtime_ed25519 fb_runtime_db
             fb_runtime_bigint fb_runtime_decimal fb_runtime_tz fb_runtime_zlib fb_runtime_zstd
             fb_runtime_lz4 fb_runtime_xz fb_runtime_zip fb_runtime_unicode fb_runtime_regex
             fb_runtime_markdown fb_runtime_stb fb_runtime_webp fb_runtime_svg fb_runtime_http)
  if(MSVC)
    target_compile_options(${part} PRIVATE /EHs-c-)
  else()
    target_compile_options(${part} PRIVATE -fno-exceptions)
  endif()
endforeach()
