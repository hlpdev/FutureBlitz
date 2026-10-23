// std.compress's XZ, over liblzma: an encoder of .xz streams by a preset, with a CRC64 check, and
// a decoder of streams one after another. seven_zip.cpp reads 7z's archives over it too.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <lzma.h>
#include <span>

using fb::compress::chunk_size;
using fb::compress::failure_t;
using fb::compress::most_bytes;
using fb::compress::output_t;
using fb::compress::RESULT_DONE;
using fb::compress::RESULT_ENDS_TOO_SOON;
using fb::compress::RESULT_INVALID;
using fb::compress::RESULT_TOO_LARGE;
using fb::compress::RESULT_UNSUPPORTED;

namespace {

/// Panics when liblzma can't go on with what std.compress checked, as when memory runs out.
void check(const lzma_ret result) {
    if (result == LZMA_MEM_ERROR) {
        fb_panic("out of memory", nullptr);
    }

    if (result != LZMA_OK) {
        fb_panic("std.compress's XZ failed", nullptr);
    }
}

struct encoder_t {
    lzma_stream stream = LZMA_STREAM_INIT;
    std::uint32_t preset;
};

struct decoder_t {
    lzma_stream stream = LZMA_STREAM_INIT;
    std::int64_t limit;
    std::int64_t given = 0;
    failure_t failure;
};

void start(encoder_t& encoder) {
    check(lzma_easy_encoder(&encoder.stream, encoder.preset, LZMA_CHECK_CRC64));
}

void start(decoder_t& decoder) {
    check(lzma_stream_decoder(&decoder.stream, UINT64_MAX, LZMA_CONCATENATED));
}

/// Runs the encoder over `data` with `action`, putting what comes out in `output`, until it's all
/// taken in, or, when finishing, until the stream ends.
void encode_all(encoder_t& encoder, const std::span<const std::uint8_t> data,
                const lzma_action action, fb_array_t* output_array) {
    output_t output(output_array, most_bytes);
    // A run with nothing to take in would make no progress, which liblzma takes as a bug once
    // it's twice in a row.
    if (action == LZMA_RUN && data.empty()) {
        return;
    }

    std::array<std::uint8_t, chunk_size> chunk{};
    encoder.stream.next_in = data.data();
    encoder.stream.avail_in = data.size();
    while (true) {
        encoder.stream.next_out = chunk.data();
        encoder.stream.avail_out = chunk.size();
        const lzma_ret result = lzma_code(&encoder.stream, action);
        output.add_all(std::span(chunk).first(chunk.size() - encoder.stream.avail_out));
        if (result == LZMA_STREAM_END) {
            return;
        }

        check(result);
        if (action == LZMA_RUN && encoder.stream.avail_in == 0 && encoder.stream.avail_out != 0) {
            return;
        }
    }
}

/// Runs the decoder over `data` with `action`, putting what comes out in `output`: a result_t.
std::int32_t decode_all(decoder_t& decoder, const std::span<const std::uint8_t> data,
                        const lzma_action action, fb_array_t* output_array) {
    if (decoder.failure.result != RESULT_DONE) {
        output_array->count = 0;
        return decoder.failure.result;
    }

    output_t output(output_array, decoder.limit - decoder.given);
    if (action == LZMA_RUN && data.empty()) {
        return RESULT_DONE;
    }

    std::array<std::uint8_t, chunk_size> chunk{};
    decoder.stream.next_in = data.data();
    decoder.stream.avail_in = data.size();
    while (true) {
        decoder.stream.next_out = chunk.data();
        decoder.stream.avail_out = chunk.size();
        const lzma_ret result = lzma_code(&decoder.stream, action);
        const std::size_t made = chunk.size() - decoder.stream.avail_out;
        if (!output.add(std::span(chunk).first(made))) {
            return decoder.failure.fail(RESULT_TOO_LARGE, "");
        }

        decoder.given += static_cast<std::int64_t>(made);
        switch (result) {
        case LZMA_STREAM_END:
            return RESULT_DONE;
        case LZMA_OK:
            break;
        case LZMA_BUF_ERROR:
            return decoder.failure.fail(RESULT_ENDS_TOO_SOON, "");
        case LZMA_MEM_ERROR:
            check(result);
            break;
        case LZMA_FORMAT_ERROR:
            return decoder.failure.fail(RESULT_INVALID, "the bytes aren't the .xz format");
        case LZMA_OPTIONS_ERROR:
            return decoder.failure.fail(RESULT_UNSUPPORTED,
                                        "the .xz data uses options liblzma doesn't support");
        default:
            return decoder.failure.fail(RESULT_INVALID, "the .xz data is broken");
        }

        if (action == LZMA_RUN && decoder.stream.avail_in == 0 && decoder.stream.avail_out != 0) {
            return RESULT_DONE;
        }
    }
}

} // namespace

extern "C" {

void* fb_xz_encoder_new(const std::int32_t level) {
    auto* encoder = new encoder_t{ .preset = static_cast<std::uint32_t>(level) };
    start(*encoder);
    return encoder;
}

void fb_xz_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output) {
    encode_all(*static_cast<encoder_t*>(encoder), fb::compress::bytes_of(data), LZMA_RUN, output);
}

void fb_xz_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<encoder_t*>(encoder);
    encode_all(state, fb::compress::bytes_of(data), LZMA_FINISH, output);
    start(state);
}

void fb_xz_encoder_free(void* encoder) {
    auto* state = static_cast<encoder_t*>(encoder);
    lzma_end(&state->stream);
    delete state;
}

void* fb_xz_decoder_new(const std::int64_t limit) {
    auto* decoder = new decoder_t{ .limit = limit };
    start(*decoder);
    return decoder;
}

std::int32_t fb_xz_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output) {
    return decode_all(*static_cast<decoder_t*>(decoder), fb::compress::bytes_of(data), LZMA_RUN,
                      output);
}

std::int32_t fb_xz_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<decoder_t*>(decoder);
    const std::int32_t result =
        decode_all(state, fb::compress::bytes_of(data), LZMA_FINISH, output);
    start(state);
    state.given = 0;
    state.failure.result = RESULT_DONE;
    return result;
}

fb_string_t* fb_xz_decoder_message(const void* decoder) {
    return fb::compress::string_of(static_cast<const decoder_t*>(decoder)->failure.message);
}

void fb_xz_decoder_free(void* decoder) {
    auto* state = static_cast<decoder_t*>(decoder);
    lzma_end(&state->stream);
    delete state;
}
}
