#include "fb/utf8.hpp"

#include <array>
#include <string>

namespace fb {
namespace {

/// What a bad sequence decodes to. Only its first byte is used up, so decoding picks up again
/// at the next one. The length of 1 tells it apart from a real, 3-byte U+FFFD.
constexpr decoded_t invalid{ .code_point = U'\uFFFD', .length = 1 };

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

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return { text.begin(), text.end() };
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    return { std::u8string(text.begin(), text.end()) };
}

} // namespace fb
