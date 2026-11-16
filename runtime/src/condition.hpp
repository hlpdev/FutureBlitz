#pragma once

// A condition variable whose timed waits end within about a millisecond of their time on
// Windows too, where std::condition_variable's waits last whole ticks of the system's timer,
// 15.6 ms, however short they're asked to be.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace fb::runtime {

/// std::condition_variable's waits and notifies. On Windows, a timed wait waits on its thread's
/// own event and a high-resolution waitable timer, which a notify sets and the time ends, where
/// Windows has those timers; elsewhere, and without them, it's std::condition_variable's.
class condition_t {
public:
    void notify_all() {
        m_changed.notify_all();
        wake();
    }

    void notify_one() {
        m_changed.notify_one();
        wake();
    }

    void wait(std::unique_lock<std::mutex>& lock) {
        m_changed.wait(lock);
    }

    template <typename Ready>
    void wait(std::unique_lock<std::mutex>& lock, Ready ready) {
        m_changed.wait(lock, ready);
    }

    /// Waits until a notify or `until`, or not at all once it's passed; it can end early, as
    /// any condition variable's wait can.
    void wait_until(std::unique_lock<std::mutex>& lock,
                    std::chrono::steady_clock::time_point until);

    /// Waits until `ready` holds, or `until` passes, and says whether it holds.
    template <typename Ready>
    bool wait_until(std::unique_lock<std::mutex>& lock,
                    const std::chrono::steady_clock::time_point until, Ready ready) {
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= until) {
                return ready();
            }

            wait_until(lock, until);
        }

        return true;
    }

private:
    std::condition_variable m_changed;
#ifdef _WIN32
    /// The events of the threads in a timed wait, which a notify sets.
    std::mutex m_waiting_mutex;
    std::vector<void*> m_waiting;

    void wake();
#else
    void wake() {}
#endif
};

} // namespace fb::runtime
