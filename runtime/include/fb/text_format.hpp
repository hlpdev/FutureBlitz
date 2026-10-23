#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// Text for values, as interpolation's format specs and Format write it: one header the compiler,
// which checks specs and folds constants, and the runtime, which formats at run time, both
// build, so the two always agree. Invariant: '.' for decimals and ',' for thousands everywhere.

namespace fb {

/// Where a value goes when its text is shorter than the width.
enum class format_align_t : std::uint8_t { NONE, LEFT, RIGHT, CENTER };

/// What goes before a number that isn't negative: nothing, '+', or a space.
enum class format_sign_t : std::uint8_t { NEGATIVE, ALWAYS, SPACE };

/// A spec, like ">8.2": "[[fill]align][sign][#][0][width][grouping][.precision][kind]".
struct format_spec_t {
    char32_t fill = U' ';
    format_align_t align = format_align_t::NONE;
    format_sign_t sign = format_sign_t::NEGATIVE;
    /// '#': a base's prefix, or the debug form's indented one.
    bool alternate = false;
    /// '0': zeros after a number's sign and prefix.
    bool zero = false;
    std::uint32_t width = 0;
    /// ',' or '_', or 0 for none.
    char grouping = 0;
    std::optional<std::uint32_t> precision;
    /// One of "dxXobeEf%?", or 0 for none.
    char kind = 0;
};

/// Why a spec isn't one, and where in its text.
struct format_spec_error_t {
    std::string message;
    std::size_t at = 0;
    std::size_t length = 1;
};

/// The most a spec's width or precision can be.
inline constexpr std::uint32_t max_format_count = 999;

/// What a spec formats, for which parts fit it.
enum class format_subject_t : std::uint8_t {
    INTEGER, // the integers
    FLOAT,   // Float32 and Float64
    STRING,
    TEXT,  // anything else shown as text: a Bool, an Enum, a Printable value, a tuple, an array
    DEBUG, // the debug form of any value, which takes only ? and #?
};

namespace format_detail {

/// The length in bytes of the UTF-8 character starting with `lead`, which is 1 for a byte
/// that can't start one, so a count never stops.
inline std::size_t utf8_length(const unsigned char lead) {
    if (lead >= 0xF0) {
        return 4;
    }

    if (lead >= 0xE0) {
        return 3;
    }

    return lead >= 0xC0 ? 2 : 1;
}

/// The characters (code points) in UTF-8 text.
inline std::size_t count_characters(const std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();
         i += utf8_length(static_cast<unsigned char>(text[i]))) {
        ++count;
    }

    return count;
}

/// The first `count` characters of UTF-8 text.
inline std::string_view first_characters(const std::string_view text, std::size_t count) {
    std::size_t i = 0;
    while (i < text.size() && count > 0) {
        i += utf8_length(static_cast<unsigned char>(text[i]));
        --count;
    }

    return text.substr(0, std::min(i, text.size()));
}

inline void append_utf8(std::string& out, const char32_t point) {
    if (point < 0x80) {
        out += static_cast<char>(point);
    } else if (point < 0x800) {
        out += static_cast<char>(0xC0U | (point >> 6U));
        out += static_cast<char>(0x80U | (point & 0x3FU));
    } else if (point < 0x10000) {
        out += static_cast<char>(0xE0U | (point >> 12U));
        out += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (point & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (point >> 18U));
        out += static_cast<char>(0x80U | ((point >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (point & 0x3FU));
    }
}

/// The code point that starts UTF-8 text, and its length.
inline std::pair<char32_t, std::size_t> decode(const std::string_view text) {
    const auto lead = static_cast<unsigned char>(text[0]);
    const std::size_t length = std::min(utf8_length(lead), text.size());
    if (length == 1) {
        return { lead, 1 };
    }

    char32_t point = static_cast<char32_t>(lead) & (0x7FU >> length);
    for (std::size_t i = 1; i < length; ++i) {
        point =
            (point << 6U) | (static_cast<char32_t>(static_cast<unsigned char>(text[i])) & 0x3FU);
    }

    return { point, length };
}

/// "1e+21" as "1e21", and "1.5e-07" as "1.5e-7": the exponent without '+' or leading zeros.
inline std::string tidy_exponent(std::string text) {
    const std::size_t e = text.find_first_of("eE");
    if (e == std::string::npos) {
        return text;
    }

    std::size_t digits = e + 1;
    std::string sign;
    if (digits < text.size() && (text[digits] == '+' || text[digits] == '-')) {
        sign = text[digits] == '-' ? "-" : "";
        ++digits;
    }

    std::size_t first = digits;
    while (first + 1 < text.size() && text[first] == '0') {
        ++first;
    }

    return text.substr(0, e + 1) + sign + text.substr(first);
}

/// `digits` with `separator` every `size` digits from the right.
inline std::string grouped(const std::string_view digits, const char separator,
                           const std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % size == 0) {
            out += separator;
        }

        out += digits[i];
    }

    return out;
}

/// A number's integer digits grouped by the spec, with the zeros '0' asks for before them, so
/// that they're grouped too, when `before` characters of sign and prefix go first.
inline std::string zero_grouped(std::string digits, const std::size_t before,
                                const format_spec_t& spec, const std::size_t size) {
    if (spec.grouping == 0) {
        return digits;
    }

    while (spec.zero && before + grouped(digits, spec.grouping, size).size() < spec.width) {
        digits.insert(0, 1, '0');
    }

    return grouped(digits, spec.grouping, size);
}

/// `body` (a number's digits, after its sign and prefix) padded to the spec's width.
inline std::string pad_number(const std::string_view sign, const std::string_view prefix,
                              std::string body, const format_spec_t& spec, const bool finite) {
    const std::size_t length = sign.size() + prefix.size() + count_characters(body);
    if (spec.zero && finite && length < spec.width) {
        body.insert(0, spec.width - length, '0');
    }

    std::string text = std::string(sign) + std::string(prefix) + body;
    const std::size_t shown = count_characters(text);
    if (shown >= spec.width) {
        return text;
    }

    const std::size_t missing = spec.width - shown;
    const format_align_t align =
        spec.align == format_align_t::NONE ? format_align_t::RIGHT : spec.align;
    std::string fill;
    append_utf8(fill, spec.fill);
    std::size_t before = 0;
    if (align == format_align_t::RIGHT) {
        before = missing;
    } else if (align == format_align_t::CENTER) {
        before = missing / 2;
    }

    std::string out;
    for (std::size_t i = 0; i < before; ++i) {
        out += fill;
    }

    out += text;
    for (std::size_t i = before; i < missing; ++i) {
        out += fill;
    }

    return out;
}

/// The sign a number shows, by the spec.
inline std::string_view sign_of(const bool negative, const format_spec_t& spec) {
    if (negative) {
        return "-";
    }

    if (spec.sign == format_sign_t::ALWAYS) {
        return "+";
    }

    return spec.sign == format_sign_t::SPACE ? " " : "";
}

/// A float's digits, as to_chars writes them. The longest is a Float64's 309 digits before the
/// point and 1001 after, which '%' asks for with a precision of 999.
template <typename Float>
std::string float_chars(const Float value, const std::optional<std::chars_format> format,
                        const std::optional<std::uint32_t> precision) {
    std::array<char, 1400> buffer{};
    std::to_chars_result result{};
    if (!format) {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    } else if (!precision) {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, *format);
    } else {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, *format,
                               static_cast<int>(*precision));
    }

    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string();
}

/// A float's shortest digits that read back, in fixed form: "1" and 307 zeros for 1e307, whose
/// fixed text to_chars writes with every digit of the float's exact value.
template <typename Float>
std::string shortest_fixed(const Float value) {
    // Its digits and exponent, from "1.5e+07" or "7e-03".
    const std::string scientific = float_chars(value, std::chars_format::scientific, std::nullopt);
    const std::size_t e = scientific.find('e');
    std::string digits = scientific.substr(0, e);
    std::erase(digits, '.');
    int exponent = 0;
    const std::string_view written = std::string_view(scientific).substr(e + 1);
    std::from_chars(written.data() + (written.front() == '+' ? 1 : 0),
                    written.data() + written.size(), exponent);
    const auto count = static_cast<int>(digits.size());
    if (exponent >= count - 1) {
        return digits + std::string(static_cast<std::size_t>(exponent) + 1 - digits.size(), '0');
    }

    if (exponent >= 0) {
        const std::size_t whole = static_cast<std::size_t>(exponent) + 1;
        return digits.substr(0, whole) + "." + digits.substr(whole);
    }

    return "0." + std::string(static_cast<std::size_t>(-exponent) - 1, '0') + digits;
}

/// A number's fixed text, "12.345" or "7", times 100: its point moved two digits on, which is
/// exact, as multiplying the float isn't.
inline std::string hundred_times(const std::string_view fixed) {
    const std::size_t point = std::min(fixed.find('.'), fixed.size());
    std::string fraction(point < fixed.size() ? fixed.substr(point + 1) : std::string_view());
    std::string whole(fixed.substr(0, point));
    fraction.resize(std::max<std::size_t>(fraction.size(), 2), '0');
    whole += fraction.substr(0, 2);
    whole.erase(0, std::min(whole.find_first_not_of('0'), whole.size() - 1));
    return fraction.size() > 2 ? whole + "." + fraction.substr(2) : whole;
}

} // namespace format_detail

/// A spec's text read, or why it isn't one.
inline std::expected<format_spec_t, format_spec_error_t>
parse_format_spec(const std::string_view text) {
    format_spec_t spec;
    std::size_t at = 0;
    const auto is_align = [](const char c) { return c == '<' || c == '>' || c == '^'; };
    const auto align_of = [](const char c) {
        if (c == '<') {
            return format_align_t::LEFT;
        }

        return c == '>' ? format_align_t::RIGHT : format_align_t::CENTER;
    };

    // A fill is any character before an align.
    if (!text.empty()) {
        const auto [point, length] = format_detail::decode(text);
        if (length < text.size() && is_align(text[length])) {
            if (point == U'{' || point == U'}') {
                return std::unexpected(format_spec_error_t{
                    .message = "a format spec's fill can't be a brace",
                    .length = length,
                });
            }

            spec.fill = point;
            spec.align = align_of(text[length]);
            at = length + 1;
        } else if (is_align(text[0])) {
            spec.align = align_of(text[0]);
            at = 1;
        }
    }

    if (at < text.size() && (text[at] == '+' || text[at] == '-' || text[at] == ' ')) {
        if (text[at] != '-') {
            spec.sign = text[at] == '+' ? format_sign_t::ALWAYS : format_sign_t::SPACE;
        }

        ++at;
    }

    if (at < text.size() && text[at] == '#') {
        spec.alternate = true;
        ++at;
    }

    if (at < text.size() && text[at] == '0') {
        if (spec.align != format_align_t::NONE) {
            return std::unexpected(format_spec_error_t{
                .message = "a format spec takes '0' or an alignment, not both",
                .at = at,
            });
        }

        spec.zero = true;
        ++at;
    }

    // A count, like a width or a precision, of up to max_format_count.
    const auto count =
        [&](const std::string_view what) -> std::expected<std::uint32_t, format_spec_error_t> {
        const std::size_t start = at;
        std::uint64_t value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = std::min<std::uint64_t>(
                (value * 10) + static_cast<std::uint64_t>(text[at] - '0'), max_format_count + 1);
            ++at;
        }

        if (value > max_format_count) {
            return std::unexpected(format_spec_error_t{
                .message = std::string("a format spec's ") + std::string(what) + " is at most 999",
                .at = start,
                .length = at - start,
            });
        }

        return static_cast<std::uint32_t>(value);
    };

    if (const std::expected<std::uint32_t, format_spec_error_t> width = count("width")) {
        spec.width = *width;
    } else {
        return std::unexpected(width.error());
    }

    if (at < text.size() && (text[at] == ',' || text[at] == '_')) {
        spec.grouping = text[at];
        ++at;
    }

    if (at < text.size() && text[at] == '.') {
        ++at;
        if (at >= text.size() || text[at] < '0' || text[at] > '9') {
            return std::unexpected(format_spec_error_t{
                .message = "a format spec's '.' needs a precision after it, like '.2'",
                .at = at - 1,
            });
        }

        const std::expected<std::uint32_t, format_spec_error_t> precision = count("precision");
        if (!precision) {
            return std::unexpected(precision.error());
        }

        spec.precision = *precision;
    }

    if (at < text.size() && std::string_view("dxXobeEf%?").contains(text[at])) {
        spec.kind = text[at];
        ++at;
    }

    if (at < text.size()) {
        const auto [point, length] = format_detail::decode(text.substr(at));
        return std::unexpected(format_spec_error_t{
            .message = std::string("'") + std::string(text.substr(at, length)) +
                       "' isn't part of a format spec",
            .at = at,
            .length = length,
        });
    }

    return spec;
}

/// Why a spec doesn't fit what it formats, named `name` in the message, or nullopt when it fits.
inline std::optional<std::string> format_spec_misfit(const format_spec_t& spec,
                                                     const format_subject_t subject,
                                                     const std::string_view name) {
    const auto doesnt_take = [&](const std::string_view part) {
        return std::string(name) + " doesn't take " + std::string(part) + " in its format spec";
    };

    if (spec.kind == '?') {
        if (spec.sign != format_sign_t::NEGATIVE || spec.zero || spec.grouping != 0 ||
            spec.precision) {
            return "the debug form '?' takes only a fill, an alignment, a width, and '#'";
        }

        return std::nullopt;
    }

    if (subject == format_subject_t::DEBUG) {
        return std::string(name) + " has no text but its debug form, '?'";
    }

    const bool number = subject == format_subject_t::INTEGER || subject == format_subject_t::FLOAT;
    if (!number) {
        if (spec.sign != format_sign_t::NEGATIVE) {
            return doesnt_take("a sign");
        }

        if (spec.zero) {
            return doesnt_take("'0'");
        }

        if (spec.grouping != 0) {
            return doesnt_take("grouping");
        }

        if (spec.alternate) {
            return doesnt_take("'#'");
        }

        if (spec.precision && subject != format_subject_t::STRING) {
            return doesnt_take("a precision");
        }

        if (spec.kind != 0) {
            return doesnt_take(std::string("'") + spec.kind + "'");
        }

        return std::nullopt;
    }

    if (subject == format_subject_t::INTEGER) {
        if (spec.precision) {
            return doesnt_take("a precision");
        }

        if (spec.kind != 0 && !std::string_view("dxXob").contains(spec.kind)) {
            return doesnt_take(std::string("'") + spec.kind + "'");
        }

        if (spec.alternate && (spec.kind == 0 || spec.kind == 'd')) {
            return std::string("'#' gives a base's prefix, so it needs x, X, o, or b");
        }

        if (spec.grouping == ',' && spec.kind != 0 && spec.kind != 'd') {
            return std::string("',' groups a decimal's digits; '_' groups x, X, o, and b");
        }

        return std::nullopt;
    }

    if (spec.alternate) {
        return doesnt_take("'#'");
    }

    if (spec.kind != 0 && !std::string_view("eEf%").contains(spec.kind)) {
        return doesnt_take(std::string("'") + spec.kind + "'");
    }

    return std::nullopt;
}

/// A float's shortest text that reads back as the same value: "0.1", "1e21", "1.5e-7", "NaN",
/// "Inf", "-Inf". A Float32's is the shortest that reads back as that Float32.
inline std::string shortest_text(const double value, const bool float32) {
    if (std::isnan(value)) {
        return "NaN";
    }

    if (std::isinf(value)) {
        return value < 0 ? "-Inf" : "Inf";
    }

    return format_detail::tidy_exponent(
        float32 ? format_detail::float_chars(static_cast<float>(value), std::nullopt, std::nullopt)
                : format_detail::float_chars(value, std::nullopt, std::nullopt));
}

/// A String's debug form: in quotes, with the escapes source code writes, \" \\ \n \t \r \0, and
/// \xHH for the other control characters, and its braces doubled, as interpolation needs.
inline std::string debug_text(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\0':
            out += "\\0";
            break;
        case '{':
            out += "{{";
            break;
        case '}':
            out += "}}";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                constexpr std::string_view hex = "0123456789ABCDEF";
                const auto byte = static_cast<unsigned>(static_cast<unsigned char>(c));
                out += "\\x";
                out += hex[byte >> 4U];
                out += hex[byte & 0xFU];
            } else {
                out += c;
            }
        }
    }

    return out + '"';
}

/// Text padded to the spec's width, by its fill and alignment, and cut to its precision. Text
/// aligns left unless the spec says otherwise.
inline std::string format_text(std::string_view text, const format_spec_t& spec) {
    if (spec.precision) {
        text = format_detail::first_characters(text, *spec.precision);
    }

    format_spec_t padding = spec;
    padding.zero = false;
    if (padding.align == format_align_t::NONE) {
        padding.align = format_align_t::LEFT;
    }

    return format_detail::pad_number("", "", std::string(text), padding, true);
}

/// An integer's text by a spec that fits it: `magnitude`, negative or not.
inline std::string format_integer(const std::uint64_t magnitude, const bool negative,
                                  const format_spec_t& spec) {
    int base = 10;
    std::string_view prefix;
    switch (spec.kind) {
    case 'x':
    case 'X':
        base = 16;
        prefix = "0x";
        break;
    case 'o':
        base = 8;
        prefix = "0o";
        break;
    case 'b':
        base = 2;
        prefix = "0b";
        break;
    default:
        break;
    }

    std::array<char, 72> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude, base);
    std::string digits(buffer.data(), result.ptr);
    if (spec.kind == 'X') {
        std::ranges::transform(digits, digits.begin(), [](const char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
    }

    const std::string_view sign = format_detail::sign_of(negative, spec);
    if (!spec.alternate) {
        prefix = "";
    }

    digits = format_detail::zero_grouped(std::move(digits), sign.size() + prefix.size(), spec,
                                         base == 10 ? 3 : 4);
    return format_detail::pad_number(sign, prefix, std::move(digits), spec, true);
}

inline std::string format_signed(const std::int64_t value, const format_spec_t& spec) {
    const bool negative = value < 0;
    const std::uint64_t magnitude =
        negative ? ~static_cast<std::uint64_t>(value) + 1 : static_cast<std::uint64_t>(value);
    return format_integer(magnitude, negative, spec);
}

/// A float's text by a spec that fits it. A Float32 is formatted as one.
inline std::string format_float(double value, const bool float32, const format_spec_t& spec) {
    const bool negative = std::signbit(value) && !std::isnan(value);
    const std::string_view sign = format_detail::sign_of(negative, spec);
    if (!std::isfinite(value)) {
        const std::string text =
            std::string(std::isnan(value) ? "NaN" : "Inf") + (spec.kind == '%' ? "%" : "");
        return format_detail::pad_number(sign, "", text, spec, false);
    }

    const double magnitude = std::fabs(value);
    std::string body;
    std::string suffix;
    const auto chars = [&](const std::chars_format format,
                           const std::optional<std::uint32_t> precision) {
        return float32
                   ? format_detail::float_chars(static_cast<float>(magnitude), format, precision)
                   : format_detail::float_chars(magnitude, format, precision);
    };
    // Without a precision, fixed text is the shortest digits that read back.
    const auto fixed = [&] {
        return float32 ? format_detail::shortest_fixed(static_cast<float>(magnitude))
                       : format_detail::shortest_fixed(magnitude);
    };

    switch (spec.kind) {
    case 'e':
    case 'E':
        body = format_detail::tidy_exponent(chars(std::chars_format::scientific, spec.precision));
        if (spec.kind == 'E') {
            std::ranges::replace(body, 'e', 'E');
        }

        break;
    case 'f':
        body = spec.precision ? chars(std::chars_format::fixed, spec.precision) : fixed();
        break;
    case '%': {
        // The point moves two digits in the value's own text, rounded to two more decimals than
        // the precision, which is the product's, exactly.
        body = format_detail::hundred_times(
            spec.precision ? chars(std::chars_format::fixed, *spec.precision + 2) : fixed());
        suffix = "%";
        break;
    }
    default:
        // ".2" alone is fixed, and no precision the shortest text, but in fixed form, without
        // an exponent, where its digits are grouped or padded with zeros.
        if (spec.precision) {
            body = chars(std::chars_format::fixed, spec.precision);
        } else {
            body = spec.grouping != 0 || spec.zero ? fixed() : shortest_text(magnitude, float32);
        }

        break;
    }

    if (spec.grouping != 0) {
        const std::size_t end = std::min(body.find_first_of(".eE"), body.size());
        const std::size_t after = body.size() - end + suffix.size();
        body = format_detail::zero_grouped(body.substr(0, end), sign.size() + after, spec, 3) +
               body.substr(end);
    }

    return format_detail::pad_number(sign, "", body + suffix, spec, true);
}

} // namespace fb
