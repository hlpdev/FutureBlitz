#include "fb/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace {

void count_one(void* context) {
    static_cast<std::atomic<std::int64_t>*>(context)->fetch_add(1);
}

/// A job that waits for its test to let it finish.
struct gate_t {
    std::atomic<bool> open = false;
    std::atomic<bool> passed = false;
};

void wait_at_gate(void* context) {
    auto* gate = static_cast<gate_t*>(context);
    while (!gate->open.load()) {
        std::this_thread::yield();
    }

    gate->passed.store(true);
}

/// Fibonacci's numbers the slow way, with a job for each call but the last: every job waits for
/// another, so there are many more waits than workers.
struct fibonacci_t {
    std::int64_t n;
    std::int64_t result;
};

void fibonacci(void* context) {
    auto* call = static_cast<fibonacci_t*>(context);
    if (call->n < 2) {
        call->result = call->n;
        return;
    }

    fibonacci_t first{ .n = call->n - 1, .result = 0 };
    fibonacci_t second{ .n = call->n - 2, .result = 0 };
    fb_job_t* job = fb_job_start(fibonacci, &first);
    fibonacci(&second);
    fb_job_wait(job);
    fb_job_release(job);
    call->result = first.result + second.result;
}

/// How many times a range ran each index, and on which threads.
struct coverage_t {
    std::int64_t begin;
    std::vector<std::atomic<std::int32_t>> hits;
    std::atomic<std::int64_t> chunks = 0;
};

void cover(void* context, const std::int64_t first, const std::int64_t last) {
    auto* coverage = static_cast<coverage_t*>(context);
    coverage->chunks.fetch_add(1);
    for (std::int64_t i = first; i < last; ++i) {
        coverage->hits[static_cast<std::size_t>(i - coverage->begin)].fetch_add(1);
    }
}

bool covered_once(const coverage_t& coverage) {
    return std::ranges::all_of(
        coverage.hits, [](const std::atomic<std::int32_t>& hit) { return hit.load() == 1; });
}

void never(void* /*context*/, std::int64_t /*first*/, std::int64_t /*last*/) {
    FAIL("an empty range ran a chunk");
}

/// A range inside each chunk of another.
void cover_inside(void* context, const std::int64_t first, const std::int64_t last) {
    auto* total = static_cast<std::atomic<std::int64_t>*>(context);
    for (std::int64_t i = first; i < last; ++i) {
        coverage_t inner{ .begin = 0, .hits = std::vector<std::atomic<std::int32_t>>(50) };
        fb_parallel_for(0, 50, cover, &inner);
        if (covered_once(inner)) {
            total->fetch_add(1);
        }
    }
}

} // namespace

TEST_CASE("jobs run on the pool, and a thread waits for them", "[runtime][threads]") {
    CHECK(fb_pool_size() >= 1);
    std::atomic<std::int64_t> count = 0;
    std::vector<fb_job_t*> jobs;
    jobs.reserve(200);
    for (int i = 0; i < 200; ++i) {
        jobs.push_back(fb_job_start(count_one, &count));
    }

    for (fb_job_t* job : jobs) {
        fb_job_wait(job);
        CHECK(fb_job_done(job) == 1);
        fb_job_release(job);
    }

    CHECK(count.load() == 200);
    fb_pool_finish();
}

TEST_CASE("a job is done once its work returns", "[runtime][threads]") {
    gate_t gate;
    fb_job_t* job = fb_job_start(wait_at_gate, &gate);
    // A worker runs it, since this thread doesn't wait for it.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(fb_job_done(job) == 0);
    gate.open.store(true);
    fb_job_wait(job);
    CHECK(fb_job_done(job) == 1);
    CHECK(gate.passed.load());
    fb_job_release(job);
    fb_pool_finish();
}

TEST_CASE("threads that wait run other jobs, so waits can nest", "[runtime][threads]") {
    fibonacci_t call{ .n = 18, .result = 0 };
    fibonacci(&call);
    CHECK(call.result == 2584);
    fb_pool_finish();
}

TEST_CASE("finishing the pool runs the jobs left, and the next job starts it again",
          "[runtime][threads]") {
    std::atomic<std::int64_t> count = 0;
    for (int i = 0; i < 50; ++i) {
        // Released at once: the work runs anyway.
        fb_job_release(fb_job_start(count_one, &count));
    }

    fb_pool_finish();
    CHECK(count.load() == 50);

    fb_job_t* job = fb_job_start(count_one, &count);
    fb_job_wait(job);
    fb_job_release(job);
    CHECK(count.load() == 51);
    fb_pool_finish();
    fb_pool_finish();
}

TEST_CASE("a parallel range runs each index once", "[runtime][threads]") {
    coverage_t coverage{ .begin = -500, .hits = std::vector<std::atomic<std::int32_t>>(1000) };
    fb_parallel_for(-500, 500, cover, &coverage);
    CHECK(covered_once(coverage));
    CHECK(coverage.chunks.load() > 1);

    coverage_t one{ .begin = 7, .hits = std::vector<std::atomic<std::int32_t>>(1) };
    fb_parallel_for(7, 8, cover, &one);
    CHECK(covered_once(one));
    CHECK(one.chunks.load() == 1);

    fb_parallel_for(3, 3, never, nullptr);
    fb_parallel_for(5, -5, never, nullptr);

    // Ranges inside ranges: the threads that wait for the inner ones run their chunks.
    std::atomic<std::int64_t> total = 0;
    fb_parallel_for(0, 40, cover_inside, &total);
    CHECK(total.load() == 40);
    fb_pool_finish();
}

TEST_CASE("a parallel range uses more than one thread", "[runtime][threads]") {
    struct seen_t {
        std::atomic<bool> busy = false;
        std::atomic<std::int64_t> overlaps = 0;
    };

    // Each chunk sleeps a little, so with two threads or more, some chunks overlap.
    seen_t seen;
    const fb_range_body_t body = [](void* context, std::int64_t /*first*/, std::int64_t /*last*/) {
        auto* state = static_cast<seen_t*>(context);
        if (state->busy.exchange(true)) {
            state->overlaps.fetch_add(1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        state->busy.store(false);
    };

    fb_parallel_for(0, 64, body, &seen);
    if (fb_pool_size() > 1) {
        CHECK(seen.overlaps.load() > 0);
    }

    fb_pool_finish();
}

TEST_CASE("a job of its own runs on its thread, and the pool waits for it", "[runtime][threads]") {
    std::atomic<std::int64_t> count = 0;
    fb_job_t* job = fb_job_start_thread(count_one, &count);
    fb_job_wait(job);
    CHECK(fb_job_done(job) == 1);
    fb_job_release(job);
    CHECK(count.load() == 1);

    // Waiting for the pool's jobs waits for it too, even when no one waits for it.
    gate_t gate;
    fb_job_release(fb_job_start_thread(wait_at_gate, &gate));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_FALSE(gate.passed.load());
    gate.open.store(true);
    fb_pool_finish();
    CHECK(gate.passed.load());
}

TEST_CASE("jobs started on workers run on more than one thread", "[runtime][threads]") {
    struct ids_t {
        std::atomic<std::int64_t> left = 0;
        std::vector<std::thread::id> seen = std::vector<std::thread::id>(64);
    };

    // Each job sleeps a little, so the workers share them.
    ids_t ids;
    std::vector<fb_job_t*> jobs;
    jobs.reserve(ids.seen.size());
    for (std::size_t i = 0; i < ids.seen.size(); ++i) {
        jobs.push_back(fb_job_start(
            [](void* context) {
                auto* state = static_cast<ids_t*>(context);
                const auto slot = static_cast<std::size_t>(state->left.fetch_add(1));
                state->seen[slot] = std::this_thread::get_id();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            },
            &ids));
    }

    for (fb_job_t* job : jobs) {
        fb_job_wait(job);
        fb_job_release(job);
    }

    const std::set<std::thread::id> threads(ids.seen.begin(), ids.seen.end());
    if (fb_pool_size() > 1) {
        CHECK(threads.size() > 1);
    }

    fb_pool_finish();
}

TEST_CASE("a Mutex lets one thread at a time hold it", "[runtime][threads]") {
    struct guarded_t {
        fb_mutex_t mutex{};
        std::int64_t count = 0;
        std::atomic<std::int32_t> inside = 0;
        std::atomic<bool> overlapped = false;
    };

    guarded_t guarded;
    const fb_range_body_t body = [](void* context, const std::int64_t first,
                                    const std::int64_t last) {
        auto* state = static_cast<guarded_t*>(context);
        for (std::int64_t i = first; i < last; ++i) {
            fb_mutex_lock(&state->mutex, nullptr);
            if (state->inside.fetch_add(1) != 0) {
                state->overlapped.store(true);
            }

            ++state->count;
            state->inside.fetch_sub(1);
            fb_mutex_unlock(&state->mutex);
        }
    };

    fb_parallel_for(0, 20000, body, &guarded);
    CHECK(guarded.count == 20000);
    CHECK_FALSE(guarded.overlapped.load());
    CHECK(guarded.mutex.state == 0);
    CHECK(guarded.mutex.owner == 0);
    fb_pool_finish();
}

namespace {

std::atomic<std::int32_t> shared_destroyed = 0;

void count_destroy(fb_object_t* /*object*/) {
    shared_destroyed.fetch_add(1);
}

void release_nothing(fb_object_t* /*object*/) {}

void visit_none(const fb_object_t* /*object*/, fb_visitor_t /*visitor*/, void* /*context*/) {}

} // namespace

TEST_CASE("threads share a Shared Class's objects", "[runtime][threads]") {
    fb_class_t type{
        .name = "Hub",
        .size = sizeof(fb_object_t),
        .destroy = count_destroy,
        .release = release_nothing,
        .visit = visit_none,
        .live = 0,
        .shared = 1,
        .base = nullptr,
    };
    shared_destroyed = 0;
    fb_object_t* object = fb_object_new(&type);
    fb_object_retain_weak(object);

    // Every chunk retains and releases it, and reads it through the Weak reference, at once.
    const fb_range_body_t body = [](void* context, const std::int64_t first,
                                    const std::int64_t last) {
        auto* shared = static_cast<fb_object_t*>(context);
        for (std::int64_t i = first; i < last; ++i) {
            fb_object_retain(shared);
            fb_object_t* read = fb_object_load_weak(shared);
            fb_object_release(read);
            fb_object_release(shared);
        }
    };

    fb_parallel_for(0, 50000, body, object);
    CHECK(object->refcount == 1);
    CHECK(shared_destroyed.load() == 0);
    fb_object_release(object);
    CHECK(shared_destroyed.load() == 1);
    CHECK(type.live == 0);
    CHECK(fb_object_load_weak(object) == nullptr);
    fb_object_release_weak(object);
    fb_pool_finish();
}

namespace {

/// A Channel of Int64s and its Class, which the test frees.
struct ints_t {
    fb_class_t type{
        .name = "Channel<Int64>",
        .size = sizeof(fb_channel_t),
        .destroy = nullptr,
        .release = release_nothing,
        .visit = visit_none,
        .live = 0,
        .shared = 1,
        .base = nullptr,
    };
};

void send_numbers(void* context) {
    auto* channel = static_cast<fb_channel_t*>(context);
    for (std::int64_t i = 1; i <= 100; ++i) {
        fb_channel_send(channel, &i, nullptr);
    }

    fb_channel_close(channel);
}

} // namespace

TEST_CASE("a Channel carries values from one thread to another, in order", "[runtime][threads]") {
    ints_t ints;
    fb_channel_t* channel = fb_channel_new(&ints.type, sizeof(std::int64_t), 3, nullptr);
    fb_job_t* sender = fb_job_start(send_numbers, channel);
    std::int64_t value = 0;
    std::int64_t expected = 1;
    bool ordered = true;
    while (fb_channel_receive(channel, &value) != 0) {
        ordered = ordered && value == expected;
        ++expected;
    }

    CHECK(ordered);
    CHECK(expected == 101);
    fb_job_wait(sender);
    fb_job_release(sender);

    // Try doesn't wait: a full or closed Channel takes nothing, and an empty one gives nothing.
    fb_channel_t* small = fb_channel_new(&ints.type, sizeof(std::int64_t), 1, nullptr);
    std::int64_t one = 1;
    CHECK(fb_channel_try_send(small, &one) == 1);
    CHECK(fb_channel_try_send(small, &one) == 0);
    CHECK(fb_channel_try_receive(small, &value) == 1);
    CHECK(fb_channel_try_receive(small, &value) == 0);
    CHECK(fb_channel_try_send(small, &one) == 1);
    fb_channel_close(small);
    CHECK(fb_channel_try_send(small, &one) == 0);

    // What's left is taken out for the Class's release.
    CHECK(fb_channel_take_left(small) != nullptr);
    CHECK(fb_channel_take_left(small) == nullptr);
    fb_object_release(&small->header);
    fb_object_release(&channel->header);
    CHECK(ints.type.live == 0);
    fb_pool_finish();
}
