#include "fb/source.hpp"
#include "printers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>

using fb::column_unit_t;
using fb::location_t;

namespace {

fb::source_file_t make_file(std::string text) {
    return { fb::file_id_t{}, "test.fb", std::move(text) };
}

/// A 0-based line and column.
location_t at(const std::uint32_t line, const std::uint32_t column) {
    return { .line = line, .column = column };
}

} // namespace

TEST_CASE("line breaks", "[source]") {
    SECTION("an empty file has one empty line") {
        const auto file = make_file("");
        CHECK(file.line_count() == 1);
        CHECK(file.line_text(0).empty());
        CHECK(file.location_of(0, column_unit_t::UTF8) == at(0, 0));
    }

    SECTION("a trailing newline starts an empty last line") {
        const auto file = make_file("a\n");
        CHECK(file.line_count() == 2);
        CHECK(file.line_text(1).empty());
        CHECK(file.location_of(2, column_unit_t::UTF8) == at(1, 0));
    }

    SECTION("LF, CRLF, and a lone CR all end a line") {
        const auto file = make_file("a\r\nb\rc\nd");
        REQUIRE(file.line_count() == 4);
        CHECK(file.line_text(0) == "a");
        CHECK(file.line_text(1) == "b");
        CHECK(file.line_text(2) == "c");
        CHECK(file.line_text(3) == "d");

        // Byte 3 is "b" and byte 7 is "d".
        CHECK(file.location_of(3, column_unit_t::UTF8) == at(1, 0));
        CHECK(file.location_of(7, column_unit_t::UTF8) == at(3, 0));
    }

    SECTION("a leading BOM is stripped") {
        // Split so the hex escape doesn't take in "ab" as more digits.
        const auto file = make_file("\xEF\xBB\xBF"
                                    "ab");
        CHECK(file.text() == "ab");
    }
}

TEST_CASE("location_of counts columns in the requested unit", "[source]") {
    // é is 2 bytes and 1 UTF-16 unit; 😀 is 4 bytes and 2 UTF-16 units. So "x" is at byte 6.
    const auto file = make_file("é😀x\nz");
    CHECK(file.location_of(6, column_unit_t::UTF8) == at(0, 6));
    CHECK(file.location_of(6, column_unit_t::UTF16) == at(0, 3));
    CHECK(file.location_of(6, column_unit_t::CODEPOINT) == at(0, 2));
    CHECK(file.location_of(8, column_unit_t::CODEPOINT) == at(1, 0));
    CHECK(file.location_of(9, column_unit_t::CODEPOINT) == at(1, 1)); // end of file
}

TEST_CASE("an invalid byte counts as one column", "[source]") {
    const auto file = make_file("\xFF"
                                "x");
    CHECK(file.location_of(1, column_unit_t::CODEPOINT) == at(0, 1));
}

TEST_CASE("offset_of is the inverse of location_of", "[source]") {
    const auto file = make_file("é😀x\nz");
    CHECK(file.offset_of(at(0, 6), column_unit_t::UTF8) == 6);
    CHECK(file.offset_of(at(0, 3), column_unit_t::UTF16) == 6);
    CHECK(file.offset_of(at(0, 2), column_unit_t::CODEPOINT) == 6);
    CHECK(file.offset_of(at(1, 1), column_unit_t::CODEPOINT) == 9);

    // A column inside a character rounds up to the character's end.
    CHECK(file.offset_of(at(0, 2), column_unit_t::UTF16) == 6);

    // Positions past the end clamp: columns to the end of the line, lines to the end of file.
    CHECK(file.offset_of(at(0, 99), column_unit_t::CODEPOINT) == 7);
    CHECK(file.offset_of(at(5, 0), column_unit_t::CODEPOINT) == 9);
}

TEST_CASE("spans", "[source]") {
    const auto file = make_file("Local x = 1");
    constexpr fb::span_t name{ .begin = 6, .end = 7 };
    CHECK(file.slice(name) == "x");
    CHECK(fb::merge(name, { .begin = 10, .end = 11 }) == fb::span_t{ .begin = 6, .end = 11 });
}

TEST_CASE("source manager", "[source]") {
    fb::source_manager_t manager;
    const auto first = manager.add("a.fb", "");
    const auto second = manager.add("b.fb", "");
    CHECK(first != second);
    CHECK(manager.get(second).id() == second);
    CHECK(manager.get(second).path() == "b.fb");
    CHECK_FALSE(manager.load("does/not/exist.fb").has_value());
}
