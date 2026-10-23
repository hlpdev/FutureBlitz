// std.time's clocks and Sleep, which every program has: the real clock, the monotonic clock, and
// blocking the thread; and the text std.time's formats take apart.

#include "fb/time.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

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
    if (nanoseconds > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
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
