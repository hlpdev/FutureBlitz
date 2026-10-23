// A decoded image, which every decoder of std.image's gives, and the formats' signatures.

#include "fb/image.hpp"

#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace fb::image;

namespace {

const image_t& image_of(const void* image) {
    return *static_cast<const image_t*>(image);
}

/// Whether `bytes` start with `signature`.
bool starts_with(const std::span<const std::uint8_t> bytes, const std::string_view signature) {
    return bytes.size() >= signature.size() &&
           std::equal(signature.begin(), signature.end(), bytes.begin(),
                      [](const char a, const std::uint8_t b) {
                          return static_cast<std::uint8_t>(a) == b;
                      });
}

/// Whether `bytes` start as a TGA's header does: a color map type of 0 or 1, an image type of
/// one of the six stb_image reads, a color map of 15, 16, 24, or 32 bits when there's one, and
/// pixels of 8, 15, 16, 24, or 32 bits.
bool looks_like_tga(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 18) {
        return false;
    }

    const std::uint8_t map = bytes[1];
    const std::uint8_t type = bytes[2];
    const std::uint8_t map_bits = bytes[7];
    const std::uint8_t bits = bytes[16];
    const auto one_of = [](const std::uint8_t value, std::initializer_list<std::uint8_t> values) {
        return std::ranges::contains(values, value);
    };
    if (map > 1 || !one_of(type, { 1, 2, 3, 9, 10, 11 })) {
        return false;
    }

    if (map == 1 && !one_of(map_bits, { 15, 16, 24, 32 })) {
        return false;
    }

    return one_of(bits, { 8, 15, 16, 24, 32 }) && little16_at(bytes, 12) > 0 &&
           little16_at(bytes, 14) > 0;
}

} // namespace

extern "C" {

std::int32_t fb_image_result(const void* image) {
    return image_of(image).result;
}

fb_string_t* fb_image_message(const void* image) {
    const std::string& message = image_of(image).message;
    return fb_string_from_bytes(message.data(), static_cast<std::int64_t>(message.size()));
}

std::int64_t fb_image_width(const void* image) {
    return image_of(image).width;
}

std::int64_t fb_image_height(const void* image) {
    return image_of(image).height;
}

std::int64_t fb_image_frames(const void* image) {
    return static_cast<std::int64_t>(image_of(image).frames.size());
}

std::int32_t fb_image_delay(const void* image, const std::int64_t frame) {
    return image_of(image).delays[static_cast<std::size_t>(frame)];
}

std::int32_t fb_image_plays(const void* image) {
    return image_of(image).plays;
}

void fb_image_pixels(const void* image, const std::int64_t frame, fb_array_t* output) {
    const std::vector<std::uint8_t>& pixels =
        image_of(image).frames[static_cast<std::size_t>(frame)];
    set_elements<std::uint8_t>(output, pixels);
}

void fb_image_hdr_pixels(const void* image, fb_array_t* output) {
    set_elements<float>(output, image_of(image).hdr);
}

void fb_image_blank(fb_array_t* output, const std::int64_t count) {
    output->count = 0;
    if (count > output->capacity) {
        fb_array_reserve(output, 1, count, nullptr);
    }

    if (count > 0) {
        std::memset(output->data, 0, static_cast<std::size_t>(count));
    }
    output->count = count;
}

void fb_image_blank_hdr(fb_array_t* output, const std::int64_t count) {
    output->count = 0;
    if (count > output->capacity) {
        fb_array_reserve(output, sizeof(float), count, nullptr);
    }

    if (count > 0) {
        std::memset(output->data, 0, static_cast<std::size_t>(count) * sizeof(float));
    }
    output->count = count;
}

void fb_image_free(void* image) {
    delete static_cast<image_t*>(image);
}

std::int32_t fb_image_format(const fb_array_t* data) {
    const std::span<const std::uint8_t> bytes = bytes_of(data);
    if (starts_with(bytes, "\x89PNG\r\n\x1a\n")) {
        return FORMAT_PNG;
    }

    if (starts_with(bytes, "\xff\xd8\xff")) {
        return FORMAT_JPEG;
    }

    if (starts_with(bytes, "BM")) {
        return FORMAT_BMP;
    }

    if (starts_with(bytes, "GIF87a") || starts_with(bytes, "GIF89a")) {
        return FORMAT_GIF;
    }

    if (starts_with(bytes, "#?RADIANCE\n") || starts_with(bytes, "#?RGBE\n")) {
        return FORMAT_HDR;
    }

    if (starts_with(bytes, "qoif")) {
        return FORMAT_QOI;
    }

    if (starts_with(bytes, "RIFF") && bytes.size() >= 12 && starts_with(bytes.subspan(8), "WEBP")) {
        return FORMAT_WEBP;
    }

    return looks_like_tga(bytes) ? FORMAT_TGA : FORMAT_NONE;
}
}
