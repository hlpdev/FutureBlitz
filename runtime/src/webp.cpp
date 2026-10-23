// std.image's WebP over libwebp: still images, lossy and lossless, decoded to RGBA, and encoded
// from it, lossy at a quality or lossless at an effort.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <webp/decode.h>
#include <webp/encode.h>

using namespace fb::image;

namespace {

/// The image that failed for libwebp's `status`.
image_t* webp_failed(const VP8StatusCode status) {
    switch (status) {
    case VP8_STATUS_NOT_ENOUGH_DATA:
        return failed(RESULT_ENDS_TOO_SOON, "it ends too soon");
    case VP8_STATUS_UNSUPPORTED_FEATURE:
        return failed(RESULT_UNSUPPORTED, "it uses a feature libwebp doesn't support");
    case VP8_STATUS_OUT_OF_MEMORY:
        return failed(RESULT_TOO_LARGE, "libwebp ran out of memory for it");
    default:
        return failed(RESULT_INVALID, "its bitstream is broken");
    }
}

image_t* decode(const std::span<const std::uint8_t> bytes, const std::int64_t limit) {
    WebPBitstreamFeatures features{};
    const VP8StatusCode status = WebPGetFeatures(bytes.data(), bytes.size(), &features);
    if (status != VP8_STATUS_OK) {
        return webp_failed(status);
    }

    if (features.has_animation != 0) {
        return failed(RESULT_UNSUPPORTED, "it's animated, which std.image doesn't read");
    }

    const std::int64_t pixels = pixels_of(features.width, features.height);
    if (image_t* over = over_limit(pixels, limit)) {
        return over;
    }

    auto* image = new image_t{
        .width = features.width,
        .height = features.height,
        .delays = { 0 },
        .plays = 1,
    };
    std::vector<std::uint8_t>& out =
        image->frames.emplace_back(static_cast<std::size_t>(pixels) * 4);
    if (WebPDecodeRGBAInto(bytes.data(), bytes.size(), out.data(), out.size(),
                           features.width * 4) == nullptr) {
        delete image;
        // Decoding into memory that's there fails only for the bitstream, which may end too soon.
        WebPDecoderConfig config{};
        if (WebPInitDecoderConfig(&config) != 0) {
            const VP8StatusCode decoded = WebPDecode(bytes.data(), bytes.size(), &config);
            WebPFreeDecBuffer(&config.output);
            if (decoded != VP8_STATUS_OK) {
                return webp_failed(decoded);
            }
        }
        return failed(RESULT_INVALID, "its bitstream is broken");
    }
    return image;
}

std::vector<std::uint8_t> encode(const fb_array_t* pixels, const std::int64_t width,
                                 const std::int64_t height, const std::int32_t quality,
                                 const bool lossless) {
    WebPConfig config{};
    WebPPicture picture{};
    if (WebPConfigInit(&config) == 0 || WebPPictureInit(&picture) == 0) {
        fb_panic("std.image's libwebp isn't the version it was built for", nullptr);
    }

    if (lossless) {
        if (WebPConfigLosslessPreset(&config, quality) == 0) {
            fb_panic("std.image's WebP effort is out of range", nullptr);
        }
        // The colors under alpha 0 stay, as a lossless image's should.
        config.exact = 1;
    } else {
        config.quality = static_cast<float>(quality);
    }

    picture.use_argb = lossless ? 1 : 0;
    picture.width = static_cast<int>(width);
    picture.height = static_cast<int>(height);
    WebPMemoryWriter writer{};
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    const bool imported =
        WebPPictureImportRGBA(&picture, static_cast<const std::uint8_t*>(pixels->data),
                              picture.width * 4) != 0;
    const bool encoded = imported && WebPEncode(&config, &picture) != 0;
    WebPPictureFree(&picture);
    if (!encoded) {
        WebPMemoryWriterClear(&writer);
        fb_panic("out of memory: std.image's WebP encoding failed", nullptr);
    }

    std::vector<std::uint8_t> out(writer.mem, writer.mem + writer.size);
    WebPMemoryWriterClear(&writer);
    return out;
}

} // namespace

extern "C" {

void* fb_webp_decode(const fb_array_t* data, const std::int64_t limit) {
    return decode(bytes_of(data), limit);
}

void fb_webp_encode(const fb_array_t* pixels, const std::int64_t width, const std::int64_t height,
                    const std::int32_t quality, const std::int32_t lossless, fb_array_t* output) {
    set_elements<std::uint8_t>(output, encode(pixels, width, height, quality, lossless != 0));
}
}
