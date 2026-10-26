// std.image's transforms of an image's RGBA pixels: resizing by a filter, as Pillow resamples, in
// two passes, across and then down, each pixel a weighted sum of the ones under its filter, which
// widens as the image shrinks, its colors weighted by their alpha; cropping; flips and quarter
// turns; and drawing one image on another.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <vector>

using namespace fb::image;

namespace {

enum filter_t : std::uint8_t {
    FILTER_NEAREST = 0,
    FILTER_BOX = 1,
    FILTER_BILINEAR = 2,
    FILTER_BICUBIC = 3,
    FILTER_LANCZOS = 4,
};

enum orientation_t : std::uint8_t {
    FLIP_HORIZONTAL = 0,
    FLIP_VERTICAL = 1,
    ROTATE_RIGHT = 2,
    ROTATE_LEFT = 3,
    ROTATE_HALF = 4,
};

/// How far a filter reaches either side of a pixel, in pixels of the larger image.
double support_of(const std::int32_t filter) {
    switch (filter) {
    case FILTER_BOX:
        return 0.5;
    case FILTER_BILINEAR:
        return 1;
    case FILTER_BICUBIC:
        return 2;
    default:
        return 3;
    }
}

/// sin(pi * x), by a series of its own rather than the C library's sin, which can differ by a
/// little from one OS to another, so every target gives the same weights: x less its nearest
/// integer n is from -1/2 to 1/2, where the series to its 19th power is within 3e-16, and
/// sin(pi * x) is that sine, negated when n is odd.
double sin_pi(const double x) {
    const double whole = std::round(x);
    const double angle = (x - whole) * std::numbers::pi;
    const double squared = angle * angle;
    double sum = 0;
    for (int power = 19; power > 1; power -= 2) {
        sum = (1 - sum) * squared / (power * (power - 1));
    }

    const double sine = angle * (1 - sum);
    return std::fmod(whole, 2) == 0 ? sine : -sine;
}

double sinc(const double x) {
    if (x == 0) {
        return 1;
    }

    return sin_pi(x) / (x * std::numbers::pi);
}

/// A filter's weight at `x` pixels from the middle: a box's, a triangle's, Keys's cubic with an
/// `a` of -0.5, or Lanczos's of 3 lobes, as Pillow's are.
double weight_of(const std::int32_t filter, const double x) {
    const double distance = std::abs(x);
    switch (filter) {
    case FILTER_BOX:
        return x > -0.5 && x <= 0.5 ? 1 : 0;
    case FILTER_BILINEAR:
        return distance < 1 ? 1 - distance : 0;
    case FILTER_BICUBIC: {
        constexpr double a = -0.5;
        if (distance < 1) {
            return ((((a + 2) * distance) - (a + 3)) * distance * distance) + 1;
        }

        return distance < 2 ? (((((distance - 5) * distance) + 8) * distance) - 4) * a : 0;
    }
    default:
        return distance < 3 ? sinc(x) * sinc(x / 3) : 0;
    }
}

/// The pixels of a line of `size` each pixel of a line of `count` takes: the first, and each one's
/// weight, which add up to 1.
struct taps_t {
    std::vector<std::int64_t> first;
    std::vector<std::vector<double>> weights;
};

taps_t taps_of(const std::int32_t filter, const std::int64_t size, const std::int64_t count) {
    const double scale = static_cast<double>(size) / static_cast<double>(count);
    const double widened = std::max(scale, 1.0);
    const double support = support_of(filter) * widened;
    taps_t taps;
    for (std::int64_t i = 0; i < count; ++i) {
        // The filter's first and last pixels, rounded as Pillow rounds them.
        const double center = (static_cast<double>(i) + 0.5) * scale;
        const auto first = std::max<std::int64_t>(
            static_cast<std::int64_t>(std::floor(center - support + 0.5)), 0);
        const auto last = std::min<std::int64_t>(
            static_cast<std::int64_t>(std::floor(center + support + 0.5)), size);
        std::vector<double> weights;
        double total = 0;
        for (std::int64_t at = first; at < last; ++at) {
            const double weight =
                weight_of(filter, (static_cast<double>(at) - center + 0.5) / widened);
            weights.push_back(weight);
            total += weight;
        }

        if (total != 0) {
            for (double& weight : weights) {
                weight /= total;
            }
        }
        taps.first.push_back(first);
        taps.weights.push_back(std::move(weights));
    }
    return taps;
}

/// A channel's value, rounded and kept from 0 to 255.
std::uint8_t byte_of(const double value) {
    return static_cast<std::uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
}

std::vector<std::uint8_t> resized(const std::span<const std::uint8_t> rgba,
                                  const std::int64_t width, const std::int64_t height,
                                  const std::int64_t new_width, const std::int64_t new_height,
                                  const std::int32_t filter) {
    const auto columns = static_cast<std::size_t>(new_width);
    const auto rows = static_cast<std::size_t>(new_height);
    // An image of no pixels has none to work out, whatever its sides are.
    if (columns == 0 || rows == 0) {
        return {};
    }

    std::vector<std::uint8_t> out(columns * rows * 4);
    if (filter == FILTER_NEAREST) {
        const double x_scale = static_cast<double>(width) / static_cast<double>(new_width);
        const double y_scale = static_cast<double>(height) / static_cast<double>(new_height);
        for (std::size_t y = 0; y < rows; ++y) {
            const auto from_y = static_cast<std::size_t>((static_cast<double>(y) + 0.5) * y_scale);
            for (std::size_t x = 0; x < columns; ++x) {
                const auto from_x =
                    static_cast<std::size_t>((static_cast<double>(x) + 0.5) * x_scale);
                const std::size_t from = ((from_y * static_cast<std::size_t>(width)) + from_x) * 4;
                std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(from), 4,
                            out.begin() + static_cast<std::ptrdiff_t>(((y * columns) + x) * 4));
            }
        }
        return out;
    }

    // Across, then down, in doubles, the colors weighted by their alpha, so a transparent pixel's
    // color spreads to none of its neighbors'. What's between the passes is kept in floats, whose
    // 24 bits hold it to within a 100000th of a byte, at 16 bytes a pixel.
    const auto source_columns = static_cast<std::size_t>(width);
    const auto source_rows = static_cast<std::size_t>(height);
    const taps_t across = taps_of(filter, width, new_width);
    std::vector<float> wide(columns * source_rows * 4);
    for (std::size_t y = 0; y < source_rows; ++y) {
        for (std::size_t x = 0; x < columns; ++x) {
            const std::vector<double>& weights = across.weights[x];
            std::array<double, 4> sum{};
            for (std::size_t k = 0; k < weights.size(); ++k) {
                const std::size_t from =
                    ((y * source_columns) + static_cast<std::size_t>(across.first[x]) + k) * 4;
                const double alpha = rgba[from + 3] / 255.0;
                for (std::size_t c = 0; c < 3; ++c) {
                    sum[c] += rgba[from + c] * alpha * weights[k];
                }
                sum[3] += rgba[from + 3] * weights[k];
            }

            // Kept from 0 to 255 between the passes, as Pillow's are, so a filter's ringing
            // past black or white across isn't made more of down.
            for (std::size_t c = 0; c < 4; ++c) {
                wide[(((y * columns) + x) * 4) + c] =
                    static_cast<float>(std::clamp(sum[c], 0.0, 255.0));
            }
        }
    }

    const taps_t down = taps_of(filter, height, new_height);
    for (std::size_t y = 0; y < rows; ++y) {
        const std::vector<double>& weights = down.weights[y];
        for (std::size_t x = 0; x < columns; ++x) {
            std::array<double, 4> sum{};
            for (std::size_t k = 0; k < weights.size(); ++k) {
                const std::size_t from =
                    (((static_cast<std::size_t>(down.first[y]) + k) * columns) + x) * 4;
                for (std::size_t c = 0; c < 4; ++c) {
                    sum[c] += wide[from + c] * weights[k];
                }
            }

            const std::size_t to = ((y * columns) + x) * 4;
            const std::uint8_t alpha = byte_of(sum[3]);
            out[to + 3] = alpha;
            for (std::size_t c = 0; c < 3; ++c) {
                out[to + c] = alpha == 0 ? 0 : byte_of(sum[c] * 255.0 / sum[3]);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> oriented(const std::span<const std::uint8_t> rgba,
                                   const std::int64_t width, const std::int64_t height,
                                   const std::int32_t how) {
    const auto columns = static_cast<std::size_t>(width);
    const auto rows = static_cast<std::size_t>(height);
    if (rgba.empty()) {
        return {};
    }

    std::vector<std::uint8_t> out(rgba.size());
    // A quarter turn's image is `rows` wide.
    const bool turned = how == ROTATE_RIGHT || how == ROTATE_LEFT;
    const std::size_t out_columns = turned ? rows : columns;
    for (std::size_t y = 0; y < rows; ++y) {
        for (std::size_t x = 0; x < columns; ++x) {
            std::size_t to_x = x;
            std::size_t to_y = y;
            switch (how) {
            case FLIP_HORIZONTAL:
                to_x = columns - 1 - x;
                break;
            case FLIP_VERTICAL:
                to_y = rows - 1 - y;
                break;
            case ROTATE_RIGHT:
                to_x = rows - 1 - y;
                to_y = x;
                break;
            case ROTATE_LEFT:
                to_x = y;
                to_y = columns - 1 - x;
                break;
            default:
                to_x = columns - 1 - x;
                to_y = rows - 1 - y;
                break;
            }

            std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(((y * columns) + x) * 4), 4,
                        out.begin() +
                            static_cast<std::ptrdiff_t>(((to_y * out_columns) + to_x) * 4));
        }
    }
    return out;
}

} // namespace

extern "C" {

void fb_image_resize(const fb_array_t* pixels, const std::int64_t width, const std::int64_t height,
                     const std::int64_t new_width, const std::int64_t new_height,
                     const std::int32_t filter, fb_array_t* output) {
    set_elements<std::uint8_t>(
        output, resized(bytes_of(pixels), width, height, new_width, new_height, filter));
}

void fb_image_crop(const fb_array_t* pixels, const std::int64_t width, const std::int64_t x,
                   const std::int64_t y, const std::int64_t crop_width,
                   const std::int64_t crop_height, fb_array_t* output) {
    const std::span<const std::uint8_t> rgba = bytes_of(pixels);
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(crop_width * crop_height * 4));
    // A crop of no pixels has no rows to copy, however many it says it has.
    for (std::int64_t row = y; crop_width > 0 && row < y + crop_height; ++row) {
        const auto from = static_cast<std::ptrdiff_t>(((row * width) + x) * 4);
        out.insert(out.end(), rgba.begin() + from, rgba.begin() + from + (crop_width * 4));
    }
    set_elements<std::uint8_t>(output, out);
}

void fb_image_orient(const fb_array_t* pixels, const std::int64_t width, const std::int64_t height,
                     const std::int32_t how, fb_array_t* output) {
    set_elements<std::uint8_t>(output, oriented(bytes_of(pixels), width, height, how));
}

void fb_image_blit(const fb_array_t* target, const std::int64_t width, const std::int64_t height,
                   const fb_array_t* source, const std::int64_t source_width,
                   const std::int64_t source_height, const std::int64_t x, const std::int64_t y,
                   const std::int32_t blend) {
    auto* to = static_cast<std::uint8_t*>(target->data);
    const auto* from = static_cast<const std::uint8_t*>(source->data);
    // An image drawn on itself is drawn from a copy, so no pixel is read after it's drawn on.
    std::vector<std::uint8_t> copy;
    if (from == to) {
        copy.assign(from, from + target->count);
        from = copy.data();
    }

    const std::int64_t left = std::max<std::int64_t>(x, 0);
    const std::int64_t top = std::max<std::int64_t>(y, 0);
    const std::int64_t right = std::min(width, x + source_width);
    const std::int64_t bottom = std::min(height, y + source_height);
    for (std::int64_t row = top; row < bottom; ++row) {
        for (std::int64_t column = left; column < right; ++column) {
            std::uint8_t* pixel = to + (((row * width) + column) * 4);
            const std::uint8_t* drawn = from + ((((row - y) * source_width) + column - x) * 4);
            if (blend != 0) {
                over(pixel, drawn);
            } else {
                std::memcpy(pixel, drawn, 4);
            }
        }
    }
}

void fb_image_fill(const fb_array_t* target, const std::uint32_t rgba) {
    auto* bytes = static_cast<std::uint8_t*>(target->data);
    const std::array<std::uint8_t, 4> color{
        static_cast<std::uint8_t>(rgba >> 24U),
        static_cast<std::uint8_t>(rgba >> 16U),
        static_cast<std::uint8_t>(rgba >> 8U),
        static_cast<std::uint8_t>(rgba),
    };
    for (std::int64_t at = 0; at < target->count; at += 4) {
        std::memcpy(bytes + at, color.data(), 4);
    }
}
}
