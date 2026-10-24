// The I/O thread: it runs libuv's loop, takes the requests the fb_io_ functions give it, and
// finishes their Tasks, or wakes their callers, as each is done. It starts with the first request,
// and stops as the program ends, before the leak report, letting go of what nothing waits for.

#include "fb/runtime.hpp"
#include "fb/time.hpp"
#include "internal.hpp"
#include "io_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <uv.h>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

/// A blocking caller's line, which it waits on `done` for.
struct line_waiter_t {
    std::optional<std::string> line;
    bool done = false;
};

/// What waits for a line from stdin: a Task<String?> to finish, for fb_io_input_async, or a
/// blocking caller to wake, for fb_io_input.
struct line_request_t {
    fb_task_t* task = nullptr;
    line_waiter_t* waiter = nullptr;
};

/// Gives a request its line, or Null at the end of the input.
void answer(const line_request_t& request, std::optional<std::string> line);

/// What waits for time to pass: a Task to finish once `nanoseconds` have, for fb_io_after.
struct timer_request_t {
    fb_task_t* task = nullptr;
    std::int64_t nanoseconds = 0;
};

/// A timer under way on the I/O thread's loop, with the Task it finishes.
struct running_timer_t {
    uv_timer_t handle{};
    fb_task_t* task = nullptr;
};

/// Finishes a timer's Task, which the request held.
void finish_timer(fb_task_t* task) {
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
}

/// Reads stdin on the I/O thread, as far as the lines waited for need, and gives each waiter its
/// line in turn: a console's, a pipe's, or a file's.
class reader_t {
public:
    void want(uv_loop_t* loop, const line_request_t& request) {
        m_waiters.push_back(request);
        deliver();
        if (!m_waiters.empty()) {
            read_more(loop);
        }
    }

    /// Gives up reading, answering what still waits with Null, and lets the handle go.
    void close() {
        while (!m_waiters.empty()) {
            const line_request_t request = m_waiters.front();
            m_waiters.pop_front();
            answer(request, std::nullopt);
        }

        if (m_stream != nullptr) {
            uv_read_stop(m_stream);
            uv_close(reinterpret_cast<uv_handle_t*>(m_stream), nullptr);
            m_stream = nullptr;
        }

#ifndef _WIN32
        restore_stdin();
#endif

        // A file's read finishes soon, so the loop just waits for one under way.
        if (m_file && m_reading) {
            uv_cancel(reinterpret_cast<uv_req_t*>(&m_read));
        }
    }

private:
    std::deque<line_request_t> m_waiters;
    std::string m_buffer;
    bool m_opened = false;
    bool m_ended = false;
    bool m_reading = false;
    /// A console's or a pipe's handle, which libuv reads as it comes.
    uv_stream_t* m_stream = nullptr;
    uv_tty_t m_tty{};
    uv_pipe_t m_pipe{};
    /// Stdin is a file, which reads go to libuv's thread pool for.
    bool m_file = false;
#ifdef _WIN32
    bool m_console = false;
#else
    /// What stdin's flags were before libuv read a pipe's, which it gets back however the
    /// program ends, -1 before then.
    static std::atomic<int>& stdin_flags() {
        static std::atomic<int> flags = -1;
        return flags;
    }

    /// libuv made a pipe's stdin non-blocking, which the shell and what else reads it share.
    static void restore_stdin() {
        if (const int flags = stdin_flags().load(std::memory_order_acquire); flags != -1) {
            fcntl(STDIN_FILENO, F_SETFL, flags);
        }
    }
#endif
    uv_fs_t m_read{};
    std::array<char, 65536> m_chunk{};

    /// What stdin is, the first time it's read.
    void open(uv_loop_t* loop) {
        m_opened = true;
        switch (uv_guess_handle(0)) {
        case UV_TTY: {
#ifdef _WIN32
            if (uv_tty_init(loop, &m_tty, 0, 1) == 0) {
                m_stream = reinterpret_cast<uv_stream_t*>(&m_tty);
                m_console = true;
            }
#else
            // libuv reopens a terminal in place of the descriptor it's given, so it gets a copy,
            // and stdin stays as it was for what the program starts.
            const int copy = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
            if (copy >= 0 && uv_tty_init(loop, &m_tty, copy, 1) == 0) {
                m_stream = reinterpret_cast<uv_stream_t*>(&m_tty);
            } else if (copy >= 0) {
                ::close(copy);
            }
#endif

            break;
        }
        case UV_NAMED_PIPE:
#ifndef _WIN32
            stdin_flags().store(fcntl(STDIN_FILENO, F_GETFL), std::memory_order_release);
            fb::runtime::at_end(restore_stdin);
#endif
            if (uv_pipe_init(loop, &m_pipe, 0) == 0) {
                if (uv_pipe_open(&m_pipe, 0) == 0) {
                    m_stream = reinterpret_cast<uv_stream_t*>(&m_pipe);
                } else {
                    uv_close(reinterpret_cast<uv_handle_t*>(&m_pipe), nullptr);
                }
            }

            break;
        case UV_FILE:
            m_file = true;
            break;
        default:
            // Closed, or something else: there's nothing to read.
            break;
        }

        if (m_stream != nullptr) {
            m_stream->data = this;
        }

        m_read.data = this;
    }

    void read_more(uv_loop_t* loop) {
        if (m_reading || m_ended) {
            return;
        }

        if (!m_opened) {
            open(loop);
        }

        if (m_stream != nullptr) {
            m_reading = uv_read_start(m_stream, on_alloc, on_read) == 0;
        } else if (m_file) {
            const uv_buf_t buffer =
                uv_buf_init(m_chunk.data(), static_cast<unsigned>(m_chunk.size()));
            m_reading = uv_fs_read(loop, &m_read, 0, &buffer, 1, -1, on_file_read) == 0;
        }

        if (!m_reading) {
            m_ended = true;
            deliver();
        }
    }

    /// Gives each waiter, in order, the next line there is, until a line isn't all there yet.
    void deliver() {
        while (!m_waiters.empty()) {
            const std::size_t end = m_buffer.find('\n');
            if (end == std::string::npos && !m_ended) {
                return;
            }

            std::optional<std::string> line;
            if (end != std::string::npos) {
                line = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 1);
            } else if (!m_buffer.empty()) {
                line = std::exchange(m_buffer, {});
            }

            if (line && line->ends_with('\r')) {
                line->pop_back();
            }

#ifdef _WIN32
            // On a Windows console, Ctrl+Z at a line's start ends the input.
            if (line && m_console && line->starts_with('\x1a')) {
                line.reset();
                m_ended = true;
                m_buffer.clear();
            }
#endif

            const line_request_t request = m_waiters.front();
            m_waiters.pop_front();
            answer(request, std::move(line));
        }
    }

    static void on_alloc(uv_handle_t* handle, std::size_t /*suggested*/, uv_buf_t* buffer) {
        auto& self = *static_cast<reader_t*>(handle->data);
        *buffer = uv_buf_init(self.m_chunk.data(), static_cast<unsigned>(self.m_chunk.size()));
    }

    static void on_read(uv_stream_t* stream, const ssize_t count, const uv_buf_t* buffer) {
        auto& self = *static_cast<reader_t*>(stream->data);
        if (count > 0) {
            self.m_buffer.append(buffer->base, static_cast<std::size_t>(count));
        } else if (count < 0) {
            // The end, or an error, which ends what can be read too.
            self.m_ended = true;
        }

        self.deliver();
        if (self.m_reading && (self.m_ended || self.m_waiters.empty())) {
            uv_read_stop(stream);
            self.m_reading = false;
        }
    }

    static void on_file_read(uv_fs_t* request) {
        auto& self = *static_cast<reader_t*>(request->data);
        const auto count = static_cast<std::int64_t>(request->result);
        uv_loop_t* loop = request->loop;
        uv_fs_req_cleanup(request);
        self.m_reading = false;
        if (count > 0) {
            self.m_buffer.append(self.m_chunk.data(), static_cast<std::size_t>(count));
        } else {
            self.m_ended = true;
        }

        self.deliver();
        if (!self.m_waiters.empty()) {
            self.read_more(loop);
        }
    }
};

/// The I/O thread, its loop, and the requests other threads give it.
class io_t {
public:
    /// Has the I/O thread read a line for `request`, starting the thread the first time. Once the
    /// program is ending, there's nothing more to read.
    void read_line(const line_request_t& request) {
        std::call_once(m_started, [this] { start(); });
        {
            const std::scoped_lock lock(m_mutex);
            if (!m_stopped) {
                m_incoming.push_back(request);
                uv_async_send(&m_wake);
                return;
            }
        }

        answer(request, std::nullopt);
    }

    /// Has the I/O thread finish a timer's Task, starting the thread the first time. Once the
    /// program is ending, it's finished at once.
    void start_timer(const timer_request_t& request) {
        std::call_once(m_started, [this] { start(); });
        {
            const std::scoped_lock lock(m_mutex);
            if (!m_stopped) {
                m_timers_incoming.push_back(request);
                uv_async_send(&m_wake);
                return;
            }
        }

        finish_timer(request.task);
    }

    /// Runs `job` on the thread, starting it the first time, or now with no loop once the
    /// program is ending.
    void run(std::function<void(uv_loop_t*)> job) {
        std::call_once(m_started, [this] { start(); });
        {
            const std::scoped_lock lock(m_mutex);
            if (!m_stopped) {
                m_jobs.push_back(std::move(job));
                uv_async_send(&m_wake);
                return;
            }
        }

        job(nullptr);
    }

    /// Has the I/O thread run `closing` as it stops.
    void at_stop(void (*closing)(uv_loop_t* loop)) {
        const std::scoped_lock lock(m_mutex);
        m_stops.push_back(closing);
    }

    /// Waits for a blocking caller's line.
    void wait_for(const line_waiter_t& waiter) {
        std::unique_lock lock(m_mutex);
        m_answered.wait(lock, [&] { return waiter.done; });
    }

    /// Wakes a blocking caller, whose waiter now has its line.
    void wake(line_waiter_t* waiter, std::optional<std::string> line) {
        {
            const std::scoped_lock lock(m_mutex);
            waiter->line = std::move(line);
            waiter->done = true;
        }

        m_answered.notify_all();
    }

    /// Stops the thread, once what it was given is taken and what still waits is answered.
    void stop() {
        {
            const std::scoped_lock lock(m_mutex);
            m_stopped = true;
            uv_async_send(&m_wake);
        }

        m_thread.join();
        uv_loop_close(&m_loop);
    }

private:
    std::once_flag m_started;
    std::mutex m_mutex;
    std::condition_variable m_answered;
    std::vector<line_request_t> m_incoming;
    std::vector<timer_request_t> m_timers_incoming;
    /// The timers under way, which only the I/O thread touches.
    std::vector<running_timer_t*> m_timers;
    /// What other parts of the runtime gave the thread to run.
    std::vector<std::function<void(uv_loop_t*)>> m_jobs;
    /// What other parts run as the thread stops.
    std::vector<void (*)(uv_loop_t* loop)> m_stops;
    bool m_stopped = false;
    uv_loop_t m_loop{};
    uv_async_t m_wake{};
    reader_t m_reader;
    std::thread m_thread;

    void start() {
        uv_loop_init(&m_loop);
        uv_async_init(&m_loop, &m_wake, on_wake);
        m_wake.data = this;
        m_thread = std::thread([this] { uv_run(&m_loop, UV_RUN_DEFAULT); });
        fb::runtime::at_finish(stop_io);
    }

    static void stop_io();

    /// Takes the requests given meanwhile, and closes everything once the program's ending.
    static void on_wake(uv_async_t* handle) {
        auto& self = *static_cast<io_t*>(handle->data);
        std::vector<line_request_t> incoming;
        std::vector<timer_request_t> timers;
        std::vector<std::function<void(uv_loop_t*)>> jobs;
        std::vector<void (*)(uv_loop_t* loop)> stops;
        bool stopped = false;
        {
            const std::scoped_lock lock(self.m_mutex);
            incoming.swap(self.m_incoming);
            timers.swap(self.m_timers_incoming);
            jobs.swap(self.m_jobs);
            stopped = self.m_stopped;
            if (stopped) {
                stops = self.m_stops;
            }
        }

        for (const line_request_t& request : incoming) {
            self.m_reader.want(&self.m_loop, request);
        }

        // libuv's loop counts whole milliseconds from when its iteration began, so a timer can go
        // off up to one early: each waits a millisecond past its time rounded up, which is never
        // less than its time.
        if (!timers.empty()) {
            uv_update_time(&self.m_loop);
        }
        for (const timer_request_t& request : timers) {
            auto* timer = new running_timer_t{ .task = request.task };
            uv_timer_init(&self.m_loop, &timer->handle);
            timer->handle.data = &self;
            const std::int64_t nanoseconds = std::max<std::int64_t>(request.nanoseconds, 0);
            const std::int64_t milliseconds =
                nanoseconds == 0
                    ? 0
                    : (nanoseconds / 1000000) + (nanoseconds % 1000000 != 0 ? 1 : 0) + 1;
            uv_timer_start(&timer->handle, on_timer, static_cast<std::uint64_t>(milliseconds), 0);
            self.m_timers.push_back(timer);
        }

        for (const std::function<void(uv_loop_t*)>& job : jobs) {
            job(&self.m_loop);
        }

        if (stopped) {
            for (const auto stop : std::views::reverse(stops)) {
                stop(&self.m_loop);
            }

            self.m_reader.close();
            // What still waits is finished at once, as the program ends.
            for (running_timer_t* timer : self.m_timers) {
                uv_timer_stop(&timer->handle);
                finish_timer(timer->task);
                uv_close(reinterpret_cast<uv_handle_t*>(&timer->handle), on_timer_closed);
            }

            self.m_timers.clear();

            uv_close(reinterpret_cast<uv_handle_t*>(&self.m_wake), nullptr);
        }
    }

    /// A timer is done: its Task finishes, and its handle goes.
    static void on_timer(uv_timer_t* handle) {
        auto& self = *static_cast<io_t*>(handle->data);
        auto* timer = reinterpret_cast<running_timer_t*>(handle);
        std::erase(self.m_timers, timer);
        finish_timer(timer->task);
        uv_close(reinterpret_cast<uv_handle_t*>(handle), on_timer_closed);
    }

    static void on_timer_closed(uv_handle_t* handle) {
        delete reinterpret_cast<running_timer_t*>(handle);
    }
};

io_t& io() {
    static io_t instance;
    return instance;
}

void io_t::stop_io() {
    io().stop();
}

void answer(const line_request_t& request, std::optional<std::string> line) {
    if (request.waiter != nullptr) {
        io().wake(request.waiter, std::move(line));
        return;
    }

    // The Task's result, a String?, is right after its fb_task_t, and the Task is done once
    // that's set.
    fb_task_t* task = request.task;
    *reinterpret_cast<fb_string_t**>(task + 1) =
        line ? fb_string_from_bytes(line->data(), static_cast<std::int64_t>(line->size()))
             : nullptr;
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
}

} // namespace

namespace fb::runtime {

void on_io_thread(std::function<void(uv_loop_t*)> job) {
    io().run(std::move(job));
}

void at_io_stop(void (*stop)(uv_loop_t* loop)) {
    io().at_stop(stop);
}

} // namespace fb::runtime

extern "C" {

fb_string_t* fb_io_input(const fb_string_t* prompt) {
    fb::runtime::write_prompt(prompt);
    line_waiter_t waiter;
    io().read_line({ .waiter = &waiter });
    io().wait_for(waiter);
    return waiter.line ? fb_string_from_bytes(waiter.line->data(),
                                              static_cast<std::int64_t>(waiter.line->size()))
                       : nullptr;
}

void fb_io_after(fb_task_t* task, const std::int64_t nanoseconds) {
    // The request holds the Task until it's done.
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
    io().start_timer({ .task = task, .nanoseconds = nanoseconds });
}

void fb_io_input_async(fb_task_t* task, const fb_string_t* prompt) {
    fb::runtime::write_prompt(prompt);
    // The request holds the Task until it's done.
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
    io().read_line({ .task = task });
}
}
