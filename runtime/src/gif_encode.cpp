// GIF's encoder: GIF89a, each frame the whole canvas, with a palette of its own and its indexes
// LZW-compressed, as GIF's specification has it.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace fb::image;

namespace {

/// How many colors a palette holds.
constexpr std::size_t palette_size = 256;
/// The alpha below which a pixel is transparent.
constexpr std::uint8_t opaque_from = 128;
/// LZW's codes are 12 bits at most.
constexpr std::uint32_t most_code = 4095;

/// A frame as GIF has it: its palette, each pixel's index in it, and which index is transparent,
/// if one is.
struct indexed_t {
    std::vector<std::uint32_t> palette;
    std::vector<std::uint8_t> indexes;
    int transparent = -1;
};

/// A color as 0xRRGGBB.
std::uint32_t rgb_of(const std::span<const std::uint8_t> rgba, const std::size_t pixel) {
    return (std::uint32_t{ rgba[pixel * 4] } << 16U) |
           (std::uint32_t{ rgba[(pixel * 4) + 1] } << 8U) | std::uint32_t{ rgba[(pixel * 4) + 2] };
}

int channel(const std::uint32_t rgb, const std::size_t index) {
    return static_cast<int>((rgb >> (16 - (index * 8))) & 0xffU);
}

/// A color and how many pixels have it.
struct counted_t {
    std::uint32_t rgb = 0;
    std::int64_t count = 0;
};

/// The colors from `first` to `last` of a histogram, which median cut splits.
struct box_t {
    std::size_t first = 0;
    std::size_t last = 0;
    std::int64_t count = 0;
};

/// The channel along which the box's colors spread most, red first in a tie, and how far.
std::pair<std::size_t, int> widest(const std::span<const counted_t> colors, const box_t& box) {
    std::array<int, 3> least{ 255, 255, 255 };
    std::array<int, 3> most{ 0, 0, 0 };
    for (std::size_t i = box.first; i < box.last; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            least[c] = std::min(least[c], channel(colors[i].rgb, c));
            most[c] = std::max(most[c], channel(colors[i].rgb, c));
        }
    }

    std::size_t axis = 0;
    for (std::size_t c = 1; c < 3; ++c) {
        if (most[c] - least[c] > most[axis] - least[axis]) {
            axis = c;
        }
    }
    return { axis, most[axis] - least[axis] };
}

/// Median cut: `colors`, a histogram, split into `count` boxes or fewer, the box whose pixels
/// times its widest spread is largest split each time, at its pixels' median along that spread,
/// and each box's color the average of its pixels'.
std::vector<std::uint32_t> median_cut(std::vector<counted_t> colors, const std::size_t count) {
    std::vector<box_t> boxes{ { .first = 0, .last = colors.size(), .count = 0 } };
    for (const counted_t& color : colors) {
        boxes[0].count += color.count;
    }

    while (boxes.size() < count) {
        std::size_t chosen = boxes.size();
        std::int64_t best = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].last - boxes[i].first < 2) {
                continue;
            }

            const std::int64_t score = boxes[i].count * widest(colors, boxes[i]).second;
            if (chosen == boxes.size() || score > best) {
                chosen = i;
                best = score;
            }
        }

        if (chosen == boxes.size()) {
            break;
        }

        const box_t box = boxes[chosen];
        const std::size_t axis = widest(colors, box).first;
        const auto begin = colors.begin() + static_cast<std::ptrdiff_t>(box.first);
        const auto end = colors.begin() + static_cast<std::ptrdiff_t>(box.last);
        std::sort(begin, end, [axis](const counted_t& a, const counted_t& b) {
            const int first = channel(a.rgb, axis);
            const int second = channel(b.rgb, axis);
            return first != second ? first < second : a.rgb < b.rgb;
        });

        // The first color past half the pixels starts the second box, which keeps at least one.
        std::int64_t seen = 0;
        std::size_t split = box.first + 1;
        for (std::size_t i = box.first; i + 1 < box.last; ++i) {
            seen += colors[i].count;
            split = i + 1;
            if (seen * 2 >= box.count) {
                break;
            }
        }

        std::int64_t before = 0;
        for (std::size_t i = box.first; i < split; ++i) {
            before += colors[i].count;
        }

        boxes[chosen] = { .first = box.first, .last = split, .count = before };
        boxes.push_back({ .first = split, .last = box.last, .count = box.count - before });
    }

    std::vector<std::uint32_t> palette;
    for (const box_t& box : boxes) {
        std::array<std::int64_t, 3> sums{};
        for (std::size_t i = box.first; i < box.last; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                sums[c] += channel(colors[i].rgb, c) * colors[i].count;
            }
        }

        std::uint32_t rgb = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const std::int64_t average = (sums[c] + (box.count / 2)) / box.count;
            rgb = (rgb << 8U) | static_cast<std::uint32_t>(average);
        }
        palette.push_back(rgb);
    }
    return palette;
}

/// The palette's color nearest `rgb`, by the squares of the channels' differences, the first of
/// the nearest in a tie, through a cache of the colors asked for before.
class nearest_t {
public:
    explicit nearest_t(const std::span<const std::uint32_t> palette) : m_palette(palette) {
        m_keys.fill(0);
    }

    std::uint8_t operator()(const std::uint32_t rgb) {
        const std::size_t slot = (rgb * 2654435761U) >> 20U;
        if (m_keys[slot] == rgb + 1) {
            return m_values[slot];
        }

        std::size_t best = 0;
        int best_distance = 0;
        for (std::size_t i = 0; i < m_palette.size(); ++i) {
            int distance = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const int difference = channel(rgb, c) - channel(m_palette[i], c);
                distance += difference * difference;
            }

            if (i == 0 || distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }

        m_keys[slot] = rgb + 1;
        m_values[slot] = static_cast<std::uint8_t>(best);
        return m_values[slot];
    }

private:
    std::span<const std::uint32_t> m_palette;
    std::array<std::uint32_t, 4096> m_keys{};
    std::array<std::uint8_t, 4096> m_values{};
};

/// An error in sixteenths, as whole units, the nearest, a half up, for one below 0 as for one
/// above, which division by 16 would round toward 0 instead, and so make dithered colors lighter.
int whole(const int sixteenths) {
    return static_cast<int>(std::floor((sixteenths + 8) / 16.0));
}

/// The frame's pixels as indexes into a palette of their colors, or of median cut's, dithered by
/// Floyd and Steinberg's weights, 7, 3, 5, and 1 sixteenths of each pixel's error.
indexed_t indexed(const std::span<const std::uint8_t> rgba, const std::int64_t width,
                  const std::int64_t height) {
    const std::size_t count = rgba.size() / 4;
    indexed_t frame;
    std::vector<std::uint32_t> colors;
    colors.reserve(count);
    bool transparent = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (rgba[(i * 4) + 3] < opaque_from) {
            transparent = true;
        } else {
            colors.push_back(rgb_of(rgba, i));
        }
    }

    std::ranges::sort(colors);
    std::vector<counted_t> histogram;
    for (const std::uint32_t rgb : colors) {
        if (histogram.empty() || histogram.back().rgb != rgb) {
            histogram.push_back({ .rgb = rgb, .count = 0 });
        }
        ++histogram.back().count;
    }

    const std::size_t room = palette_size - (transparent ? 1 : 0);
    const bool exact = histogram.size() <= room;
    if (exact) {
        for (const counted_t& color : histogram) {
            frame.palette.push_back(color.rgb);
        }
    } else {
        frame.palette = median_cut(std::move(histogram), room);
    }

    if (transparent) {
        frame.transparent = static_cast<int>(frame.palette.size());
        frame.palette.push_back(0);
    }

    frame.indexes.resize(count);
    if (exact) {
        const auto opaque =
            std::span(frame.palette).first(frame.palette.size() - (transparent ? 1 : 0));
        for (std::size_t i = 0; i < count; ++i) {
            if (rgba[(i * 4) + 3] < opaque_from) {
                frame.indexes[i] = static_cast<std::uint8_t>(frame.transparent);
            } else {
                const auto found = std::ranges::lower_bound(opaque, rgb_of(rgba, i));
                frame.indexes[i] = static_cast<std::uint8_t>(found - opaque.begin());
            }
        }
        return frame;
    }

    // Each row's errors, in sixteenths, for the row and the one after, with a column either side.
    const auto columns = static_cast<std::size_t>(width);
    std::vector<int> errors((columns + 2) * 3, 0);
    std::vector<int> next((columns + 2) * 3, 0);
    nearest_t nearest(std::span(frame.palette).first(room));
    for (std::int64_t y = 0; y < height; ++y) {
        std::ranges::fill(next, 0);
        for (std::size_t x = 0; x < columns; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * columns) + x;
            if (rgba[(i * 4) + 3] < opaque_from) {
                frame.indexes[i] = static_cast<std::uint8_t>(frame.transparent);
                continue;
            }

            std::uint32_t wanted = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const int value = rgba[(i * 4) + c] + whole(errors[((x + 1) * 3) + c]);
                wanted = (wanted << 8U) | static_cast<std::uint32_t>(std::clamp(value, 0, 255));
            }

            const std::uint8_t index = nearest(wanted);
            frame.indexes[i] = index;
            for (std::size_t c = 0; c < 3; ++c) {
                const int error = channel(wanted, c) - channel(frame.palette[index], c);
                errors[((x + 2) * 3) + c] += error * 7;
                next[(x * 3) + c] += error * 3;
                next[((x + 1) * 3) + c] += error * 5;
                next[((x + 2) * 3) + c] += error;
            }
        }
        std::swap(errors, next);
    }
    return frame;
}

/// The bits of codes, the first in the least significant bits, in bytes.
class bits_t {
public:
    void put(const std::uint32_t code, const unsigned size) {
        m_bits |= code << m_count;
        m_count += size;
        while (m_count >= 8) {
            m_bytes.push_back(static_cast<std::uint8_t>(m_bits & 0xffU));
            m_bits >>= 8U;
            m_count -= 8;
        }
    }

    std::vector<std::uint8_t> finish() {
        if (m_count > 0) {
            m_bytes.push_back(static_cast<std::uint8_t>(m_bits & 0xffU));
        }
        return std::move(m_bytes);
    }

private:
    std::vector<std::uint8_t> m_bytes;
    std::uint32_t m_bits = 0;
    unsigned m_count = 0;
};

/// The indexes as GIF's LZW codes of `least` bits and more: a clear code first, and again once
/// the table is full, and the end code last.
std::vector<std::uint8_t> compressed(const std::span<const std::uint8_t> indexes,
                                     const unsigned least) {
    const std::uint32_t clear = 1U << least;
    const std::uint32_t end = clear + 1;
    // The table of strings, by their prefix's code and their last index, in open addressing.
    constexpr std::size_t slots = 8192;
    std::vector<std::int32_t> keys(slots, -1);
    std::vector<std::uint16_t> values(slots, 0);
    std::uint32_t last_code = end;
    unsigned size = least + 1;
    bits_t bits;
    bits.put(clear, size);
    std::uint32_t prefix = indexes[0];
    for (std::size_t i = 1; i < indexes.size(); ++i) {
        const std::uint32_t index = indexes[i];
        const auto key = static_cast<std::int32_t>((prefix << 8U) | index);
        std::size_t slot = (static_cast<std::uint32_t>(key) * 2654435761U) >> 19U;
        while (keys[slot] != -1 && keys[slot] != key) {
            slot = (slot + 1) % slots;
        }

        if (keys[slot] == key) {
            prefix = values[slot];
            continue;
        }

        bits.put(prefix, size);
        keys[slot] = key;
        values[slot] = static_cast<std::uint16_t>(++last_code);
        if (last_code >= (1U << size)) {
            ++size;
        }

        if (last_code == most_code) {
            bits.put(clear, size);
            std::ranges::fill(keys, -1);
            last_code = end;
            size = least + 1;
        }
        prefix = index;
    }

    // The decoder makes a string of the last code too, which can widen the end code.
    bits.put(prefix, size);
    if (last_code + 1 >= (1U << size) && size < 12) {
        ++size;
    }
    bits.put(end, size);
    return bits.finish();
}

class gif_encoder_t {
public:
    gif_encoder_t(const std::int64_t width, const std::int64_t height, const std::int32_t plays)
        : m_width(width), m_height(height), m_plays(plays) {}

    [[nodiscard]] std::int64_t width() const {
        return m_width;
    }
    [[nodiscard]] std::int64_t height() const {
        return m_height;
    }

    void add(const std::span<const std::uint8_t> rgba, const std::int32_t delay) {
        m_frames.push_back({ .frame = indexed(rgba, m_width, m_height), .delay = delay });
    }

    std::vector<std::uint8_t> finish() {
        std::vector<std::uint8_t> out{ 'G', 'I', 'F', '8', '9', 'a' };
        put_little(out, static_cast<std::uint32_t>(m_width), 2);
        put_little(out, static_cast<std::uint32_t>(m_height), 2);
        // No global palette, 8 bits a channel, and the background the first color.
        out.insert(out.end(), { 0x70, 0, 0 });
        // An animation, even of one frame, with its delays and plays; a still image has one frame
        // shown once, at once.
        const bool animated =
            m_frames.size() > 1 || m_plays != 1 ||
            std::ranges::any_of(m_frames, [](const frame_t& frame) { return frame.delay != 0; });
        if (animated && m_plays != 1) {
            out.insert(out.end(), {
                                      0x21,
                                      0xff,
                                      0x0b,
                                      'N',
                                      'E',
                                      'T',
                                      'S',
                                      'C',
                                      'A',
                                      'P',
                                      'E',
                                      '2',
                                      '.',
                                      '0',
                                      0x03,
                                      0x01,
                                  });
            put_little(out, static_cast<std::uint32_t>(m_plays == 0 ? 0 : m_plays - 1), 2);
            out.push_back(0);
        }

        for (const frame_t& entry : m_frames) {
            const indexed_t& frame = entry.frame;
            if (animated || frame.transparent >= 0) {
                // Each frame goes back to the background, which is transparent, before the next.
                const unsigned disposal = animated ? 2 : 0;
                out.insert(out.end(), { 0x21, 0xf9, 0x04 });
                out.push_back(static_cast<std::uint8_t>((disposal << 2U) |
                                                        (frame.transparent >= 0 ? 1U : 0U)));
                put_little(out, static_cast<std::uint32_t>((entry.delay + 5) / 10), 2);
                out.push_back(static_cast<std::uint8_t>(std::max(frame.transparent, 0)));
                out.push_back(0);
            }

            unsigned bits = 1;
            while ((std::size_t{ 1 } << bits) < frame.palette.size()) {
                ++bits;
            }

            out.push_back(0x2c);
            put_little(out, 0, 4);
            put_little(out, static_cast<std::uint32_t>(m_width), 2);
            put_little(out, static_cast<std::uint32_t>(m_height), 2);
            out.push_back(static_cast<std::uint8_t>(0x80U | (bits - 1)));
            for (std::size_t i = 0; i < (std::size_t{ 1 } << bits); ++i) {
                put_big(out, i < frame.palette.size() ? frame.palette[i] : 0, 3);
            }

            const unsigned least = std::max(bits, 2U);
            out.push_back(static_cast<std::uint8_t>(least));
            const std::vector<std::uint8_t> codes = compressed(frame.indexes, least);
            for (std::size_t at = 0; at < codes.size(); at += 255) {
                const std::size_t size = std::min<std::size_t>(255, codes.size() - at);
                out.push_back(static_cast<std::uint8_t>(size));
                out.insert(out.end(), codes.begin() + static_cast<std::ptrdiff_t>(at),
                           codes.begin() + static_cast<std::ptrdiff_t>(at + size));
            }
            out.push_back(0);
        }

        out.push_back(0x3b);
        m_frames.clear();
        return out;
    }

private:
    struct frame_t {
        indexed_t frame;
        std::int32_t delay = 0;
    };

    std::int64_t m_width;
    std::int64_t m_height;
    std::int32_t m_plays;
    std::vector<frame_t> m_frames;
};

} // namespace

extern "C" {

void* fb_image_gif_encoder_new(const std::int64_t width, const std::int64_t height,
                               const std::int32_t plays) {
    return new gif_encoder_t(width, height, plays);
}

void fb_image_gif_encoder_add(void* encoder, const std::uint8_t* pixels, const std::int32_t delay) {
    auto* gif = static_cast<gif_encoder_t*>(encoder);
    gif->add(pixels_at(pixels, gif->width(), gif->height()), delay);
}

void fb_image_gif_encoder_finish(void* encoder, fb_array_t* output) {
    set_elements<std::uint8_t>(output, static_cast<gif_encoder_t*>(encoder)->finish());
}

void fb_image_gif_encoder_free(void* encoder) {
    delete static_cast<gif_encoder_t*>(encoder);
}
}
