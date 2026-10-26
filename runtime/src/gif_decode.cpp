// GIF's decoder: GIF87a and GIF89a, each frame's LZW-compressed indexes drawn on the canvas at
// its place, by its palette, its transparent index, and the disposal of the frame before, as
// browsers draw them: the canvas starts transparent, whatever the background color is, and a
// frame disposed of to the background leaves its place transparent.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace fb::image;

namespace {

constexpr std::uint8_t block_image = 0x2c;
constexpr std::uint8_t block_extension = 0x21;
constexpr std::uint8_t block_trailer = 0x3b;
constexpr std::uint8_t label_control = 0xf9;
constexpr std::uint8_t label_application = 0xff;
constexpr std::uint8_t dispose_background = 2;
constexpr std::uint8_t dispose_previous = 3;
/// LZW's codes are 12 bits at most, so its table holds 4096 strings.
constexpr std::size_t table_size = 4096;

/// The bytes, read from the start on.
class reader_t {
public:
    explicit reader_t(const std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

    [[nodiscard]] bool has(const std::size_t count) const {
        return m_bytes.size() - m_at >= count;
    }

    [[nodiscard]] bool done() const {
        return m_at == m_bytes.size();
    }

    std::uint8_t byte() {
        return m_bytes[m_at++];
    }

    std::uint32_t little16() {
        const std::uint32_t value = little16_at(m_bytes, m_at);
        m_at += 2;
        return value;
    }

    std::span<const std::uint8_t> take(const std::size_t count) {
        const std::span<const std::uint8_t> taken = m_bytes.subspan(m_at, count);
        m_at += count;
        return taken;
    }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_at = 0;
};

/// A palette's colors, as 0xRRGGBB.
struct palette_t {
    std::array<std::uint32_t, 256> colors{};
    std::size_t size = 0;
};

/// Why decoding stopped, if it did.
struct problem_t {
    result_t result = RESULT_DONE;
    std::string message;
};

/// Reads a palette of `2 << (flags & 7)` colors into `palette`, or gives false when the bytes end.
bool read_palette(reader_t& reader, const std::uint8_t flags, palette_t& palette) {
    palette.size = std::size_t{ 2 } << (flags & 7U);
    if (!reader.has(palette.size * 3)) {
        return false;
    }

    for (std::size_t i = 0; i < palette.size; ++i) {
        const std::span<const std::uint8_t> rgb = reader.take(3);
        palette.colors[i] = (std::uint32_t{ rgb[0] } << 16U) | (std::uint32_t{ rgb[1] } << 8U) |
                            std::uint32_t{ rgb[2] };
    }
    return true;
}

/// Reads sub-blocks up to the empty one that ends them, adding their bytes to `data` unless it's
/// null, or gives false when the bytes end first.
bool read_blocks(reader_t& reader, std::vector<std::uint8_t>* data) {
    while (true) {
        if (!reader.has(1)) {
            return false;
        }

        const std::uint8_t size = reader.byte();
        if (size == 0) {
            return true;
        }

        if (!reader.has(size)) {
            return false;
        }

        const std::span<const std::uint8_t> block = reader.take(size);
        if (data != nullptr) {
            data->insert(data->end(), block.begin(), block.end());
        }
    }
}

/// Gives `emit` each of the indexes of `count` pixels in `codes`, LZW-compressed with codes of
/// `least` bits and more, with its place, as it's decoded, so a frame takes no memory of its own,
/// whatever size its descriptor says. Codes that stop before the pixels do give fewer, which
/// leaves the rest undrawn, as browsers do. Gives false for a code that can't be one yet.
template <typename Emit>
bool decompress(const std::span<const std::uint8_t> codes, const unsigned least,
                const std::size_t count, const Emit& emit) {
    const std::uint32_t clear = 1U << least;
    const std::uint32_t end = clear + 1;
    std::array<std::uint16_t, table_size> prefixes{};
    std::array<std::uint8_t, table_size> suffixes{};
    std::array<std::uint8_t, table_size> firsts{};
    std::array<std::uint8_t, table_size> string{};
    for (std::uint32_t i = 0; i < clear; ++i) {
        suffixes[i] = static_cast<std::uint8_t>(i);
        firsts[i] = static_cast<std::uint8_t>(i);
    }

    std::uint32_t next = clear + 2;
    unsigned size = least + 1;
    std::int32_t last = -1;
    std::uint32_t bits = 0;
    unsigned held = 0;
    std::size_t at = 0;
    std::size_t given = 0;
    const auto give = [&](const std::uint8_t index) { emit(given++, index); };
    while (given < count) {
        while (held < size && at < codes.size()) {
            bits |= std::uint32_t{ codes[at++] } << held;
            held += 8;
        }

        if (held < size) {
            return true;
        }

        const std::uint32_t code = bits & ((1U << size) - 1);
        bits >>= size;
        held -= size;
        if (code == clear) {
            next = clear + 2;
            size = least + 1;
            last = -1;
            continue;
        }

        if (code == end) {
            return true;
        }

        if (last < 0) {
            if (code >= clear) {
                return false;
            }

            give(static_cast<std::uint8_t>(code));
            last = static_cast<std::int32_t>(code);
            continue;
        }

        // The code's string, or the last one's and its own first index when it's the code the
        // table is about to have.
        std::uint32_t walk = code;
        std::uint8_t first = 0;
        if (code < next) {
            first = firsts[code];
        } else if (code == next) {
            first = firsts[static_cast<std::uint32_t>(last)];
            walk = static_cast<std::uint32_t>(last);
        } else {
            return false;
        }

        std::size_t length = 0;
        while (walk >= clear) {
            string[length++] = suffixes[walk];
            walk = prefixes[walk];
        }
        string[length++] = static_cast<std::uint8_t>(walk);
        for (std::size_t i = length; i > 0 && given < count; --i) {
            give(string[i - 1]);
        }

        if (code == next && given < count) {
            give(first);
        }

        if (next < table_size) {
            prefixes[next] = static_cast<std::uint16_t>(last);
            suffixes[next] = first;
            firsts[next] = firsts[static_cast<std::uint32_t>(last)];
            ++next;
            if (next == (1U << size) && size < 12) {
                ++size;
            }
        }
        last = static_cast<std::int32_t>(code);
    }
    return true;
}

/// The rows of an interlaced image of `height` rows, in the order its indexes have them: every
/// eighth from 0, every eighth from 4, every fourth from 2, then every second from 1.
std::vector<std::uint32_t> interlaced_rows(const std::uint32_t height) {
    std::vector<std::uint32_t> rows;
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> passes{
        { { 0, 8 }, { 4, 8 }, { 2, 4 }, { 1, 2 } },
    };
    for (const auto& [start, step] : passes) {
        for (std::uint32_t row = start; row < height; row += step) {
            rows.push_back(row);
        }
    }
    return rows;
}

image_t* decode(const std::span<const std::uint8_t> bytes, const bool first_only,
                const std::int64_t limit) {
    reader_t reader(bytes);
    if (!reader.has(6)) {
        return failed(RESULT_ENDS_TOO_SOON, "its header ends too soon");
    }

    const std::span<const std::uint8_t> signature = reader.take(6);
    const std::string_view text(reinterpret_cast<const char*>(signature.data()), 6);
    if (text != "GIF87a" && text != "GIF89a") {
        return failed(RESULT_INVALID, R"(it doesn't start with "GIF87a" or "GIF89a")");
    }

    if (!reader.has(7)) {
        return failed(RESULT_ENDS_TOO_SOON, "its header ends too soon");
    }

    const std::uint32_t width = reader.little16();
    const std::uint32_t height = reader.little16();
    const std::uint8_t flags = reader.byte();
    reader.take(2);
    if (width == 0 || height == 0) {
        return failed(RESULT_INVALID, "its screen has a size of 0");
    }

    if (image_t* over = over_limit(pixels_of(width, height), limit)) {
        return over;
    }

    palette_t global;
    if ((flags & 0x80U) != 0 && !read_palette(reader, flags, global)) {
        return failed(RESULT_ENDS_TOO_SOON, "its palette ends too soon");
    }

    auto* image = new image_t{ .width = width, .height = height, .plays = 1 };
    const std::size_t columns = width;
    std::vector<std::uint8_t> canvas(std::size_t{ width } * height * 4, 0);
    std::vector<std::uint8_t> previous;
    // What the frame before asked for once it was shown, and where it was.
    std::uint8_t disposal = 0;
    std::array<std::uint32_t, 4> place{};
    // What the next frame's Graphic Control Extension says.
    std::uint8_t next_disposal = 0;
    std::int32_t transparent = -1;
    std::int32_t delay = 0;
    const auto fail = [&](const result_t result, std::string message) {
        delete image;
        return failed(result, std::move(message));
    };

    while (true) {
        if (!reader.has(1)) {
            if (image->frames.empty()) {
                return fail(RESULT_ENDS_TOO_SOON, "it ends before its first frame");
            }
            break;
        }

        const std::uint8_t block = reader.byte();
        if (block == block_trailer) {
            break;
        }

        if (block == block_extension) {
            if (!reader.has(1)) {
                return fail(RESULT_ENDS_TOO_SOON, "an extension ends too soon");
            }

            const std::uint8_t label = reader.byte();
            std::vector<std::uint8_t> data;
            if (!read_blocks(reader, &data)) {
                return fail(RESULT_ENDS_TOO_SOON, "an extension ends too soon");
            }

            if (label == label_control && data.size() >= 4) {
                const unsigned control = data[0];
                next_disposal = static_cast<std::uint8_t>((control >> 2U) & 7U);
                transparent = (control & 1U) != 0 ? data[3] : -1;
                delay = static_cast<std::int32_t>(little16_at(data, 1)) * 10;
            } else if (label == label_application && data.size() >= 14) {
                const std::string_view name(reinterpret_cast<const char*>(data.data()), 11);
                if ((name == "NETSCAPE2.0" || name == "ANIMEXTS1.0") && data[11] == 1) {
                    const std::uint32_t repeats = little16_at(data, 12);
                    image->plays = repeats == 0 ? 0 : static_cast<std::int32_t>(repeats) + 1;
                }
            }
            continue;
        }

        if (block != block_image) {
            return fail(RESULT_INVALID, "it has a block that isn't an image, an extension, or "
                                        "the trailer");
        }

        if (!reader.has(9)) {
            return fail(RESULT_ENDS_TOO_SOON, "a frame ends too soon");
        }

        const std::uint32_t left = reader.little16();
        const std::uint32_t top = reader.little16();
        const std::uint32_t frame_width = reader.little16();
        const std::uint32_t frame_height = reader.little16();
        const std::uint8_t image_flags = reader.byte();
        palette_t local;
        if ((image_flags & 0x80U) != 0 && !read_palette(reader, image_flags, local)) {
            return fail(RESULT_ENDS_TOO_SOON, "a frame's palette ends too soon");
        }

        const palette_t& palette = (image_flags & 0x80U) != 0 ? local : global;
        if (palette.size == 0) {
            return fail(RESULT_INVALID, "a frame has no palette, its own or the GIF's");
        }

        if (!reader.has(1)) {
            return fail(RESULT_ENDS_TOO_SOON, "a frame ends too soon");
        }

        const unsigned least = reader.byte();
        std::vector<std::uint8_t> codes;
        if (!read_blocks(reader, &codes)) {
            return fail(RESULT_ENDS_TOO_SOON, "a frame's pixels end too soon");
        }

        if (least < 1 || least > 11) {
            return fail(RESULT_INVALID, "a frame's codes start at a size GIF doesn't have");
        }

        // The frame before goes as it asked, once it's been shown.
        const std::uint32_t right = std::min(width, place[0] + place[2]);
        const std::uint32_t bottom = std::min(height, place[1] + place[3]);
        if (disposal == dispose_background || disposal == dispose_previous) {
            for (std::uint32_t y = place[1]; y < bottom; ++y) {
                for (std::uint32_t x = place[0]; x < right; ++x) {
                    const std::size_t at = ((std::size_t{ y } * columns) + x) * 4;
                    for (std::size_t c = 0; c < 4; ++c) {
                        canvas[at + c] = disposal == dispose_previous ? previous[at + c] : 0;
                    }
                }
            }
        }

        if (next_disposal == dispose_previous) {
            previous = canvas;
        }

        // Each index is drawn as it's decoded, but those past the screen, so a frame takes no more
        // memory than the screen, whatever its descriptor says; one not interlaced stops at its
        // first row past the screen.
        const std::vector<std::uint32_t> rows = (image_flags & 0x40U) != 0
                                                    ? interlaced_rows(frame_height)
                                                    : std::vector<std::uint32_t>{};
        std::size_t count = std::size_t{ frame_width } * frame_height;
        if (rows.empty()) {
            count = std::size_t{ frame_width } *
                    (top < height ? std::min(frame_height, height - top) : 0);
        }
        const auto draw = [&](const std::size_t i, const std::uint8_t index) {
            const auto column = static_cast<std::uint32_t>(i % frame_width);
            const auto row_index = static_cast<std::uint32_t>(i / frame_width);
            const std::uint32_t row = rows.empty() ? row_index : rows[row_index];
            const std::uint32_t x = left + column;
            const std::uint32_t y = top + row;
            if (x >= width || y >= height || std::cmp_equal(index, transparent) ||
                index >= palette.size) {
                return;
            }

            const std::size_t at = ((std::size_t{ y } * columns) + x) * 4;
            const std::uint32_t rgb = palette.colors[index];
            canvas[at] = static_cast<std::uint8_t>(rgb >> 16U);
            canvas[at + 1] = static_cast<std::uint8_t>(rgb >> 8U);
            canvas[at + 2] = static_cast<std::uint8_t>(rgb);
            canvas[at + 3] = 255;
        };
        if (!decompress(codes, least, count, draw)) {
            return fail(RESULT_INVALID, "a frame's pixels have a code that can't be one");
        }

        const auto shown = static_cast<std::int64_t>(image->frames.size()) + 1;
        if (pixels_of(width, height, shown) > limit) {
            delete image;
            return over_limit(pixels_of(width, height, shown), limit);
        }

        image->frames.push_back(canvas);
        image->delays.push_back(delay);
        if (first_only) {
            break;
        }

        disposal = next_disposal;
        place = { left, top, frame_width, frame_height };
        next_disposal = 0;
        transparent = -1;
        delay = 0;
    }
    return image;
}

} // namespace

extern "C" {

void* fb_image_gif_decode(const fb_array_t* data, const std::int32_t first_only,
                          const std::int64_t limit) {
    return decode(bytes_of(data), first_only != 0, limit);
}
}
