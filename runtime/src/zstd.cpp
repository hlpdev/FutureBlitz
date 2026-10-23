// std.compress's Zstd, over zstd's streaming API: an encoder of frames with their checksums, and
// a decoder of frames one after another.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <zstd.h>

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

/// Panics when zstd can't go on with what std.compress checked, as when memory runs out.
void check(const std::size_t result) {
    if (ZSTD_isError(result) != 0) {
        fb_panic("std.compress's Zstd failed", nullptr);
    }
}

struct encoder_t {
    ZSTD_CCtx* context;
    /// Whether the frame it's making has any bytes yet, before which finishing can say its size.
    bool begun = false;
};

struct decoder_t {
    ZSTD_DCtx* context;
    std::int64_t limit;
    std::int64_t given = 0;
    /// Whether the last frame ended at the last byte it had, or it has had none.
    bool ended = true;
    bool begun = false;
    failure_t failure;
};

/// Compresses `data` with `directive`, putting what comes out in `output`, until it's all taken
/// in, or, when ending the frame, until it's ended.
void compress_all(encoder_t& encoder, const std::span<const std::uint8_t> data,
                  const ZSTD_EndDirective directive, fb_array_t* output_array) {
    output_t output(output_array, most_bytes);
    std::array<std::uint8_t, chunk_size> chunk{};
    // Nothing to go on with leaves the frame unbegun, so finishing it can still say its size.
    if (data.empty() && directive == ZSTD_e_continue) {
        output_array->count = 0;
        return;
    }

    ZSTD_inBuffer in{ .src = data.data(), .size = data.size(), .pos = 0 };
    encoder.begun = encoder.begun || !data.empty();
    while (true) {
        ZSTD_outBuffer out{ .dst = chunk.data(), .size = chunk.size(), .pos = 0 };
        const std::size_t left = ZSTD_compressStream2(encoder.context, &out, &in, directive);
        check(left);
        output.add_all(std::span(chunk).first(out.pos));
        const bool flushed = directive == ZSTD_e_end ? left == 0 : out.pos < out.size;
        if (in.pos == in.size && flushed) {
            return;
        }
    }
}

/// Decompresses `data`, putting what comes out in `output`: a result_t.
std::int32_t decompress_all(decoder_t& decoder, const std::span<const std::uint8_t> data,
                            fb_array_t* output_array) {
    if (decoder.failure.result != RESULT_DONE) {
        output_array->count = 0;
        return decoder.failure.result;
    }

    output_t output(output_array, decoder.limit - decoder.given);
    std::array<std::uint8_t, chunk_size> chunk{};
    ZSTD_inBuffer in{ .src = data.data(), .size = data.size(), .pos = 0 };
    decoder.begun = decoder.begun || !data.empty();
    // A chunk it filled may have more after it, unless the frame ended there.
    bool full = false;
    while (in.pos < in.size || (full && !decoder.ended)) {
        ZSTD_outBuffer out{ .dst = chunk.data(), .size = chunk.size(), .pos = 0 };
        const std::size_t hint = ZSTD_decompressStream(decoder.context, &out, &in);
        if (ZSTD_isError(hint) != 0) {
            const ZSTD_ErrorCode error = ZSTD_getErrorCode(hint);
            if (error == ZSTD_error_memory_allocation) {
                fb_panic("out of memory", nullptr);
            }

            // A frame whose window is past zstd's default of 128 MiB, as from zstd's --long,
            // takes that much memory, which zstd doesn't ask for unless told, and one made with
            // a dictionary needs it.
            const bool unsupported = error == ZSTD_error_frameParameter_windowTooLarge ||
                                     error == ZSTD_error_dictionary_wrong;
            return decoder.failure.fail(unsupported ? RESULT_UNSUPPORTED : RESULT_INVALID,
                                        ZSTD_getErrorName(hint));
        }

        if (!output.add(std::span(chunk).first(out.pos))) {
            return decoder.failure.fail(RESULT_TOO_LARGE, "");
        }

        decoder.given += static_cast<std::int64_t>(out.pos);
        decoder.ended = hint == 0;
        full = out.pos == out.size;
    }

    return RESULT_DONE;
}

} // namespace

extern "C" {

void* fb_zstd_encoder_new(const std::int32_t level) {
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    check(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level));
    check(ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1));
    return new encoder_t{ .context = context };
}

void fb_zstd_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output) {
    compress_all(*static_cast<encoder_t*>(encoder), fb::compress::bytes_of(data), ZSTD_e_continue,
                 output);
}

void fb_zstd_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<encoder_t*>(encoder);
    // A frame made at once says its size, which decoders can make room for.
    if (!state.begun) {
        check(ZSTD_CCtx_setPledgedSrcSize(state.context, static_cast<std::uint64_t>(data->count)));
    }

    compress_all(state, fb::compress::bytes_of(data), ZSTD_e_end, output);
    state.begun = false;
}

void fb_zstd_encoder_free(void* encoder) {
    auto* state = static_cast<encoder_t*>(encoder);
    ZSTD_freeCCtx(state->context);
    delete state;
}

void* fb_zstd_decoder_new(const std::int64_t limit) {
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (context == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    return new decoder_t{ .context = context, .limit = limit };
}

std::int32_t fb_zstd_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output) {
    return decompress_all(*static_cast<decoder_t*>(decoder), fb::compress::bytes_of(data), output);
}

std::int32_t fb_zstd_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<decoder_t*>(decoder);
    std::int32_t result = decompress_all(state, fb::compress::bytes_of(data), output);
    if (result == RESULT_DONE && (!state.ended || !state.begun)) {
        result = state.failure.fail(RESULT_ENDS_TOO_SOON, "");
    }

    check(ZSTD_DCtx_reset(state.context, ZSTD_reset_session_only));
    state.given = 0;
    state.ended = true;
    state.begun = false;
    state.failure.result = RESULT_DONE;
    return result;
}

fb_string_t* fb_zstd_decoder_message(const void* decoder) {
    return fb::compress::string_of(static_cast<const decoder_t*>(decoder)->failure.message);
}

void fb_zstd_decoder_free(void* decoder) {
    auto* state = static_cast<decoder_t*>(decoder);
    ZSTD_freeDCtx(state->context);
    delete state;
}
}
