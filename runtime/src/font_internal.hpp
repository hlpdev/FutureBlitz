#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// std.image's fonts: what font.cpp and cff.cpp read of one, which draw.cpp lays text out with.
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

/// An INDEX of CFF's: how many entries it has, where their offsets start and how wide each is,
/// and where its data starts, the byte before the first, as its offsets count from 1.
struct cff_index_t {
    std::size_t count = 0;
    std::size_t offsets = 0;
    std::size_t width = 0;
    std::size_t data = 0;
};

/// A TrueType or OpenType font: its bytes, copied, and where its tables are.
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

    // A CFF font's: its table's end, its glyphs' charstrings, the global subroutines, each Font
    // DICT's local ones (a font that isn't a CID font's one), where its FDSelect is, or 0, and,
    // for a font that isn't a CID font, how its charset names its glyphs: where it is, or 0 for
    // ISOAdobe's, or that it's one of the expert charsets.
    bool m_cff = false;
    std::size_t m_cff_end = 0;
    cff_index_t m_charstrings;
    cff_index_t m_global_subrs;
    std::vector<cff_index_t> m_local_subrs;
    std::size_t m_fd_select = 0;
    std::size_t m_charset = 0;
    bool m_expert_charset = false;

    [[nodiscard]] bool has(std::size_t at, std::size_t count) const;
    [[nodiscard]] std::uint32_t u8(std::size_t at) const;
    [[nodiscard]] std::uint32_t u16(std::size_t at) const;
    [[nodiscard]] int s16(std::size_t at) const;
    [[nodiscard]] std::uint32_t u32(std::size_t at) const;
    [[nodiscard]] bool glyph_range(std::uint32_t glyph, std::size_t& at, std::size_t& size) const;
    /// Adds the glyph's outline to the path through `transform`, its parts' too, `depth` parts
    /// deep, counting what it reads in `reads`: false when it's broken, or reads too much.
    [[nodiscard]] bool add_outline(std::uint32_t glyph, const std::array<double, 6>& transform,
                                   int depth, font_path_t& path, std::size_t& reads) const;
    bool find_tables(std::size_t start);

    // cff.cpp's.
    bool read_cff(std::size_t at, std::size_t size);
    [[nodiscard]] bool read_index(std::size_t at, cff_index_t& index, std::size_t& after) const;
    [[nodiscard]] bool entry(const cff_index_t& index, std::size_t i, std::size_t& at,
                             std::size_t& size) const;
    [[nodiscard]] std::size_t font_dict_of(std::uint32_t glyph) const;
    [[nodiscard]] std::optional<std::uint32_t> standard_glyph(double code) const;
    [[nodiscard]] bool cff_glyph(std::uint32_t glyph, double x, double y, bool component,
                                 font_path_t& path) const;
    void cff_outline(std::uint32_t glyph, font_path_t& path) const;
};

} // namespace fb::image
