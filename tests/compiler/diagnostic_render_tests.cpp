#include "fb/diagnostic_render.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <string>
#include <utility>

using Catch::Matchers::ContainsSubstring;
using fb::diagnostic_t;
using fb::render_pretty;
using fb::render_short;
using fb::severity_t;

namespace {

/// A source manager holding one file, "main.fb".
struct fixture_t {
    fb::source_manager_t sources;
    fb::file_id_t file;

    explicit fixture_t(std::string text) : file(sources.add("main.fb", std::move(text))) {}

    [[nodiscard]] fb::span_t span(const std::uint32_t begin, const std::uint32_t end) const {
        return { .file = file, .begin = begin, .end = end };
    }
};

} // namespace

TEST_CASE("render_short is one clang-style line", "[render]") {
    // 20-25 is "scroe": line 2 starts at byte 15, so it's at column 6.
    const fixture_t fixture("Global hp: Int\nhp = scroe + 1\n");
    const diagnostic_t diagnostic{
        .code = "E0102",
        .message = "unknown variable 'scroe'",
        .span = fixture.span(20, 25),
    };

    CHECK(render_short(diagnostic, fixture.sources) ==
          "main.fb:2:6: error[E0102]: unknown variable 'scroe'\n");
}

TEST_CASE("render_short leaves out an empty code", "[render]") {
    const fixture_t fixture("Local x = 1\n");
    const diagnostic_t diagnostic{
        .severity = severity_t::WARNING,
        .message = "unused variable 'x'",
        .span = fixture.span(6, 7),
    };

    CHECK(render_short(diagnostic, fixture.sources) ==
          "main.fb:1:7: warning: unused variable 'x'\n");
}

TEST_CASE("render_pretty underlines the span", "[render]") {
    const fixture_t fixture("Global hp: Int\nhp = scroe + 1\n");
    const diagnostic_t diagnostic{
        .code = "E0102",
        .message = "unknown variable 'scroe'",
        .span = fixture.span(20, 25),
    };

    CHECK(render_pretty(diagnostic, fixture.sources) ==
          "main.fb:2:6: error[E0102]: unknown variable 'scroe'\n"
          "  |\n"
          "2 | hp = scroe + 1\n"
          "  |      ^^^^^\n");
}

TEST_CASE("render_pretty shows labels and notes", "[render]") {
    // The error is on the second "hp" (19-21) and the label on the first (6-8). Labels are
    // underlined with '-'.
    const fixture_t fixture("Local hp = 1\nLocal hp = 2\n");
    diagnostic_t diagnostic{ .message = "'hp' is already declared", .span = fixture.span(19, 21) };
    diagnostic.with_label(fixture.span(6, 8), "first declared here")
        .with_note("rename one of them");

    CHECK(render_pretty(diagnostic, fixture.sources) ==
          "main.fb:2:7: error: 'hp' is already declared\n"
          "  |\n"
          "2 | Local hp = 2\n"
          "  |       ^^\n"
          "main.fb:1:7: note: first declared here\n"
          "  |\n"
          "1 | Local hp = 1\n"
          "  |       --\n"
          "  = note: rename one of them\n");
}

TEST_CASE("the gutter fits the widest line number", "[render]") {
    // 18-22 is "last", on line 10.
    const fixture_t fixture("a\nb\nc\nd\ne\nf\ng\nh\ni\nlast\n");
    diagnostic_t diagnostic{ .message = "oops", .span = fixture.span(18, 22) };
    diagnostic.with_label(fixture.span(0, 1), "here");

    CHECK(render_pretty(diagnostic, fixture.sources) == "main.fb:10:1: error: oops\n"
                                                        "   |\n"
                                                        "10 | last\n"
                                                        "   | ^^^^\n"
                                                        "main.fb:1:1: note: here\n"
                                                        "   |\n"
                                                        " 1 | a\n"
                                                        "   | -\n");
}

TEST_CASE("tabs are copied so the markers line up", "[render]") {
    // é is one code point but two bytes, so it gets one space of padding.
    const fixture_t fixture("\tLocal é = y\n");
    const diagnostic_t diagnostic{
        .message = "unknown variable 'y'",
        .span = fixture.span(12, 13),
    };

    CHECK(render_pretty(diagnostic, fixture.sources) ==
          "main.fb:1:12: error: unknown variable 'y'\n"
          "  |\n"
          "1 | \tLocal é = y\n"
          "  | \t          ^\n");
}

TEST_CASE("an empty span gets one marker", "[render]") {
    const fixture_t fixture("F(1\n");
    const diagnostic_t diagnostic{ .message = "expected ')'", .span = fixture.span(3, 3) };

    CHECK(render_pretty(diagnostic, fixture.sources) == "main.fb:1:4: error: expected ')'\n"
                                                        "  |\n"
                                                        "1 | F(1\n"
                                                        "  |    ^\n");
}

TEST_CASE("a span over several lines is underlined to the end of its first line", "[render]") {
    const fixture_t fixture("Call(x,\n     y)\n");
    const diagnostic_t diagnostic{ .message = "bad call", .span = fixture.span(4, 15) };

    CHECK(render_pretty(diagnostic, fixture.sources) == "main.fb:1:5: error: bad call\n"
                                                        "  |\n"
                                                        "1 | Call(x,\n"
                                                        "  |     ^^^\n");
}

TEST_CASE("color adds ANSI codes only when asked", "[render]") {
    const fixture_t fixture("x\n");
    const diagnostic_t diagnostic{ .message = "oops", .span = fixture.span(0, 1) };

    CHECK_FALSE(render_pretty(diagnostic, fixture.sources).contains('\x1b'));

    // "error" in bold red, then a reset.
    CHECK_THAT(render_pretty(diagnostic, fixture.sources, { .color = true }),
               ContainsSubstring("\x1b[1;31merror\x1b[0m"));
}

namespace {

/// A snippet's marker row: the gutter, then `padding` spaces, then `markers`.
std::string marker_row(const std::size_t padding, const std::string_view markers) {
    return "  | " + std::string(padding, ' ') + std::string(markers) + "\n";
}

} // namespace

TEST_CASE("markers line up under wide characters and combining marks", "[render]") {
    // 日本 takes four columns: "x = \"" is 5, the String 6, and " " 1 more before y.
    const fixture_t wide("x = \"日本\" y\n");
    const diagnostic_t after_wide{ .message = "unknown 'y'", .span = wide.span(13, 14) };
    CHECK(render_pretty(after_wide, wide.sources) == "main.fb:1:10: error: unknown 'y'\n"
                                                     "  |\n"
                                                     "1 | x = \"日本\" y\n" +
                                                         marker_row(11, "^"));
    const diagnostic_t on_wide{ .message = "a String", .span = wide.span(4, 12) };
    CHECK(render_pretty(on_wide, wide.sources) == "main.fb:1:5: error: a String\n"
                                                  "  |\n"
                                                  "1 | x = \"日本\" y\n" +
                                                      marker_row(4, "^^^^^^"));

    // An e with a combining acute accent takes one column.
    const fixture_t combining("e\xCC\x81 + z\n");
    const diagnostic_t after_mark{ .message = "unknown 'z'", .span = combining.span(6, 7) };
    CHECK(render_pretty(after_mark, combining.sources) == "main.fb:1:6: error: unknown 'z'\n"
                                                          "  |\n"
                                                          "1 | e\xCC\x81 + z\n" +
                                                              marker_row(4, "^"));
}

TEST_CASE("a snippet shows control characters escaped and bad bytes as U+FFFD", "[render]") {
    // An ESC in a comment can't reach the terminal: it shows as the 4 columns of "\x1B". A byte
    // that isn't UTF-8 shows as U+FFFD.
    const fixture_t fixture("a \x1b[31m \xff b\n");
    const diagnostic_t diagnostic{ .message = "unknown 'b'", .span = fixture.span(10, 11) };
    CHECK(render_pretty(diagnostic, fixture.sources) == "main.fb:1:11: error: unknown 'b'\n"
                                                        "  |\n"
                                                        "1 | a \\x1B[31m \xEF\xBF\xBD b\n" +
                                                            marker_row(13, "^"));
}

TEST_CASE("a long line shows the part around the span", "[render]") {
    // 300 columns of "x, ", with the span on the last x, which shows 40 columns after "...".
    std::string line;
    for (int i = 0; i < 100; ++i) {
        line += "x, ";
    }

    const fixture_t fixture(line + "\n");
    const diagnostic_t diagnostic{ .message = "here", .span = fixture.span(297, 298) };
    std::string shown = "... ";
    for (int i = 0; i < 14; ++i) {
        shown += "x, ";
    }

    CHECK(render_pretty(diagnostic, fixture.sources) == "main.fb:1:298: error: here\n"
                                                        "  |\n"
                                                        "1 | " +
                                                            shown + "\n" + marker_row(43, "^"));
}
