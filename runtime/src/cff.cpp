// std.image's CFF outlines, an OpenType font's 'CFF ' table, read by the runtime itself: its
// header, INDEXes, and DICTs, each offset checked against the table, a CID font's Font DICTs
// and FDSelect, and each glyph's Type 2 charstring, run with its local and global subroutines,
// its arithmetic, and its storage into a path of lines and cubics, and an accented character
// of endchar's, which Type 2 deprecates, from its base and accent glyphs, by StandardEncoding and
// the charset. A charstring that does what Type 2 doesn't allow gives its glyph no outline.

#include "font_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace fb::image {

namespace {

/// The most numbers a charstring's stack holds, as Type 2 has it.
constexpr std::size_t most_stack = 48;
/// How deep subroutines call each other, as Type 2 has it.
constexpr int most_depth = 10;
/// How many numbers put and get store, as Type 2 has it.
constexpr std::size_t most_transient = 32;
/// How far from the origin a glyph's points may be, in font units, as its arithmetic could put
/// them anywhere: far past any em, but near enough that drawing them can't overflow a Double.
constexpr double most_units = 0x1p24;
/// The most numbers and operators one glyph's charstring runs, its subroutines' counted.
constexpr std::size_t most_steps = std::size_t{ 1 } << 20U;

/// What's added to a subroutine's number, by how many there are, as Type 2 has it.
int bias_of(const std::size_t count) {
    if (count < 1240) {
        return 107;
    }
    return count < 33900 ? 1131 : 32768;
}

/// The DICT operators std.image reads: one byte, or 12 and one, as 1200 and it.
constexpr int op_charset = 15;
constexpr int op_charstrings = 17;
constexpr int op_private = 18;
constexpr int op_subrs = 19;
constexpr int op_charstring_type = 1206;
constexpr int op_ros = 1230;
constexpr int op_fd_array = 1236;
constexpr int op_fd_select = 1237;

/// A DICT's operators and their operands, the last of each it has.
struct dict_t {
    std::vector<std::pair<int, std::vector<double>>> entries;

    [[nodiscard]] const std::vector<double>* find(const int op) const {
        for (const auto& [entry_op, operands] : std::views::reverse(entries)) {
            if (entry_op == op) {
                return &operands;
            }
        }
        return nullptr;
    }
};

} // namespace

bool font_t::read_index(const std::size_t at, cff_index_t& index, std::size_t& after) const {
    if (at + 2 > m_cff_end) {
        return false;
    }

    index = {};
    index.count = u16(at);
    if (index.count == 0) {
        after = at + 2;
        return true;
    }

    index.width = u8(at + 2);
    index.offsets = at + 3;
    if (index.width < 1 || index.width > 4 ||
        index.offsets + ((index.count + 1) * index.width) > m_cff_end) {
        return false;
    }

    index.data = index.offsets + ((index.count + 1) * index.width) - 1;
    std::size_t last = 0;
    for (std::size_t i = 0; i < index.width; ++i) {
        last = (last << 8U) | u8(index.offsets + (index.count * index.width) + i);
    }

    if (last < 1 || index.data + last > m_cff_end) {
        return false;
    }
    after = index.data + last;
    return true;
}

bool font_t::entry(const cff_index_t& index, const std::size_t i, std::size_t& at,
                   std::size_t& size) const {
    if (i >= index.count) {
        return false;
    }

    std::size_t start = 0;
    std::size_t end = 0;
    for (std::size_t b = 0; b < index.width; ++b) {
        start = (start << 8U) | u8(index.offsets + (i * index.width) + b);
        end = (end << 8U) | u8(index.offsets + ((i + 1) * index.width) + b);
    }

    if (start < 1 || end < start || index.data + end > m_cff_end) {
        return false;
    }
    at = index.data + start;
    size = end - start;
    return true;
}

namespace {

/// A DICT's operators and operands, from `at` until `end` in the font's bytes, which the
/// caller checked are there, or false when it's broken.
template <typename Byte>
bool parse_dict(const Byte& byte, const std::size_t at, const std::size_t end, dict_t& dict) {
    std::vector<double> operands;
    std::size_t cursor = at;
    while (cursor < end) {
        const std::uint32_t b0 = byte(cursor++);
        if (b0 <= 21) {
            int op = static_cast<int>(b0);
            if (b0 == 12) {
                if (cursor >= end) {
                    return false;
                }
                op = 1200 + static_cast<int>(byte(cursor++));
            }
            dict.entries.emplace_back(op, std::move(operands));
            operands.clear();
        } else if (b0 == 28 || b0 == 29) {
            const std::size_t size = b0 == 28 ? 2 : 4;
            if (cursor + size > end) {
                return false;
            }
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < size; ++i) {
                value = (value << 8U) | byte(cursor++);
            }
            operands.push_back(b0 == 28 ? static_cast<std::int16_t>(value)
                                        : static_cast<std::int32_t>(value));
        } else if (b0 == 30) {
            // A real, in nibbles up to one of 0xf: none std.image reads is one, so it's 0.
            bool ended = false;
            while (!ended && cursor < end) {
                const std::uint32_t nibbles = byte(cursor++);
                ended = (nibbles & 0x0FU) == 0x0F || (nibbles >> 4U) == 0x0F;
            }
            operands.push_back(0);
        } else if (b0 >= 32 && b0 <= 246) {
            operands.push_back(static_cast<int>(b0) - 139);
        } else if (b0 >= 247 && b0 <= 254) {
            if (cursor >= end) {
                return false;
            }
            const auto b1 = static_cast<int>(byte(cursor++));
            operands.push_back(b0 <= 250 ? ((static_cast<int>(b0) - 247) * 256) + b1 + 108
                                         : -((static_cast<int>(b0) - 251) * 256) - b1 - 108);
        } else {
            return false;
        }

        if (operands.size() > most_stack) {
            return false;
        }
    }
    return true;
}

} // namespace

bool font_t::read_cff(const std::size_t at, const std::size_t size) {
    m_cff_end = at + size;
    const auto byte = [this](const std::size_t where) { return u8(where); };
    if (size < 4 || u8(at) != 1 || u8(at + 2) < 4) {
        error = "its CFF table's header isn't CFF's";
        return false;
    }

    // The Name INDEX, the Top DICT INDEX, the String INDEX, and the Global Subr INDEX.
    cff_index_t names;
    cff_index_t tops;
    cff_index_t strings;
    std::size_t after = 0;
    if (!read_index(at + u8(at + 2), names, after) || !read_index(after, tops, after) ||
        !read_index(after, strings, after) || !read_index(after, m_global_subrs, after)) {
        error = "its CFF table's INDEXes are broken";
        return false;
    }

    std::size_t top_at = 0;
    std::size_t top_size = 0;
    dict_t top;
    if (!entry(tops, 0, top_at, top_size) || !parse_dict(byte, top_at, top_at + top_size, top)) {
        error = "its CFF table's Top DICT is broken";
        return false;
    }

    const std::vector<double>* type = top.find(op_charstring_type);
    if (type != nullptr && (type->size() != 1 || (*type)[0] != 2)) {
        unsupported = true;
        error = "its charstrings aren't Type 2's";
        return false;
    }

    const std::vector<double>* charstrings = top.find(op_charstrings);
    if (charstrings == nullptr || charstrings->size() != 1 || (*charstrings)[0] < 0 ||
        !read_index(at + static_cast<std::size_t>((*charstrings)[0]), m_charstrings, after)) {
        error = "its CFF table's CharStrings INDEX is broken";
        return false;
    }

    // A Private DICT's local subroutines, from its Subrs offset, which counts from the DICT.
    const auto local_subrs = [&](const dict_t& font, cff_index_t& subrs) {
        subrs = {};
        const std::vector<double>* place = font.find(op_private);
        if (place == nullptr) {
            return true;
        }
        if (place->size() != 2 || (*place)[0] < 0 || (*place)[1] < 0) {
            return false;
        }

        const auto private_size = static_cast<std::size_t>((*place)[0]);
        const std::size_t private_at = at + static_cast<std::size_t>((*place)[1]);
        dict_t privates;
        if (private_at + private_size > m_cff_end ||
            !parse_dict(byte, private_at, private_at + private_size, privates)) {
            return false;
        }

        const std::vector<double>* offset = privates.find(op_subrs);
        if (offset == nullptr) {
            return true;
        }
        std::size_t ignored = 0;
        return offset->size() == 1 && (*offset)[0] >= 0 &&
               read_index(private_at + static_cast<std::size_t>((*offset)[0]), subrs, ignored);
    };

    if (top.find(op_ros) == nullptr) {
        cff_index_t subrs;
        if (!local_subrs(top, subrs)) {
            error = "its CFF table's Private DICT is broken";
            return false;
        }
        m_local_subrs.push_back(subrs);

        // Its charset: ISOAdobe's, 0, an expert one, 1 or 2, or its own, at the offset.
        const std::vector<double>* charset = top.find(op_charset);
        if (charset != nullptr && (charset->size() != 1 || (*charset)[0] < 0)) {
            error = "its CFF table's charset is broken";
            return false;
        }
        const double offset = charset != nullptr ? (*charset)[0] : 0;
        m_expert_charset = offset == 1 || offset == 2;
        m_charset = offset > 2 ? at + static_cast<std::size_t>(offset) : 0;
        m_cff = true;
        return true;
    }

    // A CID font: a Font DICT for each FDSelect gives a glyph, each with its own Private DICT.
    const std::vector<double>* fd_array = top.find(op_fd_array);
    const std::vector<double>* fd_select = top.find(op_fd_select);
    cff_index_t fonts;
    if (fd_array == nullptr || fd_select == nullptr || fd_array->size() != 1 ||
        fd_select->size() != 1 || (*fd_array)[0] < 0 || (*fd_select)[0] < 0 ||
        !read_index(at + static_cast<std::size_t>((*fd_array)[0]), fonts, after)) {
        error = "its CFF table's FDArray is broken";
        return false;
    }

    for (std::size_t i = 0; i < fonts.count; ++i) {
        std::size_t font_at = 0;
        std::size_t font_size = 0;
        dict_t font;
        cff_index_t subrs;
        if (!entry(fonts, i, font_at, font_size) ||
            !parse_dict(byte, font_at, font_at + font_size, font) || !local_subrs(font, subrs)) {
            error = "its CFF table's Font DICTs are broken";
            return false;
        }
        m_local_subrs.push_back(subrs);
    }

    m_fd_select = at + static_cast<std::size_t>((*fd_select)[0]);
    const std::uint32_t format = u8(m_fd_select);
    if (m_fd_select >= m_cff_end || (format != 0 && format != 3)) {
        error = "its CFF table's FDSelect isn't of format 0 or 3";
        return false;
    }
    m_cff = true;
    return true;
}

std::size_t font_t::font_dict_of(const std::uint32_t glyph) const {
    if (m_fd_select == 0) {
        return 0;
    }

    if (u8(m_fd_select) == 0) {
        return m_fd_select + 1 + glyph < m_cff_end ? u8(m_fd_select + 1 + glyph) : 0;
    }

    // Format 3: ranges of glyphs from each's first to the next's, the last ended by a sentinel.
    const std::size_t ranges = u16(m_fd_select + 1);
    std::size_t found = 0;
    for (std::size_t i = 0; i < ranges; ++i) {
        const std::size_t range = m_fd_select + 3 + (i * 3);
        if (range + 5 > m_cff_end) {
            break;
        }
        if (u16(range) > glyph) {
            break;
        }
        found = u8(range + 2);
    }
    return found;
}

namespace {

/// The SID of the glyph name StandardEncoding gives a code, as CFF's Appendix B has it, or 0
/// when it gives none: from 32 to 126 the SIDs from 1, and from 161 the rest, from 96.
std::uint32_t standard_sid(const std::uint32_t code) {
    static constexpr std::array<std::uint8_t, 91> from_161{
        96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 0,
        111, 112, 113, 114, 0,   115, 116, 117, 118, 119, 120, 121, 122, 0,   123, 0,
        124, 125, 126, 127, 128, 129, 130, 131, 0,   132, 133, 0,   134, 135, 136, 137,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        138, 0,   139, 0,   0,   0,   0,   140, 141, 142, 143, 0,   0,   0,   0,   0,
        144, 0,   0,   0,   145, 0,   0,   146, 147, 148, 149,
    };
    if (code >= 32 && code <= 126) {
        return code - 31;
    }
    return code >= 161 && code <= 251 ? from_161[code - 161] : 0;
}

} // namespace

std::optional<std::uint32_t> font_t::standard_glyph(const double code) const {
    if (m_fd_select != 0 || m_expert_charset || code < 0 || code > 255 ||
        code != std::trunc(code)) {
        return std::nullopt;
    }

    const std::uint32_t sid = standard_sid(static_cast<std::uint32_t>(code));
    const std::size_t glyphs = m_charstrings.count;
    if (sid == 0) {
        return std::nullopt;
    }
    if (m_charset == 0) {
        // ISOAdobe's: each glyph's SID is its number, up to 228.
        return sid < glyphs && sid <= 228 ? std::optional<std::uint32_t>(sid) : std::nullopt;
    }

    // The charset's own: from the second glyph, as the first is .notdef, each's SID, of format
    // 0, or ranges of SIDs, each its first and how many more, a byte of format 1 or two of 2.
    const std::uint32_t format = u8(m_charset);
    std::size_t cursor = m_charset + 1;
    for (std::size_t glyph = 1; glyph < glyphs;) {
        if (format == 0) {
            if (cursor + 2 > m_cff_end) {
                break;
            }
            if (u16(cursor) == sid) {
                return static_cast<std::uint32_t>(glyph);
            }
            cursor += 2;
            ++glyph;
            continue;
        }

        const std::size_t size = format == 1 ? 3 : 4;
        if ((format != 1 && format != 2) || cursor + size > m_cff_end) {
            break;
        }
        const std::uint32_t first = u16(cursor);
        const std::size_t more = format == 1 ? u8(cursor + 2) : u16(cursor + 2);
        if (sid >= first && sid - first <= more && glyph + (sid - first) < glyphs) {
            return static_cast<std::uint32_t>(glyph + (sid - first));
        }
        cursor += size;
        glyph += more + 1;
    }
    return std::nullopt;
}

namespace {

/// Runs Type 2 charstrings into a path, relative moves, lines, and curves made absolute, from
/// where it starts.
class charstring_t {
public:
    charstring_t(const cff_index_t& global, const cff_index_t& local, font_path_t& path,
                 const double x, const double y)
        : m_global(global), m_local(local), m_path(path), m_x(x), m_y(y) {}

    /// Whether the glyph ended as an accented character, endchar's base and accent, and if so,
    /// the accent's offset and their StandardEncoding codes.
    [[nodiscard]] bool accented(std::array<double, 4>& accent) const {
        accent = m_accent;
        return m_accented;
    }

    /// Runs the charstring from `at`, `size` bytes, a subroutine `depth` deep, which `byte`
    /// reads and `entry` finds subroutines for. Gives false when it's broken; true when it
    /// ends, or returns from a subroutine.
    template <typename Byte, typename Entry>
    bool run(const Byte& byte, const Entry& entry, const std::size_t at, const std::size_t size,
             const int depth) {
        if (depth > most_depth) {
            return false;
        }

        std::size_t cursor = at;
        const std::size_t end = at + size;
        while (cursor < end) {
            if (++m_steps > most_steps) {
                return false;
            }

            const std::uint32_t b0 = byte(cursor++);
            if (b0 >= 32 || b0 == 28) {
                double value = 0;
                if (b0 == 28) {
                    if (cursor + 2 > end) {
                        return false;
                    }
                    value = static_cast<std::int16_t>((byte(cursor) << 8U) | byte(cursor + 1));
                    cursor += 2;
                } else if (b0 <= 246) {
                    value = static_cast<int>(b0) - 139;
                } else if (b0 <= 254) {
                    if (cursor >= end) {
                        return false;
                    }
                    const auto b1 = static_cast<int>(byte(cursor++));
                    value = b0 <= 250 ? ((static_cast<int>(b0) - 247) * 256) + b1 + 108
                                      : -((static_cast<int>(b0) - 251) * 256) - b1 - 108;
                } else {
                    if (cursor + 4 > end) {
                        return false;
                    }
                    const std::uint32_t fixed = (byte(cursor) << 24U) | (byte(cursor + 1) << 16U) |
                                                (byte(cursor + 2) << 8U) | byte(cursor + 3);
                    value = static_cast<std::int32_t>(fixed) / 65536.0;
                    cursor += 4;
                }
                if (m_count >= most_stack) {
                    return false;
                }
                m_stack[m_count++] = value;
                continue;
            }

            if (b0 == 10 || b0 == 29) {
                // callsubr and callgsubr: the number on top, biased, is the subroutine's.
                if (m_count == 0) {
                    return false;
                }
                const cff_index_t& subrs = b0 == 10 ? m_local : m_global;
                const double number = m_stack[--m_count] + bias_of(subrs.count);
                std::size_t subr_at = 0;
                std::size_t subr_size = 0;
                if (number < 0 || number >= static_cast<double>(subrs.count) ||
                    !entry(subrs, static_cast<std::size_t>(number), subr_at, subr_size) ||
                    !run(byte, entry, subr_at, subr_size, depth + 1)) {
                    return false;
                }
                if (m_ended) {
                    return true;
                }
                continue;
            }

            if (b0 == 11) {
                // return, to the charstring that called this one.
                return depth > 0;
            }

            std::uint32_t op = b0;
            if (b0 == 12) {
                if (cursor >= end) {
                    return false;
                }
                op = 1200 + byte(cursor++);
            }

            if (op >= 1203 && op <= 1230) {
                if (!compute(op)) {
                    return false;
                }
                continue;
            }

            if (op == 19 || op == 20) {
                // hintmask and cntrmask: stems the stack has are vstems, then a bit each.
                take_width(true);
                m_stems += (m_count - m_first) / 2;
                cursor += (m_stems + 7) / 8;
                if (cursor > end) {
                    return false;
                }
                m_count = 0;
                continue;
            }

            if (!operate(op)) {
                return false;
            }
            if (m_ended) {
                return true;
            }
        }
        // A charstring that ends without endchar, or a subroutine without return, as readers
        // let them.
        return true;
    }

private:
    const cff_index_t& m_global;
    const cff_index_t& m_local;
    font_path_t& m_path;
    std::array<double, most_stack> m_stack{};
    std::size_t m_count = 0;
    /// What put stores and get gives.
    std::array<double, most_transient> m_transient{};
    /// random's state, the same for each glyph, so every target draws it the same.
    std::uint32_t m_random = 0x2545F491;
    /// Where the operator's own operands start: past the width, if the stack had it.
    std::size_t m_first = 0;
    bool m_width_seen = false;
    std::size_t m_stems = 0;
    std::size_t m_steps = 0;
    bool m_open = false;
    bool m_ended = false;
    bool m_accented = false;
    std::array<double, 4> m_accent{};
    double m_x = 0;
    double m_y = 0;

    /// The first operator that clears the stack takes the glyph's width first, when the stack
    /// has one more than it would otherwise: an odd count, for the stems' pairs.
    void take_width(const bool odd) {
        m_first = 0;
        if (!m_width_seen) {
            m_width_seen = true;
            if (odd && m_count % 2 == 1) {
                m_first = 1;
            }
        }
    }

    /// The same, for an operator of `operands` and no more, or, with `alternative`, of that many
    /// instead.
    void take_width_beyond(const std::size_t operands, const std::size_t alternative = 0) {
        m_first = 0;
        if (!m_width_seen) {
            m_width_seen = true;
            if (m_count == operands + 1 || (alternative != 0 && m_count == alternative + 1)) {
                m_first = 1;
            }
        }
    }

    [[nodiscard]] std::span<const double> operands() const {
        return { m_stack.data() + m_first, m_count - m_first };
    }

    /// Whether a point is as near the origin as a glyph's may be.
    [[nodiscard]] static bool near(const double x, const double y) {
        return std::abs(x) <= most_units && std::abs(y) <= most_units;
    }

    bool move(const double dx, const double dy) {
        m_x += dx;
        m_y += dy;
        m_path.push_back({ .kind = font_segment_t::MOVE, .x = m_x, .y = m_y });
        m_open = true;
        return near(m_x, m_y);
    }

    bool line(const double dx, const double dy) {
        if (!m_open) {
            return false;
        }
        m_x += dx;
        m_y += dy;
        m_path.push_back({ .kind = font_segment_t::LINE, .x = m_x, .y = m_y });
        return near(m_x, m_y);
    }

    bool curve(const double dx1, const double dy1, const double dx2, const double dy2,
               const double dx3, const double dy3) {
        if (!m_open) {
            return false;
        }
        const double x1 = m_x + dx1;
        const double y1 = m_y + dy1;
        const double x2 = x1 + dx2;
        const double y2 = y1 + dy2;
        m_x = x2 + dx3;
        m_y = y2 + dy3;
        m_path.push_back({
            .kind = font_segment_t::CUBIC,
            .x1 = x1,
            .y1 = y1,
            .x2 = x2,
            .y2 = y2,
            .x = m_x,
            .y = m_y,
        });
        return near(x1, y1) && near(x2, y2) && near(m_x, m_y);
    }

    bool operate(const std::uint32_t op) {
        bool fine = true;
        switch (op) {
        case 1:
        case 3:
        case 18:
        case 23: {
            // hstem, vstem, hstemhm, and vstemhm: hints, which only count for a hintmask.
            take_width(true);
            m_stems += (m_count - m_first) / 2;
            break;
        }
        case 21: {
            take_width_beyond(2);
            const std::span<const double> s = operands();
            fine = s.size() >= 2 && move(s[0], s[1]);
            break;
        }
        case 22:
        case 4: {
            take_width_beyond(1);
            const std::span<const double> s = operands();
            fine = !s.empty() && move(op == 22 ? s[0] : 0, op == 22 ? 0 : s[0]);
            break;
        }
        case 5: {
            const std::span<const double> s = operands_of_path();
            fine = s.size() >= 2;
            for (std::size_t i = 0; fine && i + 1 < s.size(); i += 2) {
                fine = line(s[i], s[i + 1]);
            }
            break;
        }
        case 6:
        case 7: {
            // hlineto and vlineto: lines across and down by turns.
            const std::span<const double> s = operands_of_path();
            fine = !s.empty();
            bool across = op == 6;
            for (std::size_t i = 0; fine && i < s.size(); ++i, across = !across) {
                fine = across ? line(s[i], 0) : line(0, s[i]);
            }
            break;
        }
        case 8: {
            const std::span<const double> s = operands_of_path();
            fine = s.size() >= 6;
            for (std::size_t i = 0; fine && i + 5 < s.size(); i += 6) {
                fine = curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            }
            break;
        }
        case 24: {
            // rcurveline: curves, then a line.
            const std::span<const double> s = operands_of_path();
            fine = s.size() >= 8;
            std::size_t i = 0;
            for (; fine && i + 5 < s.size() - 2; i += 6) {
                fine = curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            }
            fine = fine && i + 1 < s.size() && line(s[i], s[i + 1]);
            break;
        }
        case 25: {
            // rlinecurve: lines, then a curve.
            const std::span<const double> s = operands_of_path();
            fine = s.size() >= 8;
            std::size_t i = 0;
            for (; fine && i + 1 < s.size() - 6; i += 2) {
                fine = line(s[i], s[i + 1]);
            }
            fine = fine && i + 5 < s.size() &&
                   curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            break;
        }
        case 26:
        case 27: {
            // vvcurveto and hhcurveto: curves that start and end down, or across, the first with
            // a little the other way, if the count is odd.
            const std::span<const double> s = operands_of_path();
            fine = s.size() >= 4;
            std::size_t i = 0;
            double first = 0;
            if (s.size() % 2 == 1) {
                first = s[0];
                i = 1;
            }
            for (; fine && i + 3 < s.size(); i += 4) {
                fine = op == 27 ? curve(s[i], first, s[i + 1], s[i + 2], s[i + 3], 0)
                                : curve(first, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
                first = 0;
            }
            break;
        }
        case 30:
        case 31: {
            // vhcurveto and hvcurveto: curves that turn from down to across, or across to down,
            // by turns, the last with a little more its way if there's one more.
            const std::span<const double> s = operands_of_path();
            fine = s.size() >= 4;
            bool across = op == 31;
            for (std::size_t i = 0; fine && i + 3 < s.size(); i += 4, across = !across) {
                const double last = s.size() - i == 5 ? s[i + 4] : 0;
                fine = across ? curve(s[i], 0, s[i + 1], s[i + 2], last, s[i + 3])
                              : curve(0, s[i], s[i + 1], s[i + 2], s[i + 3], last);
            }
            break;
        }
        case 14: {
            // endchar: the glyph is done, and with four operands, it's an accented character's,
            // as Type 1's seac was, which Type 2 deprecates.
            take_width_beyond(0, 4);
            const std::span<const double> s = operands();
            if (s.size() == 4) {
                m_accented = true;
                std::ranges::copy(s, m_accent.begin());
            }
            m_ended = true;
            break;
        }
        case 1200:
            // dotsection, which Type 2 deprecates, and does nothing.
            break;
        case 1235: {
            // flex: two curves, by 13 operands, the last how flat they may be drawn.
            const std::span<const double> s = operands_of_path();
            fine = s.size() == 13 && curve(s[0], s[1], s[2], s[3], s[4], s[5]) &&
                   curve(s[6], s[7], s[8], s[9], s[10], s[11]);
            break;
        }
        case 1234: {
            // hflex: two curves across, the second back down as far as the first goes up.
            const std::span<const double> s = operands_of_path();
            fine = s.size() == 7 && curve(s[0], 0, s[1], s[2], s[3], 0) &&
                   curve(s[4], 0, s[5], -s[2], s[6], 0);
            break;
        }
        case 1236: {
            // hflex1: two curves across, the second back to where the first started.
            const std::span<const double> s = operands_of_path();
            fine = s.size() == 9 && curve(s[0], s[1], s[2], s[3], s[4], 0) &&
                   curve(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
            break;
        }
        case 1237: {
            // flex1: two curves whose last point is the last operand along the way they went
            // most, and back to the start the other way.
            const std::span<const double> s = operands_of_path();
            fine = s.size() == 11;
            if (fine) {
                const double dx = s[0] + s[2] + s[4] + s[6] + s[8];
                const double dy = s[1] + s[3] + s[5] + s[7] + s[9];
                const bool across = std::abs(dx) > std::abs(dy);
                fine = curve(s[0], s[1], s[2], s[3], s[4], s[5]) &&
                       curve(s[6], s[7], s[8], s[9], across ? s[10] : -dx, across ? -dy : s[10]);
            }
            break;
        }
        default:
            // What's reserved.
            fine = false;
            break;
        }
        m_count = 0;
        return fine;
    }

    /// The whole number a number on the stack names, or -1 when it's past `most`, or below 0.
    [[nodiscard]] static std::ptrdiff_t whole(const double value, const std::size_t most) {
        return value >= 0 && value < static_cast<double>(most) ? static_cast<std::ptrdiff_t>(value)
                                                               : -1;
    }

    /// Type 2's arithmetic and storage, which take their operands from the stack's top and leave
    /// what they give there, as the path operators' operands. False for what Type 2 doesn't
    /// allow: too few operands, a division by 0, a square root of what's below 0, a result that
    /// isn't finite, a place past the stack or put's numbers, or an operator that's reserved.
    bool compute(const std::uint32_t op) {
        // and, or, add, sub, div, eq, and mul take two; ifelse four; the rest one, or none.
        std::size_t needs = 1;
        if (op == 1203 || op == 1204 || op == 1210 || op == 1211 || op == 1212 || op == 1215 ||
            op == 1220 || op == 1224 || op == 1228 || op == 1230) {
            needs = 2;
        } else if (op == 1222) {
            needs = 4;
        } else if (op == 1223) {
            needs = 0;
        }
        if (m_count < needs) {
            return false;
        }

        double* top = m_stack.data() + m_count - needs;
        double result = 0;
        switch (op) {
        case 1203:
            result = top[0] != 0 && top[1] != 0 ? 1 : 0;
            break;
        case 1204:
            result = top[0] != 0 || top[1] != 0 ? 1 : 0;
            break;
        case 1205:
            result = top[0] == 0 ? 1 : 0;
            break;
        case 1209:
            result = std::abs(top[0]);
            break;
        case 1210:
            result = top[0] + top[1];
            break;
        case 1211:
            result = top[0] - top[1];
            break;
        case 1212:
            if (top[1] == 0) {
                return false;
            }
            result = top[0] / top[1];
            break;
        case 1214:
            result = -top[0];
            break;
        case 1215:
            result = top[0] == top[1] ? 1 : 0;
            break;
        case 1218:
            // drop.
            --m_count;
            return true;
        case 1220: {
            // put: a number into the transient array, at the place on top.
            const std::ptrdiff_t place = whole(top[1], most_transient);
            if (place < 0) {
                return false;
            }
            m_transient[static_cast<std::size_t>(place)] = top[0];
            m_count -= 2;
            return true;
        }
        case 1221: {
            // get: the transient array's number at the place on top.
            const std::ptrdiff_t place = whole(top[0], most_transient);
            if (place < 0) {
                return false;
            }
            result = m_transient[static_cast<std::size_t>(place)];
            break;
        }
        case 1222:
            // ifelse: the first if the third is at most the fourth, or else the second.
            result = top[2] <= top[3] ? top[0] : top[1];
            break;
        case 1223:
            // random: a number above 0 and at most 1, by xorshift.
            m_random ^= m_random << 13U;
            m_random ^= m_random >> 17U;
            m_random ^= m_random << 5U;
            result = static_cast<double>((m_random & 0xFFFFU) + 1) / 65536.0;
            break;
        case 1224:
            result = top[0] * top[1];
            break;
        case 1226:
            if (top[0] < 0) {
                return false;
            }
            result = std::sqrt(top[0]);
            break;
        case 1227:
            // dup.
            if (m_count >= most_stack) {
                return false;
            }
            m_stack[m_count++] = top[0];
            return true;
        case 1228:
            // exch.
            std::swap(top[0], top[1]);
            return true;
        case 1229: {
            // index: a copy of the number as far below the place on top, or of the top's next
            // when the place is below 0.
            const double place = top[0];
            if (m_count < 2) {
                return false;
            }
            const std::ptrdiff_t below = place < 0 ? 0 : whole(place, m_count - 1);
            if (below < 0) {
                return false;
            }
            top[0] = m_stack[m_count - 2 - static_cast<std::size_t>(below)];
            return true;
        }
        case 1230: {
            // roll: the top's next `count` numbers turned up by `by`, each that far nearer the top
            // and the top's around to the bottom.
            const std::ptrdiff_t count = whole(top[0], m_count - 1);
            if (count < 0 || !std::isfinite(top[1])) {
                return false;
            }
            const double by = top[1];
            m_count -= 2;
            if (count > 0) {
                const auto turn = static_cast<std::ptrdiff_t>(
                    std::fmod(std::trunc(by), static_cast<double>(count)));
                double* first = m_stack.data() + m_count - count;
                std::rotate(first, first + ((count - turn) % count), first + count);
            }
            return true;
        }
        default:
            return false;
        }

        if (!std::isfinite(result)) {
            return false;
        }
        m_count -= needs;
        m_stack[m_count++] = result;
        return true;
    }

    /// A path operator's operands: all of the stack, as only the stack-clearing operators that
    /// move or hint take the width.
    std::span<const double> operands_of_path() {
        m_first = 0;
        m_width_seen = true;
        return operands();
    }
};

} // namespace

bool font_t::cff_glyph(const std::uint32_t glyph, const double x, const double y,
                       const bool component, font_path_t& path) const {
    std::size_t at = 0;
    std::size_t size = 0;
    if (!entry(m_charstrings, glyph, at, size)) {
        return false;
    }

    const std::size_t font = font_dict_of(glyph);
    static const cff_index_t none;
    const cff_index_t& local = font < m_local_subrs.size() ? m_local_subrs[font] : none;
    charstring_t charstring(m_global_subrs, local, path, x, y);
    const auto byte = [this](const std::size_t where) { return u8(where); };
    const auto find = [this](const cff_index_t& index, const std::size_t i, std::size_t& where,
                             std::size_t& count) { return entry(index, i, where, count); };
    if (!charstring.run(byte, find, at, size, 0)) {
        return false;
    }

    // An accented character's base glyph, where it is, and its accent's, by the offset, each by
    // its StandardEncoding code through the charset, and neither accented itself.
    std::array<double, 4> accent{};
    if (!charstring.accented(accent)) {
        return true;
    }
    const std::optional<std::uint32_t> base = standard_glyph(accent[2]);
    const std::optional<std::uint32_t> mark = standard_glyph(accent[3]);
    return !component && base && mark && cff_glyph(*base, x, y, true, path) &&
           cff_glyph(*mark, x + accent[0], y + accent[1], true, path);
}

void font_t::cff_outline(const std::uint32_t glyph, font_path_t& path) const {
    if (!cff_glyph(glyph, 0, 0, false, path)) {
        path.clear();
    }
}

} // namespace fb::image
