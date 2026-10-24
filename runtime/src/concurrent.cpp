// std.concurrent's locks: a Semaphore, a Barrier, a WaitGroup, a Once, and an RwLock, each a
// mutex and a condition variable over its counts, the signals that finish a TaskSource's Tasks,
// and a Cancellation's flag, which wakes the waits that take it. A Shared Class holds each one's
// handle, the address as a UInt64.

#include "fb/concurrent.hpp"

#include "fb/runtime.hpp"
#include "fb/time.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

static_assert(sizeof(void*) == sizeof(std::uint64_t), "a handle holds an address");

/// The most an Int holds, which a count of permits or a WaitGroup's count stays within.
constexpr std::int64_t most = std::numeric_limits<std::int32_t>::max();

template <typename T>
std::uint64_t handle_of(T* object) {
    return std::bit_cast<std::uint64_t>(object);
}

template <typename T>
T& object_of(const std::uint64_t handle) {
    return *std::bit_cast<T*>(handle);
}

struct semaphore_t {
    std::mutex mutex;
    std::condition_variable released;
    std::int64_t count;
};

struct barrier_t {
    std::mutex mutex;
    std::condition_variable passed;
    std::int64_t count;
    /// How many threads wait in this round, and how many rounds have passed.
    std::int64_t waiting = 0;
    std::uint64_t round = 0;
};

struct wait_group_t {
    std::mutex mutex;
    std::condition_variable emptied;
    std::int64_t count = 0;
};

struct once_t {
    std::mutex mutex;
    std::condition_variable ran;
    bool running = false;
    bool done = false;
    /// The thread running the work, while one is.
    std::thread::id runner;
};

struct rwlock_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::int64_t readers = 0;
    std::int64_t writers_waiting = 0;
    bool writing = false;
    std::thread::id writer;
};

/// The RwLocks the thread holds, as reader or writer, so taking one again panics instead of
/// waiting for itself.
thread_local std::vector<const rwlock_t*> t_held;

bool holds(const rwlock_t& lock) {
    return std::ranges::contains(t_held, &lock);
}

struct signal_t {
    std::mutex mutex;
    bool given = false;
    /// The Tasks to finish once it's given, which it holds until then.
    std::vector<fb_task_t*> tasks;
};

/// A wait on a lock's condition variable, under the lock's mutex, that a Cancellation wakes.
struct waker_t {
    std::mutex* mutex;
    std::condition_variable* condition;
};

struct cancel_t {
    /// Guards `waiting`, and the waits for the flag itself, which `set` wakes.
    std::mutex mutex;
    std::condition_variable set_now;
    std::atomic<bool> set = false;
    /// Each wait's own, so many waits on one lock are each taken off as they end.
    std::vector<const waker_t*> waiting;
};

/// While it lives, a wait on `condition` under `mutex` is one that setting `cancel`, unless it's
/// 0, wakes. It's made before the wait takes `mutex`, and goes once the wait has let go of it,
/// so it never holds `mutex` while it takes the Cancellation's, which fb_cancel_set holds while it
/// takes `mutex`.
class registration_t {
public:
    registration_t(const std::uint64_t cancel, std::mutex& mutex,
                   std::condition_variable& condition)
        : m_cancel(cancel == 0 ? nullptr : &object_of<cancel_t>(cancel)),
          m_waker{ .mutex = &mutex, .condition = &condition } {
        if (m_cancel != nullptr) {
            const std::scoped_lock lock(m_cancel->mutex);
            m_cancel->waiting.push_back(&m_waker);
        }
    }

    registration_t(const registration_t&) = delete;
    registration_t& operator=(const registration_t&) = delete;
    registration_t(registration_t&&) = delete;
    registration_t& operator=(registration_t&&) = delete;

    ~registration_t() {
        if (m_cancel != nullptr) {
            const std::scoped_lock lock(m_cancel->mutex);
            std::erase(m_cancel->waiting, &m_waker);
        }
    }

    /// Whether the Cancellation is set, which the wait asks under its mutex.
    [[nodiscard]] bool cancelled() const {
        return m_cancel != nullptr && m_cancel->set.load(std::memory_order_acquire);
    }

private:
    cancel_t* m_cancel;
    waker_t m_waker;
};

} // namespace

extern "C" {

std::uint64_t fb_semaphore_new(const std::int64_t count) {
    return handle_of(new semaphore_t{ .count = count });
}

void fb_semaphore_acquire(const std::uint64_t semaphore) {
    auto& state = object_of<semaphore_t>(semaphore);
    std::unique_lock lock(state.mutex);
    state.released.wait(lock, [&] { return state.count > 0; });
    --state.count;
}

std::int32_t fb_semaphore_acquire_cancel(const std::uint64_t semaphore,
                                         const std::uint64_t cancel) {
    auto& state = object_of<semaphore_t>(semaphore);
    const registration_t registration(cancel, state.mutex, state.released);
    std::unique_lock lock(state.mutex);
    state.released.wait(lock, [&] { return state.count > 0 || registration.cancelled(); });
    if (registration.cancelled()) {
        // A Release may have woken this wait alone, for a permit it leaves, so it wakes another.
        if (state.count > 0) {
            lock.unlock();
            state.released.notify_one();
        }

        return 0;
    }

    --state.count;
    return 1;
}

std::int32_t fb_semaphore_try_acquire(const std::uint64_t semaphore) {
    auto& state = object_of<semaphore_t>(semaphore);
    const std::scoped_lock lock(state.mutex);
    if (state.count == 0) {
        return 0;
    }

    --state.count;
    return 1;
}

std::int32_t fb_semaphore_release(const std::uint64_t semaphore, const std::int64_t count) {
    auto& state = object_of<semaphore_t>(semaphore);
    {
        const std::scoped_lock lock(state.mutex);
        if (count > most - state.count) {
            return 0;
        }

        state.count += count;
    }

    if (count == 1) {
        state.released.notify_one();
    } else {
        state.released.notify_all();
    }

    return 1;
}

std::int64_t fb_semaphore_count(const std::uint64_t semaphore) {
    auto& state = object_of<semaphore_t>(semaphore);
    const std::scoped_lock lock(state.mutex);
    return state.count;
}

void fb_semaphore_free(const std::uint64_t semaphore) {
    delete &object_of<semaphore_t>(semaphore);
}

std::uint64_t fb_barrier_new(const std::int64_t count) {
    return handle_of(new barrier_t{ .count = count });
}

std::int32_t fb_barrier_wait(const std::uint64_t barrier) {
    auto& state = object_of<barrier_t>(barrier);
    std::unique_lock lock(state.mutex);
    const std::uint64_t round = state.round;
    ++state.waiting;
    if (state.waiting == state.count) {
        state.waiting = 0;
        ++state.round;
        lock.unlock();
        state.passed.notify_all();
        return 1;
    }

    state.passed.wait(lock, [&] { return state.round != round; });
    return 0;
}

void fb_barrier_free(const std::uint64_t barrier) {
    delete &object_of<barrier_t>(barrier);
}

std::uint64_t fb_wait_group_new() {
    return handle_of(new wait_group_t);
}

std::int32_t fb_wait_group_add(const std::uint64_t group, const std::int64_t count) {
    auto& state = object_of<wait_group_t>(group);
    {
        const std::scoped_lock lock(state.mutex);
        const std::int64_t next = state.count + count;
        if (next < 0) {
            return 1;
        }

        if (next > most) {
            return 2;
        }

        state.count = next;
        if (next != 0) {
            return 0;
        }
    }

    state.emptied.notify_all();
    return 0;
}

void fb_wait_group_wait(const std::uint64_t group) {
    auto& state = object_of<wait_group_t>(group);
    std::unique_lock lock(state.mutex);
    state.emptied.wait(lock, [&] { return state.count == 0; });
}

std::int32_t fb_wait_group_wait_cancel(const std::uint64_t group, const std::uint64_t cancel) {
    auto& state = object_of<wait_group_t>(group);
    const registration_t registration(cancel, state.mutex, state.emptied);
    std::unique_lock lock(state.mutex);
    state.emptied.wait(lock, [&] { return state.count == 0 || registration.cancelled(); });
    return registration.cancelled() ? 0 : 1;
}

std::int64_t fb_wait_group_count(const std::uint64_t group) {
    auto& state = object_of<wait_group_t>(group);
    const std::scoped_lock lock(state.mutex);
    return state.count;
}

void fb_wait_group_free(const std::uint64_t group) {
    delete &object_of<wait_group_t>(group);
}

std::uint64_t fb_once_new() {
    return handle_of(new once_t);
}

std::int32_t fb_once_begin(const std::uint64_t once) {
    auto& state = object_of<once_t>(once);
    std::unique_lock lock(state.mutex);
    if (state.done) {
        return 0;
    }

    if (state.running && state.runner == std::this_thread::get_id()) {
        return 2;
    }

    if (state.running) {
        state.ran.wait(lock, [&] { return state.done; });
        return 0;
    }

    state.running = true;
    state.runner = std::this_thread::get_id();
    return 1;
}

void fb_once_end(const std::uint64_t once) {
    auto& state = object_of<once_t>(once);
    {
        const std::scoped_lock lock(state.mutex);
        state.running = false;
        state.done = true;
    }

    state.ran.notify_all();
}

std::int32_t fb_once_done(const std::uint64_t once) {
    auto& state = object_of<once_t>(once);
    const std::scoped_lock lock(state.mutex);
    return state.done ? 1 : 0;
}

void fb_once_free(const std::uint64_t once) {
    delete &object_of<once_t>(once);
}

std::uint64_t fb_rwlock_new() {
    return handle_of(new rwlock_t);
}

std::int32_t fb_rwlock_read(const std::uint64_t lock) {
    auto& state = object_of<rwlock_t>(lock);
    if (holds(state)) {
        return 1;
    }

    std::unique_lock guard(state.mutex);
    state.changed.wait(guard, [&] { return !state.writing && state.writers_waiting == 0; });
    ++state.readers;
    t_held.push_back(&state);
    return 0;
}

std::int32_t fb_rwlock_write(const std::uint64_t lock) {
    auto& state = object_of<rwlock_t>(lock);
    if (holds(state)) {
        return 1;
    }

    std::unique_lock guard(state.mutex);
    ++state.writers_waiting;
    state.changed.wait(guard, [&] { return !state.writing && state.readers == 0; });
    --state.writers_waiting;
    state.writing = true;
    state.writer = std::this_thread::get_id();
    t_held.push_back(&state);
    return 0;
}

void fb_rwlock_release(const std::uint64_t lock) {
    auto& state = object_of<rwlock_t>(lock);
    std::erase(t_held, &state);
    {
        const std::scoped_lock guard(state.mutex);
        if (state.writing && state.writer == std::this_thread::get_id()) {
            state.writing = false;
            state.writer = {};
        } else {
            --state.readers;
            if (state.readers != 0) {
                return;
            }
        }
    }

    state.changed.notify_all();
}

void fb_rwlock_free(const std::uint64_t lock) {
    delete &object_of<rwlock_t>(lock);
}

std::uint64_t fb_signal_new() {
    return handle_of(new signal_t);
}

void fb_signal_task(fb_task_t* task, const std::uint64_t signal) {
    auto& state = object_of<signal_t>(signal);
    {
        const std::scoped_lock lock(state.mutex);
        if (!state.given) {
            fb_object_retain(&task->header);
            state.tasks.push_back(task);
            return;
        }
    }

    fb_task_finish(task);
}

std::int32_t fb_signal_give(const std::uint64_t signal) {
    auto& state = object_of<signal_t>(signal);
    std::vector<fb_task_t*> tasks;
    {
        const std::scoped_lock lock(state.mutex);
        if (state.given) {
            return 0;
        }

        state.given = true;
        tasks = std::exchange(state.tasks, {});
    }

    for (fb_task_t* task : tasks) {
        fb_task_finish(task);
        fb_object_release(&task->header);
    }

    return 1;
}

std::int32_t fb_signal_given(const std::uint64_t signal) {
    auto& state = object_of<signal_t>(signal);
    const std::scoped_lock lock(state.mutex);
    return state.given ? 1 : 0;
}

void fb_signal_free(const std::uint64_t signal) {
    auto* state = &object_of<signal_t>(signal);
    for (fb_task_t* task : state->tasks) {
        fb_object_release(&task->header);
    }

    delete state;
}

std::uint64_t fb_cancel_new() {
    return handle_of(new cancel_t);
}

std::int32_t fb_cancel_set(const std::uint64_t cancel) {
    auto& state = object_of<cancel_t>(cancel);
    {
        const std::scoped_lock lock(state.mutex);
        if (state.set.exchange(true, std::memory_order_acq_rel)) {
            return 0;
        }

        // A wait looks at the flag under its own mutex, so one that looked before it was set is
        // waiting by the time this has its mutex, and the notify reaches it.
        for (const waker_t* waker : state.waiting) {
            {
                const std::scoped_lock wait(*waker->mutex);
            }

            waker->condition->notify_all();
        }
    }

    state.set_now.notify_all();
    // The waits for a change in the pool's generation, a Channel's, look again too.
    fb_pool_signal();
    return 1;
}

std::int32_t fb_cancel_is_set(const std::uint64_t cancel) {
    return object_of<cancel_t>(cancel).set.load(std::memory_order_acquire) ? 1 : 0;
}

std::int32_t fb_cancel_wait(const std::uint64_t cancel, const std::int64_t nanoseconds) {
    auto& state = object_of<cancel_t>(cancel);
    const auto end = fb::time::sleep_end(std::chrono::steady_clock::now(), nanoseconds);
    std::unique_lock lock(state.mutex);
    // A day at most at a time, as fb_time_sleep sleeps, which any OS's wait takes.
    while (!state.set.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) {
            return 0;
        }

        const auto until = end - now < std::chrono::hours(24) ? end : now + std::chrono::hours(24);
        state.set_now.wait_until(lock, until);
    }

    return 1;
}

void fb_cancel_free(const std::uint64_t cancel) {
    delete &object_of<cancel_t>(cancel);
}
}
