// std.image's PNG and APNG encoders, the runtime's own, over zlib-ng: 8 bits a channel, each row
// filtered by the filter libpng's heuristic picks, and deflated at a level. stb_image reads them.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "image_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>
#include <zlib.h>

using namespace fb::image;

namespace {

constexpr std::array<std::uint8_t, 8> signature{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
constexpr std::uint8_t color_rgb = 2;
constexpr std::uint8_t color_rgba = 6;
/// The most bytes of an IDAT or fdAT chunk, past which they're split into more.
constexpr std::size_t chunk_bytes = std::size_t{ 1 } << 20U;

/// Paeth's predictor: whichever of the left, up, and up-left bytes is nearest left plus up minus
/// up-left, left first in a tie, then up.
int paeth(const int left, const int up, const int corner) {
    const int guess = left + up - corner;
    const int to_left = std::abs(guess - left);
    const int to_up = std::abs(guess - up);
    const int to_corner = std::abs(guess - corner);
    if (to_left <= to_up && to_left <= to_corner) {
        return left;
    }

    return to_up <= to_corner ? up : corner;
}

/// The pixels as rows of `channels` bytes a pixel, the first 3 or all 4 of RGBA, each after its
/// filter's byte: of PNG's five filters, the one whose bytes, as signed values, have the least sum
/// of sizes, the first in a tie.
std::vector<std::uint8_t> filtered(const std::span<const std::uint8_t> rgba,
                                   const std::int64_t width, const std::int64_t height,
                                   const std::size_t channels) {
    const auto columns = static_cast<std::size_t>(width);
    const std::size_t row_size = columns * channels;
    std::vector<std::uint8_t> out;
    out.reserve((row_size + 1) * static_cast<std::size_t>(height));
    std::vector<std::uint8_t> up(row_size, 0);
    std::vector<std::uint8_t> row(row_size);
    std::array<std::vector<std::uint8_t>, 5> tried;
    for (std::vector<std::uint8_t>& candidate : tried) {
        candidate.resize(row_size);
    }

    for (std::int64_t y = 0; y < height; ++y) {
        const std::size_t first = static_cast<std::size_t>(y) * columns * 4;
        for (std::size_t x = 0; x < columns; ++x) {
            for (std::size_t c = 0; c < channels; ++c) {
                row[(x * channels) + c] = rgba[first + (x * 4) + c];
            }
        }

        std::size_t best = 0;
        std::int64_t best_sum = 0;
        for (std::size_t filter = 0; filter < tried.size(); ++filter) {
            std::int64_t sum = 0;
            for (std::size_t i = 0; i < row_size; ++i) {
                const int left = i >= channels ? row[i - channels] : 0;
                const int corner = i >= channels ? up[i - channels] : 0;
                int predicted = 0;
                if (filter == 1) {
                    predicted = left;
                } else if (filter == 2) {
                    predicted = up[i];
                } else if (filter == 3) {
                    predicted = (left + up[i]) / 2;
                } else if (filter == 4) {
                    predicted = paeth(left, up[i], corner);
                }

                const auto value = static_cast<std::uint8_t>(row[i] - predicted);
                tried[filter][i] = value;
                sum += std::abs(static_cast<int>(static_cast<std::int8_t>(value)));
            }

            if (filter == 0 || sum < best_sum) {
                best = filter;
                best_sum = sum;
            }
        }

        out.push_back(static_cast<std::uint8_t>(best));
        out.insert(out.end(), tried[best].begin(), tried[best].end());
        std::swap(up, row);
    }
    return out;
}

/// `data` in zlib's format, deflated at `level`.
std::vector<std::uint8_t> deflated(const std::span<const std::uint8_t> data, const int level) {
    z_stream stream{};
    if (deflateInit(&stream, level) != Z_OK) {
        fb_panic("out of memory", nullptr);
    }

    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 65536> buffer{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib's next_in isn't const.
    stream.next_in = const_cast<std::uint8_t*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = deflate(&stream, Z_FINISH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            deflateEnd(&stream);
            fb_panic("std.image's PNG deflating failed", nullptr);
        }

        out.insert(out.end(), buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(buffer.size() - stream.avail_out));
    }

    deflateEnd(&stream);
    return out;
}

/// Appends a chunk: its data's size, its type, its data, which is `head` and then `data`, and
/// the CRC of its type and data.
void chunk(std::vector<std::uint8_t>& out, const std::string_view type,
           const std::span<const std::uint8_t> head, const std::span<const std::uint8_t> data) {
    put_big(out, static_cast<std::uint32_t>(head.size() + data.size()), 4);
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), data.begin(), data.end());
    const std::span<const std::uint8_t> covered = std::span(out).subspan(start);
    put_big(
        out,
        static_cast<std::uint32_t>(::crc32(0, covered.data(), static_cast<uInt>(covered.size()))),
        4);
}

void chunk(std::vector<std::uint8_t>& out, const std::string_view type,
           const std::span<const std::uint8_t> data) {
    chunk(out, type, {}, data);
}

/// The signature and the IHDR chunk of an image of 8 bits a channel, not interlaced.
std::vector<std::uint8_t> started(const std::int64_t width, const std::int64_t height,
                                  const std::uint8_t color) {
    std::vector<std::uint8_t> out(signature.begin(), signature.end());
    std::vector<std::uint8_t> header;
    put_big(header, static_cast<std::uint32_t>(width), 4);
    put_big(header, static_cast<std::uint32_t>(height), 4);
    header.insert(header.end(), { 8, color, 0, 0, 0 });
    chunk(out, "IHDR", header);
    return out;
}

/// Appends `data` as chunks of `type`, each of a megabyte or less, an fdAT's each after the next
/// sequence number.
void data_chunks(std::vector<std::uint8_t>& out, const std::string_view type,
                 const std::span<const std::uint8_t> data, std::uint32_t* sequence) {
    std::size_t at = 0;
    do {
        const std::size_t size = std::min(chunk_bytes, data.size() - at);
        std::array<std::uint8_t, 4> number{};
        std::span<const std::uint8_t> head;
        if (sequence != nullptr) {
            const std::uint32_t next = (*sequence)++;
            for (std::size_t i = 0; i < number.size(); ++i) {
                number.at(i) = static_cast<std::uint8_t>(next >> ((number.size() - 1 - i) * 8));
            }

            head = number;
        }

        chunk(out, type, head, data.subspan(at, size));
        at += size;
    } while (at < data.size());
}

std::vector<std::uint8_t> encode(const std::span<const std::uint8_t> rgba, const std::int64_t width,
                                 const std::int64_t height, const int level) {
    bool opaque = true;
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] != 255) {
            opaque = false;
            break;
        }
    }

    std::vector<std::uint8_t> out = started(width, height, opaque ? color_rgb : color_rgba);
    data_chunks(out, "IDAT", deflated(filtered(rgba, width, height, opaque ? 3 : 4), level),
                nullptr);
    chunk(out, "IEND", {});
    return out;
}

class apng_encoder_t {
public:
    apng_encoder_t(const std::int64_t width, const std::int64_t height, const std::int32_t plays,
                   const int level)
        : m_width(width), m_height(height), m_plays(plays), m_level(level) {}

    [[nodiscard]] std::int64_t width() const {
        return m_width;
    }
    [[nodiscard]] std::int64_t height() const {
        return m_height;
    }

    void add(const std::span<const std::uint8_t> rgba, const std::int32_t delay) {
        m_frames.push_back(deflated(filtered(rgba, m_width, m_height, 4), m_level));
        m_delays.push_back(delay);
    }

    std::vector<std::uint8_t> finish() {
        std::vector<std::uint8_t> out = started(m_width, m_height, color_rgba);
        std::vector<std::uint8_t> control;
        put_big(control, static_cast<std::uint32_t>(m_frames.size()), 4);
        put_big(control, static_cast<std::uint32_t>(m_plays), 4);
        chunk(out, "acTL", control);
        std::uint32_t sequence = 0;
        for (std::size_t i = 0; i < m_frames.size(); ++i) {
            // A delay in milliseconds fits in 16 bits up to a minute, in hundredths up to 655.35
            // seconds, and in seconds up to 65535 of them, the longest an Animation has.
            const auto delay = static_cast<std::uint32_t>(m_delays[i]);
            std::uint32_t unit = 1;
            if (delay > 655350) {
                unit = 1000;
            } else if (delay > 65535) {
                unit = 10;
            }

            std::vector<std::uint8_t> frame;
            put_big(frame, sequence++, 4);
            put_big(frame, static_cast<std::uint32_t>(m_width), 4);
            put_big(frame, static_cast<std::uint32_t>(m_height), 4);
            put_big(frame, 0, 4);
            put_big(frame, 0, 4);
            put_big(frame, (delay + (unit / 2)) / unit, 2);
            put_big(frame, 1000 / unit, 2);
            // It's kept as it is, and the next frame replaces all of it.
            frame.insert(frame.end(), { 0, 0 });
            chunk(out, "fcTL", frame);
            if (i == 0) {
                data_chunks(out, "IDAT", m_frames[i], nullptr);
            } else {
                data_chunks(out, "fdAT", m_frames[i], &sequence);
            }
        }

        chunk(out, "IEND", {});
        m_frames.clear();
        m_delays.clear();
        return out;
    }

private:
    std::int64_t m_width;
    std::int64_t m_height;
    std::int32_t m_plays;
    int m_level;
    std::vector<std::vector<std::uint8_t>> m_frames;
    std::vector<std::int32_t> m_delays;
};

} // namespace

extern "C" {

void fb_zlib_png_encode(const std::uint8_t* pixels, const std::int64_t width,
                        const std::int64_t height, const std::int32_t level, fb_array_t* output) {
    set_elements<std::uint8_t>(output,
                               encode(pixels_at(pixels, width, height), width, height, level));
}

void* fb_zlib_apng_encoder_new(const std::int64_t width, const std::int64_t height,
                               const std::int32_t plays, const std::int32_t level) {
    return new apng_encoder_t(width, height, plays, level);
}

void fb_zlib_apng_encoder_add(void* encoder, const std::uint8_t* pixels, const std::int32_t delay) {
    auto* apng = static_cast<apng_encoder_t*>(encoder);
    apng->add(pixels_at(pixels, apng->width(), apng->height()), delay);
}

void fb_zlib_apng_encoder_finish(void* encoder, fb_array_t* output) {
    set_elements<std::uint8_t>(output, static_cast<apng_encoder_t*>(encoder)->finish());
}

void fb_zlib_apng_encoder_free(void* encoder) {
    delete static_cast<apng_encoder_t*>(encoder);
}
}
