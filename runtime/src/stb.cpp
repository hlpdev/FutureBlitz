// std.image's PNG, JPEG, BMP, TGA, and HDR decoding over stb_image, and its JPEG, BMP, TGA, and
// HDR encoding over stb_image_write.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stb_image.h>
#include <stb_image_write.h>
#include <string>
#include <string_view>
#include <vector>

using namespace fb::image;

/// stb.c's stbi_info_from_memory for the format the data is, so why it fails is that format's.
extern "C" int fb_stbi_info_of(int format, const stbi_uc* data, int size, int* width, int* height,
                               int* channels);

namespace {

/// Frees what stb_image gives once it goes.
struct stb_free_t {
    void operator()(void* pixels) const {
        stbi_image_free(pixels);
    }
};

/// Why stb_image failed, as an image that failed: for what its message says it doesn't support,
/// like a BMP's run-length encoding or a JPEG's 12 bits, or what's too large, or else for data
/// that isn't valid, which is all stb_image tells apart.
image_t* stb_failed() {
    const char* reason = stbi_failure_reason();
    const std::string_view message = reason == nullptr ? "" : reason;
    if (message.contains("not supported") || message.contains("Unsupported")) {
        return failed(RESULT_UNSUPPORTED, std::string(message));
    }

    if (message.contains("too large") || message.contains("Very large")) {
        return failed(RESULT_TOO_LARGE, std::string(message));
    }
    return failed(RESULT_INVALID, std::string(message));
}

const char* name_of(const std::int32_t format) {
    switch (format) {
    case FORMAT_PNG:
        return "a PNG";
    case FORMAT_JPEG:
        return "a JPEG";
    case FORMAT_BMP:
        return "a BMP";
    case FORMAT_TGA:
        return "a TGA";
    default:
        return "an HDR image";
    }
}

image_t* decode(const fb_array_t* data, const std::int32_t format, const std::int64_t limit) {
    if (fb_image_format(data) != format) {
        return failed(RESULT_INVALID, std::string("it isn't ") + name_of(format));
    }

    // A BMP ends too soon when its file header, or the header after it, which says how long it
    // is, does: stb_image reads what's past the end as 0s, a header it doesn't support.
    const std::span<const std::uint8_t> all = bytes_of(data);
    if (format == FORMAT_BMP &&
        (all.size() < 18 ||
         all.size() < 14 + (little16_at(all, 14) | (std::size_t{ little16_at(all, 16) } << 16U)))) {
        return failed(RESULT_ENDS_TOO_SOON, "its headers end too soon");
    }

    // stb_image reads no BMP that's run-length encoded, and says it doesn't know the format. The
    // compression is in every header but OS/2's first, of 12 bytes, whose palette is there.
    if (format == FORMAT_BMP && all.size() >= 34 && little16_at(all, 14) != 12 &&
        (all[30] == 1 || all[30] == 2) && all[31] == 0 && all[32] == 0 && all[33] == 0) {
        return failed(RESULT_UNSUPPORTED, "it's run-length encoded, which stb_image doesn't read");
    }

    const auto* bytes = static_cast<const stbi_uc*>(data->data);
    const auto size = static_cast<int>(data->count);
    int width = 0;
    int height = 0;
    int channels = 0;
    if (fb_stbi_info_of(format, bytes, size, &width, &height, &channels) == 0) {
        return stb_failed();
    }

    if (image_t* over = over_limit(pixels_of(width, height), limit)) {
        return over;
    }

    auto* image = new image_t{ .width = width, .height = height, .delays = { 0 }, .plays = 1 };
    if (format == FORMAT_HDR) {
        const std::unique_ptr<float, stb_free_t> pixels(
            stbi_loadf_from_memory(bytes, size, &width, &height, &channels, 4));
        if (pixels == nullptr) {
            delete image;
            return stb_failed();
        }

        image->hdr.assign(pixels.get(), pixels.get() + (pixels_of(width, height) * 4));
        return image;
    }

    const std::unique_ptr<stbi_uc, stb_free_t> pixels(
        stbi_load_from_memory(bytes, size, &width, &height, &channels, 4));
    if (pixels == nullptr) {
        delete image;
        return stb_failed();
    }

    image->frames.emplace_back(pixels.get(), pixels.get() + (pixels_of(width, height) * 4));
    return image;
}

/// stb_image_write's writer, which appends to a vector. Its data isn't const, as stb_image_write
/// has it.
// NOLINTNEXTLINE(misc-const-correctness)
void append(void* context, void* data, const int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

extern "C" {

void* fb_stb_decode(const fb_array_t* data, const std::int32_t format, const std::int64_t limit) {
    return decode(data, format, limit);
}

void fb_stb_encode(const fb_array_t* pixels, const std::int64_t width, const std::int64_t height,
                   const std::int32_t format, const std::int32_t quality, fb_array_t* output) {
    std::vector<std::uint8_t> out;
    const auto w = static_cast<int>(width);
    const auto h = static_cast<int>(height);
    int written = 0;
    if (format == FORMAT_JPEG) {
        written = stbi_write_jpg_to_func(append, &out, w, h, 4, pixels->data, quality);
    } else if (format == FORMAT_BMP) {
        written = stbi_write_bmp_to_func(append, &out, w, h, 4, pixels->data);
    } else {
        written = stbi_write_tga_to_func(append, &out, w, h, 4, pixels->data);
    }

    if (written == 0) {
        fb_panic("std.image's encoding failed", nullptr);
    }
    set_elements<std::uint8_t>(output, out);
}

void fb_stb_encode_hdr(const fb_array_t* pixels, const std::int64_t width,
                       const std::int64_t height, fb_array_t* output) {
    // stb_image_write's RGBE takes channels from 0 to the most it holds, 255 times 2^119, and
    // what's outside that would be converted to a byte it can't be, which C leaves undefined; so
    // what's negative or NaN is 0, and what's past the most, the most.
    constexpr float most = 0x1.FEp126F;
    std::vector<float> values(floats_of(pixels).begin(), floats_of(pixels).end());
    for (float& value : values) {
        value = std::isnan(value) ? 0 : std::clamp(value, 0.0F, most);
    }

    std::vector<std::uint8_t> out;
    if (stbi_write_hdr_to_func(append, &out, static_cast<int>(width), static_cast<int>(height), 4,
                               values.data()) == 0) {
        fb_panic("std.image's HDR encoding failed", nullptr);
    }
    set_elements<std::uint8_t>(output, out);
}
}
