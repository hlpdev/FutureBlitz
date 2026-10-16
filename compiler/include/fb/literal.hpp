#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// 0-15 for hex digits, 16 for anything else.
[[nodiscard]] constexpr std::uint32_t digit_value(const char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint32_t>(c - '0');
    }

    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint32_t>(c - 'a' + 10);
    }

    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint32_t>(c - 'A' + 10);
    }

    return 16;
}

/// The value of an integer literal like "1_000" or "0xFF". nullopt if it doesn't fit in 64 bits.
[[nodiscard]] std::optional<std::uint64_t> parse_integer(std::string_view text);

/// One decoded escape sequence, like "\n" or "\u{1F600}".
struct escape_t {
    /// U+FFFD when the escape is invalid.
    char32_t code_point;
    /// Bytes from the backslash to the end of the escape, or of what was read of a bad one.
    std::uint32_t length;
    /// Empty when the escape is valid.
    std::string_view error;
};

/// Decodes the escape sequence starting at the backslash at text[pos].
[[nodiscard]] escape_t decode_escape(std::string_view text, std::size_t pos);

/// The value of a decimal literal like "1_000.5" or "1e-9". nullopt if it's out of range.
[[nodiscard]] std::optional<double> parse_decimal(std::string_view text);

/// The code point of a character literal's token text, quotes included, like "'a'" or "'\n'".
/// U+FFFD when it's empty or invalid; the lexer has already reported that.
[[nodiscard]] char32_t decode_character(std::string_view text);

/// The text of a string literal. Escapes, "{{", and "}}" are decoded unless it's raw, line
/// breaks become "\n", and a multi-line string loses its indentation. `pieces` are its tokens'
/// text: one STRING, or a STRING_START, any STRING_MIDDLEs, and a STRING_END. There's one result
/// per piece: the text between the interpolated expressions.
[[nodiscard]] std::vector<std::string> decode_string(std::span<const std::string_view> pieces);

} // namespace fb
