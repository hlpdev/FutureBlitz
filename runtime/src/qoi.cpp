// QOI, the Quite OK Image format, as qoiformat.org's specification has it: a 14-byte header,
// then the pixels as ops, each pixel the last one again (a run), one seen before (by a hash of
// its value), a small difference from the last one, or its whole value, then an end of seven 0
// bytes and a 1.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace fb::image;

namespace {

constexpr std::size_t header_size = 14;
constexpr std::array<std::uint8_t, 8> end_marker{ 0, 0, 0, 0, 0, 0, 0, 1 };
constexpr std::uint8_t op_index = 0x00;
constexpr std::uint8_t op_diff = 0x40;
constexpr std::uint8_t op_luma = 0x80;
constexpr std::uint8_t op_run = 0xc0;
constexpr std::uint8_t op_rgb = 0xfe;
constexpr std::uint8_t op_rgba = 0xff;
constexpr std::uint8_t op_mask = 0xc0;
/// The most pixels one op gives: a run's.
constexpr std::int64_t longest_run = 62;

struct pixel_t {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;

    bool operator==(const pixel_t&) const = default;
};

/// Where a pixel goes in the 64 seen before.
std::size_t hash_of(const pixel_t& pixel) {
    return ((std::size_t{ pixel.r } * 3) + (std::size_t{ pixel.g } * 5) +
            (std::size_t{ pixel.b } * 7) + (std::size_t{ pixel.a } * 11)) %
           64;
}

/// A difference plus its bias, which an op's bits hold.
unsigned biased(const int difference, const int bias) {
    return static_cast<unsigned>(difference + bias);
}

/// The difference from `from` to `to`, as a byte's wraps it, from -128 to 127.
int difference(const std::uint8_t to, const std::uint8_t from) {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(to - from));
}

image_t* decode(const std::span<const std::uint8_t> bytes, const std::int64_t limit) {
    if (bytes.size() < header_size) {
        return failed(RESULT_ENDS_TOO_SOON, "its header ends too soon");
    }

    if (bytes[0] != 'q' || bytes[1] != 'o' || bytes[2] != 'i' || bytes[3] != 'f') {
        return failed(RESULT_INVALID, "it doesn't start with \"qoif\"");
    }

    const std::int64_t width = big_at(bytes, 4);
    const std::int64_t height = big_at(bytes, 8);
    const std::uint8_t channels = bytes[12];
    const std::uint8_t colorspace = bytes[13];
    if (width == 0 || height == 0 || (channels != 3 && channels != 4) || colorspace > 1) {
        return failed(RESULT_INVALID, "its header has a size of 0, or channels or a colorspace "
                                      "QOI doesn't have");
    }

    const std::int64_t pixels = pixels_of(width, height);
    if (image_t* over = over_limit(pixels, limit)) {
        return over;
    }

    // The ops go up to the end marker, and each gives 62 pixels at most, so ops too few for the
    // pixels take no memory first.
    const std::size_t end = bytes.size() >= header_size + end_marker.size()
                                ? bytes.size() - end_marker.size()
                                : header_size;
    if (pixels > static_cast<std::int64_t>(end - header_size) * longest_run) {
        return failed(RESULT_ENDS_TOO_SOON, "its pixels end too soon");
    }

    auto* image = new image_t{ .width = width, .height = height, .delays = { 0 }, .plays = 1 };
    std::vector<std::uint8_t>& out = image->frames.emplace_back();
    out.reserve(static_cast<std::size_t>(pixels) * 4);
    std::array<pixel_t, 64> seen{};
    pixel_t pixel{ .a = 255 };
    std::size_t at = header_size;
    std::int64_t run = 0;
    for (std::int64_t i = 0; i < pixels; ++i) {
        if (run > 0) {
            --run;
        } else {
            if (at >= end) {
                delete image;
                return failed(RESULT_ENDS_TOO_SOON, "its pixels end too soon");
            }

            const std::uint8_t op = bytes[at++];
            if (op == op_rgb || op == op_rgba) {
                const std::size_t size = op == op_rgb ? 3 : 4;
                if (at + size > end) {
                    delete image;
                    return failed(RESULT_ENDS_TOO_SOON, "its pixels end too soon");
                }

                pixel.r = bytes[at];
                pixel.g = bytes[at + 1];
                pixel.b = bytes[at + 2];
                if (op == op_rgba) {
                    pixel.a = bytes[at + 3];
                }
                at += size;
            } else if ((op & op_mask) == op_index) {
                pixel = seen[op & 0x3fU];
            } else if ((op & op_mask) == op_diff) {
                const unsigned diff = op;
                pixel.r =
                    static_cast<std::uint8_t>(pixel.r + static_cast<int>((diff >> 4U) & 3U) - 2);
                pixel.g =
                    static_cast<std::uint8_t>(pixel.g + static_cast<int>((diff >> 2U) & 3U) - 2);
                pixel.b = static_cast<std::uint8_t>(pixel.b + static_cast<int>(diff & 3U) - 2);
            } else if ((op & op_mask) == op_luma) {
                if (at >= end) {
                    delete image;
                    return failed(RESULT_ENDS_TOO_SOON, "its pixels end too soon");
                }

                const unsigned next = bytes[at++];
                const int green = static_cast<int>(op & 0x3fU) - 32;
                const int red = green - 8 + static_cast<int>((next >> 4U) & 0x0fU);
                const int blue = green - 8 + static_cast<int>(next & 0x0fU);
                pixel.r = static_cast<std::uint8_t>(pixel.r + red);
                pixel.g = static_cast<std::uint8_t>(pixel.g + green);
                pixel.b = static_cast<std::uint8_t>(pixel.b + blue);
            } else {
                run = op & 0x3fU;
            }

            seen[hash_of(pixel)] = pixel;
        }

        // The header's channels only say what the pixels are, as the specification has it, so
        // a 3-channel image's alpha is what its ops give, 255 unless one gives another.
        out.push_back(pixel.r);
        out.push_back(pixel.g);
        out.push_back(pixel.b);
        out.push_back(pixel.a);
    }

    return image;
}

std::vector<std::uint8_t> encode(const std::span<const std::uint8_t> rgba, const std::int64_t width,
                                 const std::int64_t height) {
    std::vector<std::uint8_t> out{ 'q', 'o', 'i', 'f' };
    put_big(out, static_cast<std::uint32_t>(width), 4);
    put_big(out, static_cast<std::uint32_t>(height), 4);
    out.push_back(4);
    out.push_back(0);
    std::array<pixel_t, 64> seen{};
    pixel_t last{ .a = 255 };
    int run = 0;
    const std::size_t count = rgba.size() / 4;
    for (std::size_t i = 0; i < count; ++i) {
        const pixel_t pixel{
            .r = rgba[i * 4],
            .g = rgba[(i * 4) + 1],
            .b = rgba[(i * 4) + 2],
            .a = rgba[(i * 4) + 3],
        };
        if (pixel == last) {
            ++run;
            if (run == longest_run || i + 1 == count) {
                out.push_back(static_cast<std::uint8_t>(op_run | static_cast<unsigned>(run - 1)));
                run = 0;
            }
            continue;
        }

        if (run > 0) {
            out.push_back(static_cast<std::uint8_t>(op_run | static_cast<unsigned>(run - 1)));
            run = 0;
        }

        const std::size_t hash = hash_of(pixel);
        if (seen[hash] == pixel) {
            out.push_back(static_cast<std::uint8_t>(op_index | hash));
        } else {
            seen[hash] = pixel;
            if (pixel.a == last.a) {
                const int red = difference(pixel.r, last.r);
                const int green = difference(pixel.g, last.g);
                const int blue = difference(pixel.b, last.b);
                const int red_green = red - green;
                const int blue_green = blue - green;
                if (red >= -2 && red <= 1 && green >= -2 && green <= 1 && blue >= -2 && blue <= 1) {
                    out.push_back(static_cast<std::uint8_t>(op_diff | (biased(red, 2) << 4U) |
                                                            (biased(green, 2) << 2U) |
                                                            biased(blue, 2)));
                } else if (green >= -32 && green <= 31 && red_green >= -8 && red_green <= 7 &&
                           blue_green >= -8 && blue_green <= 7) {
                    out.push_back(static_cast<std::uint8_t>(op_luma | biased(green, 32)));
                    out.push_back(static_cast<std::uint8_t>((biased(red_green, 8) << 4U) |
                                                            biased(blue_green, 8)));
                } else {
                    out.insert(out.end(), { op_rgb, pixel.r, pixel.g, pixel.b });
                }
            } else {
                out.insert(out.end(), { op_rgba, pixel.r, pixel.g, pixel.b, pixel.a });
            }
        }

        last = pixel;
    }

    out.insert(out.end(), end_marker.begin(), end_marker.end());
    return out;
}

} // namespace

extern "C" {

void* fb_image_qoi_decode(const fb_array_t* data, const std::int64_t limit) {
    return decode(bytes_of(data), limit);
}

void fb_image_qoi_encode(const fb_array_t* pixels, const std::int64_t width,
                         const std::int64_t height, fb_array_t* output) {
    set_elements<std::uint8_t>(output, encode(bytes_of(pixels), width, height));
}
}
