// std.image's APNG decoding: an APNG's frames, each a PNG of its own made from its fcTL and its
// data chunks, decoded by stb_image, and drawn on the canvas by its blend and disposal, as the
// APNG specification has it. A PNG without an acTL chunk is one frame, the PNG's image.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <vector>

using namespace fb::image;

namespace {

constexpr std::array<std::uint8_t, 8> signature{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

constexpr std::uint8_t dispose_none = 0;
constexpr std::uint8_t dispose_background = 1;
constexpr std::uint8_t dispose_previous = 2;
constexpr std::uint8_t blend_source = 0;

/// A chunk: its type and its data, in the bytes.
struct chunk_t {
    std::string_view type;
    std::span<const std::uint8_t> data;
};

/// A frame's control and its data, an fdAT's without its sequence number.
struct frame_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::int32_t delay = 0;
    std::uint8_t dispose = dispose_none;
    std::uint8_t blend = blend_source;
    std::vector<std::span<const std::uint8_t>> data;
};

struct stb_free_t {
    void operator()(void* pixels) const {
        stbi_image_free(pixels);
    }
};

/// The chunks after the signature, or none when they don't each fit in the bytes, with whether
/// they reached an IEND.
std::vector<chunk_t> chunks_of(const std::span<const std::uint8_t> bytes, bool& ended) {
    std::vector<chunk_t> chunks;
    std::size_t at = signature.size();
    ended = false;
    while (at + 12 <= bytes.size()) {
        const std::size_t size = big_at(bytes, at);
        if (size > bytes.size() - at - 12) {
            return chunks;
        }

        const std::string_view type(reinterpret_cast<const char*>(&bytes[at + 4]), 4);
        chunks.push_back({ .type = type, .data = bytes.subspan(at + 8, size) });
        at += size + 12;
        if (type == "IEND") {
            ended = true;
            break;
        }
    }
    return chunks;
}

/// The milliseconds of a delay's fraction of a second, a denominator of 0 being 100.
std::int32_t milliseconds(const std::uint32_t numerator, const std::uint32_t denominator) {
    const std::uint32_t over = denominator == 0 ? 100 : denominator;
    return static_cast<std::int32_t>(((std::uint64_t{ numerator } * 1000) + (over / 2)) / over);
}

/// The PNG of one frame: the image's header with the frame's size, the chunks before its first
/// data but the animation's, and the frame's data as IDAT chunks.
std::vector<std::uint8_t> frame_png(const std::span<const chunk_t> before, const chunk_t& header,
                                    const frame_t& frame) {
    std::vector<std::uint8_t> out(signature.begin(), signature.end());
    const auto add = [&](const std::string_view type, const std::span<const std::uint8_t> data) {
        put_big(out, static_cast<std::uint32_t>(data.size()), 4);
        const std::size_t start = out.size();
        out.insert(out.end(), type.begin(), type.end());
        out.insert(out.end(), data.begin(), data.end());
        const std::uint32_t crc = crc32(0, std::span(out).subspan(start));
        put_big(out, crc, 4);
    };

    std::vector<std::uint8_t> sized(header.data.begin(), header.data.end());
    std::vector<std::uint8_t> size;
    put_big(size, frame.width, 4);
    put_big(size, frame.height, 4);
    std::ranges::copy(size, sized.begin());
    add("IHDR", sized);
    for (const chunk_t& chunk : before) {
        if (chunk.type != "IHDR" && chunk.type != "acTL" && chunk.type != "fcTL") {
            add(chunk.type, chunk.data);
        }
    }

    for (const std::span<const std::uint8_t> data : frame.data) {
        add("IDAT", data);
    }
    add("IEND", {});
    return out;
}

/// Draws `pixels`, a frame of `frame`'s size, on the canvas at its place, replacing what's there
/// or over it, by straight alpha's "over".
void draw(std::vector<std::uint8_t>& canvas, const std::int64_t width, const frame_t& frame,
          const stbi_uc* pixels) {
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        for (std::uint32_t column = 0; column < frame.width; ++column) {
            const std::size_t from = ((std::size_t{ row } * frame.width) + column) * 4;
            const std::size_t to =
                (((std::size_t{ frame.y } + row) * static_cast<std::size_t>(width)) + frame.x +
                 column) *
                4;
            if (frame.blend == blend_source) {
                std::copy_n(pixels + from, 4, canvas.begin() + static_cast<std::ptrdiff_t>(to));
            } else {
                over(&canvas[to], pixels + from);
            }
        }
    }
}

/// Clears the frame's place on the canvas to transparent black.
void clear(std::vector<std::uint8_t>& canvas, const std::int64_t width, const frame_t& frame) {
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        const std::size_t to =
            (((std::size_t{ frame.y } + row) * static_cast<std::size_t>(width)) + frame.x) * 4;
        std::fill_n(canvas.begin() + static_cast<std::ptrdiff_t>(to),
                    std::size_t{ frame.width } * 4, 0);
    }
}

image_t* decode(const std::span<const std::uint8_t> bytes, const std::int64_t limit) {
    if (bytes.size() < signature.size() || !std::ranges::equal(bytes.first(8), signature)) {
        return failed(RESULT_INVALID, "it isn't a PNG");
    }

    bool ended = false;
    const std::vector<chunk_t> chunks = chunks_of(bytes, ended);
    if (chunks.empty() || chunks[0].type != "IHDR" || chunks[0].data.size() != 13) {
        return failed(ended ? RESULT_INVALID : RESULT_ENDS_TOO_SOON,
                      "it doesn't start with an IHDR chunk");
    }

    const auto control = std::ranges::find(chunks, "acTL", &chunk_t::type);
    const auto first_data = std::ranges::find(chunks, "IDAT", &chunk_t::type);
    if (first_data == chunks.end()) {
        return failed(ended ? RESULT_INVALID : RESULT_ENDS_TOO_SOON, "it has no IDAT chunk");
    }

    const std::int64_t width = big_at(chunks[0].data, 0);
    const std::int64_t height = big_at(chunks[0].data, 4);
    if (control == chunks.end() || control > first_data || control->data.size() != 8) {
        // A PNG, or an APNG whose animation is broken, is the PNG's image alone.
        if (image_t* over = over_limit(pixels_of(width, height), limit)) {
            return over;
        }

        int w = 0;
        int h = 0;
        int channels = 0;
        const std::unique_ptr<stbi_uc, stb_free_t> pixels(stbi_load_from_memory(
            bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4));
        if (pixels == nullptr) {
            const char* reason = stbi_failure_reason();
            return failed(RESULT_INVALID, reason == nullptr ? "" : reason);
        }

        auto* image = new image_t{ .width = w, .height = h, .delays = { 0 }, .plays = 1 };
        image->frames.emplace_back(pixels.get(), pixels.get() + (pixels_of(w, h) * 4));
        return image;
    }

    const std::uint32_t count = big_at(control->data, 0);
    const auto plays = static_cast<std::int32_t>(std::min<std::uint32_t>(
        big_at(control->data, 4), static_cast<std::uint32_t>(most_elements)));
    if (image_t* over = over_limit(pixels_of(width, height, count), limit)) {
        return over;
    }

    // The frames, each from its fcTL to the next, with the IDATs after an fcTL the first's.
    std::vector<frame_t> frames;
    for (const auto& chunk : chunks) {
        if (chunk.type == "fcTL") {
            if (chunk.data.size() != 26) {
                return failed(RESULT_INVALID, "an fcTL chunk isn't 26 bytes");
            }

            const std::span<const std::uint8_t> data = chunk.data;
            frames.push_back({
                .width = big_at(data, 4),
                .height = big_at(data, 8),
                .x = big_at(data, 12),
                .y = big_at(data, 16),
                .delay = milliseconds((std::uint32_t{ data[20] } << 8U) | data[21],
                                      (std::uint32_t{ data[22] } << 8U) | data[23]),
                .dispose = data[24],
                .blend = data[25],
            });
            const frame_t& frame = frames.back();
            if (frame.width == 0 || frame.height == 0 || frame.x > width - frame.width ||
                frame.y > height - frame.height || frame.dispose > dispose_previous ||
                frame.blend > 1) {
                return failed(RESULT_INVALID, "a frame isn't inside the image");
            }
        } else if ((chunk.type == "IDAT" || chunk.type == "fdAT") && !frames.empty()) {
            if (chunk.type == "fdAT") {
                if (chunk.data.size() < 4) {
                    return failed(RESULT_INVALID, "an fdAT chunk has no sequence number");
                }
                frames.back().data.push_back(chunk.data.subspan(4));
            } else {
                frames.back().data.push_back(chunk.data);
            }
        }
    }

    if (frames.empty() || frames.size() != count) {
        return failed(ended ? RESULT_INVALID : RESULT_ENDS_TOO_SOON,
                      "it has " + std::to_string(frames.size()) + " frames, not the " +
                          std::to_string(count) + " its acTL says");
    }

    auto* image = new image_t{ .width = width, .height = height, .plays = plays };
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(pixels_of(width, height)) * 4, 0);
    std::vector<std::uint8_t> previous;
    const std::span<const chunk_t> before(chunks.begin(), first_data);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frame_t& frame = frames[i];
        // The first frame's going back to what was before is going back to nothing.
        if (i == 0 && frame.dispose == dispose_previous) {
            frame.dispose = dispose_background;
        }

        const std::vector<std::uint8_t> png = frame_png(before, chunks[0], frame);
        int w = 0;
        int h = 0;
        int channels = 0;
        const std::unique_ptr<stbi_uc, stb_free_t> pixels(
            stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4));
        if (pixels == nullptr) {
            delete image;
            const char* reason = stbi_failure_reason();
            return failed(RESULT_INVALID, std::string("frame ") + std::to_string(i) + ": " +
                                              (reason == nullptr ? "" : reason));
        }

        if (frame.dispose == dispose_previous) {
            previous = canvas;
        }

        draw(canvas, width, frame, pixels.get());
        image->frames.push_back(canvas);
        image->delays.push_back(frame.delay);
        if (frame.dispose == dispose_background) {
            clear(canvas, width, frame);
        } else if (frame.dispose == dispose_previous) {
            canvas = previous;
        }
    }
    return image;
}

} // namespace

extern "C" {

void* fb_stb_decode_apng(const fb_array_t* data, const std::int64_t limit) {
    return decode(bytes_of(data), limit);
}
}
