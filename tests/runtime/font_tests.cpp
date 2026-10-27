// std.image's fonts, which the runtime reads itself, broken: cut short, with a table cut short,
// and with a table's bytes changed at random. Each parses or not, and draws what it can, all of
// it in its image.

#include "fb/image.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string_view>
#include <vector>

namespace {

/// image_text's fonts, which its make_data.py wrote: TrueType, CFF, and a CID font's CFF.
constexpr std::array<std::string_view, 3> font_names{ "font.ttf", "cff.otf", "cid.otf" };

std::vector<std::uint8_t> font_file(const std::string_view name) {
    std::ifstream file(std::filesystem::path(FB_TEST_FONTS) / name, std::ios::binary);
    REQUIRE(file);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// An array of `count` bytes from `data`, as std.image passes the runtime a UInt8[].
fb_array_t array_of(std::uint8_t* data, const std::size_t count) {
    return {
        .header = {},
        .count = static_cast<std::int64_t>(count),
        .capacity = static_cast<std::int64_t>(count),
        .data = data,
    };
}

constexpr std::int64_t width = 48;
constexpr std::int64_t height = 24;
/// Bytes either side of the image, which drawing leaves as they are.
constexpr std::size_t guard = 64;
constexpr std::uint8_t guarded = 0x5A;

/// Text of each font's glyphs, one it has none for, and a second line.
constexpr std::string_view text = "AVTo.iD\xC3\xB6LOcf~HZN\xC3\x96x\xF0\x9F\x99\x82\nAV";

/// What a font of the bytes did: whether they were one, and if so, whether its text drew only
/// in its image, and its width was a number.
struct outcome_t {
    bool parsed = false;
    bool in_image = true;
    bool finite = true;
};

outcome_t parse_and_draw(std::vector<std::uint8_t> bytes) {
    const fb_array_t data = array_of(bytes.data(), bytes.size());
    void* font = fb_font_parse(&data);
    fb_string_t* error = fb_font_error(font);
    outcome_t outcome{ .parsed = error->length == 0 };
    fb_string_release(error);
    if (outcome.parsed) {
        const auto size = static_cast<std::size_t>(width * height * 4);
        std::vector<std::uint8_t> pixels(guard + size + guard, guarded);
        std::fill_n(pixels.data() + guard, size, std::uint8_t{ 0 });
        fb_string_t* string =
            fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
        fb_image_fill_text(pixels.data() + guard, width, height, font, string, 12, 1, 15,
                           0x000000FF, 1);
        fb_image_fill_text(pixels.data() + guard, width, height, font, string, 7.5, -3, 4,
                           0x204080C0, 0);
        outcome.finite = std::isfinite(fb_font_width(font, string, 12));
        fb_string_release(string);
        for (std::size_t i = 0; i < guard; ++i) {
            outcome.in_image =
                outcome.in_image && pixels[i] == guarded && pixels[guard + size + i] == guarded;
        }
    }
    fb_font_free(font);
    return outcome;
}

/// A font's table: where its directory's record is, and where its bytes are.
struct table_t {
    std::string_view tag;
    std::size_t record = 0;
    std::size_t at = 0;
    std::size_t size = 0;
};

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, const std::size_t at) {
    return (std::uint32_t{ bytes[at] } << 24U) | (std::uint32_t{ bytes[at + 1] } << 16U) |
           (std::uint32_t{ bytes[at + 2] } << 8U) | bytes[at + 3];
}

std::vector<table_t> tables_of(const std::vector<std::uint8_t>& bytes) {
    std::vector<table_t> tables;
    const std::size_t count = (std::size_t{ bytes[4] } << 8U) | bytes[5];
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t record = 12 + (i * 16);
        tables.push_back({
            .tag = std::string_view(reinterpret_cast<const char*>(bytes.data() + record), 4),
            .record = record,
            .at = u32(bytes, record + 8),
            .size = u32(bytes, record + 12),
        });
    }
    return tables;
}

} // namespace

TEST_CASE("std.image's fonts parse, and draw in their images") {
    for (const std::string_view name : font_names) {
        CAPTURE(name);
        const std::vector<std::uint8_t> bytes = font_file(name);
        const outcome_t outcome = parse_and_draw(bytes);
        CHECK(outcome.parsed);
        CHECK(outcome.in_image);
        CHECK(outcome.finite);
    }
}

TEST_CASE("a font cut short anywhere isn't one, as a table goes past its end") {
    for (const std::string_view name : font_names) {
        CAPTURE(name);
        const std::vector<std::uint8_t> bytes = font_file(name);
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            CAPTURE(length);
            const outcome_t outcome = parse_and_draw({ bytes.data(), bytes.data() + length });
            REQUIRE_FALSE(outcome.parsed);
        }
    }
}

TEST_CASE("a font with a table cut short parses or not, and draws only in its image") {
    for (const std::string_view name : font_names) {
        CAPTURE(name);
        const std::vector<std::uint8_t> bytes = font_file(name);
        for (const table_t& table : tables_of(bytes)) {
            CAPTURE(table.tag);
            for (std::size_t length = 0; length < table.size; ++length) {
                CAPTURE(length);
                std::vector<std::uint8_t> cut = bytes;
                for (std::size_t i = 0; i < 4; ++i) {
                    cut[table.record + 12 + i] =
                        static_cast<std::uint8_t>(length >> (24 - (8 * i)));
                }
                const outcome_t outcome = parse_and_draw(cut);
                REQUIRE(outcome.in_image);
                REQUIRE(outcome.finite);
            }
        }
    }
}

TEST_CASE("a font with a table's bytes changed at random parses or not, and draws only in its "
          "image") {
    // The same changes each run, so a failure repeats.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(25);
    for (const std::string_view name : font_names) {
        CAPTURE(name);
        const std::vector<std::uint8_t> bytes = font_file(name);
        for (const table_t& table : tables_of(bytes)) {
            CAPTURE(table.tag);
            std::size_t parsed = 0;
            for (int round = 0; round < 100; ++round) {
                CAPTURE(round);
                std::vector<std::uint8_t> broken = bytes;
                const std::size_t changes = 1 + (random() % 8);
                for (std::size_t change = 0; change < changes; ++change) {
                    broken[table.at + (random() % table.size)] =
                        static_cast<std::uint8_t>(random());
                }
                const outcome_t outcome = parse_and_draw(broken);
                parsed += outcome.parsed ? 1 : 0;
                REQUIRE(outcome.in_image);
                REQUIRE(outcome.finite);
            }
            // Some changes leave a font, though what it draws may be broken.
            CHECK(parsed > 0);
        }
    }
}
