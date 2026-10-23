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

void pause() {
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
    pause();
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
    pause();
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
    pause();
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
    pause();
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
        pause();
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
