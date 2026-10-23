#include "fb/runtime.hpp"
#include "fb/time.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

/// A String the test owns, as a literal is.
struct literal_t {
    fb_string_t header;
    std::array<char, 64> bytes;
};

literal_t literal(const std::string_view text) {
    literal_t made{};
    made.header.refcount = -1;
    made.header.length = static_cast<std::int64_t>(text.size());
    text.copy(made.bytes.data(), made.bytes.size());
    return made;
}

void free_numbers(fb_object_t* object) {
    fb_array_free(reinterpret_cast<fb_array_t*>(object));
}

void visit_nothing(const fb_object_t* /*object*/, fb_visitor_t /*visitor*/, void* /*context*/) {}

/// The Class of an Int32[], whose elements need no releasing.
fb_class_t numbers_class() {
    return {
        .name = "Int32[]",
        .size = sizeof(fb_array_t),
        .destroy = nullptr,
        .release = free_numbers,
        .visit = visit_nothing,
        .live = 0,
        .base = nullptr,
    };
}

/// An Int32[] of the values.
template <std::size_t N>
fb_array_t* numbers(fb_class_t& type, const std::array<std::int32_t, N>& values) {
    fb_array_t* array = fb_array_new(&type, sizeof(std::int32_t), 0);
    for (const std::int32_t value : values) {
        *static_cast<std::int32_t*>(fb_array_push(array, sizeof(std::int32_t), nullptr)) = value;
    }
    return array;
}

std::int32_t zone(const std::string_view name) {
    const literal_t text = literal(name);
    return fb_tz_find(&text.header);
}

} // namespace

TEST_CASE("std.time's text is the UTF-8 of its code points, with U+FFFD for what isn't one",
          "[runtime][time]") {
    fb_class_t type = numbers_class();
    fb_array_t* chars = numbers<7>(type, { 'A', 0xE9, 0x20AC, 0x1F600, 0xD800, 0x110000, -1 });
    CHECK(take(fb_time_text(chars, 0, 4)) == "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(take(fb_time_text(chars, 4, 7)) == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    // A range past the code points is cut to them.
    CHECK(take(fb_time_text(chars, -5, 2)) == "A\xC3\xA9");
    CHECK(take(fb_time_text(chars, 6, 100)) == "\xEF\xBF\xBD");
    CHECK(take(fb_time_text(chars, 3, 1)).empty());
    fb_object_release(&chars->header);
    CHECK(type.live == 0);
}

TEST_CASE("the real clock is past 2020, the monotonic one never goes back, and sleeping waits",
          "[runtime][time]") {
    CHECK(fb_time_now() > 1577836800LL * 1000000000LL);
    const std::int64_t start = fb_time_monotonic();
    fb_time_sleep(10000000);
    fb_time_sleep(0);
    fb_time_sleep(-1);
    CHECK(fb_time_monotonic() - start >= 10000000);

    // Never a little early, as the OS can wake a thread.
    for (std::int64_t milliseconds = 1; milliseconds <= 20; ++milliseconds) {
        const std::int64_t before = fb_time_monotonic();
        fb_time_sleep(milliseconds * 1000000);
        CHECK(fb_time_monotonic() - before >= milliseconds * 1000000);
    }
}

TEST_CASE("a Sleep longer than the monotonic clock counts ends at its latest time",
          "[runtime][time]") {
    // As long as Duration.MaxValue(), a never-passing Deadline's Remaining, the sum would wrap
    // into the past, and Sleep would return at once.
    using std::chrono::steady_clock;
    const steady_clock::time_point now = steady_clock::now();
    const std::int64_t longest = std::numeric_limits<std::int64_t>::max();
    CHECK(fb::time::sleep_end(now, longest) == steady_clock::time_point::max());
    CHECK(fb::time::sleep_end(now, longest - 1000) == steady_clock::time_point::max());
    const std::int64_t left =
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::time_point::max() - now)
            .count();
    CHECK(fb::time::sleep_end(now, left) == steady_clock::time_point::max());
    CHECK(fb::time::sleep_end(now, left - 1) == now + std::chrono::nanoseconds(left - 1));
    CHECK(fb::time::sleep_end(now, 5) == now + std::chrono::nanoseconds(5));
}

TEST_CASE("zones are found by name, in any letter case, and links are names of their own",
          "[runtime][time]") {
    const std::int32_t york = zone("America/New_York");
    CHECK(york > 0);
    CHECK(zone("america/NEW_YORK") == york);
    CHECK(zone("US/Eastern") > 0);
    CHECK(zone("US/Eastern") != york);
    CHECK(zone("Nowhere/Land") == 0);
    CHECK(zone("") == 0);
    CHECK(take(fb_tz_name(york)) == "America/New_York");
    CHECK(take(fb_tz_version()) == "2026e");
    CHECK(fb_tz_count() > 590);
    CHECK(take(fb_tz_name(1)) == "Africa/Abidjan");
    CHECK(take(fb_tz_name(fb_tz_count())) == "Zulu");
}

TEST_CASE("a zone's offsets come from its changes, and from its rule after the last",
          "[runtime][time]") {
    const std::int32_t york = zone("America/New_York");
    // 2026's changes, a second either side.
    CHECK(fb_tz_offset(york, 1772953199) == -18000);
    CHECK(fb_tz_offset(york, 1772953200) == -14400);
    CHECK(fb_tz_offset(york, 1793512799) == -14400);
    CHECK(fb_tz_offset(york, 1793512800) == -18000);
    // 2100's, from the rule, and local mean time before the first change.
    CHECK(fb_tz_offset(york, 4108690799) == -18000);
    CHECK(fb_tz_offset(york, 4108690800) == -14400);
    CHECK(fb_tz_offset(york, -5364662400) == -17762);
    CHECK(take(fb_tz_abbreviation(york, 1772953199)) == "EST");
    CHECK(take(fb_tz_abbreviation(york, 1772953200)) == "EDT");
    CHECK(take(fb_tz_abbreviation(york, -5364662400)) == "LMT");
    CHECK(fb_tz_daylight(york, 1772953199) == 0);
    CHECK(fb_tz_daylight(york, 1772953200) == 1);

    // In the south, daylight saving time spans the new year.
    const std::int32_t sydney = zone("Australia/Sydney");
    CHECK(fb_tz_offset(sydney, 1775318399) == 39600);
    CHECK(fb_tz_offset(sydney, 1775318400) == 36000);
    CHECK(fb_tz_offset(sydney, 4102444800) == 39600);
    CHECK(take(fb_tz_abbreviation(zone("Asia/Kathmandu"), 0)) == "+0530");
    CHECK(take(fb_tz_abbreviation(zone("Asia/Kathmandu"), 1793512800)) == "+0545");
    CHECK(fb_tz_offset(zone("UTC"), 1793512800) == 0);
}

TEST_CASE("a local time has one offset, two where the clocks go back, or none where they skip",
          "[runtime][time]") {
    const std::int32_t york = zone("America/New_York");
    fb_class_t type = numbers_class();
    fb_array_t* offsets = fb_array_new(&type, sizeof(std::int32_t), 0);
    const auto offset = [&](const std::int64_t index) {
        return static_cast<const std::int32_t*>(offsets->data)[index];
    };

    CHECK(fb_tz_local_offsets(york, 1783166400, offsets) == 1);
    CHECK(offsets->count == 2);
    CHECK((offset(0) == -14400 && offset(1) == -14400));
    CHECK(fb_tz_local_offsets(york, 1793496600, offsets) == 2);
    CHECK((offset(0) == -14400 && offset(1) == -18000));
    CHECK(fb_tz_local_offsets(york, 1772937000, offsets) == 0);
    CHECK((offset(0) == -18000 && offset(1) == -14400));
    fb_object_release(&offsets->header);
    CHECK(type.live == 0);
}

TEST_CASE("the OS's zone has a name or an offset", "[runtime][time]") {
    const std::string name = take(fb_tz_local_name());
    CHECK((name.empty() || zone(name) > 0 || !name.contains('/')));
    const std::int32_t offset = fb_tz_system_offset(fb_time_now() / 1000000000);
    CHECK((offset > -86400 && offset < 86400));
}
