// std.compress's Lz4, over LZ4's frame API: an encoder of frames with their content's checksum,
// and a decoder of frames one after another.

#include "compress_internal.hpp"
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <lz4frame.h>
#include <span>
#include <vector>

using fb::compress::chunk_size;
using fb::compress::failure_t;
using fb::compress::most_bytes;
using fb::compress::output_t;
using fb::compress::RESULT_DONE;
using fb::compress::RESULT_ENDS_TOO_SOON;
using fb::compress::RESULT_INVALID;
using fb::compress::RESULT_TOO_LARGE;

namespace {

/// Panics when LZ4 can't go on with what std.compress checked, as when memory runs out.
std::size_t check(const std::size_t result) {
    if (LZ4F_isError(result) != 0) {
        fb_panic("std.compress's Lz4 failed", nullptr);
    }

    return result;
}

struct encoder_t {
    LZ4F_cctx* context;
    LZ4F_preferences_t preferences;
    /// Room for what compressing a chunk, or ending a frame, can make.
    std::vector<std::uint8_t> room;
    /// Whether the frame it's making has begun.
    bool begun = false;
};

struct decoder_t {
    LZ4F_dctx* context;
    std::int64_t limit;
    std::int64_t given = 0;
    /// Whether the last frame ended at the last byte it had, or it has had none.
    bool ended = true;
    bool begun = false;
    failure_t failure;
};

/// Begins a frame, unless one has: of `size` bytes when that's known, or else 0.
void begin(encoder_t& encoder, const std::uint64_t size, output_t& output) {
    if (encoder.begun) {
        return;
    }

    encoder.preferences.frameInfo.contentSize = size;
    const std::size_t made = check(LZ4F_compressBegin(encoder.context, encoder.room.data(),
                                                      encoder.room.size(), &encoder.preferences));
    output.add_all(std::span(encoder.room).first(made));
    encoder.begun = true;
}

/// Compresses `data`, a chunk at a time, putting what comes out in `output`.
void compress_all(encoder_t& encoder, const std::span<const std::uint8_t> data, output_t& output) {
    for (std::size_t at = 0; at < data.size(); at += chunk_size) {
        const std::span<const std::uint8_t> piece =
            data.subspan(at, std::min(chunk_size, data.size() - at));
        const std::size_t made =
            check(LZ4F_compressUpdate(encoder.context, encoder.room.data(), encoder.room.size(),
                                      piece.data(), piece.size(), nullptr));
        output.add_all(std::span(encoder.room).first(made));
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
    std::size_t at = 0;
    decoder.begun = decoder.begun || !data.empty();
    // A chunk it filled may have more after it, unless the frame ended there.
    bool full = false;
    while (at < data.size() || (full && !decoder.ended)) {
        std::size_t made = chunk.size();
        std::size_t taken = data.size() - at;
        const std::size_t hint = LZ4F_decompress(decoder.context, chunk.data(), &made,
                                                 data.data() + at, &taken, nullptr);
        if (LZ4F_isError(hint) != 0) {
            return decoder.failure.fail(RESULT_INVALID, LZ4F_getErrorName(hint));
        }

        at += taken;
        if (!output.add(std::span(chunk).first(made))) {
            return decoder.failure.fail(RESULT_TOO_LARGE, "");
        }

        decoder.given += static_cast<std::int64_t>(made);
        decoder.ended = hint == 0;
        full = made == chunk.size();
    }

    return RESULT_DONE;
}

} // namespace

extern "C" {

void* fb_lz4_encoder_new(const std::int32_t level) {
    LZ4F_cctx* context = nullptr;
    check(LZ4F_createCompressionContext(&context, LZ4F_VERSION));
    LZ4F_preferences_t preferences = LZ4F_INIT_PREFERENCES;
    preferences.compressionLevel = level;
    preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    const std::size_t room =
        std::max<std::size_t>(LZ4F_compressBound(chunk_size, &preferences), LZ4F_HEADER_SIZE_MAX);
    return new encoder_t{
        .context = context,
        .preferences = preferences,
        .room = std::vector<std::uint8_t>(room),
    };
}

void fb_lz4_encoder_add(void* encoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<encoder_t*>(encoder);
    output_t out(output, most_bytes);
    begin(state, 0, out);
    compress_all(state, fb::compress::bytes_of(data), out);
}

void fb_lz4_encoder_finish(void* encoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<encoder_t*>(encoder);
    output_t out(output, most_bytes);
    // A frame made at once says its size.
    begin(state, static_cast<std::uint64_t>(data->count), out);
    compress_all(state, fb::compress::bytes_of(data), out);
    const std::size_t made =
        check(LZ4F_compressEnd(state.context, state.room.data(), state.room.size(), nullptr));
    out.add_all(std::span(state.room).first(made));
    state.begun = false;
}

void fb_lz4_encoder_free(void* encoder) {
    auto* state = static_cast<encoder_t*>(encoder);
    LZ4F_freeCompressionContext(state->context);
    delete state;
}

void* fb_lz4_decoder_new(const std::int64_t limit) {
    LZ4F_dctx* context = nullptr;
    check(LZ4F_createDecompressionContext(&context, LZ4F_VERSION));
    return new decoder_t{ .context = context, .limit = limit };
}

std::int32_t fb_lz4_decoder_add(void* decoder, const fb_array_t* data, fb_array_t* output) {
    return decompress_all(*static_cast<decoder_t*>(decoder), fb::compress::bytes_of(data), output);
}

std::int32_t fb_lz4_decoder_finish(void* decoder, const fb_array_t* data, fb_array_t* output) {
    auto& state = *static_cast<decoder_t*>(decoder);
    std::int32_t result = decompress_all(state, fb::compress::bytes_of(data), output);
    if (result == RESULT_DONE && (!state.ended || !state.begun)) {
        result = state.failure.fail(RESULT_ENDS_TOO_SOON, "");
    }

    LZ4F_resetDecompressionContext(state.context);
    state.given = 0;
    state.ended = true;
    state.begun = false;
    state.failure.result = RESULT_DONE;
    return result;
}

fb_string_t* fb_lz4_decoder_message(const void* decoder) {
    return fb::compress::string_of(static_cast<const decoder_t*>(decoder)->failure.message);
}

void fb_lz4_decoder_free(void* decoder) {
    auto* state = static_cast<decoder_t*>(decoder);
    LZ4F_freeDecompressionContext(state->context);
    delete state;
}
}
