// std.time's clocks and Sleep, which every program has: the real clock, the monotonic clock, and
// blocking the thread; and the text std.time's formats take apart.

#include "fb/time.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Windows 10 1803's, which older headers don't name.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace {

#ifdef _WIN32

/// A thread's high-resolution waitable timer, or none where Windows hasn't them.
class sleep_timer_t {
public:
    sleep_timer_t()
        : m_handle(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS)) {}

    sleep_timer_t(const sleep_timer_t&) = delete;
    sleep_timer_t& operator=(const sleep_timer_t&) = delete;
    sleep_timer_t(sleep_timer_t&&) = delete;
    sleep_timer_t& operator=(sleep_timer_t&&) = delete;

    ~sleep_timer_t() {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
    }

    /// Sleeps for `time`, or gives false when it can't.
    [[nodiscard]] bool sleep(const std::chrono::nanoseconds time) const {
        // Due after `time`, as a negative count of 100-nanosecond intervals, rounded up.
        LARGE_INTEGER due{};
        due.QuadPart = -((time.count() + 99) / 100);
        return m_handle != nullptr &&
               SetWaitableTimerEx(m_handle, &due, 0, nullptr, nullptr, nullptr, 0) != 0 &&
               WaitForSingleObject(m_handle, INFINITE) == WAIT_OBJECT_0;
    }

private:
    HANDLE m_handle;
};

/// Sleeps with the system's timer at 1 ms for the sleep alone, as Windows before 10 1803 needs
/// for a sleep shorter than its tick. winmm is loaded for it only there, so a program needn't
/// link it.
void sleep_at_a_millisecond(const std::chrono::nanoseconds time) {
    // Through void (*)(), which a function pointer of any type converts to and from.
    using period_t = UINT(WINAPI*)(UINT);
    static const HMODULE winmm =
        LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto begin = winmm != nullptr
                                  ? reinterpret_cast<period_t>(reinterpret_cast<void (*)()>(
                                        GetProcAddress(winmm, "timeBeginPeriod")))
                                  : nullptr;
    static const auto end =
        winmm != nullptr ? reinterpret_cast<period_t>(
                               reinterpret_cast<void (*)()>(GetProcAddress(winmm, "timeEndPeriod")))
                         : nullptr;
    const bool raised = begin != nullptr && end != nullptr && begin(1) == 0;
    std::this_thread::sleep_for(time);
    if (raised) {
        end(1);
    }
}

#endif

} // namespace

void fb::time::sleep_precisely(const std::chrono::nanoseconds time) {
    if (time <= std::chrono::nanoseconds::zero()) {
        return;
    }

#ifdef _WIN32
    thread_local const sleep_timer_t timer;
    if (!timer.sleep(time)) {
        sleep_at_a_millisecond(time);
    }
#else
    std::this_thread::sleep_for(time);
#endif
}

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
        fb::time::sleep_precisely(std::chrono::ceil<std::chrono::nanoseconds>(
            std::min<std::chrono::steady_clock::duration>(end - now, std::chrono::hours(24))));
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
