#include "fb/lsp/outbox.hpp"

#include "fb/lsp/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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

outbox_t::outbox_t(std::ostream& out, const std::size_t most, std::function<void()> full)
    : m_out(out), m_most(most), m_full(std::move(full)), m_writer([this] { write_all(); }) {}

outbox_t::~outbox_t() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }

    m_ready.notify_one();
    m_writer.join();
}

void outbox_t::push(std::string body, std::string replaces) {
    bool full = false;
    {
        const std::scoped_lock lock(m_mutex);
        const auto same = replaces.empty()
                              ? m_waiting.end()
                              : std::ranges::find(m_waiting, replaces, &waiting_t::replaces);
        const std::size_t freed = same != m_waiting.end() ? same->body.size() : 0;
        if (m_bytes - freed + body.size() > m_most) {
            full = true;
        } else {
            m_bytes += body.size() - freed;
            if (same != m_waiting.end()) {
                same->body = std::move(body);
            } else {
                m_waiting.push_back({ .body = std::move(body), .replaces = std::move(replaces) });
            }
        }
    }

    if (full) {
        if (m_full) {
            m_full();
        }

        return;
    }

    m_ready.notify_one();
}

bool outbox_t::flush(const std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_mutex);
    return m_written.wait_for(lock, timeout, [this] { return m_waiting.empty() && !m_writing; });
}

void outbox_t::write_all() {
    std::unique_lock lock(m_mutex);
    while (true) {
        m_ready.wait(lock, [this] { return m_stopping || !m_waiting.empty(); });
        if (m_waiting.empty()) {
            return;
        }

        const std::string body = std::move(m_waiting.front().body);
        m_waiting.pop_front();
        m_writing = true;
        lock.unlock();
        write_message(m_out, body);
        // What it writes to is gone, and nothing will read another message.
        const bool gone = !m_out.good() && m_full && !m_told_gone;
        lock.lock();
        m_bytes -= body.size();
        m_writing = false;
        m_written.notify_all();
        if (gone) {
            m_told_gone = true;
            lock.unlock();
            m_full();
            lock.lock();
        }
    }
}

fd_buffer_t::fd_buffer_t(const int fd)
    : m_write([fd](const char* text, const std::size_t size) -> wrote_t {
          if (fd < 0) {
              return {};
          }
#ifdef _WIN32
          // WriteFile, not _write, which says the same of a full disk as of a pipe that's full.
          auto* const handle = reinterpret_cast<HANDLE>( // NOLINT(performance-no-int-to-ptr)
              _get_osfhandle(fd));
          DWORD wrote = 0;
          if (WriteFile(handle, text, static_cast<DWORD>(size), &wrote, nullptr) == 0) {
              return {};
          }

          // A pipe that's full and doesn't block takes nothing, and says it's fine.
          return { .count = wrote, .again = wrote == 0 && GetFileType(handle) == FILE_TYPE_PIPE };
#else
          const ssize_t wrote = write(fd, text, size);
          if (wrote > 0) {
              return { .count = static_cast<std::size_t>(wrote) };
          }

          return { .again =
                       wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) };
#endif
      }) {
}

std::streamsize fd_buffer_t::xsputn(const char* text, const std::streamsize count) {
    // A pipe that doesn't block takes a piece no larger than its room, or none of it.
    constexpr std::streamsize piece = 4096;
    std::streamsize written = 0;
    while (written < count) {
        const std::streamsize size = std::min(count - written, piece);
        const wrote_t wrote = m_write(text + written, static_cast<std::size_t>(size));
        if (wrote.count > 0) {
            written += static_cast<std::streamsize>(wrote.count);
        } else if (wrote.again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            return written;
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
    // A stderr that's closed gets the null device first: what takes stdout would otherwise be given
    // its number, and write the messages to stderr.
#ifdef _WIN32
    const auto is_open = [](const int fd) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr): what _get_osfhandle gives is a HANDLE
        auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        return handle != INVALID_HANDLE_VALUE && handle != nullptr &&
               reinterpret_cast<std::intptr_t>(handle) != -2 &&
               GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    if (!is_open(2)) {
        const int null = _open("NUL", _O_WRONLY);
        if (null != 2) {
            static_cast<void>(_dup2(null, 2));
            static_cast<void>(_close(null));
        }
    }

    const int fd = is_open(1) ? _dup(1) : -1;
    if (fd >= 0) {
        // Bytes as they are: a message's length counts them, and text mode changes line breaks.
        static_cast<void>(_setmode(fd, _O_BINARY));
        // NOLINTNEXTLINE(performance-no-int-to-ptr): what _get_osfhandle gives is a HANDLE
        auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        static_cast<void>(SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0));
    }

    static_cast<void>(_dup2(2, 1));
#else
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl takes what its command takes
    if (fcntl(2, F_GETFD) < 0) {
        const int null = open("/dev/null", O_WRONLY);
        if (null != 2) {
            static_cast<void>(dup2(null, 2));
            static_cast<void>(close(null));
        }
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl takes what its command takes
    const int fd = fcntl(1, F_DUPFD_CLOEXEC, 3);
    static_cast<void>(dup2(2, 1));
#endif
    return fd;
}

} // namespace fb::lsp
