#include "fb/lsp/outbox.hpp"
#include "fb/lsp/protocol.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <format>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

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

namespace {

/// What a client that stops reading is: a write waits until it's let go.
class held_buffer_t : public std::stringbuf {
public:
    void let_go() {
        {
            const std::scoped_lock lock(m_mutex);
            m_held = false;
        }

        m_changed.notify_all();
    }

protected:
    std::streamsize xsputn(const char* text, const std::streamsize count) override {
        wait();
        return std::stringbuf::xsputn(text, count);
    }

    int_type overflow(const int_type c) override {
        wait();
        return std::stringbuf::overflow(c);
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_held = true;

    void wait() {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [this] { return !m_held; });
    }
};

} // namespace

TEST_CASE("a client that stops reading keeps no one waiting, and gets every message in order",
          "[lsp]") {
    held_buffer_t buffer;
    std::ostream out(&buffer);
    {
        fb::lsp::outbox_t outbox(out);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) {
            outbox.push(std::to_string(i));
        }

        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        CHECK_FALSE(outbox.flush(std::chrono::milliseconds(50)));
        buffer.let_go();
        CHECK(outbox.flush(std::chrono::seconds(30)));
    }

    std::istringstream written(buffer.str());
    for (int i = 0; i < 1000; ++i) {
        CHECK(fb::lsp::read_message(written) == std::optional(std::to_string(i)));
    }

    CHECK_FALSE(fb::lsp::read_message(written));
}

TEST_CASE("what's waiting is written when the outbox goes", "[lsp]") {
    std::ostringstream out;
    {
        fb::lsp::outbox_t outbox(out);
        outbox.push("one");
        outbox.push("two");
    }

    CHECK(out.str() == "Content-Length: 3\r\n\r\noneContent-Length: 3\r\n\r\ntwo");
}

namespace {

/// How many writes a stream's buffer got.
class counting_buffer_t : public std::stringbuf {
public:
    int writes = 0;

protected:
    std::streamsize xsputn(const char* text, const std::streamsize count) override {
        ++writes;
        return std::stringbuf::xsputn(text, count);
    }
};

/// A message of `size` bytes that says which it is all the way along, so one with bytes gone, or
/// moved, is told from the rest.
std::string body_of(const std::size_t index, const std::size_t size) {
    const std::string mark = std::format("{{{}}}", index);
    std::string body;
    body.reserve(size + mark.size());
    while (body.size() < size) {
        body += mark;
    }

    return body;
}

int read_some(const int fd, char* into, const unsigned int size) {
#ifdef _WIN32
    return _read(fd, into, size);
#else
    return static_cast<int>(read(fd, into, size));
#endif
}

} // namespace

TEST_CASE("a message's header and body are one write", "[lsp]") {
    counting_buffer_t buffer;
    std::ostream out(&buffer);
    fb::lsp::write_message(out, "{}");
    CHECK(buffer.writes == 1);
    CHECK(buffer.str() == "Content-Length: 2\r\n\r\n{}");
}

TEST_CASE("every message is whole over a real pipe, to a slow reader, in a burst", "[lsp]") {
    const bool blocks = GENERATE(true, false);
    CAPTURE(blocks);
    std::array<int, 2> ends{};
#ifdef _WIN32
    REQUIRE(_pipe(ends.data(), 4096, _O_BINARY) == 0);
    if (!blocks) {
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        // NOLINTNEXTLINE(performance-no-int-to-ptr): what _get_osfhandle gives is a HANDLE
        auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(ends[1]));
        REQUIRE(SetNamedPipeHandleState(handle, &mode, nullptr, nullptr));
    }
#else
    REQUIRE(pipe(ends.data()) == 0);
    if (!blocks) {
        REQUIRE(fcntl(ends[1], F_SETFL, fcntl(ends[1], F_GETFL) | O_NONBLOCK) == 0);
    }
#endif

    // Small ones, as refreshes are, among diagnostics and code lenses that fill the pipe.
    constexpr std::array<std::size_t, 3> sizes{ 37, 3000, 70'000 };
    std::vector<std::string> bodies(150);
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        bodies[i] = body_of(i, sizes[i % sizes.size()]);
    }

    std::string received;
    std::thread reader([&] {
        std::array<char, 2048> bytes{};
        int reads = 0;
        while (true) {
            const int got = read_some(ends[0], bytes.data(), bytes.size());
            if (got <= 0) {
                return;
            }

            received.append(bytes.data(), static_cast<std::size_t>(got));
            if (++reads % 64 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
    {
        fb::lsp::fd_buffer_t buffer(ends[1]);
        std::ostream out(&buffer);
        fb::lsp::outbox_t outbox(out);
        for (const std::string& body : bodies) {
            outbox.push(body);
        }

        CHECK(outbox.flush(std::chrono::minutes(2)));
    }

#ifdef _WIN32
    _close(ends[1]);
#else
    close(ends[1]);
#endif
    reader.join();
#ifdef _WIN32
    _close(ends[0]);
#else
    close(ends[0]);
#endif
    std::istringstream frames(received);
    for (const std::string& body : bodies) {
        // Not REQUIRE(frame == body), which would print them.
        REQUIRE((fb::lsp::read_message(frames) == std::optional(body)));
    }

    CHECK_FALSE(fb::lsp::read_message(frames));
}
