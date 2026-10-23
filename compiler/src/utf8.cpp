#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace fb {
namespace {

/// What a bad sequence decodes to. Only its first byte is used up, so decoding picks up again
/// at the next one. The length of 1 tells it apart from a real, 3-byte U+FFFD.
constexpr decoded_t invalid{ .code_point = U'\uFFFD', .length = 1 };

/// A run of code points, first to last.
struct range_t {
    char32_t first;
    char32_t last;
};

// The tables list runs as {first, last}, which designators would make twice as long.
// NOLINTBEGIN(modernize-use-designated-initializers)

/// Code points that take no column: combining marks, which go on the character before them,
/// Hangul's medial and final jamo, and the invisible format characters, like the zero width
/// joiner, the variation selectors, and emoji's skin tones.
constexpr std::array zero_width{
    range_t{ 0x0300, 0x036F },   range_t{ 0x0483, 0x0489 },   range_t{ 0x0591, 0x05BD },
    range_t{ 0x05BF, 0x05BF },   range_t{ 0x05C1, 0x05C2 },   range_t{ 0x05C4, 0x05C5 },
    range_t{ 0x05C7, 0x05C7 },   range_t{ 0x0610, 0x061A },   range_t{ 0x061C, 0x061C },
    range_t{ 0x064B, 0x065F },   range_t{ 0x0670, 0x0670 },   range_t{ 0x06D6, 0x06DC },
    range_t{ 0x06DF, 0x06E4 },   range_t{ 0x06E7, 0x06E8 },   range_t{ 0x06EA, 0x06ED },
    range_t{ 0x0711, 0x0711 },   range_t{ 0x0730, 0x074A },   range_t{ 0x07A6, 0x07B0 },
    range_t{ 0x07EB, 0x07F3 },   range_t{ 0x0816, 0x0819 },   range_t{ 0x081B, 0x0823 },
    range_t{ 0x0825, 0x0827 },   range_t{ 0x0829, 0x082D },   range_t{ 0x0859, 0x085B },
    range_t{ 0x08D3, 0x08E1 },   range_t{ 0x08E3, 0x0902 },   range_t{ 0x093A, 0x093A },
    range_t{ 0x093C, 0x093C },   range_t{ 0x0941, 0x0948 },   range_t{ 0x094D, 0x094D },
    range_t{ 0x0951, 0x0957 },   range_t{ 0x0962, 0x0963 },   range_t{ 0x0981, 0x0981 },
    range_t{ 0x09BC, 0x09BC },   range_t{ 0x09C1, 0x09C4 },   range_t{ 0x09CD, 0x09CD },
    range_t{ 0x09E2, 0x09E3 },   range_t{ 0x0A01, 0x0A02 },   range_t{ 0x0A3C, 0x0A3C },
    range_t{ 0x0A41, 0x0A51 },   range_t{ 0x0A70, 0x0A71 },   range_t{ 0x0A75, 0x0A75 },
    range_t{ 0x0A81, 0x0A82 },   range_t{ 0x0ABC, 0x0ABC },   range_t{ 0x0AC1, 0x0AC8 },
    range_t{ 0x0ACD, 0x0ACD },   range_t{ 0x0AE2, 0x0AE3 },   range_t{ 0x0B01, 0x0B01 },
    range_t{ 0x0B3C, 0x0B3C },   range_t{ 0x0B3F, 0x0B3F },   range_t{ 0x0B41, 0x0B44 },
    range_t{ 0x0B4D, 0x0B4D },   range_t{ 0x0B56, 0x0B56 },   range_t{ 0x0B62, 0x0B63 },
    range_t{ 0x0B82, 0x0B82 },   range_t{ 0x0BC0, 0x0BC0 },   range_t{ 0x0BCD, 0x0BCD },
    range_t{ 0x0C00, 0x0C00 },   range_t{ 0x0C3E, 0x0C40 },   range_t{ 0x0C46, 0x0C56 },
    range_t{ 0x0C62, 0x0C63 },   range_t{ 0x0CBC, 0x0CBC },   range_t{ 0x0CCC, 0x0CCD },
    range_t{ 0x0CE2, 0x0CE3 },   range_t{ 0x0D00, 0x0D01 },   range_t{ 0x0D41, 0x0D44 },
    range_t{ 0x0D4D, 0x0D4D },   range_t{ 0x0D62, 0x0D63 },   range_t{ 0x0DCA, 0x0DCA },
    range_t{ 0x0DD2, 0x0DD6 },   range_t{ 0x0E31, 0x0E31 },   range_t{ 0x0E34, 0x0E3A },
    range_t{ 0x0E47, 0x0E4E },   range_t{ 0x0EB1, 0x0EB1 },   range_t{ 0x0EB4, 0x0EBC },
    range_t{ 0x0EC8, 0x0ECD },   range_t{ 0x0F18, 0x0F19 },   range_t{ 0x0F35, 0x0F35 },
    range_t{ 0x0F37, 0x0F37 },   range_t{ 0x0F39, 0x0F39 },   range_t{ 0x0F71, 0x0F7E },
    range_t{ 0x0F80, 0x0F84 },   range_t{ 0x0F86, 0x0F87 },   range_t{ 0x0F8D, 0x0FBC },
    range_t{ 0x0FC6, 0x0FC6 },   range_t{ 0x102D, 0x1030 },   range_t{ 0x1032, 0x1037 },
    range_t{ 0x1039, 0x103A },   range_t{ 0x103D, 0x103E },   range_t{ 0x1058, 0x1059 },
    range_t{ 0x105E, 0x1060 },   range_t{ 0x1071, 0x1074 },   range_t{ 0x1082, 0x1082 },
    range_t{ 0x1085, 0x1086 },   range_t{ 0x108D, 0x108D },   range_t{ 0x109D, 0x109D },
    range_t{ 0x1160, 0x11FF },   range_t{ 0x135D, 0x135F },   range_t{ 0x1712, 0x1714 },
    range_t{ 0x1732, 0x1734 },   range_t{ 0x1752, 0x1753 },   range_t{ 0x1772, 0x1773 },
    range_t{ 0x17B4, 0x17B5 },   range_t{ 0x17B7, 0x17BD },   range_t{ 0x17C6, 0x17C6 },
    range_t{ 0x17C9, 0x17D3 },   range_t{ 0x17DD, 0x17DD },   range_t{ 0x180B, 0x180F },
    range_t{ 0x1885, 0x1886 },   range_t{ 0x18A9, 0x18A9 },   range_t{ 0x1920, 0x1922 },
    range_t{ 0x1927, 0x1928 },   range_t{ 0x1932, 0x1932 },   range_t{ 0x1939, 0x193B },
    range_t{ 0x1A17, 0x1A18 },   range_t{ 0x1A1B, 0x1A1B },   range_t{ 0x1A56, 0x1A56 },
    range_t{ 0x1A58, 0x1A60 },   range_t{ 0x1A62, 0x1A62 },   range_t{ 0x1A65, 0x1A6C },
    range_t{ 0x1A73, 0x1A7F },   range_t{ 0x1AB0, 0x1AFF },   range_t{ 0x1B00, 0x1B03 },
    range_t{ 0x1B34, 0x1B34 },   range_t{ 0x1B36, 0x1B3A },   range_t{ 0x1B3C, 0x1B3C },
    range_t{ 0x1B42, 0x1B42 },   range_t{ 0x1B6B, 0x1B73 },   range_t{ 0x1B80, 0x1B81 },
    range_t{ 0x1BA2, 0x1BA5 },   range_t{ 0x1BA8, 0x1BA9 },   range_t{ 0x1BAB, 0x1BAD },
    range_t{ 0x1BE6, 0x1BE6 },   range_t{ 0x1BE8, 0x1BE9 },   range_t{ 0x1BED, 0x1BED },
    range_t{ 0x1BEF, 0x1BF1 },   range_t{ 0x1C2C, 0x1C33 },   range_t{ 0x1C36, 0x1C37 },
    range_t{ 0x1CD0, 0x1CD2 },   range_t{ 0x1CD4, 0x1CE0 },   range_t{ 0x1CE2, 0x1CE8 },
    range_t{ 0x1CED, 0x1CED },   range_t{ 0x1CF4, 0x1CF4 },   range_t{ 0x1CF8, 0x1CF9 },
    range_t{ 0x1DC0, 0x1DFF },   range_t{ 0x200B, 0x200F },   range_t{ 0x202A, 0x202E },
    range_t{ 0x2060, 0x2064 },   range_t{ 0x2066, 0x206F },   range_t{ 0x20D0, 0x20F0 },
    range_t{ 0x2CEF, 0x2CF1 },   range_t{ 0x2D7F, 0x2D7F },   range_t{ 0x2DE0, 0x2DFF },
    range_t{ 0x302A, 0x302D },   range_t{ 0x3099, 0x309A },   range_t{ 0xA66F, 0xA672 },
    range_t{ 0xA674, 0xA67D },   range_t{ 0xA69E, 0xA69F },   range_t{ 0xA6F0, 0xA6F1 },
    range_t{ 0xA802, 0xA802 },   range_t{ 0xA806, 0xA806 },   range_t{ 0xA80B, 0xA80B },
    range_t{ 0xA825, 0xA826 },   range_t{ 0xA8C4, 0xA8C5 },   range_t{ 0xA8E0, 0xA8F1 },
    range_t{ 0xA8FF, 0xA8FF },   range_t{ 0xA926, 0xA92D },   range_t{ 0xA947, 0xA951 },
    range_t{ 0xA980, 0xA982 },   range_t{ 0xA9B3, 0xA9B3 },   range_t{ 0xA9B6, 0xA9B9 },
    range_t{ 0xA9BC, 0xA9BD },   range_t{ 0xA9E5, 0xA9E5 },   range_t{ 0xAA29, 0xAA2E },
    range_t{ 0xAA31, 0xAA32 },   range_t{ 0xAA35, 0xAA36 },   range_t{ 0xAA43, 0xAA43 },
    range_t{ 0xAA4C, 0xAA4C },   range_t{ 0xAA7C, 0xAA7C },   range_t{ 0xAAB0, 0xAAB0 },
    range_t{ 0xAAB2, 0xAAB4 },   range_t{ 0xAAB7, 0xAAB8 },   range_t{ 0xAABE, 0xAABF },
    range_t{ 0xAAC1, 0xAAC1 },   range_t{ 0xAAEC, 0xAAED },   range_t{ 0xAAF6, 0xAAF6 },
    range_t{ 0xABE5, 0xABE5 },   range_t{ 0xABE8, 0xABE8 },   range_t{ 0xABED, 0xABED },
    range_t{ 0xD7B0, 0xD7FF },   range_t{ 0xFB1E, 0xFB1E },   range_t{ 0xFE00, 0xFE0F },
    range_t{ 0xFE20, 0xFE2F },   range_t{ 0xFEFF, 0xFEFF },   range_t{ 0xFFF9, 0xFFFB },
    range_t{ 0x101FD, 0x101FD }, range_t{ 0x102E0, 0x102E0 }, range_t{ 0x10376, 0x1037A },
    range_t{ 0x10A01, 0x10A0F }, range_t{ 0x10A38, 0x10A3F }, range_t{ 0x10AE5, 0x10AE6 },
    range_t{ 0x10D24, 0x10D27 }, range_t{ 0x10F46, 0x10F50 }, range_t{ 0x11001, 0x11001 },
    range_t{ 0x11038, 0x11046 }, range_t{ 0x1107F, 0x11081 }, range_t{ 0x110B3, 0x110B6 },
    range_t{ 0x110B9, 0x110BA }, range_t{ 0x11100, 0x11102 }, range_t{ 0x11127, 0x1112B },
    range_t{ 0x1112D, 0x11134 }, range_t{ 0x11173, 0x11173 }, range_t{ 0x11180, 0x11181 },
    range_t{ 0x111B6, 0x111BE }, range_t{ 0x1D167, 0x1D169 }, range_t{ 0x1D173, 0x1D182 },
    range_t{ 0x1D185, 0x1D18B }, range_t{ 0x1D1AA, 0x1D1AD }, range_t{ 0x1D242, 0x1D244 },
    range_t{ 0x1E000, 0x1E02A }, range_t{ 0x1E130, 0x1E136 }, range_t{ 0x1E2EC, 0x1E2EF },
    range_t{ 0x1E8D0, 0x1E8D6 }, range_t{ 0x1E944, 0x1E94A }, range_t{ 0x1F3FB, 0x1F3FF },
    range_t{ 0xE0001, 0xE007F }, range_t{ 0xE0100, 0xE01EF },
};

/// Code points two columns wide: East Asian wide and fullwidth characters, and emoji that show
/// as emoji by themselves.
constexpr std::array double_width{
    range_t{ 0x1100, 0x115F },   range_t{ 0x231A, 0x231B },   range_t{ 0x2329, 0x232A },
    range_t{ 0x23E9, 0x23EC },   range_t{ 0x23F0, 0x23F0 },   range_t{ 0x23F3, 0x23F3 },
    range_t{ 0x25FD, 0x25FE },   range_t{ 0x2614, 0x2615 },   range_t{ 0x2648, 0x2653 },
    range_t{ 0x267F, 0x267F },   range_t{ 0x2693, 0x2693 },   range_t{ 0x26A1, 0x26A1 },
    range_t{ 0x26AA, 0x26AB },   range_t{ 0x26BD, 0x26BE },   range_t{ 0x26C4, 0x26C5 },
    range_t{ 0x26CE, 0x26CE },   range_t{ 0x26D4, 0x26D4 },   range_t{ 0x26EA, 0x26EA },
    range_t{ 0x26F2, 0x26F3 },   range_t{ 0x26F5, 0x26F5 },   range_t{ 0x26FA, 0x26FA },
    range_t{ 0x26FD, 0x26FD },   range_t{ 0x2705, 0x2705 },   range_t{ 0x270A, 0x270B },
    range_t{ 0x2728, 0x2728 },   range_t{ 0x274C, 0x274C },   range_t{ 0x274E, 0x274E },
    range_t{ 0x2753, 0x2755 },   range_t{ 0x2757, 0x2757 },   range_t{ 0x2795, 0x2797 },
    range_t{ 0x27B0, 0x27B0 },   range_t{ 0x27BF, 0x27BF },   range_t{ 0x2B1B, 0x2B1C },
    range_t{ 0x2B50, 0x2B50 },   range_t{ 0x2B55, 0x2B55 },   range_t{ 0x2E80, 0x303E },
    range_t{ 0x3041, 0x33FF },   range_t{ 0x3400, 0x4DBF },   range_t{ 0x4E00, 0x9FFF },
    range_t{ 0xA000, 0xA4CF },   range_t{ 0xA960, 0xA97F },   range_t{ 0xAC00, 0xD7A3 },
    range_t{ 0xF900, 0xFAFF },   range_t{ 0xFE10, 0xFE19 },   range_t{ 0xFE30, 0xFE6F },
    range_t{ 0xFF00, 0xFF60 },   range_t{ 0xFFE0, 0xFFE6 },   range_t{ 0x16FE0, 0x16FE4 },
    range_t{ 0x17000, 0x187F7 }, range_t{ 0x18800, 0x18CD5 }, range_t{ 0x1B000, 0x1B2FF },
    range_t{ 0x1F004, 0x1F004 }, range_t{ 0x1F0CF, 0x1F0CF }, range_t{ 0x1F18E, 0x1F18E },
    range_t{ 0x1F191, 0x1F19A }, range_t{ 0x1F200, 0x1F202 }, range_t{ 0x1F210, 0x1F23B },
    range_t{ 0x1F240, 0x1F248 }, range_t{ 0x1F250, 0x1F251 }, range_t{ 0x1F260, 0x1F265 },
    range_t{ 0x1F300, 0x1F320 }, range_t{ 0x1F32D, 0x1F335 }, range_t{ 0x1F337, 0x1F37C },
    range_t{ 0x1F37E, 0x1F393 }, range_t{ 0x1F3A0, 0x1F3CA }, range_t{ 0x1F3CF, 0x1F3D3 },
    range_t{ 0x1F3E0, 0x1F3F0 }, range_t{ 0x1F3F4, 0x1F3F4 }, range_t{ 0x1F3F8, 0x1F43E },
    range_t{ 0x1F440, 0x1F440 }, range_t{ 0x1F442, 0x1F4FC }, range_t{ 0x1F4FF, 0x1F53D },
    range_t{ 0x1F54B, 0x1F54E }, range_t{ 0x1F550, 0x1F567 }, range_t{ 0x1F57A, 0x1F57A },
    range_t{ 0x1F595, 0x1F596 }, range_t{ 0x1F5A4, 0x1F5A4 }, range_t{ 0x1F5FB, 0x1F64F },
    range_t{ 0x1F680, 0x1F6C5 }, range_t{ 0x1F6CC, 0x1F6CC }, range_t{ 0x1F6D0, 0x1F6D2 },
    range_t{ 0x1F6D5, 0x1F6D7 }, range_t{ 0x1F6DC, 0x1F6DF }, range_t{ 0x1F6EB, 0x1F6EC },
    range_t{ 0x1F6F4, 0x1F6FC }, range_t{ 0x1F7E0, 0x1F7EB }, range_t{ 0x1F7F0, 0x1F7F0 },
    range_t{ 0x1F90C, 0x1F93A }, range_t{ 0x1F93C, 0x1F945 }, range_t{ 0x1F947, 0x1F9FF },
    range_t{ 0x1FA70, 0x1FAFF }, range_t{ 0x20000, 0x2FFFD }, range_t{ 0x30000, 0x3FFFD },
};

// NOLINTEND(modernize-use-designated-initializers)

/// Whether a sorted table of runs has `code_point`.
template <std::size_t Size>
constexpr bool has(const std::array<range_t, Size>& table, const char32_t code_point) {
    const auto after = std::ranges::upper_bound(table, code_point, {}, &range_t::first);
    return after != table.begin() && code_point <= std::prev(after)->last;
}

static_assert(std::ranges::is_sorted(zero_width, {}, &range_t::first));
static_assert(std::ranges::is_sorted(double_width, {}, &range_t::first));

} // namespace

decoded_t decode_utf8(const std::string_view text, const std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        return { .code_point = lead, .length = 1 };
    }

    // The lead byte gives the length and the code point's top bits. A continuation byte
    // (10xxxxxx) or F8-FF can't start a character.
    std::uint32_t length = 0;
    char32_t code_point = 0;

    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        code_point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        code_point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        code_point = lead & 0x07U;
    } else {
        return invalid;
    }

    if (pos + length > text.size()) {
        return invalid;
    }

    // Each continuation byte is 10xxxxxx and adds 6 more bits.
    for (std::uint32_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[pos + i]);
        if ((byte & 0xC0U) != 0x80U) {
            return invalid;
        }

        code_point = (code_point << 6U) | (byte & 0x3FU);
    }

    // UTF-8 forbids overlong forms (C0 80 for U+0000), UTF-16 surrogates, and anything past
    // U+10FFFF.
    constexpr std::array<char32_t, 5> min_for_length = { 0, 0, 0x80, 0x800, 0x10000 };
    const bool overlong = code_point < min_for_length[length];
    const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;

    if (overlong || surrogate || code_point > 0x10FFFFU) {
        return invalid;
    }

    return { .code_point = code_point, .length = length };
}

std::string escape_controls(const std::string_view text) {
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        const decoded_t decoded = decode_utf8(text, at);
        if (!decoded.valid()) {
            append_utf8(out, U'\uFFFD');
        } else if (decoded.code_point != U'\t' && is_control(decoded.code_point)) {
            out += std::format("\\x{:02X}", static_cast<std::uint32_t>(decoded.code_point));
        } else {
            out += text.substr(at, decoded.length);
        }

        at += decoded.length;
    }

    return out;
}

void append_utf8(std::string& out, char32_t code_point) {
    const auto value = static_cast<std::uint32_t>(code_point);
    const auto byte = [&out](const std::uint32_t bits) { out += static_cast<char>(bits); };
    if (value < 0x80) {
        byte(value);
    } else if (value < 0x800) {
        byte(0xC0U | (value >> 6U));
        byte(0x80U | (value & 0x3FU));
    } else if (value < 0x10000) {
        byte(0xE0U | (value >> 12U));
        byte(0x80U | ((value >> 6U) & 0x3FU));
        byte(0x80U | (value & 0x3FU));
    } else {
        byte(0xF0U | (value >> 18U));
        byte(0x80U | ((value >> 12U) & 0x3FU));
        byte(0x80U | ((value >> 6U) & 0x3FU));
        byte(0x80U | (value & 0x3FU));
    }
}

int display_width(const char32_t code_point) {
    if (has(zero_width, code_point)) {
        return 0;
    }

    return has(double_width, code_point) ? 2 : 1;
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return { text.begin(), text.end() };
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    return { std::u8string(text.begin(), text.end()) };
}

} // namespace fb
