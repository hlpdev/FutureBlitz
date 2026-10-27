#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.concurrent's functions in the runtime: the Semaphore, Barrier, WaitGroup, Once, and RwLock
// that its Shared Classes hold, the signals that finish a TaskSource's Tasks, and a
// Cancellation's flag. Each is a handle, the address of the runtime's object as a UInt64, which a
// Shared Class's ReadOnly Field can hold, since a Ptr isn't Sendable; its _free function gives it
// back. What waits blocks its thread and runs nothing else meanwhile, as a Channel's Send and
// Receive do. std.concurrent checks counts first and panics where the program went wrong, so what
// these give back says what it panics for.

extern "C" {

/// A Semaphore with `count` permits, 0 or more.
std::uint64_t fb_semaphore_new(std::int64_t count);
/// Takes a permit, waiting while there's none.
void fb_semaphore_acquire(std::uint64_t semaphore);
/// Takes a permit, waiting while there's none, unless `cancel`, a Cancellation's flag, is set
/// first: 1 when it took one, and 0 when it's set, before or while it waits.
std::int32_t fb_semaphore_acquire_cancel(std::uint64_t semaphore, std::uint64_t cancel);
/// Takes a permit unless there's none: 1 when it took one, and 0 when it didn't.
std::int32_t fb_semaphore_try_acquire(std::uint64_t semaphore);
/// Gives back `count` permits, 1 or more, waking the threads waiting for them: 1, or 0 with
/// nothing given back when that would take the permits past the most an Int holds.
std::int32_t fb_semaphore_release(std::uint64_t semaphore, std::int64_t count);
/// How many permits there are now.
std::int64_t fb_semaphore_count(std::uint64_t semaphore);
void fb_semaphore_free(std::uint64_t semaphore);

/// A Barrier that `count` threads meet at each round, 1 or more.
std::uint64_t fb_barrier_new(std::int64_t count);
/// Waits until the round's last thread comes, and gives 1 to that one, which starts the next
/// round, and 0 to the others.
std::int32_t fb_barrier_wait(std::uint64_t barrier);
void fb_barrier_free(std::uint64_t barrier);

/// A WaitGroup counting 0.
std::uint64_t fb_wait_group_new();
/// Adds `count`, which can be negative, waking the threads waiting once the count is 0: 0, or
/// with the count left as it was, 1 when it would go below 0, or 2 when it would go past the
/// most an Int holds.
std::int32_t fb_wait_group_add(std::uint64_t group, std::int64_t count);
/// Waits until the count is 0.
void fb_wait_group_wait(std::uint64_t group);
/// Waits until the count is 0 unless `cancel` is set first: 1 when it's 0, and 0 when `cancel` is
/// set, before or while it waits.
std::int32_t fb_wait_group_wait_cancel(std::uint64_t group, std::uint64_t cancel);
std::int64_t fb_wait_group_count(std::uint64_t group);
void fb_wait_group_free(std::uint64_t group);

/// A Once that hasn't run.
std::uint64_t fb_once_new();
/// 1 when the calling thread is the first, which runs the work and then calls fb_once_end; 0
/// once the work has run, waiting while another thread runs it; or 2 when this thread runs it
/// already, which would wait for itself forever.
std::int32_t fb_once_begin(std::uint64_t once);
/// The work has run, which wakes the threads waiting for it.
void fb_once_end(std::uint64_t once);
/// 1 once the work has run, and 0 before.
std::int32_t fb_once_done(std::uint64_t once);
void fb_once_free(std::uint64_t once);

/// An RwLock's lock, which any number of readers hold at once, or one writer alone. A writer
/// waiting goes before the readers who come after it, so writers aren't kept waiting forever.
std::uint64_t fb_rwlock_new();
/// Takes it to read, waiting while a writer holds it or waits for it: 0, or 1 without taking it
/// when this thread holds it already, which could wait for itself forever.
std::int32_t fb_rwlock_read(std::uint64_t lock);
/// Takes it to write, waiting while anyone holds it: 0, or 1 as fb_rwlock_read gives it.
std::int32_t fb_rwlock_write(std::uint64_t lock);
/// Lets go of it, as this thread took it, waking the threads waiting for it.
void fb_rwlock_release(std::uint64_t lock);
void fb_rwlock_free(std::uint64_t lock);

/// A signal, which finishes the Tasks made for it once it's given.
std::uint64_t fb_signal_new();
/// Finishes `task`, a new Task with no result, once the signal is given, or at once when it was.
void fb_signal_task(fb_task_t* task, std::uint64_t signal);
/// Gives the signal, finishing its Tasks: 1, or 0 when it was given already.
std::int32_t fb_signal_give(std::uint64_t signal);
/// 1 once the signal is given, and 0 before.
std::int32_t fb_signal_given(std::uint64_t signal);
/// Lets go of the Tasks it hasn't finished, which never are.
void fb_signal_free(std::uint64_t signal);

/// A Cancellation's flag, not set, which the waits that take it give up at once it is.
std::uint64_t fb_cancel_new();
/// Sets it, waking every wait that takes it: 1, or 0 when it was set already.
std::int32_t fb_cancel_set(std::uint64_t cancel);
/// 1 once it's set, and 0 before.
std::int32_t fb_cancel_is_set(std::uint64_t cancel);
/// Waits until it's set, for `nanoseconds` at most, which fb_time_sleep's limits bound: 1 once
/// it's set, and 0 when the time's up first.
std::int32_t fb_cancel_wait(std::uint64_t cancel, std::int64_t nanoseconds);
void fb_cancel_free(std::uint64_t cancel);

/// The job pool's generation, which goes up whenever a job is started or done, a Channel or a
/// Task changes, or fb_pool_signal is called, so code that looks at Channels and finds nothing
/// can wait for a change since it looked.
std::uint64_t fb_pool_generation();
/// Wakes the threads waiting for the generation to change.
void fb_pool_signal();
/// Waits until the generation isn't `seen`, unless `cancel`, a Cancellation's flag or 0 for none,
/// is set first, for `nanoseconds` at most, which fb_time_sleep's limits bound: 1 once it changed,
/// 2 once `cancel` is set, and 0 when the time's up.
std::int32_t fb_pool_wait(std::uint64_t seen, std::uint64_t cancel, std::int64_t nanoseconds);

/// The calling thread's number, which no other thread has while it runs, and never 0.
std::uint32_t fb_thread_number();
}
