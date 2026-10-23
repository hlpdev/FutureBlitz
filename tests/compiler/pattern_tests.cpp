#include "fb/pattern.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

using fb::check_pattern;
using fb::literal_place;

namespace {

/// Where literal_place puts `offset` of `source`, as (begin, length), or (0, 0) for nullopt.
std::pair<std::size_t, std::size_t> place(const std::string_view source, const std::size_t offset) {
    const std::optional<fb::literal_place_t> found = literal_place(source, offset);
    return found ? std::pair(found->begin, found->length) : std::pair<std::size_t, std::size_t>();
}

} // namespace

TEST_CASE("check_pattern compiles a pattern as std.text.regex does", "[pattern]") {
    CHECK_FALSE(check_pattern(R"((?<year>\d{4})-(\d\d))", 0).has_value());
    CHECK_FALSE(check_pattern(R"(\p{L}+\b)", 0).has_value());
    CHECK_FALSE(check_pattern("é+😀?", 0).has_value());

    // A valid pattern would give this, which no CHECK takes.
    const fb::pattern_problem_t valid{ .message = "valid", .offset = 0 };
    const fb::pattern_problem_t open = check_pattern("(ab", 0).value_or(valid);
    CHECK(open.message == "missing closing parenthesis");
    CHECK(open.offset == 3);
    CHECK(check_pattern("a{3,1}", 0).value_or(valid).message ==
          "numbers out of order in {} quantifier");

    // \C could split a character, which a String's positions never do.
    REQUIRE(check_pattern(R"(a\Cb)", 0).has_value());

    // EXTENDED skips a comment, so what's after its # isn't read; IGNORE_CASE and the rest
    // change nothing that's valid.
    REQUIRE(check_pattern("a # (", 0).has_value());
    CHECK_FALSE(check_pattern("a # (", 8).has_value());
    // IGNORE_CASE, MULTILINE, and DOT_ALL.
    CHECK_FALSE(check_pattern("a.b$", 7).has_value());
}

TEST_CASE("literal_place finds a literal's character in its source", "[pattern]") {
    // A raw literal's text is its source; past its end is the closing quote.
    CHECK(place(R"(r"(\d")", 0) == std::pair<std::size_t, std::size_t>(2, 1));
    CHECK(place(R"(r"(\d")", 2) == std::pair<std::size_t, std::size_t>(4, 1));
    CHECK(place(R"(r"(\d")", 3) == std::pair<std::size_t, std::size_t>(5, 1));

    // A regular one's escape is one character, and so is "{{".
    CHECK(place(R"("\\d{{2,1}}")", 0) == std::pair<std::size_t, std::size_t>(1, 2));
    CHECK(place(R"("\\d{{2,1}}")", 1) == std::pair<std::size_t, std::size_t>(3, 1));
    CHECK(place(R"("\\d{{2,1}}")", 2) == std::pair<std::size_t, std::size_t>(4, 2));
    CHECK(place(R"("\\d{{2,1}}")", 6) == std::pair<std::size_t, std::size_t>(9, 2));
    CHECK(place(R"("\u{1F600}x")", 3) == std::pair<std::size_t, std::size_t>(1, 9));
    CHECK(place(R"("\u{1F600}x")", 4) == std::pair<std::size_t, std::size_t>(10, 1));

    // A character beyond ASCII, and a combining mark, take their bytes in both.
    CHECK(place("\"e\xCC\x81[\"", 1) == std::pair<std::size_t, std::size_t>(2, 2));
    CHECK(place("\"e\xCC\x81[\"", 3) == std::pair<std::size_t, std::size_t>(4, 1));
    CHECK(place("\"😀(\"", 4) == std::pair<std::size_t, std::size_t>(5, 1));

    // A multi-line string, or one with an interpolation, has no place to show.
    CHECK_FALSE(literal_place("\"\"\"\n    (\n    \"\"\"", 0).has_value());
    CHECK_FALSE(literal_place(R"("({x}")", 1).has_value());
}
