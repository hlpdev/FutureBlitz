// std.data's JSON tokens: a reader that goes through a document's text once, as RFC 8259 has it,
// giving its tokens one at a time, so a document of any size or depth reads without recursion;
// and the text JSON writes for Strings and Doubles. Integers stay exact as Int64s, or UInt64s
// past them; other numbers are Doubles, read by fast_float.

#include "fb/data.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fast_float/fast_float.h>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

/// A token, or what's wrong, as fb/data.hpp numbers them.
enum token_t : std::int8_t {
    TOKEN_END = 0,
    TOKEN_BEGIN_OBJECT = 1,
    TOKEN_END_OBJECT = 2,
    TOKEN_BEGIN_ARRAY = 3,
    TOKEN_END_ARRAY = 4,
    TOKEN_KEY = 5,
    TOKEN_STRING = 6,
    TOKEN_INT = 7,
    TOKEN_UINT = 8,
    TOKEN_DOUBLE = 9,
    TOKEN_TRUE = 10,
    TOKEN_FALSE = 11,
    TOKEN_NULL = 12,
    TOKEN_SYNTAX = -1,
    TOKEN_UNEXPECTED_END = -2,
    TOKEN_OUT_OF_RANGE = -3,
};

/// What the reader takes next.
enum class expect_t : std::uint8_t {
    /// The document's value, or an array's next element after a comma.
    VALUE,
    /// An array's first element, or its end.
    ELEMENT_OR_END,
    /// An object's first key, or its end.
    KEY_OR_END,
    /// An object's next key, after a comma.
    KEY,
    /// The colon after a key, then its value.
    COLON,
    /// A comma or the end of what holds the value, or the document's end.
    AFTER_VALUE,
    /// Nothing: the document ended, or isn't JSON.
    DONE,
};

struct reader_t {
    std::string text;
    std::size_t at = 0;
    std::size_t most = 0;
    /// The arrays and objects open now, '[' or '{' each.
    std::string open;
    expect_t expect = expect_t::VALUE;
    std::int32_t last = TOKEN_END;
    /// Where the token, or what's wrong, starts.
    std::size_t start = 0;
    std::string string;
    std::int64_t integer = 0;
    std::uint64_t unsigned_integer = 0;
    double number = 0.0;
    std::string error;
};

std::string_view view_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(const char c) {
    return c >= '0' && c <= '9';
}

/// The length of the well-formed UTF-8 code point at `at`, or 0 for a byte that doesn't start
/// one: a stray continuation byte, a lead byte without all its continuations, an overlong form,
/// a surrogate, or one past U+10FFFF.
std::size_t utf8_length(const std::string_view text, const std::size_t at) {
    const auto byte_at = [&](const std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(text[index]));
    };
    const std::uint32_t lead = byte_at(at);
    std::size_t length = 0;
    std::uint32_t point = 0;
    std::uint32_t smallest = 0;
    if (lead < 0x80) {
        return 1;
    }

    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        point = lead & 0x1FU;
        smallest = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        point = lead & 0x0FU;
        smallest = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        point = lead & 0x07U;
        smallest = 0x10000;
    } else {
        return 0;
    }

    if (at + length > text.size()) {
        return 0;
    }

    for (std::size_t j = 1; j < length; ++j) {
        const std::uint32_t next = byte_at(at + j);
        if ((next & 0xC0U) != 0x80U) {
            return 0;
        }

        point = (point << 6U) | (next & 0x3FU);
    }

    if (point < smallest || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
        return 0;
    }

    return length;
}

void append_utf8(std::string& text, const std::uint32_t point) {
    if (point < 0x80) {
        text += static_cast<char>(point);
    } else if (point < 0x800) {
        text += static_cast<char>(0xC0U | (point >> 6U));
        text += static_cast<char>(0x80U | (point & 0x3FU));
    } else if (point < 0x10000) {
        text += static_cast<char>(0xE0U | (point >> 12U));
        text += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        text += static_cast<char>(0x80U | (point & 0x3FU));
    } else {
        text += static_cast<char>(0xF0U | (point >> 18U));
        text += static_cast<char>(0x80U | ((point >> 12U) & 0x3FU));
        text += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        text += static_cast<char>(0x80U | (point & 0x3FU));
    }
}

/// The reader can't go on: `token` says why, and `message` what, at `at`.
std::int32_t fail(reader_t& reader, const std::int32_t token, const std::size_t at,
                  const std::string_view message) {
    reader.expect = expect_t::DONE;
    reader.last = token;
    reader.start = at;
    reader.error = message;
    return token;
}

/// What's at the reader's place that isn't what it expects: the text's end, or `message`.
std::int32_t unexpected(reader_t& reader, const std::string_view message) {
    if (reader.at >= reader.text.size()) {
        return fail(reader, TOKEN_UNEXPECTED_END, reader.at, "the JSON ends too soon");
    }

    return fail(reader, TOKEN_SYNTAX, reader.at, message);
}

void skip_space(reader_t& reader) {
    while (reader.at < reader.text.size() && is_space(reader.text[reader.at])) {
        ++reader.at;
    }
}

/// The 4 hex digits at `at` as a number, or -1 when they aren't.
std::int32_t hex4(const std::string_view text, const std::size_t at) {
    if (at + 4 > text.size()) {
        return -1;
    }

    std::int32_t value = 0;
    for (std::size_t i = at; i < at + 4; ++i) {
        const char c = text[i];
        std::int32_t digit = 0;
        if (is_digit(c)) {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return -1;
        }

        value = (value * 16) + digit;
    }

    return value;
}

/// Reads the string that starts at the reader's '"' into `reader.string`: its escapes decoded, a
/// surrogate pair as its code point, and a lone surrogate as U+FFFD, since a String is
/// well-formed UTF-8. Gives `token`, or what's wrong.
std::int32_t read_string(reader_t& reader, const std::int32_t token) {
    const std::string_view text = reader.text;
    reader.start = reader.at;
    reader.string.clear();
    std::size_t at = reader.at + 1;
    while (true) {
        if (at >= text.size()) {
            reader.at = at;
            return fail(reader, TOKEN_UNEXPECTED_END, at, "a string doesn't end");
        }

        const char c = text[at];
        if (c == '"') {
            reader.at = at + 1;
            return token;
        }

        if (static_cast<unsigned char>(c) < 0x20) {
            return fail(reader, TOKEN_SYNTAX, at, "a string has a control character, unescaped");
        }

        if (c != '\\') {
            const std::size_t length = utf8_length(text, at);
            if (length == 0) {
                return fail(reader, TOKEN_SYNTAX, at, "a string's bytes aren't UTF-8");
            }

            reader.string.append(text.substr(at, length));
            at += length;
            continue;
        }

        if (at + 1 >= text.size()) {
            return fail(reader, TOKEN_UNEXPECTED_END, at + 1, "a string doesn't end");
        }

        const char escaped = text[at + 1];
        at += 2;
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            reader.string += escaped;
            continue;
        case 'b':
            reader.string += '\b';
            continue;
        case 'f':
            reader.string += '\f';
            continue;
        case 'n':
            reader.string += '\n';
            continue;
        case 'r':
            reader.string += '\r';
            continue;
        case 't':
            reader.string += '\t';
            continue;
        case 'u':
            break;
        default:
            return fail(reader, TOKEN_SYNTAX, at - 2, "a string has an escape JSON doesn't");
        }

        const std::int32_t unit = hex4(text, at);
        if (unit < 0) {
            return fail(reader, at + 4 > text.size() ? TOKEN_UNEXPECTED_END : TOKEN_SYNTAX, at - 2,
                        "a \\u escape needs 4 hex digits");
        }

        at += 4;
        auto point = static_cast<std::uint32_t>(unit);
        if (point >= 0xD800 && point <= 0xDBFF) {
            // A high surrogate with a low one after it is one code point.
            const std::int32_t low = text.substr(at, 2) == "\\u" ? hex4(text, at + 2) : -1;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                point = 0x10000 + ((point - 0xD800) << 10U) +
                        (static_cast<std::uint32_t>(low) - 0xDC00);
                at += 6;
            } else {
                point = 0xFFFD;
            }
        } else if (point >= 0xDC00 && point <= 0xDFFF) {
            point = 0xFFFD;
        }

        append_utf8(reader.string, point);
    }
}

/// Reads the number at the reader's place, as RFC 8259 writes one: an integer that fits an
/// Int64 or a UInt64 as one, and the rest as the nearest Double.
std::int32_t read_number(reader_t& reader) {
    const std::string_view text = reader.text;
    const std::size_t begin = reader.at;
    reader.start = begin;
    std::size_t at = begin;
    const auto digits = [&] {
        const std::size_t first = at;
        while (at < text.size() && is_digit(text[at])) {
            ++at;
        }

        return at - first;
    };
    if (at < text.size() && text[at] == '-') {
        ++at;
    }

    if (at < text.size() && text[at] == '0') {
        ++at;
    } else if (digits() == 0) {
        reader.at = at;
        return unexpected(reader, "a number needs a digit");
    }

    bool integer = true;
    if (at < text.size() && text[at] == '.') {
        integer = false;
        ++at;
        if (digits() == 0) {
            reader.at = at;
            return unexpected(reader, "a number needs a digit after its point");
        }
    }

    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
        integer = false;
        ++at;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
            ++at;
        }

        if (digits() == 0) {
            reader.at = at;
            return unexpected(reader, "a number needs a digit in its exponent");
        }
    }

    reader.at = at;
    const char* first = text.data() + begin;
    const char* last = text.data() + at;
    if (integer) {
        if (std::from_chars(first, last, reader.integer).ec == std::errc()) {
            return TOKEN_INT;
        }

        if (text[begin] != '-' &&
            std::from_chars(first, last, reader.unsigned_integer).ec == std::errc()) {
            return TOKEN_UINT;
        }
    }

    // An integer too large for either is a Double, as one with a point would be.
    const fast_float::from_chars_result read = fast_float::from_chars(first, last, reader.number);
    if (read.ptr != last || !std::isfinite(reader.number)) {
        return fail(reader, TOKEN_OUT_OF_RANGE, begin, "a number is too large for a Double");
    }

    return TOKEN_DOUBLE;
}

/// Reads the literal `word` at the reader's place, giving `token`.
std::int32_t read_word(reader_t& reader, const std::string_view word, const std::int32_t token) {
    reader.start = reader.at;
    for (const char c : word) {
        if (reader.at >= reader.text.size() || reader.text[reader.at] != c) {
            return unexpected(reader, "there's a word JSON doesn't have");
        }

        ++reader.at;
    }

    return token;
}

/// Reads a value: a scalar, or the start of an array or object.
std::int32_t read_value(reader_t& reader) {
    if (reader.at >= reader.text.size()) {
        return unexpected(reader, "");
    }

    const char c = reader.text[reader.at];
    std::int32_t token = TOKEN_SYNTAX;
    if (c == '{' || c == '[') {
        if (reader.open.size() >= reader.most) {
            return fail(reader, TOKEN_OUT_OF_RANGE, reader.at,
                        "arrays and objects are nested too deep");
        }

        reader.start = reader.at++;
        reader.open += c;
        reader.expect = c == '{' ? expect_t::KEY_OR_END : expect_t::ELEMENT_OR_END;
        return c == '{' ? TOKEN_BEGIN_OBJECT : TOKEN_BEGIN_ARRAY;
    }

    if (c == '"') {
        token = read_string(reader, TOKEN_STRING);
    } else if (c == '-' || is_digit(c)) {
        token = read_number(reader);
    } else if (c == 't') {
        token = read_word(reader, "true", TOKEN_TRUE);
    } else if (c == 'f') {
        token = read_word(reader, "false", TOKEN_FALSE);
    } else if (c == 'n') {
        token = read_word(reader, "null", TOKEN_NULL);
    } else {
        return fail(reader, TOKEN_SYNTAX, reader.at, "a value can't start here");
    }

    if (token > 0) {
        reader.expect = expect_t::AFTER_VALUE;
    }

    return token;
}

/// Ends the array or object open at the top, as `close` does.
std::int32_t close(reader_t& reader, const char close) {
    const char opened = reader.open.back();
    if ((opened == '{') != (close == '}')) {
        return fail(reader, TOKEN_SYNTAX, reader.at,
                    opened == '{' ? "an object ends with ']'" : "an array ends with '}'");
    }

    reader.start = reader.at++;
    reader.open.pop_back();
    reader.expect = expect_t::AFTER_VALUE;
    return close == '}' ? TOKEN_END_OBJECT : TOKEN_END_ARRAY;
}

std::int32_t next(reader_t& reader) {
    while (true) {
        skip_space(reader);
        const bool more = reader.at < reader.text.size();
        const char c = more ? reader.text[reader.at] : '\0';
        switch (reader.expect) {
        case expect_t::DONE:
            return reader.last;
        case expect_t::VALUE:
            return read_value(reader);
        case expect_t::ELEMENT_OR_END:
            return more && c == ']' ? close(reader, c) : read_value(reader);
        case expect_t::KEY_OR_END:
        case expect_t::KEY:
            if (more && c == '}' && reader.expect == expect_t::KEY_OR_END) {
                return close(reader, c);
            }

            if (!more || c != '"') {
                return unexpected(reader, "an object needs a key, a string, here");
            }

            reader.expect = expect_t::COLON;
            return read_string(reader, TOKEN_KEY);
        case expect_t::COLON:
            if (!more || c != ':') {
                return unexpected(reader, "a key needs a ':' after it");
            }

            ++reader.at;
            reader.expect = expect_t::VALUE;
            continue;
        case expect_t::AFTER_VALUE:
            if (reader.open.empty()) {
                if (more) {
                    return fail(reader, TOKEN_SYNTAX, reader.at, "there's more after the JSON");
                }

                reader.expect = expect_t::DONE;
                reader.last = TOKEN_END;
                reader.start = reader.at;
                return TOKEN_END;
            }

            if (more && c == ',') {
                ++reader.at;
                reader.expect = reader.open.back() == '{' ? expect_t::KEY : expect_t::VALUE;
                continue;
            }

            if (more && (c == '}' || c == ']')) {
                return close(reader, c);
            }

            return unexpected(reader, reader.open.back() == '{' ? "an object needs a ',' or '}'"
                                                                : "an array needs a ',' or ']'");
        }
    }
}

reader_t* make_reader(const std::string_view text, const std::int64_t depth) {
    auto* reader =
        new reader_t{ .text = std::string(text), .most = static_cast<std::size_t>(depth) };
    // A byte order mark before the JSON is left out, as RFC 8259 allows.
    if (reader->text.starts_with("\xEF\xBB\xBF")) {
        reader->at = 3;
    }

    return reader;
}

reader_t& reader_of(void* reader) {
    return *static_cast<reader_t*>(reader);
}

} // namespace

extern "C" {

void* fb_json_reader_new(const fb_string_t* text, const std::int64_t depth) {
    return make_reader(view_of(text), depth);
}

void* fb_json_reader_bytes(const fb_array_t* bytes, const std::int64_t depth) {
    return make_reader(
        { static_cast<const char*>(bytes->data), static_cast<std::size_t>(bytes->count) }, depth);
}

void fb_json_reader_free(void* reader) {
    delete static_cast<reader_t*>(reader);
}

std::int32_t fb_json_next(void* reader) {
    return next(reader_of(reader));
}

fb_string_t* fb_json_text(void* reader) {
    return string_of(reader_of(reader).string);
}

std::int64_t fb_json_int(void* reader) {
    return reader_of(reader).integer;
}

std::uint64_t fb_json_uint(void* reader) {
    return reader_of(reader).unsigned_integer;
}

double fb_json_double(void* reader) {
    return reader_of(reader).number;
}

std::int64_t fb_json_depth(void* reader) {
    return static_cast<std::int64_t>(reader_of(reader).open.size());
}

std::int64_t fb_json_line(void* reader) {
    const reader_t& given = reader_of(reader);
    const std::string_view before = std::string_view(given.text).substr(0, given.start);
    return static_cast<std::int64_t>(std::ranges::count(before, '\n')) + 1;
}

std::int64_t fb_json_column(void* reader) {
    const reader_t& given = reader_of(reader);
    const std::string_view before = std::string_view(given.text).substr(0, given.start);
    const std::size_t line = before.rfind('\n');
    const std::string_view on_line =
        line == std::string_view::npos ? before : before.substr(line + 1);
    // Each code point is a column: the bytes that aren't continuation bytes.
    return static_cast<std::int64_t>(std::ranges::count_if(
               on_line,
               [](const char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; })) +
           1;
}

fb_string_t* fb_json_error(void* reader) {
    return string_of(reader_of(reader).error);
}

fb_string_t* fb_json_quote(const fb_string_t* text) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string quoted = "\"";
    for (const char c : view_of(text)) {
        const auto code = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            quoted += "\\\"";
            break;
        case '\\':
            quoted += "\\\\";
            break;
        case '\b':
            quoted += "\\b";
            break;
        case '\f':
            quoted += "\\f";
            break;
        case '\n':
            quoted += "\\n";
            break;
        case '\r':
            quoted += "\\r";
            break;
        case '\t':
            quoted += "\\t";
            break;
        default:
            if (code < 0x20) {
                quoted += "\\u00";
                quoted += digits[code >> 4U];
                quoted += digits[code & 15U];
            } else {
                quoted += c;
            }
        }
    }

    quoted += '"';
    return string_of(quoted);
}

fb_string_t* fb_json_number(const double value) {
    if (!std::isfinite(value)) {
        return nullptr;
    }

    std::array<char, 64> buffer{};
    const std::to_chars_result written =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    std::string text(buffer.data(), written.ptr);
    if (text.find_first_of(".e") == std::string::npos) {
        text += ".0";
    }

    return string_of(text);
}
}
