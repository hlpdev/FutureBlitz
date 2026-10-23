// std.compress's Deflate, Zlib, and Gzip, over zlib-ng's zlib API: an encoder and a decoder for
// each, which take bytes in turn and give what comes out.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <zlib.h>

using fb::compress::chunk_size;
using fb::compress::failure_t;
using fb::compress::most_bytes;
using fb::compress::output_t;
using fb::compress::RESULT_DONE;
using fb::compress::RESULT_ENDS_TOO_SOON;
using fb::compress::RESULT_INVALID;
using fb::compress::RESULT_TOO_LARGE;

namespace {

constexpr std::int32_t format_deflate = 1;
constexpr std::int32_t format_gzip = 3;

/// The first byte of a gzip member, which a second member after the first starts with.
constexpr std::uint8_t gzip_first = 0x1f;

/// zlib's window bits for a format: raw Deflate's are negative, and Gzip's have 16 added.
int window_bits(const std::int32_t format) {
    if (format == format_deflate) {
        return -MAX_WBITS;
    }

    return format == format_gzip ? MAX_WBITS + 16 : MAX_WBITS;
}

const char* name_of(const std::int32_t format) {
    if (format == format_deflate) {
        return "Deflate";
    }

    return format == format_gzip ? "Gzip" : "Zlib";
}

/// Panics when zlib can't go on with what std.compress checked, which only running out of
/// memory does.
void check(const int status) {
    if (status == Z_MEM_ERROR) {
        fb_panic("out of memory", nullptr);
    }

    if (status != Z_OK) {
        fb_panic("std.compress's Deflate failed", nullptr);
    }
}

/// A gzip header that says nothing of where it was made, so every OS writes the same bytes.
gz_header plain_header() {
    gz_header header{};
    header.os = 255;
    return header;
}

struct encoder_t {
    z_stream stream{};
    std::int32_t format;
    gz_header header = plain_header();
};

struct decoder_t {
    z_stream stream{};
    std::int32_t format;
    std::int64_t limit;
    /// How many bytes it gave since it started.
    std::int64_t given = 0;
    /// Whether a stream ended at the last byte it had, which a Gzip member may follow.
    bool ended = false;
    failure_t failure;
};

void reset(encoder_t& encoder) {
    check(deflateReset(&encoder.stream));
    if (encoder.format == format_gzip) {
        check(deflateSetHeader(&encoder.stream, &encoder.header));
    }
}

/// Deflates `data` with `flush`, putting what comes out in `output`, until it's all taken in, or,
/// when finishing, until the stream ends.
void deflate_all(encoder_t& encoder, const std::span<const std::uint8_t> data, const int flush,
                 output_t& output) {
    std::array<std::uint8_t, chunk_size> chunk{};
    std::size_t at = 0;
    while (true) {
        const std::size_t taking = std::min<std::size_t>(data.size() - at, UINT_MAX);
        encoder.stream.next_in = const_cast<std::uint8_t*>(data.data() + at);
        encoder.stream.avail_in = static_cast<uInt>(taking);
        encoder.stream.next_out = chunk.data();
        encoder.stream.avail_out = static_cast<uInt>(chunk.size());
        const bool last = at + taking == data.size();
        const int status = deflate(&encoder.stream, last ? flush : Z_NO_FLUSH);
        at += taking - encoder.stream.avail_in;
        output.add_all(std::span(chunk).first(chunk.size() - encoder.stream.avail_out));
        if (status == Z_STREAM_END) {
            return;
        }

        if (status != Z_OK && status != Z_BUF_ERROR) {
            check(status);
        }

        // Done with this input once it's all in and there was room for what came out.
        if (at == data.size() && encoder.stream.avail_out != 0 && flush == Z_NO_FLUSH) {
            return;
        }
    }
}

/// Inflates `data`, putting what comes out in `output`: a result_t.
std::int32_t inflate_all(decoder_t& decoder, const std::span<const std::uint8_t> data,
                         fb_array_t* output_array) {
    if (decoder.failure.result != RESULT_DONE) {
        output_array->count = 0;
        return decoder.failure.result;
    }

    output_t output(output_array, decoder.limit - decoder.given);
    std::array<std::uint8_t, chunk_size> chunk{};
    std::size_t at = 0;
    while (at < data.size() || (decoder.stream.avail_out == 0 && !decoder.ended)) {
        if (decoder.ended) {
            if (decoder.format != format_gzip || data[at] != gzip_first) {
                return decoder.failure.fail(
                    RESULT_INVALID, std::string("there are more bytes after the end of the ") +
                                        name_of(decoder.format) + " data");
            }

            check(inflateReset(&decoder.stream));
            decoder.ended = false;
        }

        const std::size_t taking = std::min<std::size_t>(data.size() - at, UINT_MAX);
        decoder.stream.next_in = const_cast<std::uint8_t*>(data.data() + at);
        decoder.stream.avail_in = static_cast<uInt>(taking);
        decoder.stream.next_out = chunk.data();
        decoder.stream.avail_out = static_cast<uInt>(chunk.size());
        const int status = inflate(&decoder.stream, Z_NO_FLUSH);
        at += taking - decoder.stream.avail_in;
        const std::size_t made = chunk.size() - decoder.stream.avail_out;
        if (!output.add(std::span(chunk).first(made))) {
            return decoder.failure.fail(RESULT_TOO_LARGE, "");
        }

        decoder.given += static_cast<std::int64_t>(made);
        if (status == Z_STREAM_END) {
            decoder.ended = true;
            decoder.stream.avail_out = 1;
        } else if (status == Z_BUF_ERROR) {
            // No progress: it needs more bytes than there are.
            break;
        } else if (status == Z_MEM_ERROR) {
            check(status);
        } else if (status != Z_OK) {
            const char* message = decoder.stream.msg != nullptr ? decoder.stream.msg : "";
            return decoder.failure.fail(RESULT_INVALID, message);
        }
    }

    return RESULT_DONE;
}

} // namespace

extern "C" {

void* fb_zlib_encoder_new(const std::int32_t format, const std::int32_t level) {
    auto* encoder = new encoder_t{ .format = format };
    check(deflateInit2(&encoder->stream, level, Z_DEFLATED, window_bits(format), 8,
                       Z_DEFAULT_STRATEGY));
    if (format == format_gzip) {
        check(deflateSetHeader(&encoder->stream, &encoder->header));
    }

    return encoder;
}

void fb_zlib_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output) {
    output_t out(output, most_bytes);
    deflate_all(*static_cast<encoder_t*>(encoder), fb::compress::bytes_of(data), Z_NO_FLUSH, out);
}

void fb_zlib_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<encoder_t*>(encoder);
    output_t out(output, most_bytes);
    deflate_all(state, fb::compress::bytes_of(data), Z_FINISH, out);
    reset(state);
}

void fb_zlib_encoder_free(void* encoder) {
    auto* state = static_cast<encoder_t*>(encoder);
    deflateEnd(&state->stream);
    delete state;
}

void* fb_zlib_decoder_new(const std::int32_t format, const std::int64_t limit) {
    auto* decoder = new decoder_t{ .format = format, .limit = limit };
    check(inflateInit2(&decoder->stream, window_bits(format)));
    return decoder;
}

std::int32_t fb_zlib_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output) {
    return inflate_all(*static_cast<decoder_t*>(decoder), fb::compress::bytes_of(data), output);
}

std::int32_t fb_zlib_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<decoder_t*>(decoder);
    std::int32_t result = inflate_all(state, fb::compress::bytes_of(data), output);
    if (result == RESULT_DONE && !state.ended) {
        result = state.failure.fail(RESULT_ENDS_TOO_SOON, "");
    }

    // It starts again, keeping why it failed for its message.
    check(inflateReset(&state.stream));
    state.given = 0;
    state.ended = false;
    state.failure.result = RESULT_DONE;
    return result;
}

fb_string_t* fb_zlib_decoder_message(const void* decoder) {
    return fb::compress::string_of(static_cast<const decoder_t*>(decoder)->failure.message);
}

void fb_zlib_decoder_free(void* decoder) {
    auto* state = static_cast<decoder_t*>(decoder);
    inflateEnd(&state->stream);
    delete state;
}
}
