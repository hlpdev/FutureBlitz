#include "fb/lsp/outbox.hpp"

#include "fb/lsp/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fb::lsp {

outbox_t::outbox_t(std::ostream& out) : m_out(out), m_writer([this] { write_all(); }) {}

outbox_t::~outbox_t() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }

    m_ready.notify_one();
    m_writer.join();
}

void outbox_t::push(std::string body) {
    {
        const std::scoped_lock lock(m_mutex);
        m_bodies.push_back(std::move(body));
    }

    m_ready.notify_one();
}

bool outbox_t::flush(const std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_written.wait_for(lock, timeout, [this] { return m_bodies.empty() && !m_writing; });
}

void outbox_t::write_all() {
    std::unique_lock lock(m_mutex);
    while (true) {
        m_ready.wait(lock, [this] { return m_stopping || !m_bodies.empty(); });
        if (m_bodies.empty()) {
            return;
        }

        const std::string body = std::move(m_bodies.front());
        m_bodies.pop_front();
        m_writing = true;
        lock.unlock();
        write_message(m_out, body);
        lock.lock();
        m_writing = false;
        m_written.notify_all();
    }
}

std::streamsize fd_buffer_t::xsputn(const char* text, const std::streamsize count) {
    // A pipe that doesn't block takes a piece no larger than its room, or none of it.
    constexpr std::streamsize piece = 4096;
    std::streamsize written = 0;
    while (written < count) {
        const std::streamsize size = std::min(count - written, piece);
#ifdef _WIN32
        const int wrote = _write(m_fd, text + written, static_cast<unsigned int>(size));
        // What msvcrt says of a pipe that's full and doesn't block.
        const bool full = wrote == 0 || (wrote < 0 && (errno == ENOSPC || errno == EAGAIN));
#else
        const ssize_t wrote = write(m_fd, text + written, static_cast<std::size_t>(size));
        const bool full = wrote == 0 || (wrote < 0 && (errno == EAGAIN || errno == EINTR));
#endif
        if (full) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (wrote < 0) {
            return written;
        } else {
            written += wrote;
        }
    }

    return written;
}

fd_buffer_t::int_type fd_buffer_t::overflow(const int_type c) {
    if (traits_type::eq_int_type(c, traits_type::eof())) {
        return traits_type::not_eof(c);
    }

    const char byte = traits_type::to_char_type(c);
    return xsputn(&byte, 1) == 1 ? c : traits_type::eof();
}

int take_stdout() {
#ifdef _WIN32
    const int fd = _dup(1);
    // Bytes as they are: a message's length counts them, and text mode would change line breaks.
    static_cast<void>(_setmode(fd, _O_BINARY));
    // NOLINTNEXTLINE(performance-no-int-to-ptr): what _get_osfhandle gives is a HANDLE
    auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    static_cast<void>(SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0));
    static_cast<void>(_dup2(2, 1));
#else
    const int fd = fcntl(1, F_DUPFD_CLOEXEC, 3);
    static_cast<void>(dup2(2, 1));
#endif
    return fd;
}

} // namespace fb::lsp
