#pragma once

#include "fb/image.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the runtime's parts for std.image share, each over its library, so it's all inline: the
// image a decoder gives, the results, and how bytes go into the arrays std.image makes for them.
namespace fb::image {

/// What a decoder gives.
enum result_t : std::uint8_t {
    RESULT_DONE = 0,
    /// The bytes aren't the format, or are broken.
    RESULT_INVALID = 1,
    /// They stop before the format says they end.
    RESULT_ENDS_TOO_SOON = 2,
    /// Their pixels are more than the limit.
    RESULT_TOO_LARGE = 3,
    /// They're the format, but use what isn't supported.
    RESULT_UNSUPPORTED = 4,
};

/// The formats fb_image_format tells apart, as std.image numbers them.
enum format_t : std::uint8_t {
    FORMAT_NONE = 0,
    FORMAT_PNG = 1,
    FORMAT_JPEG = 2,
    FORMAT_BMP = 3,
    FORMAT_TGA = 4,
    FORMAT_GIF = 5,
    FORMAT_HDR = 6,
    FORMAT_QOI = 7,
    FORMAT_WEBP = 8,
};

/// The most elements an array holds, which an Int counts.
constexpr std::int64_t most_elements = std::numeric_limits<std::int32_t>::max();

/// A decoded image: its frames' RGBA bytes, each width * height * 4 of them, or an HDR image's
/// floats, or why there are none.
struct image_t {
    std::int32_t result = RESULT_DONE;
    std::string message;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::int32_t> delays;
    std::int32_t plays = 0;
    std::vector<float> hdr;
};

/// An image that failed, for `result`, saying why.
inline image_t* failed(const result_t result, std::string message) {
    return new image_t{ .result = result, .message = std::move(message) };
}

/// The image that failed for being over `limit`, of `pixels`, or null when it isn't.
inline image_t* over_limit(const std::int64_t pixels, const std::int64_t limit) {
    if (pixels <= limit) {
        return nullptr;
    }

    return failed(RESULT_TOO_LARGE, "its " + std::to_string(pixels) + " pixels are more than " +
                                        std::to_string(limit));
}

/// The pixels of `width` by `height` frames, `frames` of them, or the most an Int64 holds when
/// that's more.
inline std::int64_t pixels_of(const std::int64_t width, const std::int64_t height,
                              const std::int64_t frames = 1) {
    constexpr std::int64_t most = std::numeric_limits<std::int64_t>::max();
    if (width <= 0 || height <= 0 || frames <= 0) {
        return 0;
    }

    if (width > most / height || width * height > most / frames) {
        return most;
    }

    return width * height * frames;
}

inline std::span<const std::uint8_t> bytes_of(const fb_array_t* array) {
    return { static_cast<const std::uint8_t*>(array->data),
             static_cast<std::size_t>(array->count) };
}

/// The bytes of an image's `width` by `height` pixels at `pixels`, 4 each, as an Image's
/// Buffer<UInt8> holds them.
inline std::span<const std::uint8_t> pixels_at(const std::uint8_t* pixels, const std::int64_t width,
                                               const std::int64_t height) {
    return { pixels, static_cast<std::size_t>(width * height * 4) };
}

inline std::string_view text_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

/// Puts `values` in `array`, an array of T, in place of what it had. Panics when they're more
/// than an array holds, which only an encoder's output can be.
template <typename T>
void set_elements(fb_array_t* array, const std::span<const T> values) {
    if (std::cmp_greater(values.size(), most_elements)) {
        fb_panic("the encoded bytes are more than a UInt8[] holds", nullptr);
    }

    const auto count = static_cast<std::int64_t>(values.size());
    array->count = 0;
    if (count > array->capacity) {
        fb_array_reserve(array, sizeof(T), count, nullptr);
    }

    if (count > 0) {
        std::memcpy(array->data, values.data(), values.size() * sizeof(T));
    }

    array->count = count;
}

/// Appends `value` to `bytes` as `size` bytes, the most significant first.
inline void put_big(std::vector<std::uint8_t>& bytes, const std::uint32_t value, const int size) {
    for (int shift = (size - 1) * 8; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

/// Appends `value` to `bytes` as `size` bytes, the least significant first.
inline void put_little(std::vector<std::uint8_t>& bytes, const std::uint32_t value,
                       const int size) {
    for (int shift = 0; shift < size * 8; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

/// The 4 bytes at `at`, the most significant first.
inline std::uint32_t big_at(const std::span<const std::uint8_t> bytes, const std::size_t at) {
    return (std::uint32_t{ bytes[at] } << 24U) | (std::uint32_t{ bytes[at + 1] } << 16U) |
           (std::uint32_t{ bytes[at + 2] } << 8U) | std::uint32_t{ bytes[at + 3] };
}

/// The 2 bytes at `at`, the least significant first.
inline std::uint32_t little16_at(const std::span<const std::uint8_t> bytes, const std::size_t at) {
    return std::uint32_t{ bytes[at] } | (std::uint32_t{ bytes[at + 1] } << 8U);
}

/// Draws the RGBA pixel at `from` over the one at `to`, by straight alpha's "over", in 255ths:
/// the result's alpha is a + b(1 - a), and its color the two colors by their alphas, rounded.
inline void over(std::uint8_t* to, const std::uint8_t* from) {
    const std::uint32_t alpha = from[3];
    if (alpha == 255) {
        std::memcpy(to, from, 4);
        return;
    }

    if (alpha == 0) {
        return;
    }

    const std::uint32_t under = to[3] * (255 - alpha);
    const std::uint32_t total = (alpha * 255) + under;
    for (std::size_t c = 0; c < 3; ++c) {
        const std::uint32_t mixed = (from[c] * alpha * 255) + (to[c] * under);
        to[c] = static_cast<std::uint8_t>((mixed + (total / 2)) / total);
    }
    to[3] = static_cast<std::uint8_t>((total + 127) / 255);
}

/// PNG's CRC-32, of the ISO 3309 polynomial, as zlib's crc32 gives it, for the parts that don't
/// link zlib-ng.
inline std::uint32_t crc32(std::uint32_t crc, const std::span<const std::uint8_t> bytes) {
    static constexpr std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xedb88320U ^ (c >> 1U) : c >> 1U;
            }
            values[n] = c;
        }
        return values;
    }();

    crc = ~crc;
    for (const std::uint8_t byte : bytes) {
        crc = table[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
    }
    return ~crc;
}

} // namespace fb::image
