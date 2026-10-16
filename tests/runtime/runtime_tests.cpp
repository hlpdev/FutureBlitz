#include "fb/runtime.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace {

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    // The bytes follow the header.
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

} // namespace

TEST_CASE("numbers become text the way the compiler folds them", "[runtime]") {
    CHECK(take(fb_string_from_int(-42)) == "-42");
    CHECK(take(fb_string_from_uint(std::numeric_limits<std::uint64_t>::max())) ==
          "18446744073709551615");
    CHECK(take(fb_string_from_float32(0.1F)) == "0.1");
    CHECK(take(fb_string_from_float32(0.1F)) == std::format("{}", 0.1F));
    CHECK(take(fb_string_from_float64(0.1)) == "0.1");
    CHECK(take(fb_string_from_float64(1e100)) == std::format("{}", 1e100));
    CHECK(take(fb_string_from_float64(2.5)) == "2.5");
    CHECK(take(fb_string_from_bool(1)) == "True");
    CHECK(take(fb_string_from_bool(0)) == "False");
}

TEST_CASE("Strings join, concatenate, and compare", "[runtime]") {
    fb_string_t* yes = fb_string_from_bool(1);
    fb_string_t* seven = fb_string_from_int(7);
    CHECK(take(fb_string_concat(yes, seven)) == "True7");

    const std::vector<const fb_string_t*> parts{ seven, yes, seven };
    CHECK(take(fb_string_join(parts.data(), static_cast<std::int64_t>(parts.size()))) == "7True7");
    CHECK(take(fb_string_join(parts.data(), 0)).empty());

    fb_string_t* other_seven = fb_string_from_int(7);
    CHECK(fb_string_equal(seven, other_seven) == 1);
    CHECK(fb_string_equal(seven, yes) == 0);
    fb_string_release(other_seven);
    fb_string_release(seven);
    fb_string_release(yes);
}

TEST_CASE("retain and release count references", "[runtime]") {
    fb_string_t* string = fb_string_from_int(5);
    CHECK(string->refcount == 1);
    fb_string_retain(string);
    CHECK(string->refcount == 2);
    fb_string_release(string);
    CHECK(string->refcount == 1);
    fb_string_release(string);
}

TEST_CASE("a literal's negative refcount never changes", "[runtime]") {
    // Laid out like the compiler's String literals: the header, then the bytes.
    struct literal_t {
        fb_string_t header;
        std::array<char, 2> bytes;
    };

    literal_t literal{ .header = { .refcount = -1, .length = 2 }, .bytes = { 'h', 'i' } };
    fb_string_retain(&literal.header);
    fb_string_release(&literal.header);
    fb_string_release(&literal.header);
    CHECK(literal.header.refcount == -1);
    CHECK(take(fb_string_concat(&literal.header, &literal.header)) == "hihi");
}

TEST_CASE("integer powers wrap, or overflow when checked", "[runtime]") {
    // The arguments are base, exponent, bit width, whether overflow panics, and where it panics.
    CHECK(fb_power_int(2, 10, 32, 1, nullptr) == 1024);
    CHECK(fb_power_int(-2, 3, 32, 1, nullptr) == -8);
    CHECK(fb_power_int(-1, 1'000'001, 64, 1, nullptr) == -1);
    CHECK(fb_power_int(7, 0, 8, 1, nullptr) == 1);
    CHECK(fb_power_int(2, 7, 8, 0, nullptr) == -128); // 128 wraps around in Int8
    // 3^40 doesn't fit in Int64, so it wraps to the same bits as the UInt64 value.
    CHECK(fb_power_int(3, 40, 64, 0, nullptr) ==
          static_cast<std::int64_t>(12157665459056928801ULL));
    CHECK(fb_power_uint(2, 63, 64, 1, nullptr) == 9223372036854775808ULL);
    CHECK(fb_power_uint(2, 8, 8, 0, nullptr) == 0); // 256 wraps around in UInt8
    CHECK(fb_power_uint(10, 19, 64, 1, nullptr) == 10000000000000000000ULL);
}
