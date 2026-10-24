#include "fb/concurrent.hpp"
#include "fb/runtime.hpp"
#include "fb/time.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// The pool of worker threads that Spawn and Parallel For run on, and threads of their own for
// long, blocking work.

struct fb_job_t {
    fb_work_t work;
    void* context;
    /// The caller's reference and the pool's, which it lets go once the work has run.
    std::atomic<std::int32_t> owners;
    std::atomic<bool> done;
};

namespace {

void release(fb_job_t* job) {
    if (job->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete job;
    }
}

/// A queued job, and how many its queue had taken before it.
struct queued_t {
    fb_job_t* job = nullptr;
    std::uint64_t number = 0;
};

/// One worker's jobs: it takes the newest from the back, and other threads steal the oldest from
/// the front.
struct queue_t {
    std::mutex mutex;
    std::deque<queued_t> jobs;
    std::uint64_t taken = 0;
};

/// The thread's own queue in the pool, or none for a thread that isn't a worker.
constexpr std::size_t no_queue = std::numeric_limits<std::size_t>::max();
thread_local std::size_t t_queue = no_queue;
/// For each ThreadLocal Global this thread is making, innermost last, how many jobs its queue had
/// taken when the making began. Its waits run only its jobs from then on, which the initializer
/// started, since an older one could use the Global it's making.
thread_local std::vector<std::uint64_t> t_making;

class pool_t {
public:
    pool_t() {
        const unsigned cores = std::thread::hardware_concurrency();
        m_size = cores > 2 ? cores - 1 : 1;
        // A queue per worker, then one for the jobs that other threads start.
        for (std::size_t i = 0; i <= m_size; ++i) {
            m_queues.push_back(std::make_unique<queue_t>());
        }
    }

    pool_t(const pool_t&) = delete;
    pool_t& operator=(const pool_t&) = delete;
    pool_t(pool_t&&) = delete;
    pool_t& operator=(pool_t&&) = delete;

    ~pool_t() {
        finish();
    }

    [[nodiscard]] std::size_t size() const {
        return m_size;
    }

    void submit(fb_job_t* job) {
        {
            const std::scoped_lock lock(m_mutex);
            if (m_workers.empty()) {
                for (std::size_t i = 0; i < m_size; ++i) {
                    m_workers.emplace_back([this, i] { work(i); });
                }
            }
        }

        m_pending.fetch_add(1, std::memory_order_acq_rel);
        queue_t& queue = *m_queues[t_queue == no_queue ? m_size : t_queue];
        {
            const std::scoped_lock lock(queue.mutex);
            queue.jobs.push_back({ .job = job, .number = queue.taken++ });
        }

        signal();
    }

    /// Runs `job` on a thread of its own, which counts as a job the pool has started, so
    /// finish() waits for it. The threads that are done by now are joined, so a program that
    /// starts many over time doesn't keep them all.
    void start_thread(fb_job_t* job) {
        m_pending.fetch_add(1, std::memory_order_acq_rel);
        const std::scoped_lock lock(m_mutex);
        std::erase_if(m_threads, [this](std::thread& thread) {
            if (!std::ranges::contains(m_finished, thread.get_id())) {
                return false;
            }

            std::erase(m_finished, thread.get_id());
            thread.join();
            return true;
        });
        m_threads.emplace_back([this, job] {
            job->work(job->context);
            // The thread ends with its job, so its ThreadLocals go before what waits for it goes
            // on.
            fb::runtime::release_thread_locals();
            job->done.store(true, std::memory_order_release);
            release(job);
            // The thread uses nothing of the pool's after this, but to wake the others. Its id
            // goes in before the count goes down, under the lock, so a finish() that sees the
            // count at 0 never clears m_finished before it, which would leave the id for a new
            // thread the system gives it to.
            {
                const std::scoped_lock done(m_mutex);
                m_finished.push_back(std::this_thread::get_id());
                m_pending.fetch_sub(1, std::memory_order_acq_rel);
                m_generation.fetch_add(1, std::memory_order_acq_rel);
            }

            m_wake.notify_all();
        });
    }

    /// Returns once `job` is done. Meanwhile a worker runs the jobs it started itself, newest
    /// first, which is where a job it waits for usually is. It runs no one else's, which could
    /// wait for this thread in turn, as a Channel can.
    void wait_for(const fb_job_t& job) {
        wait_running_own([&] { return job.done.load(std::memory_order_acquire); });
    }

    /// Returns once `done` gives true, which it's asked again whenever a job is started or done,
    /// or signal() is called. Meanwhile a worker runs the jobs it started itself, as wait_for()
    /// does.
    template <typename Done>
    void wait_running_own(const Done& done) {
        while (!done()) {
            const std::uint64_t seen = m_generation.load(std::memory_order_acquire);
            if (run_own()) {
                continue;
            }

            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] {
                return done() || m_generation.load(std::memory_order_acquire) != seen;
            });
        }
    }

    /// Whether every job started is done, those on threads of their own too.
    [[nodiscard]] bool idle() const {
        return m_pending.load(std::memory_order_acquire) == 0;
    }

    /// Returns once `ready` gives true, which it's asked again whenever something changes: a job
    /// is started or done, or a Channel changes. The thread runs no jobs meanwhile, since one
    /// could wait for what this thread would do after.
    template <typename Ready>
    void wait_until(const Ready& ready) {
        while (true) {
            const std::uint64_t seen = m_generation.load(std::memory_order_acquire);
            if (ready()) {
                return;
            }

            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_generation.load(std::memory_order_acquire) != seen; });
        }
    }

    /// Returns once the generation isn't `seen`, or `stop` gives true, which it's asked again
    /// whenever the generation changes, or once the monotonic clock passes `end`: whether it was
    /// one of those first. The thread runs no jobs meanwhile, as in wait_until().
    template <typename Stop>
    bool wait_change(const std::uint64_t seen, const Stop& stop,
                     const std::chrono::steady_clock::time_point end) {
        std::unique_lock lock(m_mutex);
        const auto changed = [&] {
            return m_generation.load(std::memory_order_acquire) != seen || stop();
        };
        // A day at most at a time, as fb_time_sleep sleeps, which any OS's wait takes.
        while (!changed()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= end) {
                return false;
            }

            const auto until =
                end - now < std::chrono::hours(24) ? end : now + std::chrono::hours(24);
            m_wake.wait_until(lock, until);
        }

        return true;
    }

    [[nodiscard]] std::uint64_t generation() const {
        return m_generation.load(std::memory_order_acquire);
    }

    /// Wakes the threads that sleep until there's a job to run, one is done, or a Channel
    /// changed.
    void signal() {
        {
            const std::scoped_lock lock(m_mutex);
            m_generation.fetch_add(1, std::memory_order_acq_rel);
        }

        m_wake.notify_all();
    }

    /// Runs every job left, waits for the ones running, and stops the workers. The next job
    /// starts them again. A thread that has released its ThreadLocal Globals, as the main thread
    /// has once a Global's Destroy starts a job, only waits, while the workers that job started
    /// run them, each with its own ThreadLocals.
    void finish() {
        const bool helps = !fb::runtime::released_thread_locals();
        while (m_pending.load(std::memory_order_acquire) > 0) {
            const std::uint64_t seen = m_generation.load(std::memory_order_acquire);
            if (helps && run_one()) {
                continue;
            }

            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] {
                return m_pending.load(std::memory_order_acquire) == 0 ||
                       m_generation.load(std::memory_order_acquire) != seen;
            });
        }

        // The threads of their own are done with their work too.
        std::vector<std::thread> workers;
        {
            const std::scoped_lock lock(m_mutex);
            m_stopping = true;
            workers.swap(m_workers);
            std::ranges::move(m_threads, std::back_inserter(workers));
            m_threads.clear();
            m_finished.clear();
        }

        m_wake.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }

        const std::scoped_lock lock(m_mutex);
        m_stopping = false;
    }

    /// The calling thread begins making a ThreadLocal Global, or is done with it.
    void begin_making() {
        if (t_queue == no_queue) {
            t_making.push_back(0);
            return;
        }

        queue_t& own = *m_queues[t_queue];
        const std::scoped_lock lock(own.mutex);
        t_making.push_back(own.taken);
    }

    static void end_making() {
        t_making.pop_back();
    }

private:
    void work(const std::size_t index) {
        t_queue = index;
        while (true) {
            const std::uint64_t seen = m_generation.load(std::memory_order_acquire);
            if (run_one()) {
                continue;
            }

            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] {
                return m_stopping || m_generation.load(std::memory_order_acquire) != seen;
            });
            if (m_stopping) {
                // A Destroy that its ThreadLocals run can start a job, which takes the lock.
                lock.unlock();
                fb::runtime::release_thread_locals();
                return;
            }
        }
    }

    /// Runs a queued job, if there is one: the thread's own newest, or else the oldest that
    /// another thread started.
    bool run_one() {
        return run(take());
    }

    /// Runs the newest job this worker started that's still queued, if there is one, and, while
    /// it's making a ThreadLocal Global, only one started since.
    bool run_own() {
        if (t_queue == no_queue) {
            return false;
        }

        fb_job_t* job = nullptr;
        {
            queue_t& own = *m_queues[t_queue];
            const std::scoped_lock lock(own.mutex);
            if (!own.jobs.empty() &&
                (t_making.empty() || own.jobs.back().number >= t_making.back())) {
                job = own.jobs.back().job;
                own.jobs.pop_back();
            }
        }

        return run(job);
    }

    bool run(fb_job_t* job) {
        if (job == nullptr) {
            return false;
        }

        job->work(job->context);
        job->done.store(true, std::memory_order_release);
        release(job);
        m_pending.fetch_sub(1, std::memory_order_acq_rel);
        signal();
        return true;
    }

    fb_job_t* take() {
        if (t_queue != no_queue) {
            queue_t& own = *m_queues[t_queue];
            const std::scoped_lock lock(own.mutex);
            if (!own.jobs.empty()) {
                fb_job_t* job = own.jobs.back().job;
                own.jobs.pop_back();
                return job;
            }
        }

        // Every other queue, from the one after this one's and around, with the other threads'
        // queue after the last worker's: a worker looks at the workers after it first, and a
        // thread without a queue at the other threads' first.
        const std::size_t start = t_queue == no_queue ? 0 : t_queue + 1;
        for (std::size_t i = 0; i <= m_size; ++i) {
            const std::size_t index = (m_size + start + i) % (m_size + 1);
            if (index == t_queue) {
                continue;
            }

            queue_t& other = *m_queues[index];
            const std::scoped_lock lock(other.mutex);
            if (!other.jobs.empty()) {
                fb_job_t* job = other.jobs.front().job;
                other.jobs.pop_front();
                return job;
            }
        }

        return nullptr;
    }

    std::size_t m_size = 1;
    std::vector<std::unique_ptr<queue_t>> m_queues;
    /// Guards the workers and stopping, and changes to the generation.
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<std::thread> m_workers;
    /// The threads started for jobs of their own, until finish() or the next start_thread()
    /// joins them, and the ones of those that are done.
    std::vector<std::thread> m_threads;
    std::vector<std::thread::id> m_finished;
    bool m_stopping = false;
    /// Changes whenever a job is started or done, so a thread that found nothing to run knows to
    /// look again.
    std::atomic<std::uint64_t> m_generation = 0;
    /// The jobs started and not yet done.
    std::atomic<std::int64_t> m_pending = 0;
};

pool_t& pool() {
    static pool_t instance;
    return instance;
}

/// This thread's number for the Mutexes it holds, from 1 up, given the first time it needs one.
std::uint32_t thread_number() {
    static std::atomic<std::uint32_t> taken = 0;
    thread_local std::uint32_t number = 0;
    if (number == 0) {
        number = taken.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    return number;
}

/// What runs a task's closure and keeps its result: the first entry of the Method table that
/// compiled code puts right after its Class's fb_class_t.
using task_run_t = void (*)(fb_task_t* task);

task_run_t run_of(const fb_task_t* task) {
    const auto* methods = reinterpret_cast<void* const*>(task->header.type + 1);
    return reinterpret_cast<task_run_t>(methods[0]);
}

std::mutex& lock_of(const fb_channel_t* channel) {
    return *static_cast<std::mutex*>(channel->lock);
}

/// Copies `element` into the slot after the last one. The caller holds the lock.
void push(fb_channel_t* channel, const void* element) {
    const std::int64_t slot = (channel->head + channel->count) % channel->capacity;
    std::memcpy(channel->data + (slot * channel->element_size), element,
                static_cast<std::size_t>(channel->element_size));
    ++channel->count;
}

/// Copies the oldest element out to `element`. The caller holds the lock.
void pop(fb_channel_t* channel, void* element) {
    std::memcpy(element, channel->data + (channel->head * channel->element_size),
                static_cast<std::size_t>(channel->element_size));
    channel->head = (channel->head + 1) % channel->capacity;
    --channel->count;
}

/// A Parallel For's chunk: its body's code, the first entry of its Class's Method table.
void run_chunk(void* context, const std::int64_t first, const std::int64_t count) {
    auto* body = static_cast<fb_object_t*>(context);
    using chunk_t = void (*)(fb_object_t* body, std::int64_t first, std::int64_t count);
    const auto* methods = reinterpret_cast<void* const*>(body->type + 1);
    reinterpret_cast<chunk_t>(methods[0])(body, first, count);
}

/// A task's job: its work, and then the job lets go of the task.
void run_task(void* context) {
    auto* task = static_cast<fb_task_t*>(context);
    run_of(task)(task);
    fb_object_release(&task->header);
}

/// The thread that runs main, and the coroutines.
const std::thread::id main_thread = std::this_thread::get_id();

/// Nanoseconds on the real clock since the program started.
std::int64_t real_nanoseconds() {
    static const auto started = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                started)
        .count();
}

/// `time` and then `later` nanoseconds, or the most or least an Int64 holds past that.
std::int64_t later_by(const std::int64_t time, const std::int64_t later) {
    constexpr std::int64_t most = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t least = std::numeric_limits<std::int64_t>::min();
    if (later > 0 && time > most - later) {
        return most;
    }

    if (later < 0 && time < least - later) {
        return least;
    }

    return time + later;
}

/// Coroutines of Async Functions, each waiting for a Task, which the main thread resumes when it
/// ticks the scheduler, and the Tasks that NextFrame, Wait, and RealWait gave, which it finishes
/// then. The main scheduler's time is the real clock's, and a Scheduler's moves on as it's
/// ticked: a sum of seconds, which timers read to the nearest nanosecond, so ticks that add up to
/// a Wait's Duration reach it, though a Double's sum of them is a little short.
class scheduler_t {
public:
    explicit scheduler_t(const bool main) : m_main(main) {}

    scheduler_t(const scheduler_t&) = delete;
    scheduler_t& operator=(const scheduler_t&) = delete;
    scheduler_t(scheduler_t&&) = delete;
    scheduler_t& operator=(scheduler_t&&) = delete;

    ~scheduler_t() {
        for (const waiting_t& waiting : m_waiting) {
            fb_object_release(&waiting.task->header);
        }

        for (const timer_t& timer : m_timers) {
            fb_object_release(&timer.task->header);
        }
    }

    /// `coroutine`, which is `own`'s Async Function's, goes on once `task` is done, at a later
    /// tick.
    void add(void* coroutine, fb_task_t* own, fb_task_t* task) {
        fb_object_retain(&task->header);
        m_waiting.push_back(
            { .coroutine = coroutine, .own = own, .task = task, .since = m_frames });
    }

    /// A Task that this scheduler finishes at its next tick, or once its time reaches `due`, in
    /// nanoseconds, or the real clock's does.
    void add_timer(fb_task_t* task, const bool next_frame, const std::int64_t due,
                   const bool real) {
        fb_object_retain(&task->header);
        m_timers.push_back({
            .task = task,
            .frame = next_frame ? m_frames + 1 : 0,
            .due = due,
            .real = real,
        });
    }

    /// Its time in nanoseconds: the real clock's, or the nearest to a Scheduler's seconds.
    [[nodiscard]] std::int64_t time() const {
        if (m_main) {
            return real_nanoseconds();
        }

        const double nanoseconds = std::nearbyint(m_time * 1e9);
        return nanoseconds < 9.2e18 ? static_cast<std::int64_t>(nanoseconds)
                                    : std::numeric_limits<std::int64_t>::max();
    }

    /// Moves `task`'s coroutine here from `from`, with what it waits for there: another Async
    /// Function's coroutine, or a timer, whose time is then this scheduler's.
    void take(scheduler_t& from, const fb_task_t* task) {
        if (&from == this) {
            return;
        }

        std::vector<const fb_task_t*> tasks{ task };
        while (!tasks.empty()) {
            const fb_task_t* const moving = tasks.back();
            tasks.pop_back();
            std::erase_if(from.m_waiting, [&](waiting_t& waiting) {
                if (waiting.own != moving) {
                    return false;
                }

                tasks.push_back(waiting.task);
                waiting.since = m_frames;
                m_waiting.push_back(waiting);
                return true;
            });
            std::erase_if(from.m_timers, [&](timer_t& timer) {
                if (timer.task != moving) {
                    return false;
                }

                if (timer.frame != 0) {
                    timer.frame = m_frames + 1;
                } else if (!timer.real) {
                    timer.due =
                        later_by(time(), std::max<std::int64_t>(0, timer.due - from.time()));
                }

                m_timers.push_back(timer);
                return true;
            });
        }
    }

    /// Destroys the coroutines waiting here, the newest first, which lets go of what each one
    /// holds; their Tasks are never done.
    void cancel() {
        while (!m_waiting.empty()) {
            const waiting_t waiting = m_waiting.back();
            m_waiting.pop_back();
            fb_object_release(&waiting.task->header);
            // A coroutine's frame starts with where it resumes, then how it's destroyed.
            using destroy_t = void (*)(void* coroutine);
            static_cast<destroy_t*>(waiting.coroutine)[1](waiting.coroutine);
        }
    }

    /// Destroys the coroutines waiting here, as cancel() does, and lets go of the timers'
    /// Tasks, which are never done, until neither is left, since what's destroyed may start
    /// more.
    void end() {
        while (busy()) {
            cancel();
            for (const timer_t& timer : std::exchange(m_timers, {})) {
                fb_object_release(&timer.task->header);
            }
        }
    }

    /// Whether no coroutine waits here.
    [[nodiscard]] bool empty() const {
        return m_waiting.empty();
    }

    /// Whether the coroutines waiting here can never go on: the pool has no work that could
    /// finish a Task, and none waits for one that's done, or that a timer here or the I/O thread
    /// finishes, or that a coroutine here finishes once it goes on in turn. What they wait for is
    /// on a Scheduler, or waits for one, which only code here could tick.
    [[nodiscard]] bool stuck() const {
        // The pool first: once it has nothing left, its Tasks are done, which the rest sees.
        if (!pool().idle()) {
            return false;
        }

        // The I/O thread's Tasks before whether any is done: one it finishes meanwhile is done
        // by the time it's left out.
        std::vector<const fb_task_t*> coming = fb::runtime::requested_tasks();
        coming.reserve(coming.size() + m_timers.size());
        for (const timer_t& timer : m_timers) {
            coming.push_back(timer.task);
        }

        const auto goes_on = [&coming](const waiting_t& waiting) {
            return fb_task_done(waiting.task) != 0 || std::ranges::contains(coming, waiting.task);
        };
        bool more = true;
        while (more) {
            more = false;
            for (const waiting_t& waiting : m_waiting) {
                if (goes_on(waiting) && !std::ranges::contains(coming, waiting.own)) {
                    coming.push_back(waiting.own);
                    more = true;
                }
            }
        }

        return std::ranges::none_of(m_waiting, goes_on);
    }

    /// Whether a tick could finish something: a coroutine waits, or a timer.
    [[nodiscard]] bool busy() const {
        return !m_waiting.empty() || !m_timers.empty();
    }

    /// Whether a coroutine's Task is done, so a tick would resume it.
    [[nodiscard]] bool ready() const {
        return std::ranges::any_of(
            m_waiting, [](const waiting_t& waiting) { return fb_task_done(waiting.task) != 0; });
    }

    /// Moves time on by `seconds`, unless it's the real clock's, finishes the timers that are
    /// due, and resumes each coroutine whose Task is done, oldest first. One that starts waiting
    /// during the tick waits for the next. Gives whether anything went on.
    bool tick(double seconds);

    /// Ticks until `done` gives true, sleeping while nothing can go on.
    template <typename Done>
    void run_until(const Done& done) {
        while (!done()) {
            if (tick(0.0)) {
                continue;
            }

            // A timer is due soon, or a Task finishes elsewhere meanwhile.
            if (!m_timers.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            pool().wait_until([&] { return done() || ready(); });
        }
    }

private:
    struct waiting_t {
        void* coroutine;
        fb_task_t* own;
        fb_task_t* task;
        /// The tick it started waiting in, or before.
        std::uint64_t since;
    };

    struct timer_t {
        fb_task_t* task;
        /// For NextFrame: the tick it's done at. 0 for Wait and RealWait.
        std::uint64_t frame;
        /// For Wait and RealWait: the time it's done at, in nanoseconds.
        std::int64_t due;
        bool real;
    };

    bool m_main;
    double m_time = 0.0;
    std::uint64_t m_frames = 0;
    std::vector<waiting_t> m_waiting;
    std::vector<timer_t> m_timers;
};

} // namespace

namespace {

/// The Tasks of the requests the I/O thread has under way.
struct requests_t {
    std::mutex mutex;
    std::vector<const fb_task_t*> tasks;
};

requests_t& requests() {
    static requests_t instance;
    return instance;
}

} // namespace

void fb::runtime::start_request(const fb_task_t* task) {
    requests_t& under_way = requests();
    const std::scoped_lock lock(under_way.mutex);
    under_way.tasks.push_back(task);
}

void fb::runtime::finish_request(const fb_task_t* task) {
    {
        requests_t& under_way = requests();
        const std::scoped_lock lock(under_way.mutex);
        std::erase(under_way.tasks, task);
    }

    // Finishing the Task woke the threads that wait already, maybe before it was left out
    // here, so they're woken again to see it.
    pool().signal();
}

std::vector<const fb_task_t*> fb::runtime::requested_tasks() {
    requests_t& under_way = requests();
    const std::scoped_lock lock(under_way.mutex);
    return under_way.tasks;
}

namespace {

scheduler_t& main_scheduler() {
    static scheduler_t instance(true);
    return instance;
}

/// The scheduler that the main thread's code runs on: the main one, or a Scheduler while it's
/// ticked.
scheduler_t*& current_scheduler() {
    static scheduler_t* current = &main_scheduler();
    return current;
}

bool scheduler_t::tick(const double seconds) {
    bool ran = false;
    m_time += seconds;
    ++m_frames;
    std::erase_if(m_timers, [&](const timer_t& timer) {
        const std::int64_t now = timer.real ? real_nanoseconds() : time();
        if (timer.frame != 0 ? m_frames < timer.frame : now < timer.due) {
            return false;
        }

        fb_task_finish(timer.task);
        fb_object_release(&timer.task->header);
        ran = true;
        return true;
    });

    scheduler_t* const outer = std::exchange(current_scheduler(), this);
    std::size_t i = 0;
    while (i < m_waiting.size()) {
        const waiting_t waiting = m_waiting[i];
        if (waiting.since == m_frames || fb_task_done(waiting.task) == 0) {
            ++i;
            continue;
        }

        m_waiting.erase(m_waiting.begin() + static_cast<std::ptrdiff_t>(i));
        fb_object_release(&waiting.task->header);
        // Async code is the main thread's: another thread, like the I/O thread, only finishes
        // the Tasks it waits for.
        if (std::this_thread::get_id() != main_thread) {
            fb_panic("a coroutine went on off the main thread", nullptr);
        }

        // A coroutine's frame starts with where it resumes.
        using resume_t = void (*)(void* coroutine);
        (*static_cast<resume_t*>(waiting.coroutine))(waiting.coroutine);
        ran = true;
    }

    current_scheduler() = outer;
    return ran;
}

scheduler_t& state_of(const fb_scheduler_t* scheduler) {
    return *static_cast<scheduler_t*>(scheduler->state);
}

/// A range that a Parallel For goes over, which each thread takes chunks of until none are left.
/// The caller and each helper job hold it, since a helper may start after the caller's done.
struct range_t {
    fb_range_body_t body;
    void* context;
    std::int64_t first;
    /// The last index's distance from the first, so the full 2^64 indexes fit.
    std::uint64_t span;
    std::uint64_t size;
    std::uint64_t chunks;
    std::atomic<std::uint64_t> next;
    /// The chunks that have run.
    std::atomic<std::uint64_t> ran;
    std::atomic<std::int32_t> owners;
};

void release(range_t* range) {
    if (range->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete range;
    }
}

void take_chunks(range_t* range) {
    while (true) {
        const std::uint64_t chunk = range->next.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= range->chunks) {
            return;
        }

        // The arithmetic wraps, so a range across 0, or an unsigned one past an Int64's end,
        // works out.
        const std::uint64_t offset = chunk * range->size;
        const std::uint64_t count = std::min(range->size - 1, range->span - offset) + 1;
        const auto first =
            static_cast<std::int64_t>(static_cast<std::uint64_t>(range->first) + offset);
        range->body(range->context, first, static_cast<std::int64_t>(count));
        if (range->ran.fetch_add(1, std::memory_order_acq_rel) + 1 == range->chunks) {
            pool().signal();
        }
    }
}

/// A helper's job: the chunks left, if any, then it lets go of the range.
void help(void* context) {
    auto* range = static_cast<range_t*>(context);
    take_chunks(range);
    release(range);
}

} // namespace

extern "C" {

fb_job_t* fb_job_start(const fb_work_t work, void* context) {
    auto* job = new fb_job_t{ .work = work, .context = context, .owners = 2, .done = false };
    pool().submit(job);
    return job;
}

std::int32_t fb_job_done(const fb_job_t* job) {
    return job->done.load(std::memory_order_acquire) ? 1 : 0;
}

void fb_job_wait(const fb_job_t* job) {
    pool().wait_for(*job);
}

void fb_job_release(fb_job_t* job) {
    release(job);
}

void fb_parallel_for(const std::int64_t first, const std::int64_t last, const fb_range_body_t body,
                     void* context) {
    // A few chunks for each thread, so one that finishes early takes some of the others' share.
    const std::uint64_t span = static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first);
    const std::uint64_t threads = pool().size() + 1;
    const std::uint64_t size = std::max<std::uint64_t>(1, span / (threads * 4));
    const std::uint64_t chunks = (span / size) + 1;
    // The calling thread takes chunks too, so a range of one chunk needs no other thread.
    const std::uint64_t helpers = std::min<std::uint64_t>(pool().size(), chunks - 1);
    auto* range = new range_t{
        .body = body,
        .context = context,
        .first = first,
        .span = span,
        .size = size,
        .chunks = chunks,
        .next = 0,
        .ran = 0,
        .owners = static_cast<std::int32_t>(helpers + 1),
    };

    for (std::uint64_t i = 0; i < helpers; ++i) {
        fb_job_release(fb_job_start(help, range));
    }

    // Every chunk is taken by now, so the wait is for the threads running one: a helper that
    // hasn't started, since the workers are busy, finds none left once it does.
    take_chunks(range);
    pool().wait_running_own(
        [range] { return range->ran.load(std::memory_order_acquire) == range->chunks; });
    release(range);
}

void fb_parallel_closure(const std::int64_t first, const std::int64_t last, fb_object_t* body) {
    fb_parallel_for(first, last, run_chunk, body);
}

std::int64_t fb_pool_size() {
    return static_cast<std::int64_t>(pool().size());
}

void fb_pool_finish() {
    pool().finish();
}

std::uint64_t fb_pool_generation() {
    return pool().generation();
}

void fb_pool_signal() {
    pool().signal();
}

std::int32_t fb_pool_wait(const std::uint64_t seen, const std::uint64_t cancel,
                          const std::int64_t nanoseconds) {
    const auto cancelled = [cancel] { return cancel != 0 && fb_cancel_is_set(cancel) != 0; };
    const bool changed = pool().wait_change(
        seen, cancelled, fb::time::sleep_end(std::chrono::steady_clock::now(), nanoseconds));
    if (cancelled()) {
        return 2;
    }

    return changed ? 1 : 0;
}

void fb_thread_local_making() {
    pool().begin_making();
}

void fb_thread_local_made() {
    pool_t::end_making();
}

void fb_mutex_lock(fb_mutex_t* mutex, const fb_location_t* location) {
    // Only this thread writes its own number, so seeing it means this thread holds the Mutex.
    const std::uint32_t self = thread_number();
    const std::atomic_ref owner(mutex->owner);
    if (owner.load(std::memory_order_relaxed) == self) {
        fb_panic("this thread holds the Mutex already, so Lock would wait for itself forever",
                 location);
    }

    // Taken at once when no thread holds it. Otherwise the state says that a thread waits, which
    // the holder wakes when it lets go.
    const std::atomic_ref state(mutex->state);
    std::int32_t seen = 0;
    if (!state.compare_exchange_strong(seen, 1, std::memory_order_acquire)) {
        if (seen != 2) {
            seen = state.exchange(2, std::memory_order_acquire);
        }

        while (seen != 0) {
            state.wait(2, std::memory_order_relaxed);
            seen = state.exchange(2, std::memory_order_acquire);
        }
    }

    owner.store(self, std::memory_order_relaxed);
}

void fb_mutex_unlock(fb_mutex_t* mutex) {
    std::atomic_ref(mutex->owner).store(0, std::memory_order_relaxed);
    const std::atomic_ref state(mutex->state);
    if (state.exchange(0, std::memory_order_release) == 2) {
        state.notify_one();
    }
}

fb_channel_t* fb_channel_new(fb_class_t* type, const std::int64_t element_size,
                             const std::int64_t capacity, const fb_location_t* location) {
    if (capacity < 1) {
        std::array<char, 128> message{};
        std::snprintf(message.data(), message.size(),
                      "a Channel has room for 1 element or more, not %lld",
                      static_cast<long long>(capacity));
        fb_panic(message.data(), location);
    }

    auto* channel = reinterpret_cast<fb_channel_t*>(fb_object_new(type));
    channel->element_size = element_size;
    channel->capacity = capacity;
    // A byte or more, since calloc may give null for none, as a Struct without Fields would ask.
    channel->data = static_cast<unsigned char*>(
        std::calloc(static_cast<std::size_t>(element_size == 0 ? 1 : capacity),
                    static_cast<std::size_t>(std::max<std::int64_t>(element_size, 1))));
    if (channel->data == nullptr) {
        fb_panic("out of memory", location);
    }

    channel->lock = new std::mutex();
    return channel;
}

void fb_channel_send(fb_channel_t* channel, const void* element, const fb_location_t* location) {
    pool().wait_until([&] {
        const std::scoped_lock lock(lock_of(channel));
        if (channel->closed != 0) {
            fb_panic("this Channel is closed, so nothing more can be sent on it", location);
        }

        if (channel->count == channel->capacity) {
            return false;
        }

        push(channel, element);
        return true;
    });
    pool().signal();
}

std::int32_t fb_channel_try_send(fb_channel_t* channel, const void* element) {
    {
        const std::scoped_lock lock(lock_of(channel));
        if (channel->closed != 0 || channel->count == channel->capacity) {
            return 0;
        }

        push(channel, element);
    }

    pool().signal();
    return 1;
}

std::int32_t fb_channel_receive(fb_channel_t* channel, void* element) {
    bool received = false;
    pool().wait_until([&] {
        const std::scoped_lock lock(lock_of(channel));
        if (channel->count > 0) {
            pop(channel, element);
            received = true;
            return true;
        }

        return channel->closed != 0;
    });
    if (received) {
        pool().signal();
    }

    return received ? 1 : 0;
}

std::int32_t fb_channel_try_receive(fb_channel_t* channel, void* element) {
    {
        const std::scoped_lock lock(lock_of(channel));
        if (channel->count == 0) {
            return 0;
        }

        pop(channel, element);
    }

    pool().signal();
    return 1;
}

void fb_channel_close(fb_channel_t* channel) {
    {
        const std::scoped_lock lock(lock_of(channel));
        channel->closed = 1;
    }

    pool().signal();
}

std::int32_t fb_channel_closed(fb_channel_t* channel) {
    const std::scoped_lock lock(lock_of(channel));
    return channel->closed != 0 ? 1 : 0;
}

void* fb_channel_take_left(fb_channel_t* channel) {
    if (channel->count == 0) {
        return nullptr;
    }

    void* element = channel->data + (channel->head * channel->element_size);
    channel->head = (channel->head + 1) % channel->capacity;
    --channel->count;
    return element;
}

void* fb_channel_element(const fb_channel_t* channel, const std::int64_t index) {
    if (index >= channel->count) {
        return nullptr;
    }

    const std::int64_t slot = (channel->head + index) % channel->capacity;
    return channel->data + (slot * channel->element_size);
}

void fb_channel_free(const fb_channel_t* channel) {
    std::free(channel->data);
    delete &lock_of(channel);
}

fb_job_t* fb_job_start_thread(const fb_work_t work, void* context) {
    auto* job = new fb_job_t{ .work = work, .context = context, .owners = 2, .done = false };
    pool().start_thread(job);
    return job;
}

void fb_task_start(fb_task_t* task) {
    fb_object_retain(&task->header);
    task->job = fb_job_start(run_task, task);
}

void fb_task_start_thread(fb_task_t* task) {
    fb_object_retain(&task->header);
    task->job = fb_job_start_thread(run_task, task);
}

void fb_task_wait(const fb_task_t* task) {
    // The main thread ticks its scheduler meanwhile, which may finish what it waits for.
    if (std::this_thread::get_id() == main_thread && main_scheduler().busy()) {
        main_scheduler().run_until([&] { return fb_job_done(task->job) != 0; });
        return;
    }

    fb_job_wait(task->job);
}

void fb_task_begin(fb_task_t* task) {
    // Only the task holds it, and no one runs it: the Async Function finishes it.
    task->job = new fb_job_t{ .work = nullptr, .context = nullptr, .owners = 1, .done = false };
}

void fb_task_finish(const fb_task_t* task) {
    task->job->done.store(true, std::memory_order_release);
    pool().signal();
}

void fb_await(void* coroutine, fb_task_t* own, fb_task_t* task) {
    current_scheduler()->add(coroutine, own, task);
}

void fb_wait_frame(fb_task_t* task) {
    current_scheduler()->add_timer(task, true, 0, false);
}

void fb_wait_time(fb_task_t* task, const std::int64_t nanoseconds, const std::int32_t real) {
    scheduler_t& scheduler = *current_scheduler();
    const std::int64_t now = real != 0 ? real_nanoseconds() : scheduler.time();
    scheduler.add_timer(task, false, later_by(now, nanoseconds), real != 0);
}

fb_scheduler_t* fb_scheduler_new(fb_class_t* type) {
    auto* scheduler = reinterpret_cast<fb_scheduler_t*>(fb_object_new(type));
    scheduler->state = new scheduler_t(false);
    return scheduler;
}

void fb_scheduler_run(const fb_scheduler_t* scheduler, const fb_task_t* task) {
    state_of(scheduler).take(*current_scheduler(), task);
}

void fb_scheduler_tick(const fb_scheduler_t* scheduler, const double seconds) {
    state_of(scheduler).tick(seconds);
}

std::int32_t fb_scheduler_idle(const fb_scheduler_t* scheduler) {
    return state_of(scheduler).empty() ? 1 : 0;
}

void fb_scheduler_free(const fb_scheduler_t* scheduler) {
    scheduler_t& state = state_of(scheduler);
    state.cancel();
    delete &state;
}

void* fb_coroutine_alloc(const std::int64_t size) {
    void* frame = std::malloc(static_cast<std::size_t>(size));
    if (frame == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    return frame;
}

void fb_coroutine_free(void* frame) {
    std::free(frame);
}

void fb_scheduler_finish() {
    scheduler_t& main = main_scheduler();
    main.run_until([&main] { return main.empty() || main.stuck(); });
    main.end();
}

void fb_scheduler_end() {
    main_scheduler().end();
}

std::int32_t fb_task_done(const fb_task_t* task) {
    return fb_job_done(task->job);
}

void fb_task_release(const fb_task_t* task) {
    if (task->job != nullptr) {
        fb_job_release(task->job);
    }
}
}
