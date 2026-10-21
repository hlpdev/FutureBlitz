#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fb {

/// One code point read from UTF-8.
struct decoded_t {
    /// U+FFFD for an invalid byte.
    char32_t code_point;
    /// Bytes read. An invalid byte counts as 1, so decoding always moves on.
    std::uint32_t length;

    /// False for an invalid byte. A U+FFFD that's really in the text is 3 bytes long, so it's
    /// valid.
    [[nodiscard]] constexpr bool valid() const {
        return length > 1 || code_point < 0x80;
    }
};

/// The code point at text[pos], which must be in range. Overlong forms, surrogates, and values
/// past U+10FFFF are invalid.
[[nodiscard]] decoded_t decode_utf8(std::string_view text, std::size_t pos);

void append_utf8(std::string& out, char32_t code_point);

/// The columns a code point takes in a terminal: 2 for East Asian wide characters and emoji, 0
/// for combining marks and invisible format characters, and 1 for the rest.
[[nodiscard]] int display_width(char32_t code_point);

/// A path as UTF-8 with forward slashes. path.string() isn't UTF-8 on Windows, and it can throw.
[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);

/// A path from UTF-8 text, like a command-line argument.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);

} // namespace fb
