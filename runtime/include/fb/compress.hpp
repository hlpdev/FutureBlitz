#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.compress's functions in the runtime, each over the bundled C library its prefix names,
// which a program links only when it calls one: fb_zlib_ over zlib-ng, fb_zstd_ over zstd,
// fb_lz4_ over LZ4, fb_xz_ over liblzma (XZ, and 7z's archives), and fb_zip_ over minizip-ng.
// fb_tar_'s need no library. Arrays and Strings are only borrowed. What gives bytes puts them in
// `output`, a UInt8[] the caller made, whose old bytes they replace. std.compress checks levels and
// limits first.
//
// Each codec has an encoder and a decoder, which take bytes in turn and give what came out of
// them so far. Finishing one takes the last bytes, gives the rest, and starts it again. A
// decoder's add and finish give 0, or 1 when the bytes aren't the format or are broken, 2 when
// they end too soon, which only finishing can tell, or 3 when they'd come to more than the
// decoder's limit, all its bytes counted until it finishes. Once it fails it gives that again
// until it finishes, and its message says why, from its library. An archive's reader gives those
// too, and 4 for what it doesn't support, like a method or encryption.
//
// zlib-ng's format is 1 Deflate (RFC 1951), 2 Zlib (RFC 1950), or 3 Gzip (RFC 1952), whose
// decoder reads members one after another.

extern "C" {

void* fb_zlib_encoder_new(std::int32_t format, std::int32_t level);
void fb_zlib_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_zlib_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_zlib_encoder_free(void* encoder);
void* fb_zlib_decoder_new(std::int32_t format, std::int64_t limit);
std::int32_t fb_zlib_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output);
std::int32_t fb_zlib_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output);
fb_string_t* fb_zlib_decoder_message(const void* decoder);
void fb_zlib_decoder_free(void* decoder);

/// Zstd's frames, each with its content's checksum, and its size when the encoder has all the
/// bytes at once. The decoder reads frames one after another, skipping skippable ones.
void* fb_zstd_encoder_new(std::int32_t level);
void fb_zstd_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_zstd_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_zstd_encoder_free(void* encoder);
void* fb_zstd_decoder_new(std::int64_t limit);
std::int32_t fb_zstd_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output);
std::int32_t fb_zstd_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output);
fb_string_t* fb_zstd_decoder_message(const void* decoder);
void fb_zstd_decoder_free(void* decoder);

/// LZ4's frame format, each frame with its content's checksum; the decoder reads frames one
/// after another.
void* fb_lz4_encoder_new(std::int32_t level);
void fb_lz4_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_lz4_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_lz4_encoder_free(void* encoder);
void* fb_lz4_decoder_new(std::int64_t limit);
std::int32_t fb_lz4_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output);
std::int32_t fb_lz4_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output);
fb_string_t* fb_lz4_decoder_message(const void* decoder);
void fb_lz4_decoder_free(void* decoder);

/// The .xz format, by one of its presets, with a CRC64 check; the decoder reads streams one
/// after another, and the padding between them.
void* fb_xz_encoder_new(std::int32_t level);
void fb_xz_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_xz_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output);
void fb_xz_encoder_free(void* encoder);
void* fb_xz_decoder_new(std::int64_t limit);
std::int32_t fb_xz_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output);
std::int32_t fb_xz_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output);
fb_string_t* fb_xz_decoder_message(const void* decoder);
void fb_xz_decoder_free(void* decoder);

// An archive's reader takes a copy of its bytes, and gives its entries by index, from 0, in the
// order they're in: each one's name, as UTF-8, its size, its kind (0 a file, 1 a folder, or 2 a
// link), and a link's target. Its result is 0, or 1 when the bytes aren't the archive's format,
// and its message says why. Reading an entry gives its bytes, or a result that isn't 0, with the
// message. Its _free function gives it back either way.

/// A zip, which names its entries in UTF-8 or, when they don't say so and aren't UTF-8, code page
/// 437. Its writer adds each entry, a file of `data` by `method` (0 stored, 1 Deflate, 2 LZMA, 3
/// XZ, or 4 Zstd) at `level`, or a folder when `folder` isn't 0, made by Unix on 1980-01-01, and
/// finishing gives the zip and starts again.
void* fb_zip_reader_new(const fb_array_t* data);
std::int32_t fb_zip_reader_result(const void* reader);
std::int64_t fb_zip_reader_count(const void* reader);
fb_string_t* fb_zip_reader_name(const void* reader, std::int64_t index);
std::int64_t fb_zip_reader_size(const void* reader, std::int64_t index);
std::int32_t fb_zip_reader_kind(const void* reader, std::int64_t index);
fb_string_t* fb_zip_reader_target(const void* reader, std::int64_t index);
std::int32_t fb_zip_reader_read(void* reader, std::int64_t index, fb_array_t* output);
fb_string_t* fb_zip_reader_message(const void* reader);
void fb_zip_reader_free(void* reader);
void* fb_zip_writer_new();
void fb_zip_writer_add(void* writer, const fb_string_t* name, const fb_array_t* data,
                       std::int32_t method, std::int32_t level, std::int32_t folder);
void fb_zip_writer_finish(void* writer, fb_array_t* output);
void fb_zip_writer_free(void* writer);

/// A tar: POSIX's ustar, with pax's extended headers and GNU's long names read, of names that
/// aren't UTF-8 read with U+FFFD for each byte that isn't. Its writer adds each entry, a file of
/// `data` or a folder when `folder` isn't 0, as ustar, with a pax header for a name too long for
/// ustar's, owned by root, at 1970's start, and finishing gives the tar and starts again.
void* fb_tar_reader_new(const fb_array_t* data);
std::int32_t fb_tar_reader_result(const void* reader);
std::int64_t fb_tar_reader_count(const void* reader);
fb_string_t* fb_tar_reader_name(const void* reader, std::int64_t index);
std::int64_t fb_tar_reader_size(const void* reader, std::int64_t index);
std::int32_t fb_tar_reader_kind(const void* reader, std::int64_t index);
fb_string_t* fb_tar_reader_target(const void* reader, std::int64_t index);
std::int32_t fb_tar_reader_read(void* reader, std::int64_t index, fb_array_t* output);
fb_string_t* fb_tar_reader_message(const void* reader);
void fb_tar_reader_free(void* reader);
void* fb_tar_writer_new();
void fb_tar_writer_add(void* writer, const fb_string_t* name, const fb_array_t* data,
                       std::int32_t folder);
void fb_tar_writer_finish(void* writer, fb_array_t* output);
void fb_tar_writer_free(void* writer);

/// A 7z, read over liblzma: its folders' coders can be stored, LZMA, LZMA2, Delta, and the branch
/// filters for x86, PowerPC, IA-64, ARM, ARM Thumb, ARM64, SPARC, and RISC-V, as one coder or a
/// chain of them. Others, like BCJ2, PPMd, and AES, read as unsupported.
void* fb_xz_7z_reader_new(const fb_array_t* data);
std::int32_t fb_xz_7z_reader_result(const void* reader);
std::int64_t fb_xz_7z_reader_count(const void* reader);
fb_string_t* fb_xz_7z_reader_name(const void* reader, std::int64_t index);
std::int64_t fb_xz_7z_reader_size(const void* reader, std::int64_t index);
std::int32_t fb_xz_7z_reader_kind(const void* reader, std::int64_t index);
fb_string_t* fb_xz_7z_reader_target(const void* reader, std::int64_t index);
std::int32_t fb_xz_7z_reader_read(void* reader, std::int64_t index, fb_array_t* output);
fb_string_t* fb_xz_7z_reader_message(const void* reader);
void fb_xz_7z_reader_free(void* reader);
}
