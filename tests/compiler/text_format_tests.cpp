#include "fb/text_format.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

// The formatter that interpolation's format specs, Format, and the compiler's folding share,
// fb/text_format.hpp.

namespace {

using namespace std::string_view_literals;

fb::format_spec_t spec(const std::string_view text) {
    const std::expected<fb::format_spec_t, fb::format_spec_error_t> read =
        fb::parse_format_spec(text);
    INFO(text);
    REQUIRE(read);
    return *read;
}

/// Why `text` isn't a spec, or "" when it is one.
std::string spec_error(const std::string_view text) {
    const std::expected<fb::format_spec_t, fb::format_spec_error_t> read =
        fb::parse_format_spec(text);
    return read ? std::string() : read.error().message;
}

std::string integer(const std::int64_t value, const std::string_view text) {
    return fb::format_signed(value, spec(text));
}

std::string number(const double value, const std::string_view text) {
    return fb::format_float(value, false, spec(text));
}

/// Why a spec doesn't fit what it formats, or "" when it fits.
std::string misfit(const std::string_view text, const fb::format_subject_t subject) {
    return fb::format_spec_misfit(spec(text), subject, "the value").value_or("");
}

} // namespace

TEST_CASE("a format spec reads fill, alignment, sign, flags, width, grouping, precision, and kind",
          "[text_format]") {
    const fb::format_spec_t full = spec("*^+#12,.3x");
    CHECK(full.fill == U'*');
    CHECK(full.align == fb::format_align_t::CENTER);
    CHECK(full.sign == fb::format_sign_t::ALWAYS);
    CHECK(full.alternate);
    CHECK_FALSE(full.zero);
    CHECK(full.width == 12);
    CHECK(full.grouping == ',');
    CHECK(full.precision == 3);
    CHECK(full.kind == 'x');

    const fb::format_spec_t empty = spec("");
    CHECK(empty.align == fb::format_align_t::NONE);
    CHECK(empty.width == 0);
    CHECK_FALSE(empty.precision);
    CHECK(spec("é>4").fill == U'é');
    CHECK(spec("08").zero);
    CHECK(spec("#?").alternate);
}

TEST_CASE("a spec that isn't one says why", "[text_format]") {
    CHECK(spec_error("z") == "'z' isn't part of a format spec");
    CHECK(spec_error(".") == "a format spec's '.' needs a precision after it, like '.2'");
    CHECK(spec_error("1000") == "a format spec's width is at most 999");
    CHECK(spec_error(".1000") == "a format spec's precision is at most 999");
    CHECK(spec_error("{<4") == "a format spec's fill can't be a brace");
    CHECK(spec_error("<05") == "a format spec takes '0' or an alignment, not both");
    CHECK(spec_error(">5x?") == "'?' isn't part of a format spec");
    CHECK(spec_error("é") == "'é' isn't part of a format spec");
    CHECK(spec_error("999.999e").empty());
}

TEST_CASE("a spec fits what it formats, or says what doesn't", "[text_format]") {
    using enum fb::format_subject_t;
    CHECK(misfit(".2", INTEGER) == "the value doesn't take a precision in its format spec");
    CHECK(misfit("e", INTEGER) == "the value doesn't take 'e' in its format spec");
    CHECK(misfit("#", INTEGER) == "'#' gives a base's prefix, so it needs x, X, o, or b");
    CHECK(misfit(",x", INTEGER) == "',' groups a decimal's digits; '_' groups x, X, o, and b");
    CHECK(misfit("x", FLOAT) == "the value doesn't take 'x' in its format spec");
    CHECK(misfit("#e", FLOAT) == "the value doesn't take '#' in its format spec");
    CHECK(misfit("+", STRING) == "the value doesn't take a sign in its format spec");
    CHECK(misfit("05", STRING) == "the value doesn't take '0' in its format spec");
    CHECK(misfit(".3", TEXT) == "the value doesn't take a precision in its format spec");
    CHECK(misfit(">8", DEBUG) == "the value has no text but its debug form, '?'");
    CHECK(misfit("+?", DEBUG) ==
          "the debug form '?' takes only a fill, an alignment, a width, and '#'");

    CHECK(misfit("_b", INTEGER).empty());
    CHECK(misfit("+08,", INTEGER).empty());
    CHECK(misfit(",.2%", FLOAT).empty());
    CHECK(misfit("*^10.3", STRING).empty());
    CHECK(misfit(">12", TEXT).empty());
    CHECK(misfit("*<#20?", DEBUG).empty());
}

TEST_CASE("an integer's text has its sign, base, prefix, groups, and padding", "[text_format]") {
    CHECK(integer(1234567, "") == "1234567");
    CHECK(integer(1234567, ",") == "1,234,567");
    CHECK(integer(-42, "08") == "-0000042");
    CHECK(integer(42, "+") == "+42");
    CHECK(integer(42, " ") == " 42");
    CHECK(integer(255, "x") == "ff");
    CHECK(integer(255, "X") == "FF");
    CHECK(integer(255, "#x") == "0xff");
    CHECK(integer(255, "#X") == "0xFF");
    CHECK(integer(5, "#010b") == "0b00000101");
    CHECK(integer(255, "_b") == "1111_1111");
    CHECK(integer(8, "#o") == "0o10");
    CHECK(integer(1234, "010,") == "00,001,234");
    CHECK(integer(42, "*^7") == "**42***");
    CHECK(integer(42, "<5") == "42   ");
    CHECK(integer(42, "5") == "   42");
    CHECK(integer(std::numeric_limits<std::int64_t>::min(), "") == "-9223372036854775808");
    CHECK(fb::format_integer(std::numeric_limits<std::uint64_t>::max(), false, spec(",")) ==
          "18,446,744,073,709,551,615");
}

TEST_CASE("a float's text is fixed, exponent, or percent, by its precision", "[text_format]") {
    CHECK(number(3.14159, ".2") == "3.14");
    CHECK(number(3.0, ".2") == "3.00");
    CHECK(number(1234.5, "e") == "1.2345e3");
    CHECK(number(1234.5, ".2e") == "1.23e3");
    CHECK(number(1234.5, "E") == "1.2345E3");
    CHECK(number(0.000015, "e") == "1.5e-5");
    CHECK(number(1e21, "f") == "1000000000000000000000");
    CHECK(number(0.256, ".1%") == "25.6%");
    CHECK(number(1234567.891, ",.2") == "1,234,567.89");
    CHECK(number(-2.5, "+08.2") == "-0002.50");
    CHECK(number(2.5, "+") == "+2.5");
    CHECK(number(1234.5, "010,.1") == "0,001,234.5");
    CHECK(number(std::nan(""), "08") == "     NaN");
    CHECK(number(std::numeric_limits<double>::infinity(), "+") == "+Inf");
    CHECK(number(-std::numeric_limits<double>::infinity(), ">6") == "  -Inf");
    CHECK(fb::format_float(0.1, true, spec(".9")) == "0.100000001");
}

TEST_CASE("a float's longest fixed text, at the largest precision, is all there", "[text_format]") {
    // 309 digits before the point, and for '%' two more, then 999 after.
    const double largest = 1.7976931348623157e308;
    const std::string fixed = number(largest, ".999");
    CHECK(fixed.size() == 309 + 1 + 999);
    CHECK(fixed.starts_with("17976931348623157"));
    CHECK(fixed.ends_with("." + std::string(999, '0')));
    const std::string percent = number(largest, ".999%");
    CHECK(percent.size() == 311 + 1 + 999 + 1);
    CHECK(percent.starts_with("17976931348623157"));
    CHECK(percent.ends_with("." + std::string(999, '0') + "%"));
    // The double nearest 1e200 has 200 digits.
    CHECK(number(1e200, ".999").size() == 200 + 1 + 999);
}

TEST_CASE("'%' moves the point two digits, which multiplying the float doesn't", "[text_format]") {
    CHECK(number(0.57, "%") == "57%");
    CHECK(number(0.07, "%") == "7%");
    CHECK(number(0.29, "%") == "29%");
    CHECK(number(0.0005, "%") == "0.05%");
    CHECK(number(0.123456, ".2%") == "12.35%");
    CHECK(number(0.5, ".0%") == "50%");
    CHECK(number(0, "%") == "0%");
    CHECK(number(1e307, "%") == "1" + std::string(309, '0') + "%");
    CHECK(fb::format_float(1e37, true, spec("%")) == "1" + std::string(39, '0') + "%");

    // Inf and NaN keep their '%'.
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK(number(infinity, "%") == "Inf%");
    CHECK(number(-infinity, ".1%") == "-Inf%");
    CHECK(number(std::numeric_limits<double>::quiet_NaN(), "%") == "NaN%");
}

TEST_CASE("grouping and zeros write a round float's digits, not its exponent", "[text_format]") {
    CHECK(number(2000000, ",") == "2,000,000");
    CHECK(number(2000001, ",") == "2,000,001");
    CHECK(number(2000000, "012") == "000002000000");
    CHECK(fb::format_float(300000, true, spec(",")) == "300,000");
    // Alone, its shortest text has the exponent.
    CHECK(number(2000000, "") == "2e6");
    // 'f' without a precision is the shortest digits too, not every digit of a large one.
    CHECK(number(1e23, "f") == "1" + std::string(23, '0'));
    CHECK(number(1.5e-7, "f") == "0.00000015");
    CHECK(number(123.25, "f") == "123.25");
}

TEST_CASE("a float's shortest text reads back, in one form everywhere", "[text_format]") {
    CHECK(fb::shortest_text(0.1, false) == "0.1");
    CHECK(fb::shortest_text(0.1F, true) == "0.1");
    CHECK(fb::shortest_text(1e21, false) == "1e21");
    CHECK(fb::shortest_text(1.5e-7, false) == "1.5e-7");
    CHECK(fb::shortest_text(100000.0, false) == "1e5");
    CHECK(fb::shortest_text(123456.0, false) == "123456");
    CHECK(fb::shortest_text(-0.0, false) == "-0");
    CHECK(fb::shortest_text(std::nan(""), false) == "NaN");
    CHECK(fb::shortest_text(-std::nan(""), false) == "NaN");
    CHECK(fb::shortest_text(std::numeric_limits<double>::infinity(), false) == "Inf");
    CHECK(fb::shortest_text(-std::numeric_limits<double>::infinity(), false) == "-Inf");
}

TEST_CASE("text pads by characters, and a precision cuts it", "[text_format]") {
    CHECK(fb::format_text("hé", spec("*>5")) == "***hé");
    CHECK(fb::format_text("héllo", spec(".2")) == "hé");
    CHECK(fb::format_text("ab", spec("^6")) == "  ab  ");
    CHECK(fb::format_text("ab", spec("6")) == "ab    ");
    CHECK(fb::format_text("long text", spec("4")) == "long text");
}

TEST_CASE("a String's debug form is how source code writes it", "[text_format]") {
    CHECK(fb::debug_text("plain") == "\"plain\"");
    CHECK(fb::debug_text("a\"b\\c\nd\te\r\0f{x}\x01\x7F"sv) ==
          "\"a\\\"b\\\\c\\nd\\te\\r\\0f{{x}}\\x01\\x7F\"");
    CHECK(fb::debug_text("é") == "\"é\"");
}
