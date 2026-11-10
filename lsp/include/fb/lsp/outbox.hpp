#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>

namespace fb::lsp {

/// The messages going to the client, which a thread of its own writes, whole and in the order they
/// came, so a client slow to read them keeps nothing waiting: not the message thread, nor a check
/// that publishes with the workspace's lock held. It alone writes to `out`.
class outbox_t {
public:
    /// `full` is told when more than `most` bytes wait, and a client that's stopped reading isn't
    /// coming back: the message past it goes unsent, though one with nothing else waiting always
    /// goes. `gone` is told once when `out` can't be written, as what the client read from is
    /// closed.
    explicit outbox_t(std::ostream& out, std::size_t most = std::numeric_limits<std::size_t>::max(),
                      std::function<void()> full = {}, std::function<void()> gone = {});
    outbox_t(const outbox_t&) = delete;
    outbox_t& operator=(const outbox_t&) = delete;
    /// Writes what's waiting, and stops: a write the client isn't reading keeps it waiting, so a
    /// process that has to end goes through flush() and ends without this.
    ~outbox_t();

    /// Adds a message's body, which it frames, without waiting for it to be written. One that says
    /// what `replaces` says takes the place of the one waiting that does, as a document's newer
    /// diagnostics do of its older ones.
    void push(std::string body, std::string replaces = {});

    /// Waits for what's been pushed to be written, and says whether it was: for as long as it's
    /// going out, and no longer than `timeout` past the last of it that did, a message or, with
    /// set_progress(), a piece of one. A client that reads slowly, or a machine that's busy, gets
    /// all of it; one that's stopped reading holds it for `timeout`.
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout);

    /// What says how much of what's written has gone out so far, a count that only grows, as
    /// fd_buffer_t's written() is, so flush() sees a long message going out piece by piece.
    void set_progress(std::function<std::uint64_t()> written);

private:
    std::ostream& m_out;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::condition_variable m_written;
    struct waiting_t {
        std::string body;
        std::string replaces;
    };
    std::size_t m_most;
    std::function<void()> m_full;
    std::function<void()> m_gone;
    /// What's in m_waiting, which the one being written has left.
    std::size_t m_bytes = 0;
    std::deque<waiting_t> m_waiting;
    bool m_writing = false;
    /// How many messages are written, and what set_progress() gave.
    std::uint64_t m_done = 0;
    std::function<std::uint64_t()> m_progress;
    bool m_told_gone = false;
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
    /// What a write did: how much it took, and, for none, whether it's worth trying again.
    struct wrote_t {
        std::size_t count = 0;
        bool again = false;
    };
    using write_t = std::function<wrote_t(const char* text, std::size_t size)>;

    explicit fd_buffer_t(int fd);
    /// For the tests, what writes in place of a file.
    explicit fd_buffer_t(write_t write) : m_write(std::move(write)) {}

    /// How many bytes the file has taken so far, from any thread.
    [[nodiscard]] std::uint64_t written() const {
        return m_written.load(std::memory_order_relaxed);
    }

protected:
    std::streamsize xsputn(const char* text, std::streamsize count) override;
    int_type overflow(int_type c) override;

private:
    write_t m_write;
    std::atomic<std::uint64_t> m_written = 0;
};

/// Takes the process's stdout for the messages alone: a descriptor of it that child processes don't
/// inherit, which writes bytes as they are, with stdout itself pointed at stderr, so nothing else
/// that writes to it, a library or a child, lands among the messages; at the null device, with no
/// stderr to point it at. -1 with no stdout to take.
[[nodiscard]] int take_stdout();

} // namespace fb::lsp
