#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
#include <thread>

namespace fb::lsp {

/// The messages going to the client, which a thread of its own writes, whole and in the order they
/// came, so a client slow to read them keeps nothing waiting: not the message thread, nor a check
/// that publishes with the workspace's lock held. It alone writes to `out`.
class outbox_t {
public:
    explicit outbox_t(std::ostream& out);
    outbox_t(const outbox_t&) = delete;
    outbox_t& operator=(const outbox_t&) = delete;
    /// Writes what's waiting, and stops: a write the client isn't reading keeps it waiting, so a
    /// process that has to end goes through flush() and ends without this.
    ~outbox_t();

    /// Adds a message's body, which it frames, without waiting for it to be written.
    void push(std::string body);

    /// Waits for what's been pushed to be written, for at most `timeout`, and says whether it was.
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout);

private:
    std::ostream& m_out;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::condition_variable m_written;
    std::deque<std::string> m_bodies;
    bool m_writing = false;
    bool m_stopping = false;
    // Last, so it starts with the rest ready.
    std::thread m_writer;

    void write_all();
};

/// A stream's buffer over a file descriptor, with no buffer of its own: a write goes on until the
/// file has all of it, a piece at a time, whatever a pipe takes at once, and through one that's
/// full and doesn't block. It ends only when the file can't be written, as a client that's gone.
class fd_buffer_t : public std::streambuf {
public:
    explicit fd_buffer_t(int fd) : m_fd(fd) {}

protected:
    std::streamsize xsputn(const char* text, std::streamsize count) override;
    int_type overflow(int_type c) override;

private:
    int m_fd;
};

/// Takes the process's stdout for the messages alone: a descriptor of it that child processes don't
/// inherit, which writes bytes as they are, with stdout itself pointed at stderr, so nothing else
/// that writes to it, a library or a child, lands among the messages.
[[nodiscard]] int take_stdout();

} // namespace fb::lsp
