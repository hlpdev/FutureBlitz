// std.compress's Zip in a time zone with summer time: an entry's DOS date, which nothing says the
// zone of, is read and written as UTC, so a time in the hour the zone skips in spring is the same
// as in UTC, and the same entries make the same bytes.
#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace {

/// Has local time be the zone `zone` says, in the TZ variable's form, or the process's own zone
/// again for null.
void use_zone(const char* zone) noexcept {
#ifdef _WIN32
    _putenv_s("TZ", zone != nullptr ? zone : "");
    _tzset();
#else
    if (zone != nullptr) {
        setenv("TZ", zone, 1);
    } else {
        unsetenv("TZ");
    }

    tzset();
#endif
}

/// The zone of local time while it lives, which then goes back to the one before.
class zone_t {
public:
    explicit zone_t(const std::string& zone) {
        if (const char* before = std::getenv("TZ")) {
            m_before = before;
        }

        use_zone(zone.c_str());
    }

    zone_t(const zone_t&) = delete;
    zone_t& operator=(const zone_t&) = delete;

    ~zone_t() {
        use_zone(m_before ? m_before->c_str() : nullptr);
    }

private:
    std::optional<std::string> m_before;
};

/// An array of the bytes, as std.compress passes the runtime a UInt8[].
fb_array_t array_of(std::vector<std::uint8_t>& bytes) {
    return {
        .header = {},
        .count = static_cast<std::int64_t>(bytes.size()),
        .capacity = static_cast<std::int64_t>(bytes.size()),
        .data = bytes.data(),
    };
}

/// A zip of one file, a.txt, last changed `seconds` after 1970's start, made in local time's
/// zone.
std::vector<std::uint8_t> zip_at(const std::int64_t seconds) {
    void* writer = fb_zip_writer_new();
    fb_string_t* name = fb_string_from_cstring("a.txt");
    std::vector<std::uint8_t> data{ 'a' };
    const fb_array_t bytes = array_of(data);
    fb_zip_writer_add(writer, name, &bytes, 0, 0, 0, 1, seconds, -1);
    fb_array_t output{};
    REQUIRE(fb_zip_writer_finish(writer, &output, INT32_MAX) == 0);
    const auto* start = static_cast<const std::uint8_t*>(output.data);
    std::vector<std::uint8_t> zip(start, start + output.count);
    fb_array_free(&output);
    fb_string_release(name);
    fb_zip_writer_free(writer);
    return zip;
}

/// The seconds of the zip's first entry's last change, read in local time's zone.
std::int64_t seconds_of(std::vector<std::uint8_t> zip) {
    const fb_array_t bytes = array_of(zip);
    void* reader = fb_zip_reader_new(&bytes);
    REQUIRE(fb_zip_reader_result(reader) == 0);
    const std::int64_t seconds = fb_zip_reader_seconds(reader, 0);
    fb_zip_reader_free(reader);
    return seconds;
}

} // namespace

TEST_CASE("a zip's DOS dates are UTC's, in an hour a zone's summer time skips too", "[runtime]") {
    // 2107-03-13T02:30:00Z, past 2106, which only a DOS date holds, and in the hour US summer time
    // skips that spring, as EST5EDT has it.
    constexpr std::int64_t skipped = 4329426600;
    std::vector<std::uint8_t> made_in_utc;
    {
        const zone_t utc("UTC0");
        made_in_utc = zip_at(skipped);
        CHECK(seconds_of(made_in_utc) == skipped);
    }

    const zone_t eastern("EST5EDT");
    CHECK(seconds_of(made_in_utc) == skipped);
    const std::vector<std::uint8_t> made_in_eastern = zip_at(skipped);
    CHECK(made_in_eastern == made_in_utc);
    CHECK(seconds_of(made_in_eastern) == skipped);
}
