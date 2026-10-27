// std.time's TimeZone over the IANA tz database, which third_party/ compiles with its zic. A
// zone's TZif data (RFC 8536), read the first time a program asks for the zone, gives its offsets
// up to its last transition, and the POSIX TZ rule at the data's end gives them after it. The
// OS's own zone's name comes from icu.dll on Windows, and from TZ or /etc/localtime elsewhere.

#include "fb/runtime.hpp"
#include "fb/time.hpp"
#include "internal.hpp"
#include "tz_data.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace {

constexpr std::int64_t day_seconds = 86400;

/// A local time type: its offset from UTC in seconds, whether it's daylight saving time, and its
/// abbreviation.
struct type_t {
    std::int32_t offset = 0;
    bool daylight = false;
    std::string abbreviation;
};

/// When a POSIX TZ rule changes the clocks: on day `day` of the year, 1 to 365 without Feb 29
/// for kind 'J' or 0 to 365 with it for 'N'; or, for 'M', on weekday `day` (0 is Sunday) of week
/// `week` (5 is the last) of `month`. It's `time` seconds after that day's midnight, which can be
/// below 0 or past a day.
struct change_t {
    char kind = 'M';
    std::int64_t month = 0;
    std::int64_t week = 0;
    std::int64_t day = 0;
    std::int64_t time = 7200;
};

/// A POSIX TZ rule, like "CET-1CEST,M3.5.0,M10.5.0/3": standard time, and daylight saving time
/// from its start to its end, when it has one.
struct rule_t {
    type_t standard;
    std::optional<type_t> daylight;
    change_t start;
    change_t end;
};

/// A zone: the times of its transitions, in seconds since 1970, with the type each starts, its
/// types, and its rule for after the last.
struct zone_t {
    std::vector<std::int64_t> times;
    std::vector<std::uint8_t> starts;
    std::vector<type_t> types;
    std::optional<rule_t> rule;
};

[[noreturn]] void corrupt() {
    fb_panic("std.time's time zone data doesn't read", nullptr);
}

/// Big-endian numbers and bytes, taken from the front of the data.
class reader_t {
public:
    explicit reader_t(const std::span<const unsigned char> bytes) : m_bytes(bytes) {}

    std::span<const unsigned char> take(const std::uint64_t count) {
        if (count > m_bytes.size()) {
            corrupt();
        }

        const std::span<const unsigned char> taken = m_bytes.first(count);
        m_bytes = m_bytes.subspan(count);
        return taken;
    }

    std::uint64_t number(const std::size_t size) {
        std::uint64_t value = 0;
        for (const unsigned char byte : take(size)) {
            value = (value << 8U) | byte;
        }
        return value;
    }

    [[nodiscard]] std::span<const unsigned char> rest() const {
        return m_bytes;
    }

private:
    std::span<const unsigned char> m_bytes;
};

std::int64_t floor_divide(const std::int64_t a, const std::int64_t b) {
    return (a / b) - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
}

bool is_leap(const std::int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

std::int64_t days_in_month(const std::int64_t year, const std::int64_t month) {
    if (month == 2) {
        return is_leap(year) ? 29 : 28;
    }
    return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}

/// Days since 1970-01-01 of a date in the proleptic Gregorian calendar, as Howard Hinnant's
/// days_from_civil works them out.
std::int64_t days_from_civil(std::int64_t year, const std::int64_t month, const std::int64_t day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = floor_divide(year, 400);
    const std::int64_t year_of_era = year - (era * 400);
    const std::int64_t day_of_year =
        (((153 * (month > 2 ? month - 3 : month + 9)) + 2) / 5) + day - 1;
    const std::int64_t day_of_era =
        (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
    return (era * 146097) + day_of_era - 719468;
}

/// The year of a day since 1970-01-01, as civil_from_days works it out.
std::int64_t year_of(std::int64_t days) {
    days += 719468;
    const std::int64_t era = floor_divide(days, 146097);
    const std::int64_t day_of_era = days - (era * 146097);
    const std::int64_t year_of_era =
        (day_of_era - (day_of_era / 1460) + (day_of_era / 36524) - (day_of_era / 146096)) / 365;
    const std::int64_t day_of_year =
        day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
    const std::int64_t month_index = ((5 * day_of_year) + 2) / 153;
    return year_of_era + (era * 400) + (month_index >= 10 ? 1 : 0);
}

/// Reads a POSIX TZ rule, as the end of a zone's TZif data has it.
class rule_reader_t {
public:
    explicit rule_reader_t(const std::string_view text) : m_text(text) {}

    std::optional<rule_t> read() {
        rule_t rule;
        if (!name(rule.standard.abbreviation) || !offset(rule.standard.offset)) {
            return std::nullopt;
        }
        // POSIX's offsets are west of Greenwich, so they're the other way round.
        rule.standard.offset = -rule.standard.offset;
        if (done()) {
            return rule;
        }

        type_t daylight{ .offset = rule.standard.offset + 3600, .daylight = true };
        if (!name(daylight.abbreviation)) {
            return std::nullopt;
        }
        if (!done() && peek() != ',') {
            if (!offset(daylight.offset)) {
                return std::nullopt;
            }
            daylight.offset = -daylight.offset;
        }

        rule.daylight = daylight;
        if (!skip(',') || !change(rule.start) || !skip(',') || !change(rule.end) || !done()) {
            return std::nullopt;
        }
        return rule;
    }

private:
    std::string_view m_text;
    std::size_t m_at = 0;

    [[nodiscard]] bool done() const {
        return m_at == m_text.size();
    }

    [[nodiscard]] char peek() const {
        return done() ? '\0' : m_text[m_at];
    }

    bool skip(const char c) {
        if (peek() != c) {
            return false;
        }
        ++m_at;
        return true;
    }

    /// A name: letters, or anything but '>' between '<' and '>', like "<+0530>".
    bool name(std::string& out) {
        const bool quoted = skip('<');
        const std::size_t start = m_at;
        const auto letter = [](const char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        };
        while (!done() && (quoted ? peek() != '>' : letter(peek()))) {
            ++m_at;
        }

        out = std::string(m_text.substr(start, m_at - start));
        return (!quoted || skip('>')) && !out.empty();
    }

    /// A number of digits, which it gives back.
    bool digits(std::int64_t& value) {
        const std::size_t start = m_at;
        value = 0;
        while (!done() && peek() >= '0' && peek() <= '9') {
            value = (value * 10) + (peek() - '0');
            ++m_at;
        }
        return m_at > start;
    }

    /// [+-]hh[:mm[:ss]], in seconds.
    bool time(std::int64_t& seconds) {
        const bool negative = skip('-');
        if (!negative) {
            skip('+');
        }

        std::int64_t part = 0;
        if (!digits(part)) {
            return false;
        }
        seconds = part * 3600;
        for (const std::int64_t unit : { 60, 1 }) {
            if (!skip(':')) {
                break;
            }
            if (!digits(part)) {
                return false;
            }
            seconds += part * unit;
        }

        seconds = negative ? -seconds : seconds;
        return true;
    }

    bool offset(std::int32_t& seconds) {
        std::int64_t read = 0;
        if (!time(read)) {
            return false;
        }
        seconds = static_cast<std::int32_t>(read);
        return true;
    }

    /// Jn, n, or Mm.w.d, with "/time" after it or not.
    bool change(change_t& out) {
        if (skip('J')) {
            out.kind = 'J';
            if (!digits(out.day)) {
                return false;
            }
        } else if (skip('M')) {
            out.kind = 'M';
            if (!digits(out.month) || !skip('.') || !digits(out.week) || !skip('.') ||
                !digits(out.day)) {
                return false;
            }
        } else {
            out.kind = 'N';
            if (!digits(out.day)) {
                return false;
            }
        }

        return !skip('/') || time(out.time);
    }
};

/// When a change happens in `year`, in seconds since 1970 of the clocks it changes.
std::int64_t change_seconds(const std::int64_t year, const change_t& change) {
    std::int64_t days = days_from_civil(year, 1, 1);
    if (change.kind == 'J') {
        days += change.day - 1 + (is_leap(year) && change.day >= 60 ? 1 : 0);
    } else if (change.kind == 'N') {
        days += change.day;
    } else {
        const std::int64_t first = days_from_civil(year, change.month, 1);
        // 1970-01-01 was a Thursday, day 4 of a week from Sunday.
        const std::int64_t weekday = first + 4 - (floor_divide(first + 4, 7) * 7);
        std::int64_t day = ((change.day - weekday + 7) % 7) + ((change.week - 1) * 7);
        while (day >= days_in_month(year, change.month)) {
            day -= 7;
        }
        days = first + day;
    }

    return (days * day_seconds) + change.time;
}

/// The type a rule gives at `seconds` since 1970.
const type_t& rule_type(const rule_t& rule, const std::int64_t seconds) {
    if (!rule.daylight) {
        return rule.standard;
    }

    // The start is in standard time and the end in daylight saving time. South of the equator,
    // daylight saving time spans the new year, so the end comes first.
    const std::int64_t year = year_of(floor_divide(seconds + rule.standard.offset, day_seconds));
    const std::int64_t start = change_seconds(year, rule.start) - rule.standard.offset;
    const std::int64_t end = change_seconds(year, rule.end) - rule.daylight->offset;
    const bool daylight =
        start < end ? start <= seconds && seconds < end : seconds < end || start <= seconds;
    return daylight ? *rule.daylight : rule.standard;
}

/// A zone of TZif data, of any version, though zic writes its second or later.
std::unique_ptr<zone_t> read_zone(const std::span<const unsigned char> bytes) {
    reader_t reader(bytes);
    const auto header = [&reader] {
        const std::span<const unsigned char> magic = reader.take(4);
        if (magic[0] != 'T' || magic[1] != 'Z' || magic[2] != 'i' || magic[3] != 'f') {
            corrupt();
        }

        const unsigned char version = reader.take(1)[0];
        reader.take(15);
        std::array<std::uint64_t, 6> counts{};
        for (std::uint64_t& count : counts) {
            count = reader.number(4);
        }
        return std::pair{ version, counts };
    };

    auto [version, counts] = header();
    std::size_t time_size = 4;
    if (version != 0) {
        // The first version's data, with 4-byte times, then the same again with 8-byte ones.
        const auto [utc, standard, leaps, times, types, chars] = counts;
        reader.take((times * 5) + (types * 6) + chars + (leaps * 8) + standard + utc);
        counts = header().second;
        time_size = 8;
    }

    const auto [utc, standard, leaps, times, types, chars] = counts;
    if (types == 0) {
        corrupt();
    }

    auto zone = std::make_unique<zone_t>();
    for (std::uint64_t i = 0; i < times; ++i) {
        const std::uint64_t raw = reader.number(time_size);
        zone->times.push_back(time_size == 8
                                  ? static_cast<std::int64_t>(raw)
                                  : static_cast<std::int32_t>(static_cast<std::uint32_t>(raw)));
    }
    for (std::uint64_t i = 0; i < times; ++i) {
        zone->starts.push_back(reader.take(1)[0]);
        if (zone->starts.back() >= types) {
            corrupt();
        }
    }

    std::vector<std::uint64_t> names;
    for (std::uint64_t i = 0; i < types; ++i) {
        const auto offset = static_cast<std::int32_t>(static_cast<std::uint32_t>(reader.number(4)));
        const bool daylight = reader.take(1)[0] != 0;
        names.push_back(reader.take(1)[0]);
        zone->types.push_back({ .offset = offset, .daylight = daylight });
    }

    const std::span<const unsigned char> text = reader.take(chars);
    for (std::size_t i = 0; i < zone->types.size(); ++i) {
        std::string& abbreviation = zone->types[i].abbreviation;
        for (std::uint64_t at = names[i]; at < text.size() && text[at] != 0; ++at) {
            abbreviation += static_cast<char>(text[at]);
        }
    }

    reader.take((leaps * (time_size + 4)) + standard + utc);
    // The rule for after the last transition, between line breaks.
    if (version != 0) {
        const std::span<const unsigned char> rest = reader.rest();
        if (rest.size() < 2 || rest.front() != '\n' || rest.back() != '\n') {
            corrupt();
        }

        const std::string footer(rest.begin() + 1, rest.end() - 1);
        if (!footer.empty()) {
            zone->rule = rule_reader_t(footer).read();
            if (!zone->rule) {
                corrupt();
            }
        }
    }

    return zone;
}

/// The table's entry of a number from fb_tz_find.
const fb::tz::entry_t& entry_of(const std::int32_t zone) {
    if (zone < 1 || std::cmp_greater(zone, fb::tz::entries().size())) {
        fb_panic("std.time was given a time zone it doesn't have", nullptr);
    }
    return fb::tz::entries()[static_cast<std::size_t>(zone - 1)];
}

/// The zone of a number from fb_tz_find, read the first time it's asked for.
const zone_t& zone_of(const std::int32_t zone) {
    const fb::tz::entry_t& entry = entry_of(zone);
    static std::mutex mutex;
    static std::vector<std::unique_ptr<zone_t>> zones(fb::tz::entries().size());
    const std::scoped_lock lock(mutex);
    std::unique_ptr<zone_t>& found = zones[static_cast<std::size_t>(zone - 1)];
    if (!found) {
        found = read_zone(fb::tz::data().subspan(entry.offset, entry.size));
    }
    return *found;
}

/// The zone's type at `seconds` since 1970: before its first transition, its first type; after
/// its last, what its rule gives.
const type_t& type_at(const zone_t& zone, const std::int64_t seconds) {
    if ((zone.times.empty() || seconds >= zone.times.back()) && zone.rule) {
        return rule_type(*zone.rule, seconds);
    }
    if (zone.times.empty() || seconds < zone.times.front()) {
        return zone.types.front();
    }

    const auto after = std::ranges::upper_bound(zone.times, seconds);
    return zone.types[zone.starts[static_cast<std::size_t>(after - zone.times.begin() - 1)]];
}

std::string_view text_of(const fb_string_t* string) {
    return { reinterpret_cast<const char*>(string + 1), static_cast<std::size_t>(string->length) };
}

fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

bool same_ignoring_case(const std::string_view a, const std::string_view b) {
    const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
    return a.size() == b.size() && std::ranges::equal(a, b, [&](const char x, const char y) {
               return lower(x) == lower(y);
           });
}

#ifdef _WIN32
/// The OS's zone, from icu.dll's ICU, which Windows has had since 2019, loaded only when it's
/// asked for, so a program doesn't need it.
std::string os_zone_name() {
    HMODULE icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (icu == nullptr) {
        return {};
    }

    // Through void (*)(), which a function pointer of any type converts to and from.
    using default_zone_t = std::int32_t (*)(char16_t*, std::int32_t, int*);
    const auto default_zone = reinterpret_cast<default_zone_t>(
        reinterpret_cast<void (*)()>(GetProcAddress(icu, "ucal_getDefaultTimeZone")));
    std::array<char16_t, 128> name{};
    int error = 0;
    const std::int32_t length =
        default_zone == nullptr
            ? 0
            : default_zone(name.data(), static_cast<std::int32_t>(name.size()), &error);
    std::string text;
    // ICU's zone names are ASCII.
    for (std::int32_t i = 0; error <= 0 && i < length && std::cmp_less(i, name.size()); ++i) {
        text += static_cast<char>(name.at(static_cast<std::size_t>(i)) & 0x7fU);
    }
    FreeLibrary(icu);
    return text;
}
#else
/// The OS's zone: TZ's when it's set, or what /etc/localtime links to, past "zoneinfo/", or
/// Debian's /etc/timezone.
std::string os_zone_name() {
    const auto past_zoneinfo = [](std::string_view path) {
        const std::size_t at = path.find("zoneinfo/");
        return std::string(at == std::string_view::npos ? path : path.substr(at + 9));
    };

    if (const std::optional<std::string> zone = fb::runtime::environment("TZ");
        zone && !zone->empty()) {
        std::string_view name(*zone);
        if (name.front() == ':') {
            name.remove_prefix(1);
        }
        return past_zoneinfo(name);
    }

    std::array<char, 4096> link{};
    const ssize_t length = readlink("/etc/localtime", link.data(), link.size() - 1);
    if (length > 0) {
        return past_zoneinfo({ link.data(), static_cast<std::size_t>(length) });
    }

    std::ifstream file("/etc/timezone");
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}
#endif

} // namespace

extern "C" {

std::int32_t fb_tz_find(const fb_string_t* name) {
    const std::string_view wanted = text_of(name);
    const std::span<const fb::tz::entry_t> entries = fb::tz::entries();
    const auto found =
        std::ranges::lower_bound(entries, wanted, {}, [](const fb::tz::entry_t& entry) {
            return std::string_view(entry.name);
        });
    if (found != entries.end() && std::string_view(found->name) == wanted) {
        return static_cast<std::int32_t>(found - entries.begin() + 1);
    }

    const auto any_case = std::ranges::find_if(entries, [&](const fb::tz::entry_t& entry) {
        return same_ignoring_case(entry.name, wanted);
    });
    return any_case == entries.end() ? 0
                                     : static_cast<std::int32_t>(any_case - entries.begin() + 1);
}

std::int32_t fb_tz_count() {
    return static_cast<std::int32_t>(fb::tz::entries().size());
}

fb_string_t* fb_tz_name(const std::int32_t zone) {
    return string_of(entry_of(zone).name);
}

fb_string_t* fb_tz_version() {
    return string_of(fb::tz::version());
}

std::int32_t fb_tz_offset(const std::int32_t zone, const std::int64_t seconds) {
    return type_at(zone_of(zone), seconds).offset;
}

std::int32_t fb_tz_daylight(const std::int32_t zone, const std::int64_t seconds) {
    return type_at(zone_of(zone), seconds).daylight ? 1 : 0;
}

fb_string_t* fb_tz_abbreviation(const std::int32_t zone, const std::int64_t seconds) {
    return string_of(type_at(zone_of(zone), seconds).abbreviation);
}

std::int32_t fb_tz_local_offsets(const std::int32_t zone, const std::int64_t local,
                                 fb_array_t* output) {
    // The offsets two days either side are those before and after any change near it, since no
    // zone changes its clocks twice in four days (the database's closest changes are a week
    // apart). An offset is the clocks' there when the moment it gives has it.
    const zone_t& found = zone_of(zone);
    const std::int32_t before = type_at(found, local - (2 * day_seconds)).offset;
    const std::int32_t after = type_at(found, local + (2 * day_seconds)).offset;
    const bool before_valid = type_at(found, local - before).offset == before;
    const bool after_valid = type_at(found, local - after).offset == after;

    std::int32_t count = 0;
    std::array<std::int32_t, 2> offsets{ before, after };
    if (before == after || (before_valid != after_valid)) {
        count = 1;
        offsets.fill(before_valid ? before : after);
    } else if (before_valid) {
        count = 2;
    }

    fb_array_reserve(output, sizeof(std::int32_t), 2, nullptr);
    output->count = 2;
    std::ranges::copy(offsets, static_cast<std::int32_t*>(output->data));
    return count;
}

fb_string_t* fb_tz_local_name() {
    return string_of(os_zone_name());
}

std::int32_t fb_tz_system_offset(const std::int64_t seconds) {
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
    std::tm utc{};
#ifdef _WIN32
    localtime_s(&local, &time);
    gmtime_s(&utc, &time);
#else
    localtime_r(&time, &local);
    gmtime_r(&time, &utc);
#endif
    const auto seconds_of = [](const std::tm& when) {
        return (days_from_civil(when.tm_year + 1900LL, when.tm_mon + 1LL, when.tm_mday) *
                day_seconds) +
               (when.tm_hour * 3600LL) + (when.tm_min * 60LL) + when.tm_sec;
    };
    return static_cast<std::int32_t>(seconds_of(local) - seconds_of(utc));
}
}
