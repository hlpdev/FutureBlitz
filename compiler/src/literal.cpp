#include "fb/literal.hpp"

#include "fb/utf8.hpp"

#include <cstddef>
#include <cstdint>
#include <fast_float/fast_float.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fb {
namespace {

bool strip_prefix(std::string_view& text, const std::string_view prefix) {
    if (!text.starts_with(prefix)) {
        return false;
    }

    text.remove_prefix(prefix.size());
    return true;
}

bool strip_suffix(std::string_view& text, const std::string_view suffix) {
    if (!text.ends_with(suffix)) {
        return false;
    }

    text.remove_suffix(suffix.size());
    return true;
}

/// Just past the line break at text[pos], counting "\r\n" as one.
std::size_t line_break_end(const std::string_view text, const std::size_t pos) {
    return text.substr(pos).starts_with("\r\n") ? pos + 2 : pos + 1;
}

/// Decodes one piece of a string, its quotes and interpolation braces already removed. Line
/// breaks become "\n", and `indent` comes off the start of each line. `line_start` says the
/// piece begins at the start of a line.
std::string decode_text(const std::string_view text, const bool raw, const std::string_view indent,
                        bool line_start) {
    std::string out;
    std::size_t pos = 0;

    while (pos < text.size()) {
        if (line_start) {
            line_start = false;
            if (text.substr(pos).starts_with(indent)) {
                pos += indent.size();
            } else {
                // A blank line, or one the lexer already reported. Drop all its indentation.
                while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
                    ++pos;
                }
            }

            continue;
        }

        const char c = text[pos];
        if (c == '\n' || c == '\r') {
            out += '\n';
            pos = line_break_end(text, pos);
            line_start = true;
        } else if (!raw && c == '\\') {
            // The lexer reported bad escapes. They decode to U+FFFD.
            const escape_t escape = decode_escape(text, pos);
            append_utf8(out, escape.code_point);
            pos += escape.length;
        } else if (!raw && (c == '{' || c == '}') && pos + 1 < text.size() && text[pos + 1] == c) {
            out += c;
            pos += 2;
        } else {
            out += c;
            ++pos;
        }
    }

    return out;
}

/// A bad escape decodes to U+FFFD. `length` bytes are skipped, and the error covers them.
escape_t invalid_escape(const std::uint32_t length, const std::string_view error) {
    return { .code_point = U'\uFFFD', .length = length, .error = error };
}

/// \xHH: exactly two hex digits, ASCII only.
escape_t decode_hex_escape(const std::string_view text, const std::size_t pos) {
    // `length` counts the "\x" too, so two digits make it 4.
    std::uint32_t length = 2;
    std::uint32_t value = 0;
    while (length < 4 && pos + length < text.size() && digit_value(text[pos + length]) < 16) {
        value = (value * 16) + digit_value(text[pos + length]);
        ++length;
    }

    if (length < 4) {
        return invalid_escape(length, "\\x needs two hex digits");
    }

    if (value > 0x7F) {
        return invalid_escape(length, "\\x is for ASCII (00-7F); use \\u{...} instead");
    }

    return { .code_point = static_cast<char32_t>(value), .length = length };
}

/// \u{H...}: 1 to 6 hex digits naming a Unicode scalar value.
escape_t decode_unicode_escape(const std::string_view text, const std::size_t pos) {
    constexpr std::string_view usage = "\\u needs 1 to 6 hex digits in braces, like \\u{1F600}";
    if (pos + 2 >= text.size() || text[pos + 2] != '{') {
        return invalid_escape(2, usage);
    }

    // `length` counts the "\u{" too. Every hex digit is read, even past six, so the error
    // covers them all.
    std::uint32_t length = 3;
    std::uint32_t value = 0;
    while (pos + length < text.size() && digit_value(text[pos + length]) < 16) {
        value = (value * 16) + digit_value(text[pos + length]);
        ++length;
    }

    const std::uint32_t digits = length - 3;
    if (pos + length >= text.size() || text[pos + length] != '}' || digits == 0 || digits > 6) {
        return invalid_escape(length, usage);
    }

    ++length;
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return invalid_escape(length, "not a Unicode scalar value");
    }

    return { .code_point = static_cast<char32_t>(value), .length = length };
}

} // namespace

std::optional<std::uint64_t> parse_integer(std::string_view text) {
    std::uint64_t base = 10;
    if (text.size() > 2 && text[0] == '0') {
        switch (text[1]) {
        case 'x':
            base = 16;
            break;
        case 'b':
            base = 2;
            break;
        case 'o':
            base = 8;
            break;
        default:
            break;
        }

        if (base != 10) {
            text.remove_prefix(2);
        }
    }

    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value = 0;
    bool any = false;
    for (const char c : text) {
        if (c == '_') {
            continue;
        }

        // value * base + digit must not pass the 64-bit max.
        const std::uint64_t digit = digit_value(c);
        if (digit >= base || value > (max - digit) / base) {
            return std::nullopt;
        }

        value = (value * base) + digit;
        any = true;
    }

    if (!any) {
        return std::nullopt;
    }

    return value;
}

escape_t decode_escape(const std::string_view text, const std::size_t pos) {
    if (pos + 1 >= text.size()) {
        return invalid_escape(1, "unknown escape sequence");
    }

    switch (text[pos + 1]) {
    case 'n':
        return { .code_point = U'\n', .length = 2 };
    case 't':
        return { .code_point = U'\t', .length = 2 };
    case 'r':
        return { .code_point = U'\r', .length = 2 };
    case '0':
        return { .code_point = U'\0', .length = 2 };
    case '\\':
        return { .code_point = U'\\', .length = 2 };
    case '"':
        return { .code_point = U'"', .length = 2 };
    case '\'':
        return { .code_point = U'\'', .length = 2 };
    case 'x':
        return decode_hex_escape(text, pos);
    case 'u':
        return decode_unicode_escape(text, pos);
    // The line break stays out of the escape, so it still ends the line.
    case '\n':
    case '\r':
        return invalid_escape(1, "unknown escape sequence");
    // Covers the whole character after the backslash, even a multi-byte one.
    default:
        return invalid_escape(1 + decode_utf8(text, pos + 1).length, "unknown escape sequence");
    }
}

std::optional<double> parse_decimal(std::string_view text) {
    // fast_float reads it as std::from_chars does, which not every C++ library has for floats
    // yet, and neither knows "_" separators.
    std::string digits;
    for (const char c : text) {
        if (c != '_') {
            digits += c;
        }
    }

    double value = 0;
    const fast_float::from_chars_result result =
        fast_float::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }

    return value;
}

char32_t decode_character(std::string_view text) {
    strip_prefix(text, "'");
    strip_suffix(text, "'");

    // The lexer already reported an empty literal.
    if (text.empty()) {
        return U'\uFFFD';
    }

    if (text.front() == '\\') {
        return decode_escape(text, 0).code_point;
    }

    return decode_utf8(text, 0).code_point;
}

std::vector<std::string> decode_string(std::span<const std::string_view> pieces) {
    std::vector<std::string> values;
    if (pieces.empty()) {
        return values;
    }

    std::string_view opening = pieces.front();
    const bool raw = strip_prefix(opening, "r");
    const bool multi_line = opening.starts_with(R"(""")");
    const std::string_view quotes = multi_line ? R"(""")" : R"(")";

    // A multi-line string's indentation is whatever comes before the closing quotes on their
    // line. Without closing quotes (already reported), nothing is stripped.
    std::string_view indent;
    if (std::string_view closing = pieces.back(); multi_line && strip_suffix(closing, quotes)) {
        if (const std::size_t line_break = closing.find_last_of("\r\n");
            line_break != std::string_view::npos) {
            indent = closing.substr(line_break + 1);
        }
    }

    for (std::size_t i = 0; i < pieces.size(); ++i) {
        std::string_view text = pieces[i];
        const bool first = i == 0;
        const bool last = i + 1 == pieces.size();

        // Pieces after the first start with the "}" that ended an interpolation, and pieces
        // before the last end with the "{" that started one.
        if (first) {
            strip_prefix(text, raw ? "r" : "");
            strip_prefix(text, quotes);
            // The opening quotes' line, line break included, isn't part of the string.
            if (multi_line) {
                const std::size_t line_break = text.find_first_of("\r\n");
                text.remove_prefix(line_break == std::string_view::npos
                                       ? text.size()
                                       : line_break_end(text, line_break));
            }
        } else {
            strip_prefix(text, "}");
        }

        if (!last) {
            strip_suffix(text, "{");
        } else if (strip_suffix(text, quotes) && multi_line) {
            // The closing quotes' line isn't part of the string either. Drop it and the line
            // break before it.
            if (const std::size_t line_break = text.find_last_of("\r\n");
                line_break != std::string_view::npos) {
                const bool crlf = line_break > 0 && text.substr(line_break - 1).starts_with("\r\n");
                text = text.substr(0, crlf ? line_break - 1 : line_break);
            }
        }

        // Only the first piece starts at a line start. Later ones start right after a "}".
        values.push_back(decode_text(text, raw, indent, multi_line && first));
    }

    return values;
}

} // namespace fb
