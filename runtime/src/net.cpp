// std.net's sockets: TCP streams and listeners, UDP sockets, and IPC's streams and listeners,
// named pipes on Windows and Unix sockets elsewhere, over libuv's handles, which only the I/O
// thread touches. A blocking caller waits on its socket for what the I/O thread gives it, and an
// Async one for a Task<Int32> the I/O thread finishes, after which the fb code takes what came,
// so the I/O thread never touches an object of the caller's but a blocking one's memory while
// it waits. The program's end closes what's still open, before the I/O thread stops.

#include "fb/runtime.hpp"
#include "internal.hpp"
#include "io_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <uv.h>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

/// How many bytes a stream reads ahead of what's taken, before it stops until they're taken.
constexpr std::size_t read_ahead = 65536;
/// How many datagrams a UDP socket keeps that nothing took; past that it stops receiving, and the
/// OS keeps the rest, or drops them, as it does when its buffer is full.
constexpr std::size_t kept_datagrams = 64;
/// The most bytes a datagram holds, what libuv receives into.
constexpr std::size_t datagram_size = 65536;
/// How many bytes one buffer that libuv writes holds, which a ULONG counts on Windows.
constexpr std::size_t written_at_once = std::size_t{ 1 } << 30U;
/// An address's bytes, as the fb code passes them: see runtime.hpp.
constexpr std::size_t address_size = 23;
/// A timeout this long, half what an Int64 counts, waits as long as it takes, so a deadline never
/// overflows a clock.
constexpr std::int64_t forever = INT64_MAX / 2;

/// What a socket is, as fb_io_net_new's `kind` says.
enum class kind_t : std::uint8_t { TCP, PIPE, TCP_LISTENER, PIPE_LISTENER, UDP };

/// What fb_io_net_error_kind gives: NetErrorKind's ordinals, and a timeout.
enum class meaning_t : std::uint8_t {
    MEANS_REFUSED,
    MEANS_RESET,
    MEANS_UNREACHABLE,
    MEANS_HOST_NOT_FOUND,
    MEANS_CLOSED,
    MEANS_TLS,
    /// std.net.http's, which no socket gives.
    MEANS_HTTP_STATUS,
    MEANS_OTHER,
    MEANS_TIMED_OUT,
};

struct socket_t;

/// A Task waiting for its socket to change, with the timer that ends the wait early when it has
/// a timeout, and closes the socket when it's a connect's.
struct waiting_t {
    uv_timer_t timer{};
    fb_task_t* task = nullptr;
    socket_t* socket = nullptr;
    bool timed = false;
    bool closes = false;
};

/// A datagram a UDP socket received, and where from.
struct datagram_t {
    std::vector<std::uint8_t> bytes;
    sockaddr_storage from{};
};

/// A socket and its handle. What's under `mutex` blocking callers share with the I/O thread,
/// which `changed` tells them of; the rest only the I/O thread touches.
struct socket_t {
    explicit socket_t(const kind_t of) : kind(of) {}

    kind_t kind;
    union {
        uv_handle_t handle;
        uv_stream_t stream;
        uv_tcp_t tcp;
        uv_pipe_t pipe;
        uv_udp_t udp;
    };
    uv_connect_t connecting{};
    std::mutex mutex;
    std::condition_variable changed;
    /// The fb object holds it, as an accepted one's doesn't until fb_io_net_accepted takes it.
    bool held = true;
    /// It has a handle, until libuv closed it.
    bool opened = false;
    bool closed = false;
    /// The I/O thread stopped, which closes the handle if fb_io_net_close can't.
    bool stopped = false;
    /// fb_io_net_close, its fb object, or the program's end began closing it.
    bool closing = false;
    /// A stream connected, with 0, or failed to.
    std::optional<std::int32_t> connected;
    /// A stream's ShutdownWrite ended what it writes, so a write after is CLOSED.
    bool shut = false;
    /// An IPC listener's lock on the file beside its Unix socket, which says the name is taken
    /// while it listens, or -1.
    int lock = -1;
    /// A stream's other side ended it, with 0, or it failed.
    std::optional<std::int32_t> ended;
    /// The bytes read and not yet taken, from `taken` on.
    std::vector<std::uint8_t> incoming;
    std::size_t taken = 0;
    /// A listener's connections, which it holds until they're taken.
    std::deque<socket_t*> accepted;
    std::deque<datagram_t> datagrams;
    sockaddr_storage local{};
    sockaddr_storage remote{};
    /// It reads, or receives, as what waits for it wants, into `chunk`, which libuv has from
    /// each allocation until the read's callback, and which goes when it stops.
    bool reading = false;
    std::vector<char> chunk;
    std::vector<waiting_t*> waiting;

    /// libuv's callbacks, once it closed the handle, once a connect is done, for memory to read
    /// or receive into, and with what it read or received.
    static void on_closed(uv_handle_t* handle);
    static void on_connected(uv_connect_t* request, int status);
    static void on_allocate(uv_handle_t* handle, std::size_t suggested, uv_buf_t* buffer);
    static void on_read(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer);
    static void on_allocate_datagram(uv_handle_t* handle, std::size_t suggested, uv_buf_t* buffer);
    static void on_received(uv_udp_t* handle, ssize_t count, const uv_buf_t* buffer,
                            const sockaddr* from, unsigned flags);
};

/// The sockets with handles, which the program's end closes, and whether it has: only the I/O
/// thread touches them.
struct sockets_t {
    std::vector<socket_t*> open;
    bool stopping = false;
};

sockets_t& sockets() {
    static sockets_t instance;
    return instance;
}

void close_all();

#ifndef _WIN32
/// Keeps SIGPIPE from ending the program when the I/O thread writes to a socket the other side
/// reset: libuv's write gives UV_EPIPE then, which is RESET, as Windows gives it. Only the I/O
/// thread blocks it, so the program's own writes keep SIGPIPE as they had it.
void block_sigpipe(const uv_loop_t* loop) {
    if (loop == nullptr) {
        return;
    }

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
}
#endif

/// Runs `job` on the I/O thread, or at once with no loop once the program is ending. The first
/// time, it has the I/O thread block SIGPIPE first, and the program's end close the sockets
/// before the I/O thread stops, which on_io_thread has registered to by then, so that runs after.
void run(std::function<void(uv_loop_t*)> job) {
#ifndef _WIN32
    static std::once_flag blocking;
    std::call_once(blocking, [] { fb::runtime::on_io_thread(block_sigpipe); });
#endif
    fb::runtime::on_io_thread(std::move(job));
    static std::once_flag registered;
    std::call_once(registered, [] { fb::runtime::at_finish(close_all); });
}

/// Runs `job` on the I/O thread and waits for what it gives, or UV_ECANCELED once the program's
/// ending.
std::int32_t call(const std::function<std::int32_t(uv_loop_t*)>& job) {
    struct answer_t {
        std::mutex mutex;
        std::condition_variable done;
        std::optional<std::int32_t> result;
    } answer;
    run([&answer, &job](uv_loop_t* loop) {
        const std::int32_t result = loop == nullptr ? UV_ECANCELED : job(loop);
        // The caller goes, and its answer with it, once it has the lock again.
        const std::scoped_lock lock(answer.mutex);
        answer.result = result;
        answer.done.notify_all();
    });
    std::unique_lock lock(answer.mutex);
    answer.done.wait(lock, [&] { return answer.result.has_value(); });
    return answer.result.value_or(UV_ECANCELED);
}

/// Waits until `ready` holds, or `timeout` nanoseconds have gone, unless it's negative, and says
/// whether it holds.
template <typename Ready>
bool wait_until(std::unique_lock<std::mutex>& lock, socket_t* socket, const std::int64_t timeout,
                Ready ready) {
    if (timeout < 0 || timeout >= forever) {
        socket->changed.wait(lock, ready);
        return true;
    }

    return socket->changed.wait_for(lock, std::chrono::nanoseconds(timeout), ready);
}

/// Holds a new Task<Int32> until finish_task.
void hold_task(fb_task_t* task) {
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
}

/// Finishes a Task<Int32> that hold_task held, with `code`: its result is right after its
/// fb_task_t.
void finish_task(fb_task_t* task, const std::int32_t code) {
    *reinterpret_cast<std::int32_t*>(task + 1) = code;
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
}

void begin_close(socket_t* socket);

void on_wait_closed(uv_handle_t* handle) {
    delete reinterpret_cast<waiting_t*>(handle);
}

/// Finishes a waiting Task with `code`, taking it off its socket's list.
void finish_waiting(waiting_t* waiting, const std::int32_t code) {
    std::erase(waiting->socket->waiting, waiting);
    finish_task(waiting->task, code);
    if (!waiting->timed) {
        delete waiting;
        return;
    }

    uv_timer_stop(&waiting->timer);
    uv_close(reinterpret_cast<uv_handle_t*>(&waiting->timer), on_wait_closed);
}

void on_waited_too_long(uv_timer_t* timer) {
    auto* waiting = reinterpret_cast<waiting_t*>(timer);
    socket_t* socket = waiting->socket;
    const bool closes = waiting->closes;
    finish_waiting(waiting, UV_ETIMEDOUT);
    // A connect that took too long goes with its socket, which its caller lets go.
    if (closes) {
        begin_close(socket);
    }
}

/// libuv's timers count whole milliseconds from when the loop's iteration began, so a timer
/// waits a millisecond past its time rounded up, which is never less than its time.
std::uint64_t milliseconds_of(const std::int64_t nanoseconds) {
    if (nanoseconds <= 0) {
        return 0;
    }

    return static_cast<std::uint64_t>((nanoseconds / 1000000) +
                                      (nanoseconds % 1000000 != 0 ? 1 : 0) + 1);
}

/// Has `task` wait for the socket to change, for up to `timeout` nanoseconds unless it's
/// negative, closing the socket when it takes too long if `closes`.
void wait_for_change(uv_loop_t* loop, socket_t* socket, fb_task_t* task, const std::int64_t timeout,
                     const bool closes) {
    auto* waiting = new waiting_t;
    waiting->task = task;
    waiting->socket = socket;
    waiting->closes = closes;
    waiting->timed = timeout >= 0 && timeout < forever;
    if (waiting->timed) {
        uv_update_time(loop);
        uv_timer_init(loop, &waiting->timer);
        uv_timer_start(&waiting->timer, on_waited_too_long, milliseconds_of(timeout), 0);
    }

    socket->waiting.push_back(waiting);
}

/// Finishes the Tasks waiting for the socket with `code`: 0 to look again, or what went wrong.
void wake_tasks(const socket_t* socket, const std::int32_t code) {
    const std::vector<waiting_t*> waiting = socket->waiting;
    for (waiting_t* each : waiting) {
        finish_waiting(each, code);
    }
}

/// The address `bytes` has, as fb code passes it, or false when it isn't one.
bool sockaddr_of(const fb_array_t* bytes, sockaddr_storage& address) {
    if (std::cmp_not_equal(bytes->count, address_size)) {
        return false;
    }

    const auto* data = static_cast<const std::uint8_t*>(bytes->data);
    const auto port =
        static_cast<std::uint16_t>((static_cast<unsigned>(data[17]) << 8U) | data[18]);
    address = {};
    if (data[0] == 4) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        std::memcpy(&v4->sin_addr, data + 1, 4);
        return true;
    }

    if (data[0] == 6) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        std::memcpy(&v6->sin6_addr, data + 1, 16);
        std::uint32_t scope = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            scope |= static_cast<std::uint32_t>(data[19 + i]) << (i * 8);
        }
        v6->sin6_scope_id = scope;
        return true;
    }

    return false;
}

/// `address`'s bytes, as fb code reads them, or nothing when it's neither IPv4's nor IPv6's.
std::optional<std::array<std::uint8_t, address_size>> bytes_of(const sockaddr_storage& address) {
    std::array<std::uint8_t, address_size> data{};
    std::uint16_t port = 0;
    if (address.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
        data[0] = 4;
        std::memcpy(&data[1], &v4->sin_addr, 4);
        port = ntohs(v4->sin_port);
    } else if (address.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
        data[0] = 6;
        std::memcpy(&data[1], &v6->sin6_addr, 16);
        port = ntohs(v6->sin6_port);
        for (std::size_t i = 0; i < 4; ++i) {
            data[19 + i] = static_cast<std::uint8_t>(v6->sin6_scope_id >> (i * 8));
        }
    } else {
        return std::nullopt;
    }

    data[17] = static_cast<std::uint8_t>(static_cast<unsigned>(port) >> 8U);
    data[18] = static_cast<std::uint8_t>(port);
    return data;
}

/// Puts `bytes` into `output`, a UInt8[].
void give_bytes(const std::vector<std::uint8_t>& bytes, fb_array_t* output) {
    output->count = 0;
    if (!bytes.empty()) {
        fb_array_reserve(output, 1, static_cast<std::int64_t>(bytes.size()), nullptr);
        std::memcpy(output->data, bytes.data(), bytes.size());
        output->count = static_cast<std::int64_t>(bytes.size());
    }
}

/// Puts `address` into `output`, a UInt8[], as fb code reads it, or nothing when it's neither
/// IPv4's nor IPv6's.
bool give_address(const sockaddr_storage& address, fb_array_t* output) {
    const std::optional<std::array<std::uint8_t, address_size>> data = bytes_of(address);
    if (!data) {
        return false;
    }

    give_bytes({ data->begin(), data->end() }, output);
    return true;
}

std::string text_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

/// Where an IPC listener of `name` is, its letters lowercase, as Windows' named pipes and macOS's
/// files ignore case: a named pipe on Windows, and elsewhere a Unix socket in the temporary folder
/// libuv names, or in /tmp where that folder's path would make it longer than a Unix socket's can
/// be, as macOS's per-user folder can.
std::string pipe_path(const std::string& name) {
    std::string lower = name;
    std::ranges::transform(lower, lower.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
#ifdef _WIN32
    return R"(\\.\pipe\)" + lower;
#else
    std::array<char, 4096> folder{};
    std::size_t size = folder.size();
    std::string path = uv_os_tmpdir(folder.data(), &size) == 0 ? std::string(folder.data(), size)
                                                               : std::string("/tmp");
    path += "/" + lower + ".sock";
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        path = "/tmp/" + lower + ".sock";
    }
    return path;
#endif
}

/// Keeps the socket's addresses, once it's connected, accepted, or bound.
void remember_addresses(socket_t* socket) {
    sockaddr_storage local{};
    sockaddr_storage remote{};
    int size = sizeof(local);
    if (socket->kind == kind_t::UDP) {
        uv_udp_getsockname(&socket->udp, reinterpret_cast<sockaddr*>(&local), &size);
    } else if (socket->kind == kind_t::TCP || socket->kind == kind_t::TCP_LISTENER) {
        uv_tcp_getsockname(&socket->tcp, reinterpret_cast<sockaddr*>(&local), &size);
        size = sizeof(remote);
        if (socket->kind == kind_t::TCP) {
            uv_tcp_getpeername(&socket->tcp, reinterpret_cast<sockaddr*>(&remote), &size);
        }
    }

    const std::scoped_lock lock(socket->mutex);
    socket->local = local;
    socket->remote = remote;
}

/// Gives the socket a handle on the loop, unless the program is ending or it's closing: 0, or
/// libuv's error.
std::int32_t open_handle(uv_loop_t* loop, socket_t* socket) {
    {
        const std::scoped_lock lock(socket->mutex);
        if (socket->closing || socket->opened) {
            return socket->closing ? UV_ECANCELED : UV_EALREADY;
        }
    }

    if (sockets().stopping) {
        return UV_ECANCELED;
    }

    int opened = 0;
    switch (socket->kind) {
    case kind_t::TCP:
    case kind_t::TCP_LISTENER:
        opened = uv_tcp_init(loop, &socket->tcp);
        break;
    case kind_t::PIPE:
    case kind_t::PIPE_LISTENER:
        opened = uv_pipe_init(loop, &socket->pipe, 0);
        break;
    case kind_t::UDP:
        opened = uv_udp_init(loop, &socket->udp);
        break;
    }

    if (opened != 0) {
        return opened;
    }

    socket->handle.data = socket;
    sockets().open.push_back(socket);
    const std::scoped_lock lock(socket->mutex);
    socket->opened = true;
    return 0;
}

void socket_t::on_closed(uv_handle_t* handle) {
    auto* socket = static_cast<socket_t*>(handle->data);
    std::erase(sockets().open, socket);
#ifndef _WIN32
    // libuv took the Unix socket's file away as it closed, so the name is free.
    if (socket->lock >= 0) {
        ::close(socket->lock);
        socket->lock = -1;
    }
#endif
    bool unheld = false;
    {
        const std::scoped_lock lock(socket->mutex);
        socket->closed = true;
        unheld = !socket->held;
        socket->changed.notify_all();
    }

    if (unheld) {
        delete socket;
    }
}

/// Closes the socket, once: what waits for it finishes with UV_ECANCELED, a listener's
/// connections that nothing took go too, and the socket goes once libuv closed its handle, unless
/// its fb object still holds it.
void begin_close(socket_t* socket) {
    std::deque<socket_t*> accepted;
    bool opened = false;
    {
        const std::scoped_lock lock(socket->mutex);
        if (socket->closing) {
            return;
        }

        socket->closing = true;
        opened = socket->opened;
        accepted.swap(socket->accepted);
        socket->changed.notify_all();
    }

    wake_tasks(socket, UV_ECANCELED);
    for (socket_t* connection : accepted) {
        begin_close(connection);
    }

    if (opened) {
        uv_close(&socket->handle, socket_t::on_closed);
    }
}

void read_more(socket_t* socket);

/// What a socket of its own is, for a blocking caller: still connecting, or what its connect
/// gave.
void finish_connect(socket_t* socket, const std::int32_t status) {
    if (status == 0) {
        if (socket->kind == kind_t::TCP) {
            uv_tcp_nodelay(&socket->tcp, 1);
        }
        remember_addresses(socket);
    }

    {
        const std::scoped_lock lock(socket->mutex);
        socket->connected = status;
        socket->changed.notify_all();
    }

    // It reads from here on, as far as it reads ahead, so what comes is there for a Read that
    // doesn't wait.
    if (status == 0) {
        read_more(socket);
    }
    wake_tasks(socket, status);
}

void socket_t::on_connected(uv_connect_t* request, const int status) {
    finish_connect(static_cast<socket_t*>(request->data), status);
}

/// Starts connecting a stream on the I/O thread, to `target` or, for an IPC stream, `path`.
void start_connect(uv_loop_t* loop, socket_t* socket, const sockaddr_storage& target,
                   const std::string& path) {
    std::int32_t started = open_handle(loop, socket);
    if (started == 0) {
        socket->connecting.data = socket;
        if (socket->kind == kind_t::TCP) {
            started =
                uv_tcp_connect(&socket->connecting, &socket->tcp,
                               reinterpret_cast<const sockaddr*>(&target), socket_t::on_connected);
        } else {
            started = uv_pipe_connect2(&socket->connecting, &socket->pipe, path.data(), path.size(),
                                       UV_PIPE_NO_TRUNCATE, socket_t::on_connected);
        }
    }

    if (started != 0) {
        finish_connect(socket, started);
    }
}

/// Connects, blocking, for fb_io_net_connect and fb_io_net_connect_pipe.
std::int32_t connect_blocking(socket_t* socket, const sockaddr_storage& target,
                              const std::string& path, const std::int64_t timeout) {
    run([socket, target, path](uv_loop_t* loop) {
        if (loop == nullptr) {
            const std::scoped_lock lock(socket->mutex);
            socket->connected = UV_ECANCELED;
            socket->changed.notify_all();
            return;
        }

        start_connect(loop, socket, target, path);
    });
    std::unique_lock lock(socket->mutex);
    if (wait_until(lock, socket, timeout,
                   [&] { return socket->connected.has_value() || socket->closing; })) {
        return socket->connected.value_or(UV_ECANCELED);
    }

    // It took too long: what it started goes with the socket, which the caller lets go.
    lock.unlock();
    run([socket](uv_loop_t* loop) {
        if (loop != nullptr) {
            begin_close(socket);
        }
    });
    return UV_ETIMEDOUT;
}

/// Connects, finishing `task`, for fb_io_net_connect_async and fb_io_net_connect_pipe_async.
void connect_async(fb_task_t* task, socket_t* socket, const sockaddr_storage& target,
                   const std::string& path, const std::int64_t timeout) {
    hold_task(task);
    run([task, socket, target, path, timeout](uv_loop_t* loop) {
        if (loop == nullptr) {
            finish_task(task, UV_ECANCELED);
            return;
        }

        wait_for_change(loop, socket, task, timeout, true);
        start_connect(loop, socket, target, path);
    });
}

/// Whether a stream has bytes to take, or its end.
bool readable(const socket_t* socket) {
    return socket->incoming.size() > socket->taken || socket->ended.has_value() || socket->closing;
}

void socket_t::on_allocate(uv_handle_t* handle, const std::size_t suggested, uv_buf_t* buffer) {
    auto* socket = static_cast<socket_t*>(handle->data);
    socket->chunk.resize(std::max<std::size_t>(suggested, read_ahead));
    *buffer = uv_buf_init(socket->chunk.data(), static_cast<unsigned>(socket->chunk.size()));
}

/// Stops reading, or receiving, and lets go of what it read into.
void stop_reading(socket_t* socket) {
    if (socket->kind == kind_t::UDP) {
        uv_udp_recv_stop(&socket->udp);
    } else {
        uv_read_stop(&socket->stream);
    }

    socket->reading = false;
    socket->chunk = {};
}

void socket_t::on_read(uv_stream_t* stream, const ssize_t count, const uv_buf_t* buffer) {
    auto* socket = static_cast<socket_t*>(stream->data);
    bool full = false;
    if (count > 0) {
        const std::scoped_lock lock(socket->mutex);
        socket->incoming.erase(socket->incoming.begin(),
                               socket->incoming.begin() +
                                   static_cast<std::ptrdiff_t>(socket->taken));
        socket->taken = 0;
        socket->incoming.insert(socket->incoming.end(), buffer->base, buffer->base + count);
        full = socket->incoming.size() >= read_ahead;
        socket->changed.notify_all();
    } else if (count < 0) {
        const std::scoped_lock lock(socket->mutex);
        socket->ended = count == UV_EOF ? 0 : static_cast<std::int32_t>(count);
        full = true;
        socket->changed.notify_all();
    }

    if (count != 0) {
        wake_tasks(socket, 0);
    }

    if (full && socket->reading) {
        stop_reading(socket);
    }
}

/// Has a stream read, if it isn't already and can.
void read_more(socket_t* socket) {
    {
        const std::scoped_lock lock(socket->mutex);
        if (socket->reading || socket->closing || socket->ended || !socket->opened ||
            socket->connected != 0) {
            return;
        }
    }

    const int started = uv_read_start(&socket->stream, socket_t::on_allocate, socket_t::on_read);
    socket->reading = started == 0;
    if (started != 0) {
        const std::scoped_lock lock(socket->mutex);
        socket->ended = started;
        socket->changed.notify_all();
    }
}

/// Takes up to `count` of a stream's bytes into `into`, under its lock.
std::int64_t take(socket_t* socket, std::uint8_t* into, const std::int64_t count) {
    const std::size_t there = socket->incoming.size() - socket->taken;
    if (there > 0) {
        const std::size_t size = std::min<std::size_t>(there, static_cast<std::size_t>(count));
        std::memcpy(into, socket->incoming.data() + socket->taken, size);
        socket->taken += size;
        if (socket->taken == socket->incoming.size()) {
            socket->incoming.clear();
            socket->taken = 0;
        }
        return static_cast<std::int64_t>(size);
    }

    if (socket->closing) {
        return UV_ECANCELED;
    }

    return socket->ended.value_or(UV_ETIMEDOUT);
}

/// A write, a shutdown, or a send, which a blocking caller waits for, or a Task, with the copy of
/// its bytes an Async one has.
struct writing_t {
    union {
        uv_write_t write;
        uv_shutdown_t shutdown;
        uv_udp_send_t send;
    };
    socket_t* socket = nullptr;
    std::vector<std::uint8_t> copy;
    fb_task_t* task = nullptr;
    std::optional<std::int32_t> result;

    /// libuv's callbacks, once a write, a shutdown, or a send is done.
    static void on_written(uv_write_t* request, int status);
    static void on_shut(uv_shutdown_t* request, int status);
    static void on_sent(uv_udp_send_t* request, int status);
};

/// The write is done: its caller wakes, or its Task finishes, and an Async one's copy goes.
void finish_writing(writing_t* writing, const std::int32_t status) {
    if (writing->task != nullptr) {
        finish_task(writing->task, status);
        delete writing;
        return;
    }

    socket_t* socket = writing->socket;
    // The caller goes, and its writing with it, once it has the lock again.
    const std::scoped_lock lock(socket->mutex);
    writing->result = status;
    socket->changed.notify_all();
}

void writing_t::on_written(uv_write_t* request, const int status) {
    auto* writing = static_cast<writing_t*>(request->data);
    bool closing = false;
    {
        const std::scoped_lock lock(writing->socket->mutex);
        closing = writing->socket->closing;
    }

    // libuv on Windows gives UV_ECANCELED for a write to a connection the other side aborted,
    // as it does everywhere for one that closing the socket cancels.
    finish_writing(writing, status == UV_ECANCELED && !closing ? UV_ECONNRESET : status);
}

void writing_t::on_shut(uv_shutdown_t* request, const int status) {
    finish_writing(static_cast<writing_t*>(request->data), status);
}

void writing_t::on_sent(uv_udp_send_t* request, const int status) {
    finish_writing(static_cast<writing_t*>(request->data), status);
}

/// Whether a stream can write: connected, and not closing.
std::int32_t writable(socket_t* socket) {
    const std::scoped_lock lock(socket->mutex);
    if (socket->closing) {
        return UV_ECANCELED;
    }

    // What this side ended is closed, not reset by the other side, as libuv's UV_EPIPE says.
    if (socket->shut) {
        return UV_ENOTCONN;
    }

    return socket->connected.value_or(UV_ENOTCONN);
}

/// Starts writing `size` bytes from `data`, in buffers libuv counts.
void start_write(writing_t* writing, const std::uint8_t* data, const std::size_t size) {
    socket_t* socket = writing->socket;
    std::int32_t started = writable(socket);
    if (started == 0) {
        std::vector<uv_buf_t> buffers;
        for (std::size_t at = 0; at < size; at += written_at_once) {
            buffers.push_back(uv_buf_init(
                const_cast<char*>(reinterpret_cast<const char*>(data + at)),
                static_cast<unsigned>(std::min<std::size_t>(written_at_once, size - at))));
        }

        writing->write.data = writing;
        started = uv_write(&writing->write, &socket->stream, buffers.data(),
                           static_cast<unsigned>(buffers.size()), writing_t::on_written);
    }

    if (started != 0) {
        finish_writing(writing, started);
    }
}

void start_shutdown(writing_t* writing) {
    socket_t* socket = writing->socket;
    std::int32_t started = writable(socket);
    if (started == 0) {
        writing->shutdown.data = writing;
        started = uv_shutdown(&writing->shutdown, &socket->stream, writing_t::on_shut);
        if (started == 0) {
            const std::scoped_lock lock(socket->mutex);
            socket->shut = true;
        }
    }

    if (started != 0) {
        finish_writing(writing, started);
    }
}

void start_send(writing_t* writing, const std::uint8_t* data, const std::size_t size,
                const sockaddr_storage& target) {
    socket_t* socket = writing->socket;
    std::int32_t started = 0;
    {
        const std::scoped_lock lock(socket->mutex);
        if (socket->closing) {
            started = UV_ECANCELED;
        } else if (!socket->opened) {
            started = UV_EBADF;
        }
    }

    if (started == 0) {
        const uv_buf_t buffer = uv_buf_init(const_cast<char*>(reinterpret_cast<const char*>(data)),
                                            static_cast<unsigned>(size));
        writing->send.data = writing;
        started = uv_udp_send(&writing->send, &socket->udp, &buffer, 1,
                              reinterpret_cast<const sockaddr*>(&target), writing_t::on_sent);
    }

    if (started != 0) {
        finish_writing(writing, started);
    }
}

/// Runs `start` with a blocking caller's writing on the I/O thread, and waits for it.
std::int32_t write_blocking(socket_t* socket, const std::function<void(writing_t*)>& start) {
    writing_t writing;
    writing.socket = socket;
    run([&writing, &start](uv_loop_t* loop) {
        if (loop == nullptr) {
            finish_writing(&writing, UV_ECANCELED);
            return;
        }

        start(&writing);
    });
    std::unique_lock lock(socket->mutex);
    socket->changed.wait(lock, [&] { return writing.result.has_value(); });
    return writing.result.value_or(UV_ECANCELED);
}

/// Runs `start` with an Async writing on the I/O thread, which finishes `task`.
void write_async(fb_task_t* task, socket_t* socket, std::vector<std::uint8_t> copy,
                 std::function<void(writing_t*)> start) {
    hold_task(task);
    auto* writing = new writing_t;
    writing->socket = socket;
    writing->task = task;
    writing->copy = std::move(copy);
    run([writing, start = std::move(start)](uv_loop_t* loop) {
        if (loop == nullptr) {
            finish_writing(writing, UV_ECANCELED);
            return;
        }

        start(writing);
    });
}

/// A listener has a connection: it accepts it at once, as libuv asks, and keeps it until it's
/// taken.
void on_connection(uv_stream_t* server, const int status) {
    auto* listener = static_cast<socket_t*>(server->data);
    if (status < 0) {
        return;
    }

    auto* connection =
        new socket_t(listener->kind == kind_t::TCP_LISTENER ? kind_t::TCP : kind_t::PIPE);
    connection->held = false;
    std::int32_t accepted = open_handle(server->loop, connection);
    if (accepted == 0) {
        accepted = uv_accept(server, &connection->stream);
    }

    if (accepted != 0) {
        if (connection->opened) {
            begin_close(connection);
        } else {
            delete connection;
        }
        return;
    }

    if (connection->kind == kind_t::TCP) {
        uv_tcp_nodelay(&connection->tcp, 1);
    }
    remember_addresses(connection);
    connection->connected = 0;
    // It reads from here on, as a connected stream does.
    read_more(connection);
    {
        const std::scoped_lock lock(listener->mutex);
        listener->accepted.push_back(connection);
        listener->changed.notify_all();
    }

    wake_tasks(listener, 0);
}

#ifndef _WIN32
/// Whether a Unix socket's file is one that nothing listens at any more, which a program that
/// ended without closing it left.
bool left_behind(const std::string& path) {
    // A file that isn't a socket is someone's, and stays.
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode)) {
        return false;
    }

    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) {
        return false;
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) {
        return false;
    }

    const bool refused =
        ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 &&
        errno == ECONNREFUSED;
    ::close(probe);
    return refused;
}
#endif

/// Binds an IPC listener at its path, in place of a Unix socket's file nothing listens at.
std::int32_t bind_pipe(socket_t* socket, const std::string& path) {
#ifndef _WIN32
    // The lock says a listener of std's has the name, without connecting to it, which would give
    // it a connection that ends at once. The OS lets go of it however its program ends.
    const int lock = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    // libuv's errors on POSIX are errno's, negated.
    if (lock < 0) {
        return -errno;
    }

    if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock);
        return UV_EADDRINUSE;
    }
#endif
    std::int32_t bound =
        uv_pipe_bind2(&socket->pipe, path.data(), path.size(), UV_PIPE_NO_TRUNCATE);
#ifndef _WIN32
    // Nothing of std's listens at the name, so a Unix socket's file there was left behind by a
    // program that ended without closing its listener, unless one that takes no lock listens.
    if (bound == UV_EADDRINUSE && left_behind(path)) {
        ::unlink(path.c_str());
        bound = uv_pipe_bind2(&socket->pipe, path.data(), path.size(), UV_PIPE_NO_TRUNCATE);
    }

    if (bound == 0) {
        socket->lock = lock;
    } else {
        ::close(lock);
    }
#endif
    return bound;
}

void socket_t::on_received(uv_udp_t* handle, const ssize_t count, const uv_buf_t* buffer,
                           const sockaddr* from, const unsigned /*flags*/) {
    auto* socket = static_cast<socket_t*>(handle->data);
    // 0 bytes from no one is nothing more to read now, and an error leaves the socket as it was.
    bool kept = false;
    bool full = false;
    if (count >= 0 && from != nullptr) {
        datagram_t datagram;
        datagram.bytes.assign(buffer->base, buffer->base + count);
        std::memcpy(&datagram.from, from,
                    from->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));
        const std::scoped_lock lock(socket->mutex);
        socket->datagrams.push_back(std::move(datagram));
        full = socket->datagrams.size() >= kept_datagrams;
        kept = true;
        socket->changed.notify_all();
    }

    if (kept) {
        wake_tasks(socket, 0);
    }

    if (full && socket->reading) {
        stop_reading(socket);
    }
}

void socket_t::on_allocate_datagram(uv_handle_t* handle, const std::size_t /*suggested*/,
                                    uv_buf_t* buffer) {
    auto* socket = static_cast<socket_t*>(handle->data);
    socket->chunk.resize(datagram_size);
    *buffer = uv_buf_init(socket->chunk.data(), static_cast<unsigned>(socket->chunk.size()));
}

/// Has a UDP socket receive, if it isn't already and can.
void receive_more(socket_t* socket) {
    {
        const std::scoped_lock lock(socket->mutex);
        if (socket->reading || socket->closing || !socket->opened) {
            return;
        }
    }

    socket->reading =
        uv_udp_recv_start(&socket->udp, socket_t::on_allocate_datagram, socket_t::on_received) == 0;
}

/// Whether a UDP socket has a datagram to take, or is closing.
bool received(const socket_t* socket) {
    return !socket->datagrams.empty() || socket->closing;
}

/// Whether a listener has a connection to take, or is closing.
bool acceptable(const socket_t* socket) {
    return !socket->accepted.empty() || socket->closing;
}

/// Waits, blocking, until `ready` holds for the socket once `start` ran on the I/O thread: 0, or
/// UV_ECANCELED once it's closing, or UV_ETIMEDOUT.
std::int32_t wait_blocking(socket_t* socket, const std::int64_t timeout, void (*start)(socket_t*),
                           bool (*ready)(const socket_t*)) {
    std::unique_lock lock(socket->mutex);
    if (!ready(socket)) {
        lock.unlock();
        run([socket, start](uv_loop_t* loop) {
            if (loop != nullptr) {
                start(socket);
                return;
            }

            const std::scoped_lock ending(socket->mutex);
            socket->closing = true;
            socket->changed.notify_all();
        });
        lock.lock();
        if (!wait_until(lock, socket, timeout, [&] { return ready(socket); })) {
            return UV_ETIMEDOUT;
        }
    }

    return socket->closing ? UV_ECANCELED : 0;
}

/// Waits, finishing `task`, until `ready` holds for the socket once `start` ran.
void wait_async(fb_task_t* task, socket_t* socket, const std::int64_t timeout,
                void (*start)(socket_t*), bool (*ready)(const socket_t*)) {
    hold_task(task);
    run([task, socket, timeout, start, ready](uv_loop_t* loop) {
        if (loop == nullptr) {
            finish_task(task, UV_ECANCELED);
            return;
        }

        bool now = false;
        bool closing = false;
        {
            const std::scoped_lock lock(socket->mutex);
            now = ready(socket);
            closing = socket->closing;
        }

        if (now) {
            finish_task(task, closing ? UV_ECANCELED : 0);
            return;
        }

        wait_for_change(loop, socket, task, timeout, false);
        start(socket);
    });
}

void start_nothing(socket_t* /*socket*/) {}

/// Closes every socket still open as the program ends, on the I/O thread, before it stops, so
/// its loop has nothing left; a socket opened after is refused.
void close_all() {
    run([](uv_loop_t* loop) {
        if (loop == nullptr) {
            return;
        }

        sockets().stopping = true;
        const std::vector<socket_t*> open = sockets().open;
        for (socket_t* socket : open) {
            begin_close(socket);
        }
    });
}

} // namespace

extern "C" {

void* fb_io_net_new(const std::int32_t kind) {
    return new socket_t(static_cast<kind_t>(kind));
}

void fb_io_net_release(void* handle) {
    auto* socket = static_cast<socket_t*>(handle);
    run([socket](uv_loop_t* loop) {
        bool done = false;
        {
            const std::scoped_lock lock(socket->mutex);
            socket->held = false;
            done = !socket->opened || socket->closed;
        }

        if (done) {
            delete socket;
        } else if (loop != nullptr) {
            begin_close(socket);
        }
        // Otherwise the I/O thread is closing it as it stops, and on_closed lets it go.
    });
}

void fb_io_net_close(void* handle) {
    auto* socket = static_cast<socket_t*>(handle);
    run([socket](uv_loop_t* loop) {
        if (loop != nullptr) {
            begin_close(socket);
            return;
        }

        const std::scoped_lock lock(socket->mutex);
        socket->closing = true;
        socket->stopped = true;
        socket->changed.notify_all();
    });
    // Once libuv closed it, its port or name is free, so a listener closed can listen again.
    std::unique_lock lock(socket->mutex);
    socket->changed.wait(lock,
                         [&] { return socket->closed || !socket->opened || socket->stopped; });
}

std::int32_t fb_io_net_connect(void* handle, const fb_array_t* address,
                               const std::int64_t timeout) {
    sockaddr_storage target{};
    if (!sockaddr_of(address, target)) {
        return UV_EINVAL;
    }

    return connect_blocking(static_cast<socket_t*>(handle), target, {}, timeout);
}

void fb_io_net_connect_async(fb_task_t* task, void* handle, const fb_array_t* address,
                             const std::int64_t timeout) {
    sockaddr_storage target{};
    if (!sockaddr_of(address, target)) {
        hold_task(task);
        finish_task(task, UV_EINVAL);
        return;
    }

    connect_async(task, static_cast<socket_t*>(handle), target, {}, timeout);
}

std::int32_t fb_io_net_connect_pipe(void* handle, const fb_string_t* name,
                                    const std::int64_t timeout) {
    return connect_blocking(static_cast<socket_t*>(handle), {}, pipe_path(text_of(name)), timeout);
}

void fb_io_net_connect_pipe_async(fb_task_t* task, void* handle, const fb_string_t* name,
                                  const std::int64_t timeout) {
    connect_async(task, static_cast<socket_t*>(handle), {}, pipe_path(text_of(name)), timeout);
}

std::int32_t fb_io_net_listen(void* handle, const fb_array_t* address, const std::int32_t backlog) {
    auto* socket = static_cast<socket_t*>(handle);
    sockaddr_storage at{};
    if (!sockaddr_of(address, at)) {
        return UV_EINVAL;
    }

    return call([socket, &at, backlog](uv_loop_t* loop) {
        std::int32_t result = open_handle(loop, socket);
        if (result == 0) {
            result = uv_tcp_bind(&socket->tcp, reinterpret_cast<const sockaddr*>(&at), 0);
        }
        if (result == 0) {
            result = uv_listen(&socket->stream, backlog, on_connection);
        }
        if (result == 0) {
            remember_addresses(socket);
        }
        return result;
    });
}

std::int32_t fb_io_net_listen_pipe(void* handle, const fb_string_t* name,
                                   const std::int32_t backlog) {
    auto* socket = static_cast<socket_t*>(handle);
    const std::string path = pipe_path(text_of(name));
    return call([socket, &path, backlog](uv_loop_t* loop) {
        std::int32_t result = open_handle(loop, socket);
        if (result == 0) {
            result = bind_pipe(socket, path);
        }
        if (result == 0) {
            result = uv_listen(&socket->stream, backlog, on_connection);
        }
        return result;
    });
}

std::int32_t fb_io_net_accept(void* handle, const std::int64_t timeout) {
    return wait_blocking(static_cast<socket_t*>(handle), timeout, start_nothing, acceptable);
}

void fb_io_net_accept_async(fb_task_t* task, void* handle, const std::int64_t timeout) {
    wait_async(task, static_cast<socket_t*>(handle), timeout, start_nothing, acceptable);
}

void* fb_io_net_accepted(void* handle) {
    auto* listener = static_cast<socket_t*>(handle);
    const std::scoped_lock lock(listener->mutex);
    if (listener->accepted.empty() || listener->closing) {
        return nullptr;
    }

    socket_t* connection = listener->accepted.front();
    listener->accepted.pop_front();
    const std::scoped_lock taken(connection->mutex);
    connection->held = true;
    return connection;
}

std::int64_t fb_io_net_read(void* handle, std::uint8_t* into, const std::int64_t count,
                            const std::int64_t timeout) {
    auto* socket = static_cast<socket_t*>(handle);
    if (count <= 0) {
        return 0;
    }

    // A stream that has nothing reads more, which one that doesn't wait leaves for its next Read.
    std::unique_lock lock(socket->mutex);
    if (!readable(socket)) {
        lock.unlock();
        run([socket](uv_loop_t* loop) {
            if (loop != nullptr) {
                read_more(socket);
                return;
            }

            const std::scoped_lock ending(socket->mutex);
            socket->closing = true;
            socket->changed.notify_all();
        });
        lock.lock();
        if (timeout != 0 && !wait_until(lock, socket, timeout, [&] { return readable(socket); })) {
            return UV_ETIMEDOUT;
        }
    }

    return take(socket, into, count);
}

void fb_io_net_read_async(fb_task_t* task, void* handle, const std::int64_t timeout) {
    wait_async(task, static_cast<socket_t*>(handle), timeout, read_more, readable);
}

std::int32_t fb_io_net_write(void* handle, const std::uint8_t* from, const std::int64_t count) {
    if (count <= 0) {
        return 0;
    }

    const auto size = static_cast<std::size_t>(count);
    return write_blocking(static_cast<socket_t*>(handle),
                          [from, size](writing_t* writing) { start_write(writing, from, size); });
}

void fb_io_net_write_async(fb_task_t* task, void* handle, const std::uint8_t* from,
                           const std::int64_t count) {
    std::vector<std::uint8_t> copy(from, from + std::max<std::int64_t>(count, 0));
    write_async(task, static_cast<socket_t*>(handle), std::move(copy), [](writing_t* writing) {
        start_write(writing, writing->copy.data(), writing->copy.size());
    });
}

std::int32_t fb_io_net_shutdown(void* handle) {
    return write_blocking(static_cast<socket_t*>(handle), start_shutdown);
}

void fb_io_net_shutdown_async(fb_task_t* task, void* handle) {
    write_async(task, static_cast<socket_t*>(handle), {}, start_shutdown);
}

std::int32_t fb_io_net_bind(void* handle, const fb_array_t* address, const std::int32_t reuse) {
    auto* socket = static_cast<socket_t*>(handle);
    sockaddr_storage at{};
    if (!sockaddr_of(address, at)) {
        return UV_EINVAL;
    }

    return call([socket, &at, reuse](uv_loop_t* loop) {
        std::int32_t result = open_handle(loop, socket);
        if (result == 0) {
            result = uv_udp_bind(&socket->udp, reinterpret_cast<const sockaddr*>(&at),
                                 reuse != 0 ? UV_UDP_REUSEADDR : 0);
        }
        // It receives from here on, so a datagram that came is there for a Receive that doesn't
        // wait.
        if (result == 0) {
            remember_addresses(socket);
            receive_more(socket);
        }
        return result;
    });
}

std::int32_t fb_io_net_send(void* handle, const std::uint8_t* from, const std::int64_t count,
                            const fb_array_t* address) {
    sockaddr_storage target{};
    if (!sockaddr_of(address, target)) {
        return UV_EINVAL;
    }

    if (count < 0 || std::cmp_greater(count, datagram_size)) {
        return UV_EMSGSIZE;
    }

    const auto size = static_cast<std::size_t>(count);
    return write_blocking(
        static_cast<socket_t*>(handle),
        [from, size, &target](writing_t* writing) { start_send(writing, from, size, target); });
}

void fb_io_net_send_async(fb_task_t* task, void* handle, const std::uint8_t* from,
                          const std::int64_t count, const fb_array_t* address) {
    sockaddr_storage target{};
    const bool sized = count >= 0 && std::cmp_less_equal(count, datagram_size);
    if (!sized || !sockaddr_of(address, target)) {
        hold_task(task);
        finish_task(task, sized ? UV_EINVAL : UV_EMSGSIZE);
        return;
    }

    std::vector<std::uint8_t> copy(from, from + count);
    write_async(task, static_cast<socket_t*>(handle), std::move(copy),
                [target](writing_t* writing) {
                    start_send(writing, writing->copy.data(), writing->copy.size(), target);
                });
}

std::int32_t fb_io_net_receive(void* handle, const std::int64_t timeout) {
    return wait_blocking(static_cast<socket_t*>(handle), timeout, receive_more, received);
}

void fb_io_net_receive_async(fb_task_t* task, void* handle, const std::int64_t timeout) {
    wait_async(task, static_cast<socket_t*>(handle), timeout, receive_more, received);
}

std::int32_t fb_io_net_received(void* handle, fb_array_t* bytes, fb_array_t* from) {
    auto* socket = static_cast<socket_t*>(handle);
    datagram_t datagram;
    {
        const std::scoped_lock lock(socket->mutex);
        if (socket->datagrams.empty() || socket->closing) {
            return 0;
        }

        datagram = std::move(socket->datagrams.front());
        socket->datagrams.pop_front();
    }

    give_bytes(datagram.bytes, bytes);
    give_address(datagram.from, from);
    return 1;
}

std::int32_t fb_io_net_set(void* handle, const std::int32_t option, const std::int32_t value) {
    auto* socket = static_cast<socket_t*>(handle);
    return call([socket, option, value](uv_loop_t* /*loop*/) -> std::int32_t {
        {
            const std::scoped_lock lock(socket->mutex);
            if (socket->closing || !socket->opened) {
                return UV_ECANCELED;
            }
        }

        switch (option) {
        case 0:
            return uv_udp_set_broadcast(&socket->udp, value);
        case 1:
            return uv_udp_set_multicast_loop(&socket->udp, value);
        case 2:
            return uv_udp_set_multicast_ttl(&socket->udp, value);
        default:
            return UV_EINVAL;
        }
    });
}

std::int32_t fb_io_net_membership(void* handle, const fb_string_t* group, const fb_string_t* on,
                                  const std::int32_t join) {
    auto* socket = static_cast<socket_t*>(handle);
    const std::string address = text_of(group);
    const std::string local = text_of(on);
    return call([socket, &address, &local, join](uv_loop_t* /*loop*/) -> std::int32_t {
        {
            const std::scoped_lock lock(socket->mutex);
            if (socket->closing || !socket->opened) {
                return UV_ECANCELED;
            }
        }

        return uv_udp_set_membership(&socket->udp, address.c_str(),
                                     local.empty() ? nullptr : local.c_str(),
                                     join != 0 ? UV_JOIN_GROUP : UV_LEAVE_GROUP);
    });
}

std::int32_t fb_io_net_multicast_interface(void* handle, const fb_string_t* on) {
    auto* socket = static_cast<socket_t*>(handle);
    const std::string address = text_of(on);
    return call([socket, &address](uv_loop_t* /*loop*/) -> std::int32_t {
        {
            const std::scoped_lock lock(socket->mutex);
            if (socket->closing || !socket->opened) {
                return UV_ECANCELED;
            }
        }

        return uv_udp_set_multicast_interface(&socket->udp, address.c_str());
    });
}

std::int32_t fb_io_net_interfaces(fb_array_t* output) {
    uv_interface_address_t* found = nullptr;
    int count = 0;
    if (const int listed = uv_interface_addresses(&found, &count); listed != 0) {
        return listed;
    }

    std::vector<std::uint8_t> records;
    for (int i = 0; i < count; ++i) {
        const uv_interface_address_t& each = found[i];
        const int family = each.address.address4.sin_family;
        if (each.is_internal != 0 || (family != AF_INET && family != AF_INET6)) {
            continue;
        }

        // Its address, then its netmask, as an address of the same family.
        sockaddr_storage address{};
        sockaddr_storage netmask{};
        if (family == AF_INET) {
            std::memcpy(&address, &each.address.address4, sizeof(sockaddr_in));
            auto* mask = reinterpret_cast<sockaddr_in*>(&netmask);
            mask->sin_family = AF_INET;
            mask->sin_addr = each.netmask.netmask4.sin_addr;
        } else {
            std::memcpy(&address, &each.address.address6, sizeof(sockaddr_in6));
            auto* mask = reinterpret_cast<sockaddr_in6*>(&netmask);
            mask->sin6_family = AF_INET6;
            mask->sin6_addr = each.netmask.netmask6.sin6_addr;
        }

        const std::optional<std::array<std::uint8_t, address_size>> data = bytes_of(address);
        const std::optional<std::array<std::uint8_t, address_size>> mask = bytes_of(netmask);
        if (data && mask) {
            records.insert(records.end(), data->begin(), data->end());
            records.insert(records.end(), mask->begin(), mask->end());
        }
    }

    uv_free_interface_addresses(found, count);
    give_bytes(records, output);
    return 0;
}

fb_string_t* fb_io_net_host_name() {
    std::array<char, 256> name{};
    std::size_t size = name.size();
    if (uv_os_gethostname(name.data(), &size) != 0) {
        return fb_string_from_cstring("localhost");
    }

    return fb_string_from_bytes(name.data(), static_cast<std::int64_t>(size));
}

std::int32_t fb_io_net_address(void* handle, const std::int32_t remote, fb_array_t* output) {
    auto* socket = static_cast<socket_t*>(handle);
    sockaddr_storage address{};
    {
        const std::scoped_lock lock(socket->mutex);
        address = remote != 0 ? socket->remote : socket->local;
    }

    return give_address(address, output) ? 0 : UV_ENOTCONN;
}

std::int32_t fb_io_net_error_kind(const std::int32_t code) {
    meaning_t meaning = meaning_t::MEANS_OTHER;
    switch (code) {
    // An IPC listener's name with nothing there, or a Unix socket's file nothing listens at.
    case UV_ENOENT:
    case UV_ECONNREFUSED:
        meaning = meaning_t::MEANS_REFUSED;
        break;
    case UV_ECONNRESET:
    case UV_ECONNABORTED:
    case UV_EPIPE:
        meaning = meaning_t::MEANS_RESET;
        break;
    case UV_ENETUNREACH:
    case UV_EHOSTUNREACH:
    case UV_ENETDOWN:
    case UV_EHOSTDOWN:
        meaning = meaning_t::MEANS_UNREACHABLE;
        break;
    case UV_EAI_NONAME:
        meaning = meaning_t::MEANS_HOST_NOT_FOUND;
        break;
    case UV_ECANCELED:
    case UV_ENOTCONN:
    case UV_EBADF:
        meaning = meaning_t::MEANS_CLOSED;
        break;
    case UV_ETIMEDOUT:
        meaning = meaning_t::MEANS_TIMED_OUT;
        break;
    default:
        break;
    }
    return static_cast<std::int32_t>(meaning);
}
}
