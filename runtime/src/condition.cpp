// condition_t's timed wait: on Windows, on an event of the thread's and a high-resolution
// waitable timer.

#include "condition.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

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

namespace {

/// A thread's event, which a notify sets, and its high-resolution timer, which a timed wait sets
/// to its end; neither, where Windows hasn't those timers.
class waiter_t {
public:
    waiter_t()
        : m_timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS)),
          m_event(m_timer != nullptr ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr) {}

    waiter_t(const waiter_t&) = delete;
    waiter_t& operator=(const waiter_t&) = delete;
    waiter_t(waiter_t&&) = delete;
    waiter_t& operator=(waiter_t&&) = delete;

    ~waiter_t() {
        for (HANDLE handle : { m_event, m_timer }) {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    }

    [[nodiscard]] HANDLE event() const {
        return m_timer != nullptr ? m_event : nullptr;
    }

    /// Waits for the event, or for `time` to pass, and says whether it could.
    [[nodiscard]] bool wait(const std::chrono::nanoseconds time) const {
        // Due after `time`, as a negative count of 100-nanosecond intervals, rounded up.
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>((time.count() + 99) / 100, 1);
        if (SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0) == 0) {
            return false;
        }

        const std::array<HANDLE, 2> handles{ m_event, m_timer };
        const DWORD woke = WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE);
        CancelWaitableTimer(m_timer);
        return woke == WAIT_OBJECT_0 || woke == WAIT_OBJECT_0 + 1;
    }

private:
    HANDLE m_timer;
    HANDLE m_event;
};

waiter_t& own_waiter() {
    thread_local waiter_t waiter;
    return waiter;
}

} // namespace

void fb::runtime::condition_t::wait_until(std::unique_lock<std::mutex>& lock,
                                          const std::chrono::steady_clock::time_point until) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) {
        return;
    }

    const waiter_t& waiter = own_waiter();
    HANDLE event = waiter.event();
    if (event == nullptr) {
        m_changed.wait_until(lock, until);
        return;
    }

    // It's on the list before the lock goes, so a notify after that, which the caller's state
    // change comes before, sets its event, and the wait sees it at once.
    {
        const std::scoped_lock waiting(m_waiting_mutex);
        m_waiting.push_back(event);
    }

    lock.unlock();
    const bool waited = waiter.wait(until - now);
    lock.lock();
    {
        const std::scoped_lock waiting(m_waiting_mutex);
        std::erase(m_waiting, event);
    }

    if (!waited) {
        m_changed.wait_until(lock, until);
    }
}

void fb::runtime::condition_t::wake() {
    const std::scoped_lock waiting(m_waiting_mutex);
    for (void* event : m_waiting) {
        SetEvent(event);
    }
}

#else

void fb::runtime::condition_t::wait_until(std::unique_lock<std::mutex>& lock,
                                          const std::chrono::steady_clock::time_point until) {
    m_changed.wait_until(lock, until);
}

#endif
