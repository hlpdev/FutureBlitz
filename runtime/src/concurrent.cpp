// std.concurrent's locks: a Semaphore, a Barrier, a WaitGroup, a Once, and an RwLock, each a
// mutex and a condition variable over its counts, and the signals that finish a TaskSource's
// Tasks. A Shared Class holds each one's handle, the address as a UInt64.

#include "fb/concurrent.hpp"

#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <bit>
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
            fb::runtime::start_source_task(task);
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
        fb::runtime::finish_source_task(task);
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
        fb::runtime::finish_source_task(task);
        fb_object_release(&task->header);
    }

    delete state;
}
}
