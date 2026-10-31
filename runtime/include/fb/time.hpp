#pragma once

#include "fb/runtime.hpp"

#include <chrono>
#include <cstdint>

// std.time's functions in the runtime: the clocks and Sleep, which every program has; the
// fb_io_ timer, over libuv on the I/O thread; and the fb_tz_ ones over the IANA tz database,
// which third_party/ compiles with its zic and the runtime embeds, so a program links the zones
// only when it calls one. A zone is a number from 1, its place in the runtime's table, and 0
// means none. Strings are only borrowed, and Strings given back are the caller's.

extern "C" {

/// The real clock: nanoseconds since 1970-01-01T00:00:00Z, which an Int64 holds until 2262.
std::int64_t fb_time_now();
/// The monotonic clock: nanoseconds since a moment of its own, which never goes back.
std::int64_t fb_time_monotonic();
/// Blocks the thread for `nanoseconds`, and returns at once for none or fewer.
void fb_time_sleep(std::int64_t nanoseconds);
/// A String of the code points in `chars`, an Int32[], from `start` to before `finish`, which
/// std.time's formats take apart, with U+FFFD for any that isn't one.
fb_string_t* fb_time_text(const fb_array_t* chars, std::int64_t start, std::int64_t finish);

/// Finishes `task`, a new Task, once `nanoseconds` have passed on the monotonic clock, from the
/// I/O thread's timer, so an Await of it resumes where it started.
void fb_io_after(fb_task_t* task, std::int64_t nanoseconds);

/// The zone named `name`, as the tz database spells it or in any case, or 0.
std::int32_t fb_tz_find(const fb_string_t* name);
/// How many zones there are, and each one's name, from 1, in order.
std::int32_t fb_tz_count();
fb_string_t* fb_tz_name(std::int32_t zone);
/// The database's version, like "2026e".
fb_string_t* fb_tz_version();
/// The zone's offset from UTC, in seconds, at `seconds` since 1970-01-01T00:00:00Z.
std::int32_t fb_tz_offset(std::int32_t zone, std::int64_t seconds);
/// 1 when the zone is on daylight saving time then, and 0 otherwise.
std::int32_t fb_tz_daylight(std::int32_t zone, std::int64_t seconds);
/// The zone's abbreviation then, like "CEST", or an offset like "+0530" where it has none.
fb_string_t* fb_tz_abbreviation(std::int32_t zone, std::int64_t seconds);
/// How many offsets the zone's clocks show `local`, seconds since 1970-01-01T00:00 of its own:
/// 1 most of the time, 2 when they're turned back past it, or 0 when they're turned forward past
/// it. `output`, an Int32[], gets the offset before the change and the one after, the same
/// offset twice for 1.
std::int32_t fb_tz_local_offsets(std::int32_t zone, std::int64_t local, fb_array_t* output);
/// The OS's zone's name, as the tz database spells it, or "" when the OS doesn't say.
fb_string_t* fb_tz_local_name();
/// The OS's offset from UTC at `seconds`, from its own zone rules, for when its zone's name isn't
/// one the database has.
std::int32_t fb_tz_system_offset(std::int64_t seconds);
}

namespace fb::time {

/// When a Sleep of `nanoseconds` from `now` ends, or the monotonic clock's latest time for one
/// longer than it counts, past which a sum would wrap.
[[nodiscard]] std::chrono::steady_clock::time_point
sleep_end(std::chrono::steady_clock::time_point now, std::int64_t nanoseconds);

/// Blocks the thread for `time`, to about a millisecond, though the OS can wake it a little
/// early. On Windows, whose sleeps wait for its 15.6 ms timer tick, it's a high-resolution
/// waitable timer of the thread's, or where Windows has none, a sleep with the timer at 1 ms for
/// the sleep alone; the process's timer resolution stays as it was.
void sleep_precisely(std::chrono::nanoseconds time);

} // namespace fb::time
