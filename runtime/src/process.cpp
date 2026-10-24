// Child processes and signals over libuv: a thread of their own runs a loop that starts programs,
// moves the bytes of their pipes, sees them end, and counts the signals that OnInterrupt's and
// OnTerminate's threads wait for. The fb_os_process_ and fb_os_signal_ functions hand it requests
// and wait for each, or give a Task that it finishes. It starts with the first request, and stops
// as the program ends, before the leak report, letting go of what it holds.

#ifdef _WIN32
// std::min and std::max, not windows.h's macros, which libuv's header brings in.
#define NOMINMAX
#endif

#include "fb/process.hpp"

#include "fb/os.hpp"
#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <uv.h>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <pthread.h>
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#endif

namespace {

std::string_view text_of(const fb_string_t* string) {
    return { reinterpret_cast<const char*>(string + 1), static_cast<std::size_t>(string->length) };
}

void add_string(fb_array_t* array, const std::string_view text) {
    *static_cast<fb_string_t**>(fb_array_push(array, sizeof(fb_string_t*))) =
        fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

/// A String[]'s or String?[]'s elements, of which a String? may be null.
std::span<const fb_string_t* const> strings_of(const fb_array_t* array) {
    return { static_cast<const fb_string_t* const*>(array->data),
             static_cast<std::size_t>(array->count) };
}

/// A blocking caller's result, which the loop's thread gives it.
struct result_t {
    std::int64_t value = 0;
    bool done = false;
};

/// What waits for a request: a blocking caller, or a Task<Int64>, whose result follows its
/// fb_task_t.
struct waiter_t {
    result_t* blocking = nullptr;
    fb_task_t* task = nullptr;
};

/// Gives a waiter its result: wakes the caller, or finishes the Task and lets go of it.
void answer(const waiter_t& waiter, std::int64_t value);

/// A read of an output stream, which waits for a byte at least.
struct read_t {
    std::uint8_t* into = nullptr;
    std::int64_t count = 0;
    waiter_t waiter;
};

/// A write to stdin, whose bytes are its own until libuv has written them.
struct write_t {
    uv_write_t request{};
    std::string bytes;
    waiter_t waiter;
};

/// The most bytes one of libuv's buffers takes.
constexpr std::size_t buffer_limit = std::size_t{ 1 } << 30U;

/// How a Stdio is numbered.
enum class stdio_t : std::int8_t {
    INHERIT,
    PIPED,
    NONE,
};

/// One of a child's streams, when it's a pipe.
struct stream_t {
    uv_pipe_t pipe{};
    /// uv_spawn made it, and it isn't closing.
    bool open = false;
    /// stdin's: it closes once what's written has gone.
    bool closing = false;
    /// An output's bytes that no read has taken yet.
    std::string held;
    /// The output ended, or failed with `failure`.
    bool ended = false;
    std::int64_t failure = 0;
    /// Read as it comes, whether a read waits or not.
    bool draining = false;
    bool reading = false;
    std::deque<read_t> reads;
    std::vector<char> chunk;
};

/// A child process: its handle, its streams, and how it ended, once it has.
struct child_t {
    uv_process_t process{};
    std::array<stream_t, 3> streams;
    std::int64_t pid = 0;
    /// What waits for it to end.
    std::vector<waiter_t> waits;
    /// Whether it's ended, and how, which other threads read under the processes' lock.
    bool exited = false;
    std::int64_t status = 0;
    /// How many std.os objects hold it.
    std::atomic<std::int32_t> holders = 1;
    /// None holds it any more.
    bool orphaned = false;
    /// The handles libuv has yet to close, the process's and each pipe's.
    std::int32_t handles = 0;
};

child_t* child_of(const std::uint64_t handle) {
    return std::bit_cast<child_t*>(handle);
}

/// The stream whose pipe is `handle`, of the child that each pipe's data points at.
stream_t& stream_of(const uv_handle_t* handle) {
    auto* child = static_cast<child_t*>(handle->data);
    for (stream_t& stream : child->streams) {
        if (reinterpret_cast<const uv_handle_t*>(&stream.pipe) == handle) {
            return stream;
        }
    }

    return child->streams.front();
}

/// The kinds of signals: an interrupt, or a request to end.
constexpr std::size_t signal_kinds = 2;

/// The signals each kind is.
std::vector<int> signals_of(const std::size_t kind) {
#ifdef _WIN32
    // Ctrl+Break is the Ctrl+C that a program can send to another's process group.
    return kind == 0 ? std::vector<int>{ SIGINT, SIGBREAK } : std::vector<int>{ SIGTERM, SIGHUP };
#else
    return kind == 0 ? std::vector<int>{ SIGINT } : std::vector<int>{ SIGTERM, SIGHUP };
#endif
}

class processes_t;

/// The one processes_t.
processes_t& processes();

/// The thread that runs the loop for processes and signals, and what it has under way.
class processes_t {
public:
    /// Posts `work`, whose waiter is the calling thread, and waits for its result.
    std::int64_t run(const std::function<void(uv_loop_t*, const waiter_t&)>& work) {
        result_t result;
        const waiter_t waiter{ .blocking = &result };
        if (!post([&](uv_loop_t* loop) { work(loop, waiter); })) {
            return UV_ECANCELED;
        }

        std::unique_lock lock(m_mutex);
        m_answered.wait(lock, [&] { return result.done; });
        return result.value;
    }

    /// Posts `work`, whose waiter is `task`, a Task<Int64>, which the request holds until it's
    /// done.
    void run_async(fb_task_t* task, std::function<void(uv_loop_t*, const waiter_t&)> work) {
        fb_object_retain(&task->header);
        fb::runtime::start_request(task);
        const waiter_t waiter{ .task = task };
        if (!post([work = std::move(work), waiter](uv_loop_t* loop) { work(loop, waiter); })) {
            answer(waiter, UV_ECANCELED);
        }
    }

    /// Has `work` run on the loop's thread, without waiting for it.
    void send(std::function<void(uv_loop_t*)> work) {
        (void)post(std::move(work));
    }

    /// Wakes a blocking caller, whose result is now `value`.
    void wake(result_t* result, const std::int64_t value) {
        {
            const std::scoped_lock lock(m_mutex);
            result->value = value;
            result->done = true;
        }

        m_answered.notify_all();
    }

    /// The lock other threads read a child's end under.
    std::mutex& lock() {
        return m_mutex;
    }

    /// Keeps a child the loop's thread made, until it's let go of.
    void add(child_t* child) {
        m_children.push_back(child);
    }

    /// One of a child's handles has closed.
    static void on_closed(uv_handle_t* handle);

    /// Lets go of a child once nothing holds it, and libuv has closed its handles.
    void forget_if_done(child_t* child) {
        if (child->orphaned && child->handles == 0) {
            std::erase(m_children, child);
            delete child;
        }
    }

    /// Watches for signals of `kind`, from before it returns, and gives how many came before.
    std::int64_t watch(const std::size_t kind) {
        std::int64_t seen = 0;
        {
            const std::scoped_lock lock(m_signal_mutex);
            seen = m_counts.at(kind);
        }

        fb::runtime::at_code_end(end_signal_waits);
        (void)run([this, kind](uv_loop_t* loop, const waiter_t& waiter) {
            start_watching(loop, kind);
            answer(waiter, 0);
        });
        return seen;
    }

    /// Waits until more than `seen` signals of `kind` have come, and gives how many have, or 0
    /// once the program's code is done.
    std::int64_t wait_for_signal(const std::size_t kind, const std::int64_t seen) {
        std::unique_lock lock(m_signal_mutex);
        m_signalled.wait(lock, [&] { return m_code_ended || m_counts.at(kind) > seen; });
        return m_code_ended ? 0 : m_counts.at(kind);
    }

    /// The program's code is done: what waits for a signal stops, and the signals do what they do
    /// without the handlers.
    void end_signals() {
        {
            const std::scoped_lock lock(m_signal_mutex);
            m_code_ended = true;
        }

        m_signalled.notify_all();
        send([this](uv_loop_t* /*loop*/) { close_watchers(); });
    }

    /// Stops the thread, once what it was given is done, closing every handle it has.
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
    /// A signal watched, and its kind.
    struct watcher_t {
        uv_signal_t handle{};
        std::size_t kind = 0;
        bool open = false;
    };

    std::once_flag m_started;
    std::mutex m_mutex;
    std::condition_variable m_answered;
    std::vector<std::function<void(uv_loop_t*)>> m_incoming;
    bool m_stopped = false;
    uv_loop_t m_loop{};
    uv_async_t m_wake{};
    std::thread m_thread;
    /// The loop's thread's own.
    std::vector<child_t*> m_children;
    std::vector<std::unique_ptr<watcher_t>> m_watchers;
    std::array<bool, signal_kinds> m_watching{};
    /// How many signals of each kind have come, which the handlers' threads wait for until the
    /// program's code is done.
    std::mutex m_signal_mutex;
    std::condition_variable m_signalled;
    std::array<std::int64_t, signal_kinds> m_counts{};
    bool m_code_ended = false;

    /// Has `work` run on the loop's thread, starting the thread the first time. False once the
    /// program is ending, when it doesn't run.
    bool post(std::function<void(uv_loop_t*)> work) {
        std::call_once(m_started, [this] { start(); });
        const std::scoped_lock lock(m_mutex);
        if (m_stopped) {
            return false;
        }

        m_incoming.push_back(std::move(work));
        uv_async_send(&m_wake);
        return true;
    }

    void start() {
        uv_loop_init(&m_loop);
        uv_async_init(&m_loop, &m_wake, on_wake);
        m_wake.data = this;
        m_thread = std::thread([this] {
#ifndef _WIN32
            // Writing to a pipe whose child has gone gives this thread an error, not the program
            // SIGPIPE, which would end it.
            sigset_t blocked;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
            uv_run(&m_loop, UV_RUN_DEFAULT);
        });
        fb::runtime::at_finish(stop_processes);
    }

    static void stop_processes();
    static void end_signal_waits();

    void start_watching(uv_loop_t* loop, const std::size_t kind) {
        {
            const std::scoped_lock lock(m_signal_mutex);
            if (m_code_ended || m_watching.at(kind)) {
                return;
            }
        }

        m_watching.at(kind) = true;
        for (const int signal : signals_of(kind)) {
            auto watcher = std::make_unique<watcher_t>();
            watcher->kind = kind;
            watcher->handle.data = watcher.get();
            if (uv_signal_init(loop, &watcher->handle) != 0) {
                continue;
            }

            watcher->open = true;
            uv_signal_start(&watcher->handle, on_signal, signal);
            m_watchers.push_back(std::move(watcher));
        }
    }

    void close_watchers() {
        for (const std::unique_ptr<watcher_t>& watcher : m_watchers) {
            if (watcher->open) {
                watcher->open = false;
                uv_signal_stop(&watcher->handle);
                uv_close(reinterpret_cast<uv_handle_t*>(&watcher->handle), nullptr);
            }
        }
    }

    static void on_signal(uv_signal_t* handle, const int /*signal*/) {
        const auto& watcher = *static_cast<const watcher_t*>(handle->data);
        processes_t& self = processes();
        {
            const std::scoped_lock lock(self.m_signal_mutex);
            ++self.m_counts.at(watcher.kind);
        }

        self.m_signalled.notify_all();
    }

    /// Takes the requests given meanwhile, and closes everything once the program's ending.
    static void on_wake(uv_async_t* handle) {
        auto& self = *static_cast<processes_t*>(handle->data);
        std::vector<std::function<void(uv_loop_t*)>> incoming;
        bool stopped = false;
        {
            const std::scoped_lock lock(self.m_mutex);
            incoming.swap(self.m_incoming);
            stopped = self.m_stopped;
        }

        for (const std::function<void(uv_loop_t*)>& work : incoming) {
            work(&self.m_loop);
        }

        if (stopped) {
            self.close_all();
        }
    }

    /// As the program ends: what waits is answered, and every handle closes. A child that still
    /// runs goes on running.
    void close_all();
};

processes_t& processes() {
    static processes_t instance;
    return instance;
}

void processes_t::stop_processes() {
    processes().stop();
}

void processes_t::end_signal_waits() {
    processes().end_signals();
}

void answer(const waiter_t& waiter, const std::int64_t value) {
    if (waiter.blocking != nullptr) {
        processes().wake(waiter.blocking, value);
        return;
    }

    fb_task_t* task = waiter.task;
    *reinterpret_cast<std::int64_t*>(task + 1) = value;
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
}

void processes_t::on_closed(uv_handle_t* handle) {
    auto* child = static_cast<child_t*>(handle->data);
    --child->handles;
    processes().forget_if_done(child);
}

/// Closes one of a child's pipes, giving the reads that wait what its end gives.
void close_stream(stream_t& stream) {
    if (!stream.open) {
        return;
    }

    stream.open = false;
    stream.ended = true;
    if (stream.reading) {
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&stream.pipe));
        stream.reading = false;
    }

    while (!stream.reads.empty()) {
        const read_t read = stream.reads.front();
        stream.reads.pop_front();
        answer(read.waiter, stream.failure);
    }

    uv_close(reinterpret_cast<uv_handle_t*>(&stream.pipe), processes_t::on_closed);
}

void deliver(stream_t& stream);

void on_read(uv_stream_t* pipe, const ssize_t count, const uv_buf_t* buffer) {
    stream_t& stream = stream_of(reinterpret_cast<uv_handle_t*>(pipe));
    if (count > 0) {
        stream.held.append(buffer->base, static_cast<std::size_t>(count));
    } else if (count < 0) {
        stream.ended = true;
        stream.failure = count == UV_EOF ? 0 : count;
    }

    deliver(stream);
}

/// Gives each read that waits, in order, what the stream holds, while it holds anything, and at
/// its end 0 or its failure. Then reads more only for a read that waits, or a stream it drains,
/// so a child's pipe fills while nothing reads it, and the child waits.
void deliver(stream_t& stream) {
    while (!stream.reads.empty() && (!stream.held.empty() || stream.ended)) {
        const read_t read = stream.reads.front();
        stream.reads.pop_front();
        if (stream.held.empty()) {
            answer(read.waiter, stream.failure);
            continue;
        }

        const std::size_t count =
            std::min(stream.held.size(), static_cast<std::size_t>(read.count));
        std::memcpy(read.into, stream.held.data(), count);
        stream.held.erase(0, count);
        answer(read.waiter, static_cast<std::int64_t>(count));
    }

    if (stream.ended) {
        close_stream(stream);
        return;
    }

    const bool wanted = stream.draining || !stream.reads.empty();
    auto* pipe = reinterpret_cast<uv_stream_t*>(&stream.pipe);
    if (wanted && !stream.reading && stream.open) {
        // Each read goes into the stream's chunk, which deliver() empties before the next.
        const auto chunk_of = [](uv_handle_t* handle, std::size_t /*suggested*/, uv_buf_t* buffer) {
            stream_t& read = stream_of(handle);
            *buffer = uv_buf_init(read.chunk.data(), static_cast<unsigned>(read.chunk.size()));
        };
        stream.reading = uv_read_start(pipe, chunk_of, on_read) == 0;
    } else if (!wanted && stream.reading) {
        uv_read_stop(pipe);
        stream.reading = false;
    }
}

/// Closes stdin once what was written to it has gone.
void close_input(stream_t& stream) {
    if (!stream.open || stream.closing) {
        return;
    }

    stream.closing = true;
    auto* request = new uv_shutdown_t{};
    request->data = &stream;
    const int shut = uv_shutdown(request, reinterpret_cast<uv_stream_t*>(&stream.pipe),
                                 [](uv_shutdown_t* done, const int /*status*/) {
                                     auto& input = *static_cast<stream_t*>(done->data);
                                     delete done;
                                     close_stream(input);
                                 });
    if (shut != 0) {
        delete request;
        close_stream(stream);
    }
}

void processes_t::close_all() {
    // A child goes once its handles have closed, which some have already.
    const std::vector<child_t*> children = m_children;
    for (child_t* child : children) {
        for (stream_t& stream : child->streams) {
            close_stream(stream);
        }

        const std::vector<waiter_t> waits = std::exchange(child->waits, {});
        for (const waiter_t& waiter : waits) {
            answer(waiter, UV_ECANCELED);
        }

        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&child->process)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&child->process), on_closed);
        }

        child->orphaned = true;
        forget_if_done(child);
    }

    close_watchers();
    uv_close(reinterpret_cast<uv_handle_t*>(&m_wake), nullptr);
}

void on_exit(uv_process_t* process, const std::int64_t code, const int signal) {
    auto* child = static_cast<child_t*>(process->data);
    {
        const std::scoped_lock lock(processes().lock());
        child->exited = true;
        // A signal's number goes below 0, apart from the codes.
        child->status = signal != 0 ? -signal : code;
    }

    const std::vector<waiter_t> waits = std::exchange(child->waits, {});
    for (const waiter_t& waiter : waits) {
        answer(waiter, child->status);
    }

    uv_close(reinterpret_cast<uv_handle_t*>(process), processes_t::on_closed);
}

/// Whether a text has a NUL, which no program, argument, variable, or folder can.
bool has_nul(const std::string_view text) {
    return text.contains('\0');
}

/// What a spawn starts, copied out of the caller's Strings.
struct spawn_t {
    std::string program;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::optional<std::string>>> changes;
    bool cleared = false;
    std::optional<std::string> folder;
    std::array<stdio_t, 3> stdio{};
};

/// Whether two environment variables' names are one variable's, which on Windows ignores case.
bool same_variable(const std::string_view a, const std::string_view b) {
#ifdef _WIN32
    return a.size() == b.size() && std::ranges::equal(a, b, [](const char x, const char y) {
               const auto lower = [](const char c) {
                   return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
               };
               return lower(x) == lower(y);
           });
#else
    return a == b;
#endif
}

/// The child's environment, as "name=value" texts, or nullopt for this program's as it is. The
/// caller holds the environment's lock.
std::optional<std::vector<std::string>> environment_of(const spawn_t& spawn) {
    if (spawn.changes.empty() && !spawn.cleared) {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> variables;
    uv_env_item_t* items = nullptr;
    int count = 0;
    if (!spawn.cleared && uv_os_environ(&items, &count) == 0) {
        for (const uv_env_item_t& item : std::span(items, static_cast<std::size_t>(count))) {
            variables.emplace_back(item.name, item.value);
        }

        uv_os_free_environ(items, count);
    }

    for (const auto& [name, value] : spawn.changes) {
        std::erase_if(variables,
                      [&](const auto& variable) { return same_variable(variable.first, name); });
        if (value) {
            variables.emplace_back(name, *value);
        }
    }

    std::vector<std::string> texts;
    texts.reserve(variables.size());
    for (const auto& [name, value] : variables) {
        texts.push_back(std::format("{}={}", name, value));
    }

    return texts;
}

/// Starts the child on the loop's thread, and gives it, or nullptr with libuv's error in
/// `failure`.
child_t* start_child(uv_loop_t* loop, const spawn_t& spawn, std::int64_t& failure) {
    auto* child = new child_t();
    child->process.data = child;
    std::vector<char*> arguments;
    std::string program = spawn.program;
    arguments.push_back(program.data());
    std::vector<std::string> words = spawn.arguments;
    for (std::string& word : words) {
        arguments.push_back(word.data());
    }

    arguments.push_back(nullptr);
    std::array<uv_stdio_container_t, 3> stdio{};
    for (std::size_t i = 0; i < stdio.size(); ++i) {
        stream_t& stream = child->streams.at(i);
        // What isn't a pipe has no bytes for this program to read.
        stream.ended = spawn.stdio.at(i) != stdio_t::PIPED;
        switch (spawn.stdio.at(i)) {
        case stdio_t::INHERIT:
            stdio.at(i).flags = UV_INHERIT_FD;
            stdio.at(i).data.fd = static_cast<int>(i);
            break;
        case stdio_t::PIPED:
            uv_pipe_init(loop, &stream.pipe, 0);
            stream.pipe.data = child;
            ++child->handles;
            stream.open = true;
            stream.chunk.resize(65536);
            // The child reads its stdin, and writes the others.
            stdio.at(i).flags = static_cast<uv_stdio_flags>(
                static_cast<unsigned>(UV_CREATE_PIPE) |
                static_cast<unsigned>(i == 0 ? UV_READABLE_PIPE : UV_WRITABLE_PIPE));
            stdio.at(i).data.stream = reinterpret_cast<uv_stream_t*>(&stream.pipe);
            break;
        case stdio_t::NONE:
            stdio.at(i).flags = UV_IGNORE;
            break;
        }
    }

    uv_process_options_t options{};
    options.exit_cb = on_exit;
    options.file = program.c_str();
    options.args = arguments.data();
    options.cwd = spawn.folder ? spawn.folder->c_str() : nullptr;
    options.stdio_count = static_cast<int>(stdio.size());
    options.stdio = stdio.data();
    int started = 0;
    {
        // The environment the child gets is one that no thread is changing.
        const std::scoped_lock lock(fb::os::environment_lock());
        std::optional<std::vector<std::string>> environment = environment_of(spawn);
        std::vector<char*> variables;
        if (environment) {
            for (std::string& variable : *environment) {
                variables.push_back(variable.data());
            }

            variables.push_back(nullptr);
            options.env = variables.data();
        }

        started = uv_spawn(loop, &child->process, &options);
    }

    // libuv has the process's handle either way, which closes like the pipes'.
    ++child->handles;
    processes().add(child);
    if (started != 0) {
        failure = started;
        child->orphaned = true;
        for (stream_t& stream : child->streams) {
            close_stream(stream);
        }

        uv_close(reinterpret_cast<uv_handle_t*>(&child->process), processes_t::on_closed);
        return nullptr;
    }

    child->pid = child->process.pid;
    return child;
}

/// Splits `count` bytes from `from` into buffers libuv takes.
std::vector<uv_buf_t> buffers_of(std::string& bytes) {
    std::vector<uv_buf_t> buffers;
    for (std::size_t at = 0; at < bytes.size(); at += buffer_limit) {
        const std::size_t size = std::min(buffer_limit, bytes.size() - at);
        buffers.push_back(uv_buf_init(bytes.data() + at, static_cast<unsigned>(size)));
    }

    return buffers;
}

/// Writes to the child's stdin on the loop's thread, and answers the waiter once the bytes have
/// gone.
void write_input(child_t* child, std::string bytes, const waiter_t& waiter) {
    stream_t& input = child->streams.front();
    if (!input.open || input.closing) {
        answer(waiter, UV_EPIPE);
        return;
    }

    auto* write = new write_t{ .bytes = std::move(bytes), .waiter = waiter };
    write->request.data = write;
    std::vector<uv_buf_t> buffers = buffers_of(write->bytes);
    const int written =
        uv_write(&write->request, reinterpret_cast<uv_stream_t*>(&input.pipe), buffers.data(),
                 static_cast<unsigned>(buffers.size()), [](uv_write_t* request, const int status) {
                     auto* done = static_cast<write_t*>(request->data);
                     answer(done->waiter, status);
                     delete done;
                 });
    if (written != 0) {
        answer(waiter, written);
        delete write;
    }
}

/// Reads from the child's stdout or stderr on the loop's thread.
void read_output(child_t* child, const std::int32_t stream, const read_t& read) {
    stream_t& output = child->streams.at(static_cast<std::size_t>(stream));
    output.reads.push_back(read);
    deliver(output);
}

/// The kinds of error that fb_os_error_kind gives, which std.os names IOErrorKinds.
enum class error_kind_t : std::int8_t {
    NOT_FOUND,
    PERMISSION_DENIED,
    ALREADY_EXISTS,
    UNEXPECTED_END,
    INTERRUPTED,
    OTHER,
};

} // namespace

extern "C" {

std::uint64_t fb_os_process_spawn(const fb_string_t* program, const fb_array_t* arguments,
                                  const fb_array_t* names, const fb_array_t* values,
                                  const std::int32_t cleared, const fb_string_t* folder,
                                  const std::int32_t input, const std::int32_t output,
                                  const std::int32_t error, std::int64_t* failure) {
    spawn_t spawn{
        .program = std::string(text_of(program)),
        .cleared = cleared != 0,
        .stdio = { static_cast<stdio_t>(input), static_cast<stdio_t>(output),
                   static_cast<stdio_t>(error), },
    };
    for (const fb_string_t* argument : strings_of(arguments)) {
        spawn.arguments.emplace_back(text_of(argument));
    }

    const std::span<const fb_string_t* const> set = strings_of(values);
    for (std::size_t i = 0; const fb_string_t* name : strings_of(names)) {
        spawn.changes.emplace_back(std::string(text_of(name)),
                                   set[i] != nullptr ? std::optional(std::string(text_of(set[i])))
                                                     : std::nullopt);
        ++i;
    }

    if (folder->length > 0) {
        spawn.folder = std::string(text_of(folder));
    }

    *failure = 0;
    const bool nul = has_nul(spawn.program) || std::ranges::any_of(spawn.arguments, has_nul) ||
                     (spawn.folder && has_nul(*spawn.folder)) ||
                     std::ranges::any_of(spawn.changes, [](const auto& change) {
                         return change.second && has_nul(*change.second);
                     });
    if (std::ranges::any_of(spawn.changes, [](const auto& change) {
            return change.first.empty() || change.first.contains('=') || has_nul(change.first);
        })) {
        *failure = 2;
        return 0;
    }

    if (nul) {
        *failure = 1;
        return 0;
    }

    child_t* child = nullptr;
    const std::int64_t done = processes().run([&](uv_loop_t* loop, const waiter_t& waiter) {
        child = start_child(loop, spawn, *failure);
        answer(waiter, 0);
    });
    if (done != 0) {
        *failure = done;
    }

    return child != nullptr ? std::bit_cast<std::uint64_t>(child) : 0;
}

void fb_os_process_hold(const std::uint64_t child) {
    child_of(child)->holders.fetch_add(1, std::memory_order_relaxed);
}

void fb_os_process_release(const std::uint64_t child) {
    child_t* held = child_of(child);
    if (held->holders.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    processes().send([held](uv_loop_t* /*loop*/) {
        held->orphaned = true;
        close_input(held->streams.front());
        close_stream(held->streams.at(1));
        close_stream(held->streams.at(2));
        processes().forget_if_done(held);
    });
}

std::int64_t fb_os_process_pid(const std::uint64_t child) {
    return child_of(child)->pid;
}

std::int64_t fb_os_process_wait(const std::uint64_t child) {
    child_t* waited = child_of(child);
    {
        const std::scoped_lock lock(processes().lock());
        if (waited->exited) {
            return waited->status;
        }
    }

    return processes().run([waited](uv_loop_t* /*loop*/, const waiter_t& waiter) {
        if (waited->exited) {
            answer(waiter, waited->status);
        } else {
            waited->waits.push_back(waiter);
        }
    });
}

void fb_os_process_wait_async(fb_task_t* task, const std::uint64_t child) {
    child_t* waited = child_of(child);
    processes().run_async(task, [waited](uv_loop_t* /*loop*/, const waiter_t& waiter) {
        if (waited->exited) {
            answer(waiter, waited->status);
        } else {
            waited->waits.push_back(waiter);
        }
    });
}

std::int32_t fb_os_process_try_wait(const std::uint64_t child, std::int64_t* status) {
    const child_t* waited = child_of(child);
    const std::scoped_lock lock(processes().lock());
    if (!waited->exited) {
        return 0;
    }

    *status = waited->status;
    return 1;
}

std::int64_t fb_os_process_kill(const std::uint64_t child) {
    child_t* killed = child_of(child);
    return processes().run([killed](uv_loop_t* /*loop*/, const waiter_t& waiter) {
        if (killed->exited) {
            answer(waiter, 0);
            return;
        }

        const int result = uv_process_kill(&killed->process, SIGKILL);
        // One that's ended meanwhile has nothing to kill.
        answer(waiter, result == UV_ESRCH ? 0 : result);
    });
}

std::int64_t fb_os_process_read(const std::uint64_t child, const std::int32_t stream,
                                std::uint8_t* into, const std::int64_t count) {
    if (count <= 0) {
        return 0;
    }

    child_t* read = child_of(child);
    return processes().run([=](uv_loop_t* /*loop*/, const waiter_t& waiter) {
        read_output(read, stream, { .into = into, .count = count, .waiter = waiter });
    });
}

void fb_os_process_read_async(fb_task_t* task, const std::uint64_t child, const std::int32_t stream,
                              std::uint8_t* into, const std::int64_t count) {
    child_t* read = child_of(child);
    processes().run_async(task, [=](uv_loop_t* /*loop*/, const waiter_t& waiter) {
        if (count <= 0) {
            answer(waiter, 0);
            return;
        }

        read_output(read, stream, { .into = into, .count = count, .waiter = waiter });
    });
}

void fb_os_process_drain(const std::uint64_t child, const std::int32_t stream) {
    child_t* drained = child_of(child);
    processes().send([=](uv_loop_t* /*loop*/) {
        stream_t& output = drained->streams.at(static_cast<std::size_t>(stream));
        output.draining = true;
        deliver(output);
    });
}

std::int64_t fb_os_process_write(const std::uint64_t child, const std::uint8_t* from,
                                 const std::int64_t count) {
    if (count <= 0) {
        return 0;
    }

    child_t* written = child_of(child);
    std::string bytes(reinterpret_cast<const char*>(from), static_cast<std::size_t>(count));
    return processes().run([&](uv_loop_t* /*loop*/, const waiter_t& waiter) {
        write_input(written, std::move(bytes), waiter);
    });
}

void fb_os_process_write_async(fb_task_t* task, const std::uint64_t child, const std::uint8_t* from,
                               const std::int64_t count) {
    child_t* written = child_of(child);
    std::string bytes(reinterpret_cast<const char*>(from),
                      static_cast<std::size_t>(std::max<std::int64_t>(count, 0)));
    processes().run_async(task, [written, bytes = std::move(bytes)](
                                    uv_loop_t* /*loop*/, const waiter_t& waiter) mutable {
        if (bytes.empty()) {
            answer(waiter, 0);
            return;
        }

        write_input(written, std::move(bytes), waiter);
    });
}

void fb_os_process_close_input(const std::uint64_t child) {
    child_t* closed = child_of(child);
    processes().send([closed](uv_loop_t* /*loop*/) { close_input(closed->streams.front()); });
}

std::int32_t fb_os_error_kind(const std::int64_t error) {
    switch (error) {
    case UV_ENOENT:
        return static_cast<std::int32_t>(error_kind_t::NOT_FOUND);
    case UV_EACCES:
    case UV_EPERM:
        return static_cast<std::int32_t>(error_kind_t::PERMISSION_DENIED);
    case UV_EEXIST:
        return static_cast<std::int32_t>(error_kind_t::ALREADY_EXISTS);
    case UV_EOF:
        return static_cast<std::int32_t>(error_kind_t::UNEXPECTED_END);
    case UV_EINTR:
        return static_cast<std::int32_t>(error_kind_t::INTERRUPTED);
    default:
        return static_cast<std::int32_t>(error_kind_t::OTHER);
    }
}

fb_string_t* fb_os_error_text(const std::int64_t error) {
    return fb_string_from_cstring(uv_strerror(static_cast<int>(error)));
}

void fb_os_process_arguments(fb_array_t* into) {
#ifdef _WIN32
    // The command line, split as programs on Windows split theirs, and as libuv quotes a child's.
    int count = 0;
    LPWSTR* words = CommandLineToArgvW(GetCommandLineW(), &count);
    if (words == nullptr) {
        return;
    }

    for (int i = 1; i < count; ++i) {
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, words[i], -1, nullptr, 0, nullptr, nullptr);
        std::string word(static_cast<std::size_t>(std::max(size, 1)) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, words[i], -1, word.data(), size, nullptr, nullptr);
        add_string(into, word);
    }

    LocalFree(static_cast<HLOCAL>(words));
#elifdef __APPLE__
    const int count = *_NSGetArgc();
    char** words = *_NSGetArgv();
    for (int i = 1; i < count; ++i) {
        add_string(into, words[i]);
    }
#else
    // Linux keeps the arguments, each ending in a NUL, in /proc.
    std::FILE* file = std::fopen("/proc/self/cmdline", "rb");
    if (file == nullptr) {
        return;
    }

    std::string text;
    std::array<char, 4096> chunk{};
    std::size_t count = 0;
    while ((count = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) {
        text.append(chunk.data(), count);
    }

    std::fclose(file);
    std::size_t start = text.find('\0');
    while (start != std::string::npos && start + 1 < text.size()) {
        const std::size_t end = text.find('\0', start + 1);
        add_string(into, std::string_view(text).substr(start + 1, end - start - 1));
        start = end;
    }
#endif
}

std::int64_t fb_os_signal_watch(const std::int32_t signal) {
    return processes().watch(static_cast<std::size_t>(signal - 1));
}

std::int64_t fb_os_signal_wait(const std::int32_t signal, const std::int64_t seen) {
    return processes().wait_for_signal(static_cast<std::size_t>(signal - 1), seen);
}
}
