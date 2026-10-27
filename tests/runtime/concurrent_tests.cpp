#include "fb/concurrent.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// std.concurrent's locks in the runtime, from threads of the test's own. A thread that should
// wait is given a moment to go on wrongly before the test lets it.

namespace {

void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

} // namespace

TEST_CASE("a Semaphore's permits are taken and given back", "[runtime][concurrent]") {
    const std::uint64_t semaphore = fb_semaphore_new(1);
    CHECK(fb_semaphore_try_acquire(semaphore) == 1);
    CHECK(fb_semaphore_try_acquire(semaphore) == 0);
    std::atomic<bool> acquired = false;
    std::thread waiter([&] {
        fb_semaphore_acquire(semaphore);
        acquired.store(true);
    });
    settle();
    CHECK_FALSE(acquired.load());
    CHECK(fb_semaphore_release(semaphore, 1) == 1);
    waiter.join();
    CHECK(acquired.load());
    CHECK(fb_semaphore_count(semaphore) == 0);

    // Permits stay within what an Int holds.
    CHECK(fb_semaphore_release(semaphore, 2147483647) == 1);
    CHECK(fb_semaphore_release(semaphore, 1) == 0);
    CHECK(fb_semaphore_count(semaphore) == 2147483647);
    fb_semaphore_free(semaphore);
}

TEST_CASE("a Barrier lets a round's threads go once the last comes", "[runtime][concurrent]") {
    constexpr std::int64_t threads = 4;
    constexpr std::int64_t rounds = 50;
    const std::uint64_t barrier = fb_barrier_new(threads);
    std::atomic<std::int64_t> arrived = 0;
    std::atomic<std::int64_t> leaders = 0;
    std::atomic<std::int64_t> early = 0;
    std::vector<std::thread> started;
    started.reserve(threads);
    for (std::int64_t t = 0; t < threads; ++t) {
        started.emplace_back([&] {
            for (std::int64_t round = 1; round <= rounds; ++round) {
                arrived.fetch_add(1);
                leaders.fetch_add(fb_barrier_wait(barrier));
                if (arrived.load() < round * threads) {
                    early.fetch_add(1);
                }

                leaders.fetch_add(fb_barrier_wait(barrier));
            }
        });
    }

    for (std::thread& thread : started) {
        thread.join();
    }

    CHECK(leaders.load() == 2 * rounds);
    CHECK(early.load() == 0);
    fb_barrier_free(barrier);
}

TEST_CASE("a WaitGroup's Wait returns at 0, which its count stays at or above",
          "[runtime][concurrent]") {
    const std::uint64_t group = fb_wait_group_new();
    fb_wait_group_wait(group);
    CHECK(fb_wait_group_add(group, 2) == 0);
    CHECK(fb_wait_group_add(group, -3) == 1);
    CHECK(fb_wait_group_add(group, 2147483646) == 2);
    CHECK(fb_wait_group_count(group) == 2);
    std::atomic<bool> woke = false;
    std::thread waiter([&] {
        fb_wait_group_wait(group);
        woke.store(true);
    });
    CHECK(fb_wait_group_add(group, -1) == 0);
    settle();
    CHECK_FALSE(woke.load());
    CHECK(fb_wait_group_add(group, -1) == 0);
    waiter.join();
    CHECK(woke.load());
    fb_wait_group_free(group);
}

TEST_CASE("a Once runs its work on the first thread, and the others wait for it",
          "[runtime][concurrent]") {
    const std::uint64_t once = fb_once_new();
    CHECK(fb_once_begin(once) == 1);
    // The thread running it would wait for itself.
    CHECK(fb_once_begin(once) == 2);
    std::atomic<bool> returned = false;
    std::atomic<std::int32_t> began = -1;
    std::thread other([&] {
        began.store(fb_once_begin(once));
        returned.store(true);
    });
    settle();
    CHECK_FALSE(returned.load());
    CHECK(fb_once_done(once) == 0);
    fb_once_end(once);
    other.join();
    CHECK(began.load() == 0);
    CHECK(fb_once_done(once) == 1);
    CHECK(fb_once_begin(once) == 0);
    fb_once_free(once);
}

TEST_CASE("an RwLock has readers at once, or a writer alone", "[runtime][concurrent]") {
    const std::uint64_t lock = fb_rwlock_new();
    REQUIRE(fb_rwlock_read(lock) == 0);
    // Two readers at once.
    std::atomic<std::int32_t> read = -1;
    std::thread reader([&] {
        read.store(fb_rwlock_read(lock));
        fb_rwlock_release(lock);
    });
    reader.join();
    CHECK(read.load() == 0);

    // A thread that holds it can't take it again, either way.
    CHECK(fb_rwlock_read(lock) == 1);
    CHECK(fb_rwlock_write(lock) == 1);

    // A writer waits for the reader.
    std::atomic<int> order = 0;
    std::atomic<int> wrote_at = 0;
    std::atomic<std::int32_t> read_again = -1;
    std::thread writer([&] {
        if (fb_rwlock_write(lock) == 0) {
            wrote_at.store(++order);
        }

        read_again.store(fb_rwlock_read(lock));
        fb_rwlock_release(lock);
    });
    settle();
    CHECK(wrote_at.load() == 0);

    // A reader that comes while the writer waits waits for it too. One that comes before the
    // writer has started waiting reads at once, so the test tries again with another.
    std::vector<std::thread> readers;
    std::vector<std::unique_ptr<std::atomic<int>>> read_at;
    bool waited = false;
    for (int attempt = 0; attempt < 100 && !waited; ++attempt) {
        read_at.push_back(std::make_unique<std::atomic<int>>(0));
        std::atomic<int>* at = read_at.back().get();
        readers.emplace_back([&, at] {
            if (fb_rwlock_read(lock) == 0) {
                at->store(++order);
                fb_rwlock_release(lock);
            }
        });
        settle();
        waited = at->load() == 0;
    }

    CHECK(waited);
    CHECK(wrote_at.load() == 0);
    fb_rwlock_release(lock);
    writer.join();
    for (std::thread& thread : readers) {
        thread.join();
    }

    // The writer went before the reader that waited, which came last.
    CHECK(read_again.load() == 1);
    CHECK(wrote_at.load() != 0);
    CHECK(read_at.back()->load() == order.load());
    CHECK(wrote_at.load() < read_at.back()->load());
    fb_rwlock_free(lock);
}

TEST_CASE("a Cancellation stops the waits that take it, each of them", "[runtime][concurrent]") {
    const std::uint64_t cancel = fb_cancel_new();
    CHECK(fb_cancel_is_set(cancel) == 0);

    // Three wait for a permit: one gets the permit given, and the Cancel stops the other two.
    const std::uint64_t semaphore = fb_semaphore_new(0);
    std::atomic<int> took = 0;
    std::atomic<int> stopped = 0;
    std::vector<std::thread> waiters;
    waiters.reserve(3);
    for (int i = 0; i < 3; ++i) {
        waiters.emplace_back([&] {
            if (fb_semaphore_acquire_cancel(semaphore, cancel) == 1) {
                ++took;
            } else {
                ++stopped;
            }
        });
    }

    settle();
    CHECK(fb_semaphore_release(semaphore, 1) == 1);
    settle();
    CHECK(took.load() == 1);
    CHECK(fb_cancel_set(cancel) == 1);
    for (std::thread& waiter : waiters) {
        waiter.join();
    }

    CHECK(stopped.load() == 2);
    CHECK(fb_cancel_set(cancel) == 0);
    CHECK(fb_cancel_is_set(cancel) == 1);

    // Once it's set, a wait stops at once, and takes no permit there is.
    CHECK(fb_semaphore_release(semaphore, 1) == 1);
    CHECK(fb_semaphore_acquire_cancel(semaphore, cancel) == 0);
    CHECK(fb_semaphore_count(semaphore) == 1);
    const std::uint64_t group = fb_wait_group_new();
    CHECK(fb_wait_group_wait_cancel(group, cancel) == 0);
    CHECK(fb_cancel_wait(cancel, 3600000000000) == 1);
    fb_wait_group_free(group);
    fb_semaphore_free(semaphore);
    fb_cancel_free(cancel);
}

TEST_CASE("a WaitGroup's Wait and a Cancellation's own wait go on as they would without one",
          "[runtime][concurrent]") {
    const std::uint64_t cancel = fb_cancel_new();
    const std::uint64_t group = fb_wait_group_new();
    CHECK(fb_wait_group_wait_cancel(group, cancel) == 1);
    CHECK(fb_wait_group_add(group, 1) == 0);
    std::atomic<std::int32_t> waited = -1;
    std::thread waiter([&] { waited.store(fb_wait_group_wait_cancel(group, cancel)); });
    settle();
    CHECK(waited.load() == -1);
    CHECK(fb_wait_group_add(group, -1) == 0);
    waiter.join();
    CHECK(waited.load() == 1);

    // Its own wait gives up at the time, or goes on once it's set.
    CHECK(fb_cancel_wait(cancel, 1000000) == 0);
    CHECK(fb_cancel_wait(cancel, 0) == 0);
    std::thread setter([&] {
        settle();
        fb_cancel_set(cancel);
    });
    CHECK(fb_cancel_wait(cancel, 3600000000000) == 1);
    setter.join();
    fb_wait_group_free(group);
    fb_cancel_free(cancel);
}

TEST_CASE("the pool's generation wait goes on at a change, a Cancel, or its time",
          "[runtime][concurrent]") {
    const std::uint64_t seen = fb_pool_generation();
    CHECK(fb_pool_wait(seen, 0, 1000000) == 0);
    std::thread signaller([] {
        settle();
        fb_pool_signal();
    });
    CHECK(fb_pool_wait(seen, 0, 3600000000000) == 1);
    signaller.join();
    CHECK(fb_pool_generation() != seen);

    const std::uint64_t cancel = fb_cancel_new();
    const std::uint64_t now = fb_pool_generation();
    std::thread canceller([cancel] {
        settle();
        fb_cancel_set(cancel);
    });
    CHECK(fb_pool_wait(now, cancel, 3600000000000) == 2);
    canceller.join();
    CHECK(fb_pool_wait(fb_pool_generation(), cancel, 3600000000000) == 2);
    fb_cancel_free(cancel);
}

TEST_CASE("a thread's number is its own while it runs, and never 0", "[runtime][concurrent]") {
    const std::uint32_t mine = fb_thread_number();
    CHECK(mine != 0);
    CHECK(fb_thread_number() == mine);
    std::uint32_t other = 0;
    std::thread([&other] { other = fb_thread_number(); }).join();
    CHECK(other != 0);
    CHECK(other != mine);
}
