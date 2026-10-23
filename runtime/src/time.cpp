// std.time's clocks and Sleep, which every program has: the real clock, the monotonic clock, and
// blocking the thread; and the text std.time's formats take apart.

#include "fb/time.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

std::chrono::steady_clock::time_point
fb::time::sleep_end(const std::chrono::steady_clock::time_point now,
                    const std::int64_t nanoseconds) {
    const auto wanted = std::chrono::ceil<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds(nanoseconds));
    if (wanted >= std::chrono::steady_clock::time_point::max() - now) {
        return std::chrono::steady_clock::time_point::max();
    }

    return now + wanted;
}

extern "C" {

std::int64_t fb_time_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t fb_time_monotonic() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void fb_time_sleep(const std::int64_t nanoseconds) {
    // The OS can wake a thread a little before its time, as Windows does by a part of a
    // millisecond, so it sleeps again until the monotonic clock says the time has gone by. It
    // sleeps a day at most at a time, which any OS takes.
    if (nanoseconds <= 0) {
        return;
    }

    const auto end = fb::time::sleep_end(std::chrono::steady_clock::now(), nanoseconds);
    for (auto now = std::chrono::steady_clock::now(); now < end;
         now = std::chrono::steady_clock::now()) {
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(end - now, std::chrono::hours(24)));
    }
}

fb_string_t* fb_time_text(const fb_array_t* chars, std::int64_t start, std::int64_t finish) {
    start = std::clamp<std::int64_t>(start, 0, chars->count);
    finish = std::clamp<std::int64_t>(finish, start, chars->count);
    const auto* codes = static_cast<const std::int32_t*>(chars->data);
    std::string text;
    for (std::int64_t i = start; i < finish; ++i) {
        auto code = static_cast<std::uint32_t>(codes[i]);
        if (code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU)) {
            code = 0xfffdU;
        }

        if (code < 0x80U) {
            text += static_cast<char>(code);
        } else if (code < 0x800U) {
            text += static_cast<char>(0xc0U | (code >> 6U));
            text += static_cast<char>(0x80U | (code & 0x3fU));
        } else if (code < 0x10000U) {
            text += static_cast<char>(0xe0U | (code >> 12U));
            text += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
            text += static_cast<char>(0x80U | (code & 0x3fU));
        } else {
            text += static_cast<char>(0xf0U | (code >> 18U));
            text += static_cast<char>(0x80U | ((code >> 12U) & 0x3fU));
            text += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
            text += static_cast<char>(0x80U | (code & 0x3fU));
        }
    }

    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}
}
