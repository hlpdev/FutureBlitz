#include "fb/literal.hpp"
#include "fb/utf8.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using fb::decode_character;
using fb::decode_escape;
using fb::decode_string;
using fb::digit_value;
using fb::parse_decimal;
using fb::parse_integer;

namespace {

/// decode_string on a braced list of pieces, which can't become a std::span directly.
std::vector<std::string> decode(const std::vector<std::string_view>& pieces) {
    return decode_string(pieces);
}

} // namespace

TEST_CASE("parse_decimal reads decimals and rejects ones out of range", "[literal]") {
    CHECK(parse_decimal("1.5") == 1.5);
    CHECK(parse_decimal("1_000.25") == 1000.25);
    CHECK(parse_decimal("1e3") == 1000.0);
    CHECK(parse_decimal("2.5E-1") == 0.25);
    CHECK_FALSE(parse_decimal("1e999").has_value());
}

TEST_CASE("decode_character handles plain characters and escapes", "[literal]") {
    CHECK(static_cast<std::uint32_t>(decode_character("'A'")) == 'A');
    CHECK(static_cast<std::uint32_t>(decode_character("'é'")) == 0xE9);
    CHECK(static_cast<std::uint32_t>(decode_character(R"('\n')")) == '\n');
    CHECK(static_cast<std::uint32_t>(decode_character(R"('\u{1F600}')")) == 0x1F600);
}

TEST_CASE("decode_string removes quotes and decodes escapes", "[literal]") {
    CHECK(decode({ R"("a\tb")" }) == std::vector<std::string>{ "a\tb" });
    CHECK(decode({ R"("{{x}}")" }) == std::vector<std::string>{ "{x}" });
    CHECK(decode({ R"(r"C:\x{y}")" }) == std::vector<std::string>{ R"(C:\x{y})" });
    CHECK(decode({ R"("")" }) == std::vector<std::string>{ "" });
}

TEST_CASE("decode_string splits interpolated pieces", "[literal]") {
    CHECK(decode({ R"("HP: {)", "}/{", R"(}")" }) == std::vector<std::string>{ "HP: ", "/", "" });
}

TEST_CASE("decode_string strips multi-line indentation", "[literal]") {
    CHECK(decode({ "\"\"\"\n    a\n      b\n    \"\"\"" }) == std::vector<std::string>{ "a\n  b" });
    CHECK(decode({ "\"\"\"\r\n  a\r\n  \"\"\"" }) == std::vector<std::string>{ "a" });
    CHECK(decode({ "\"\"\"\n  x {", "}\n  y\n  \"\"\"" }) ==
          std::vector<std::string>{ "x ", "\ny" });
    CHECK(decode({ "r\"\"\"\n  say \"hi\" {x}\n  \"\"\"" }) ==
          std::vector<std::string>{ R"(say "hi" {x})" });
}

TEST_CASE("append_utf8 encodes every length", "[literal]") {
    for (const char32_t code_point : { U'A', U'\u00E9', U'\u20AC', U'\U0001F600' }) {
        std::string text;
        fb::append_utf8(text, code_point);
        const fb::decoded_t decoded = fb::decode_utf8(text, 0);
        CHECK(decoded.code_point == code_point);
        CHECK(decoded.length == text.size());
    }
}

TEST_CASE("digit_value covers hex digits and nothing else", "[literal]") {
    CHECK(digit_value('0') == 0);
    CHECK(digit_value('9') == 9);
    CHECK(digit_value('a') == 10);
    CHECK(digit_value('F') == 15);
    CHECK(digit_value('g') == 16);
    CHECK(digit_value('_') == 16);
}

TEST_CASE("parse_integer reads every base and skips separators", "[literal]") {
    CHECK(parse_integer("0") == 0U);
    CHECK(parse_integer("1_000_000") == 1'000'000U);
    CHECK(parse_integer("0xFF00") == 0xFF00U);
    CHECK(parse_integer("0b1010") == 0b1010U);
    CHECK(parse_integer("0o755") == 0755U);
}

TEST_CASE("parse_integer rejects values past 64 bits", "[literal]") {
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    CHECK(parse_integer("18446744073709551615") == max);
    CHECK(parse_integer("0xFFFF_FFFF_FFFF_FFFF") == max);

    // 2^64, one past the maximum.
    CHECK_FALSE(parse_integer("18446744073709551616").has_value());
    CHECK_FALSE(parse_integer("0x1_0000_0000_0000_0000").has_value());
}

TEST_CASE("valid escapes decode to their code point", "[literal]") {
    // text, code point, length
    using expected_t = std::tuple<std::string_view, std::uint32_t, std::uint32_t>;
    constexpr auto escapes = std::to_array<expected_t>({
        { "\\n", '\n', 2 },
        { "\\t", '\t', 2 },
        { "\\r", '\r', 2 },
        { "\\0", 0, 2 },
        { "\\\\", '\\', 2 },
        { "\\\"", '"', 2 },
        { "\\'", '\'', 2 },
        { "\\x41", 'A', 4 },
        { "\\u{41}", 'A', 6 },
        { "\\u{1F600}", 0x1F600, 9 },
        { "\\u{10FFFF}", 0x10FFFF, 10 },
    });

    for (const auto& [text, code_point, length] : escapes) {
        INFO(text);
        const fb::escape_t escape = decode_escape(text, 0);
        CHECK(escape.error.empty());
        CHECK(static_cast<std::uint32_t>(escape.code_point) == code_point);
        CHECK(escape.length == length);
    }
}

TEST_CASE("invalid escapes say why and how long they are", "[literal]") {
    // text, length, error
    using expected_t = std::tuple<std::string_view, std::uint32_t, std::string_view>;
    constexpr std::string_view usage = "\\u needs 1 to 6 hex digits in braces, like \\u{1F600}";

    // The length takes in the whole character after the backslash (é is two bytes), but never a
    // line break. A malformed Unicode escape stops before any '}'.
    constexpr auto escapes = std::to_array<expected_t>({
        { "\\", 1, "unknown escape sequence" },
        { "\\q", 2, "unknown escape sequence" },
        { "\\é", 3, "unknown escape sequence" },
        { "\\\n", 1, "unknown escape sequence" },
        { "\\x4", 3, "\\x needs two hex digits" },
        { "\\x80", 4, "\\x is for ASCII (00-7F); use \\u{...} instead" },
        { "\\u41", 2, usage },
        { "\\u{}", 3, usage },
        { "\\u{41", 5, usage },
        { "\\u{1234567}", 10, usage },
        { "\\u{110000}", 10, "not a Unicode scalar value" },
        { "\\u{D800}", 8, "not a Unicode scalar value" },
    });

    for (const auto& [text, length, error] : escapes) {
        INFO(text);
        const fb::escape_t escape = decode_escape(text, 0);
        CHECK(escape.error == error);
        CHECK(escape.length == length);
    }
}
