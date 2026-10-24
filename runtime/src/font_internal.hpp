#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// std.image's fonts: what font.cpp reads of one, which draw.cpp lays text out with.
// Every offset in the font is checked against its bytes before it's followed, so a font that's
// broken or made to mislead gives no glyph, or a font that fails, but never reads past it.
namespace fb::image {

/// A piece of a glyph's outline, in font units, y up: a contour's start at (x, y), or a line, a
/// quadratic through (x1, y1), or a cubic through (x1, y1) and (x2, y2), to (x, y). A contour
/// closes back to its start.
struct font_segment_t {
    enum kind_t : std::uint8_t { MOVE, LINE, QUADRATIC, CUBIC };

    kind_t kind = MOVE;
    double x1 = 0;
    double y1 = 0;
    double x2 = 0;
    double y2 = 0;
    double x = 0;
    double y = 0;
};

using font_path_t = std::vector<font_segment_t>;

/// A TrueType font: its bytes, copied, and where its tables are.
class font_t {
public:
    /// The font in `bytes`, or one whose `error` says why it isn't, and `unsupported` whether
    /// it's a font of a kind font_t doesn't read, like one of no outlines.
    explicit font_t(std::span<const std::uint8_t> bytes);

    std::string error;
    bool unsupported = false;

    [[nodiscard]] int units_per_em() const {
        return m_units;
    }
    [[nodiscard]] int ascent() const {
        return m_ascent;
    }
    /// Below the baseline, as hhea has it, so it's 0 or less.
    [[nodiscard]] int descent() const {
        return m_descent;
    }
    [[nodiscard]] int line_gap() const {
        return m_line_gap;
    }

    /// The glyph of a code point, or 0, the font's .notdef, when it has none.
    [[nodiscard]] std::uint32_t glyph_of(std::uint32_t code_point) const;
    [[nodiscard]] int advance_of(std::uint32_t glyph) const;
    /// What goes between two glyphs, from the kern table, or 0.
    [[nodiscard]] int kerning(std::uint32_t left, std::uint32_t right) const;
    /// The glyph's outline, a composite's its parts' placed, or none when it has no outline or
    /// its outline is broken.
    [[nodiscard]] font_path_t outline(std::uint32_t glyph) const;

private:
    std::vector<std::uint8_t> m_bytes;
    std::size_t m_glyf = 0;
    std::size_t m_glyf_size = 0;
    std::size_t m_loca = 0;
    std::size_t m_hmtx = 0;
    std::size_t m_cmap = 0;
    std::uint32_t m_cmap_format = 0;
    std::size_t m_kern = 0;
    std::size_t m_kern_pairs = 0;
    std::uint32_t m_glyphs = 0;
    std::uint32_t m_metrics = 0;
    bool m_long_loca = false;
    int m_units = 0;
    int m_ascent = 0;
    int m_descent = 0;
    int m_line_gap = 0;

    [[nodiscard]] bool has(std::size_t at, std::size_t count) const;
    [[nodiscard]] std::uint32_t u8(std::size_t at) const;
    [[nodiscard]] std::uint32_t u16(std::size_t at) const;
    [[nodiscard]] int s16(std::size_t at) const;
    [[nodiscard]] std::uint32_t u32(std::size_t at) const;
    [[nodiscard]] bool glyph_range(std::uint32_t glyph, std::size_t& at, std::size_t& size) const;
    void add_outline(std::uint32_t glyph, const std::array<double, 6>& transform, int depth,
                     font_path_t& path, std::size_t& visits) const;
    bool find_tables(std::size_t start);

};

} // namespace fb::image
