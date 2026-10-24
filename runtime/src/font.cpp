// std.image's TrueType fonts, read by the runtime itself: the tables drawing text needs, head,
// hhea, maxp, hmtx, cmap's formats 4 and 12, loca and glyf, simple and composite glyphs, and
// kern's format 0, of a font file or a collection's first font. Every offset is checked against
// the bytes before it's followed.

#include "fb/image.hpp"
#include "fb/runtime.hpp"
#include "font_internal.hpp"
#include "image_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace fb::image {

namespace {

/// A table's tag, as four bytes, the first most significant.
constexpr std::uint32_t tag(const std::string_view name) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(name[3]));
}

/// How deep composite glyphs go, the parts of parts, which also ends a cycle of them.
constexpr int most_depth = 8;
/// The most points one glyph's outline has, all its parts' counted.
constexpr std::size_t most_points = std::size_t{ 1 } << 20U;
/// The most glyphs one glyph's outline visits, its parts and theirs, so a font whose composites
/// each have many parts, of parts of many, takes no longer than that.
constexpr std::size_t most_visits = std::size_t{ 1 } << 16U;

/// A composite part's flags.
constexpr std::uint32_t words = 0x0001;
constexpr std::uint32_t offsets = 0x0002;
constexpr std::uint32_t scale = 0x0008;
constexpr std::uint32_t more = 0x0020;
constexpr std::uint32_t two_scales = 0x0040;
constexpr std::uint32_t matrix = 0x0080;

/// A simple glyph's point flags.
constexpr std::uint32_t on_curve = 0x01;
constexpr std::uint32_t x_short = 0x02;
constexpr std::uint32_t y_short = 0x04;
constexpr std::uint32_t repeat = 0x08;
constexpr std::uint32_t x_same = 0x10;
constexpr std::uint32_t y_same = 0x20;

/// A point of a TrueType contour, placed: on its curve, or a quadratic's control off it.
struct contour_point_t {
    double x = 0;
    double y = 0;
    bool on = true;
};

/// Adds a TrueType contour to the path: from a point on its curve, its first, its last, or else
/// halfway between them, a line to each point on it, and a quadratic through each point off it,
/// to the next point on it, or to halfway to the next point off it.
void add_contour(const std::vector<contour_point_t>& contour, font_path_t& path) {
    if (contour.empty()) {
        return;
    }

    const contour_point_t first = contour.front();
    const contour_point_t last = contour.back();
    contour_point_t start = first;
    std::span<const contour_point_t> rest(contour.begin() + 1, contour.end());
    if (!first.on) {
        if (last.on) {
            start = last;
            rest = std::span<const contour_point_t>(contour.begin(), contour.end() - 1);
        } else {
            start = { .x = (last.x + first.x) / 2, .y = (last.y + first.y) / 2, .on = true };
            rest = contour;
        }
    }

    path.push_back({ .kind = font_segment_t::MOVE, .x = start.x, .y = start.y });
    contour_point_t off{};
    bool pending = false;
    const auto visit = [&](const contour_point_t p) {
        if (p.on) {
            if (pending) {
                path.push_back({
                    .kind = font_segment_t::QUADRATIC,
                    .x1 = off.x,
                    .y1 = off.y,
                    .x = p.x,
                    .y = p.y,
                });
                pending = false;
            } else {
                path.push_back({ .kind = font_segment_t::LINE, .x = p.x, .y = p.y });
            }
            return;
        }

        if (pending) {
            const double x = (off.x + p.x) / 2;
            const double y = (off.y + p.y) / 2;
            path.push_back(
                { .kind = font_segment_t::QUADRATIC, .x1 = off.x, .y1 = off.y, .x = x, .y = y });
        }
        off = p;
        pending = true;
    };
    for (const contour_point_t& p : rest) {
        visit(p);
    }
    visit(start);
}

/// A table the font has: where it starts in the bytes, and how long it is.
struct table_t {
    std::size_t at = 0;
    std::size_t size = 0;
    bool found = false;
};

} // namespace

bool font_t::has(const std::size_t at, const std::size_t count) const {
    return at <= m_bytes.size() && count <= m_bytes.size() - at;
}

std::uint32_t font_t::u8(const std::size_t at) const {
    return has(at, 1) ? m_bytes[at] : 0;
}

std::uint32_t font_t::u16(const std::size_t at) const {
    return has(at, 2) ? (std::uint32_t{ m_bytes[at] } << 8U) | m_bytes[at + 1] : 0;
}

int font_t::s16(const std::size_t at) const {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16(at)));
}

std::uint32_t font_t::u32(const std::size_t at) const {
    return (u16(at) << 16U) | u16(at + 2);
}

font_t::font_t(const std::span<const std::uint8_t> bytes) : m_bytes(bytes.begin(), bytes.end()) {
    if (!has(0, 4)) {
        error = "it ends before its first table";
        return;
    }

    // A collection's fonts are each a font file's tables; this reads its first.
    std::size_t start = 0;
    if (u32(0) == tag("ttcf")) {
        if (!has(0, 16) || u32(8) == 0) {
            error = "it's a collection of no fonts";
            return;
        }
        start = u32(12);
    }

    const std::uint32_t version = u32(start);
    if (version != 0x00010000 && version != tag("true") && version != tag("OTTO")) {
        error = "it isn't a TrueType or OpenType font";
        return;
    }
    find_tables(start);
}

bool font_t::find_tables(const std::size_t start) {
    if (!has(start, 12)) {
        error = "its table directory ends too soon";
        return false;
    }

    const std::size_t count = u16(start + 4);
    if (!has(start + 12, count * 16)) {
        error = "its table directory ends too soon";
        return false;
    }

    table_t head;
    table_t hhea;
    table_t maxp;
    table_t hmtx;
    table_t loca;
    table_t glyf;
    table_t cmap;
    table_t kern;
    table_t cff;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t record = start + 12 + (i * 16);
        const table_t table{ .at = u32(record + 8), .size = u32(record + 12), .found = true };
        if (!has(table.at, table.size)) {
            error = "a table goes past its end";
            return false;
        }

        const std::uint32_t name = u32(record);
        for (auto [wanted, slot] : {
                 std::pair{ tag("head"), &head },
                 std::pair{ tag("hhea"), &hhea },
                 std::pair{ tag("maxp"), &maxp },
                 std::pair{ tag("hmtx"), &hmtx },
                 std::pair{ tag("loca"), &loca },
                 std::pair{ tag("glyf"), &glyf },
                 std::pair{ tag("cmap"), &cmap },
                 std::pair{ tag("kern"), &kern },
                 std::pair{ tag("CFF "), &cff },
             }) {
            if (name == wanted) {
                *slot = table;
            }
        }
    }

    if (!head.found || !hhea.found || !maxp.found || !hmtx.found || !cmap.found) {
        error = "it lacks a table every font has";
        return false;
    }

    // Its outlines, TrueType's, in glyf.
    if (!loca.found || !glyf.found) {
        unsupported = true;
        error = cff.found ? "its outlines are CFF's, which std.image doesn't read"
                          : "it has no outlines";
        return false;
    }

    if (head.size < 54 || hhea.size < 36 || maxp.size < 6) {
        error = "its head, hhea, or maxp table is too short";
        return false;
    }

    m_units = static_cast<int>(u16(head.at + 18));
    const int loca_format = s16(head.at + 50);
    m_ascent = s16(hhea.at + 4);
    m_descent = s16(hhea.at + 6);
    m_line_gap = s16(hhea.at + 8);
    m_metrics = u16(hhea.at + 34);
    m_glyphs = u16(maxp.at + 4);
    if (m_units < 16 || m_units > 16384 || (loca_format != 0 && loca_format != 1)) {
        error = "its head table's units per em or glyph offsets aren't TrueType's";
        return false;
    }

    if (m_glyphs == 0 || m_metrics == 0 || m_metrics > m_glyphs ||
        hmtx.size < (std::size_t{ m_metrics } * 4) + ((std::size_t{ m_glyphs } - m_metrics) * 2)) {
        error = "its glyphs' metrics are missing";
        return false;
    }

    m_long_loca = loca_format == 1;
    if (loca.size < (std::size_t{ m_glyphs } + 1) * (m_long_loca ? 4 : 2)) {
        error = "its loca table is too short for its glyphs";
        return false;
    }

    m_hmtx = hmtx.at;
    m_loca = loca.at;
    m_glyf = glyf.at;
    m_glyf_size = glyf.size;

    // The cmap's subtable for Unicode: one of format 12, for every plane, or else one of format 4,
    // for the first.
    const std::size_t subtables = u16(cmap.at + 2);
    if (cmap.size < 4 + (subtables * 8)) {
        error = "its cmap table is too short";
        return false;
    }

    std::size_t best = 0;
    int best_rank = 0;
    for (std::size_t i = 0; i < subtables; ++i) {
        const std::size_t record = cmap.at + 4 + (i * 8);
        const std::uint32_t platform = u16(record);
        const std::uint32_t encoding = u16(record + 2);
        const std::size_t offset = u32(record + 4);
        if (offset >= cmap.size) {
            continue;
        }

        const std::size_t sub = cmap.at + offset;
        const std::uint32_t format = u16(sub);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        const bool symbol = platform == 3 && encoding == 0;
        int rank = 0;
        if (format == 12 && unicode && has(sub, 16) &&
            has(sub + 16, std::size_t{ u32(sub + 12) } * 12)) {
            rank = 3;
        } else if (format == 4 && (unicode || symbol) && has(sub, u16(sub + 2))) {
            rank = symbol ? 1 : 2;
        }
        if (rank > best_rank) {
            best = sub;
            best_rank = rank;
        }
    }

    if (best_rank == 0) {
        error = "its cmap table has no Unicode subtable of format 4 or 12";
        return false;
    }
    m_cmap = best;
    m_cmap_format = u16(best);

    // kern's first subtable of format 0, across, if it has one.
    if (kern.found && kern.size >= 4 && u16(kern.at) == 0) {
        const std::size_t tables = u16(kern.at + 2);
        std::size_t at = kern.at + 4;
        for (std::size_t i = 0; i < tables && has(at, 6); ++i) {
            const std::uint32_t length = u16(at + 2);
            const std::uint32_t coverage = u16(at + 4);
            // Format 0, horizontal, not a minimum, not across the line.
            if ((coverage >> 8U) == 0 && (coverage & 0x07U) == 0x01) {
                const std::size_t pairs = u16(at + 6);
                if (has(at + 14, pairs * 6)) {
                    m_kern = at + 14;
                    m_kern_pairs = pairs;
                }
                break;
            }
            if (length < 6) {
                break;
            }
            at += length;
        }
    }
    return true;
}

std::uint32_t font_t::glyph_of(const std::uint32_t code_point) const {
    std::uint32_t glyph = 0;
    if (m_cmap_format == 12) {
        const std::size_t groups = u32(m_cmap + 12);
        std::size_t low = 0;
        std::size_t high = groups;
        while (low < high) {
            const std::size_t middle = low + ((high - low) / 2);
            const std::size_t group = m_cmap + 16 + (middle * 12);
            const std::uint32_t first = u32(group);
            const std::uint32_t last = u32(group + 4);
            if (code_point < first) {
                high = middle;
            } else if (code_point > last) {
                low = middle + 1;
            } else {
                glyph = u32(group + 8) + (code_point - first);
                break;
            }
        }
    } else if (code_point <= 0xFFFF) {
        const std::size_t segments = u16(m_cmap + 6) / 2;
        const std::size_t ends = m_cmap + 14;
        const std::size_t starts = ends + (segments * 2) + 2;
        const std::size_t deltas = starts + (segments * 2);
        const std::size_t ranges = deltas + (segments * 2);
        if (!has(ranges, segments * 2)) {
            return 0;
        }

        // The first segment that ends at or after the code point.
        std::size_t low = 0;
        std::size_t high = segments;
        while (low < high) {
            const std::size_t middle = low + ((high - low) / 2);
            if (u16(ends + (middle * 2)) < code_point) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        if (low < segments && u16(starts + (low * 2)) <= code_point) {
            const std::uint32_t delta = u16(deltas + (low * 2));
            const std::size_t range_at = ranges + (low * 2);
            const std::uint32_t range = u16(range_at);
            if (range == 0) {
                glyph = (code_point + delta) & 0xFFFFU;
            } else {
                const std::size_t at =
                    range_at + range + (std::size_t{ code_point - u16(starts + (low * 2)) } * 2);
                const std::uint32_t found = has(at, 2) ? u16(at) : 0;
                glyph = found == 0 ? 0 : (found + delta) & 0xFFFFU;
            }
        }
    }
    return glyph < m_glyphs ? glyph : 0;
}

int font_t::advance_of(const std::uint32_t glyph) const {
    const std::uint32_t index = std::min(glyph, m_metrics - 1);
    return static_cast<int>(u16(m_hmtx + (std::size_t{ index } * 4)));
}

int font_t::kerning(const std::uint32_t left, const std::uint32_t right) const {
    const std::uint32_t key = (left << 16U) | right;
    std::size_t low = 0;
    std::size_t high = m_kern_pairs;
    while (low < high) {
        const std::size_t middle = low + ((high - low) / 2);
        const std::size_t pair = m_kern + (middle * 6);
        const std::uint32_t found = u32(pair);
        if (found < key) {
            low = middle + 1;
        } else if (found > key) {
            high = middle;
        } else {
            return s16(pair + 4);
        }
    }
    return 0;
}

bool font_t::glyph_range(const std::uint32_t glyph, std::size_t& at, std::size_t& size) const {
    if (glyph >= m_glyphs) {
        return false;
    }

    std::size_t start = 0;
    std::size_t end = 0;
    if (m_long_loca) {
        start = u32(m_loca + (std::size_t{ glyph } * 4));
        end = u32(m_loca + (std::size_t{ glyph } * 4) + 4);
    } else {
        start = std::size_t{ u16(m_loca + (std::size_t{ glyph } * 2)) } * 2;
        end = std::size_t{ u16(m_loca + (std::size_t{ glyph } * 2) + 2) } * 2;
    }

    if (end < start || end > m_glyf_size) {
        return false;
    }
    at = m_glyf + start;
    size = end - start;
    return true;
}

font_path_t font_t::outline(const std::uint32_t glyph) const {
    font_path_t path;
    std::size_t visits = 0;
    add_outline(glyph, { 1, 0, 0, 1, 0, 0 }, 0, path, visits);
    return path;
}

void font_t::add_outline(const std::uint32_t glyph, const std::array<double, 6>& transform,
                         const int depth, font_path_t& path, std::size_t& visits) const {
    std::size_t at = 0;
    std::size_t size = 0;
    if (depth > most_depth || ++visits > most_visits || !glyph_range(glyph, at, size) ||
        size < 10) {
        return;
    }

    const std::size_t end = at + size;
    const int count = s16(at);
    if (count >= 0) {
        const auto contour_count = static_cast<std::size_t>(count);
        const std::size_t ends = at + 10;
        if (contour_count == 0 || ends + (contour_count * 2) + 2 > end) {
            return;
        }

        std::vector<std::size_t> last_points;
        for (std::size_t i = 0; i < contour_count; ++i) {
            const std::size_t last = u16(ends + (i * 2));
            if (!last_points.empty() && last <= last_points.back()) {
                return;
            }
            last_points.push_back(last);
        }

        const std::size_t points = last_points.back() + 1;
        if (path.size() + points > most_points) {
            return;
        }

        std::size_t cursor = ends + (contour_count * 2);
        cursor += 2 + u16(cursor);
        std::vector<std::uint32_t> flags;
        while (flags.size() < points) {
            if (cursor >= end) {
                return;
            }
            const std::uint32_t flag = u8(cursor++);
            flags.push_back(flag);
            if ((flag & repeat) != 0) {
                if (cursor >= end) {
                    return;
                }
                for (std::uint32_t times = u8(cursor++); times > 0 && flags.size() < points;
                     --times) {
                    flags.push_back(flag);
                }
            }
        }

        // Each coordinate is the last one plus a delta: a byte and its sign, the same, or a word.
        std::vector<int> xs(points);
        std::vector<int> ys(points);
        for (const auto& [values, short_bit, same_bit] :
             { std::tuple{ &xs, x_short, x_same }, std::tuple{ &ys, y_short, y_same } }) {
            int value = 0;
            for (std::size_t i = 0; i < points; ++i) {
                const std::uint32_t flag = flags[i];
                if ((flag & short_bit) != 0) {
                    if (cursor + 1 > end) {
                        return;
                    }
                    const auto delta = static_cast<int>(u8(cursor++));
                    value += (flag & same_bit) != 0 ? delta : -delta;
                } else if ((flag & same_bit) == 0) {
                    if (cursor + 2 > end) {
                        return;
                    }
                    value += s16(cursor);
                    cursor += 2;
                }
                (*values)[i] = value;
            }
        }

        std::size_t first = 0;
        for (const std::size_t last : last_points) {
            std::vector<contour_point_t> contour;
            for (std::size_t i = first; i <= last; ++i) {
                const double x = xs[i];
                const double y = ys[i];
                contour.push_back({
                    .x = (transform[0] * x) + (transform[2] * y) + transform[4],
                    .y = (transform[1] * x) + (transform[3] * y) + transform[5],
                    .on = (flags[i] & on_curve) != 0,
                });
            }
            first = last + 1;
            add_contour(contour, path);
        }
        return;
    }

    // A composite: its parts, each a glyph moved, and maybe scaled or turned, as a matrix of
    // 2.14 fixed-point numbers.
    std::size_t cursor = at + 10;
    std::uint32_t flags = more;
    while ((flags & more) != 0) {
        if (cursor + 4 > end) {
            return;
        }
        flags = u16(cursor);
        const std::uint32_t part = u16(cursor + 2);
        cursor += 4;
        double dx = 0;
        double dy = 0;
        const std::size_t arguments = (flags & words) != 0 ? 4 : 2;
        if (cursor + arguments > end) {
            return;
        }
        if ((flags & words) != 0) {
            dx = s16(cursor);
            dy = s16(cursor + 2);
        } else {
            dx = static_cast<std::int8_t>(static_cast<std::uint8_t>(u8(cursor)));
            dy = static_cast<std::int8_t>(static_cast<std::uint8_t>(u8(cursor + 1)));
        }
        cursor += arguments;
        // Points matched rather than an offset are put where they are.
        if ((flags & offsets) == 0) {
            dx = 0;
            dy = 0;
        }

        std::array<double, 4> m{ 1, 0, 0, 1 };
        const auto fixed = [&](const std::size_t where) { return s16(where) / 16384.0; };
        if ((flags & scale) != 0) {
            if (cursor + 2 > end) {
                return;
            }
            m[0] = m[3] = fixed(cursor);
            cursor += 2;
        } else if ((flags & two_scales) != 0) {
            if (cursor + 4 > end) {
                return;
            }
            m[0] = fixed(cursor);
            m[3] = fixed(cursor + 2);
            cursor += 4;
        } else if ((flags & matrix) != 0) {
            if (cursor + 8 > end) {
                return;
            }
            m = { fixed(cursor), fixed(cursor + 2), fixed(cursor + 4), fixed(cursor + 6) };
            cursor += 8;
        }

        // The part's points go through its matrix and offset, then this glyph's transform.
        const std::array<double, 6> combined{
            (transform[0] * m[0]) + (transform[2] * m[1]),
            (transform[1] * m[0]) + (transform[3] * m[1]),
            (transform[0] * m[2]) + (transform[2] * m[3]),
            (transform[1] * m[2]) + (transform[3] * m[3]),
            (transform[0] * dx) + (transform[2] * dy) + transform[4],
            (transform[1] * dx) + (transform[3] * dy) + transform[5],
        };
        add_outline(part, combined, depth + 1, path, visits);
    }
}

} // namespace fb::image

extern "C" {

void* fb_font_parse(const fb_array_t* data) {
    return new fb::image::font_t(fb::image::bytes_of(data));
}

fb_string_t* fb_font_error(const void* font) {
    const std::string& error = static_cast<const fb::image::font_t*>(font)->error;
    return fb_string_from_bytes(error.data(), static_cast<std::int64_t>(error.size()));
}

std::int32_t fb_font_unsupported(const void* font) {
    return static_cast<const fb::image::font_t*>(font)->unsupported ? 1 : 0;
}

std::int32_t fb_font_units(const void* font) {
    return static_cast<const fb::image::font_t*>(font)->units_per_em();
}

std::int32_t fb_font_ascent(const void* font) {
    return static_cast<const fb::image::font_t*>(font)->ascent();
}

std::int32_t fb_font_descent(const void* font) {
    return static_cast<const fb::image::font_t*>(font)->descent();
}

std::int32_t fb_font_line_gap(const void* font) {
    return static_cast<const fb::image::font_t*>(font)->line_gap();
}

std::int32_t fb_font_has(const void* font, const std::uint32_t code_point) {
    return static_cast<const fb::image::font_t*>(font)->glyph_of(code_point) != 0 ? 1 : 0;
}

void fb_font_free(void* font) {
    delete static_cast<fb::image::font_t*>(font);
}
}
