#include "client.hpp"
#include "fb/lsp/inbox.hpp"
#include "fb/lsp/outbox.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
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
        // Each returns with the buffer holding what's written, so a push that waited never would.
        for (int i = 0; i < 1000; ++i) {
            outbox.push(std::to_string(i));
        }

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

TEST_CASE("only a document's last diagnostics wait, and what waits has a limit", "[lsp]") {
    held_buffer_t buffer;
    std::ostream out(&buffer);
    int full = 0;
    {
        fb::lsp::outbox_t outbox(out, 100, [&full] { ++full; });
        // The first may be taken by the writer, which the held buffer stops.
        outbox.push("first");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        outbox.push("a1", "a");
        outbox.push("b1", "b");
        outbox.push("a2", "a");
        outbox.push(std::string(50, 'x'));
        outbox.push(std::string(50, 'y'));
        CHECK(full == 1);
        // A replacement fits, as what it replaces goes, where a new message of its size wouldn't.
        outbox.push(std::string(47, 'c'));
        CHECK(full == 2);
        outbox.push("a3", "a");
        CHECK(full == 2);
        buffer.let_go();
        CHECK(outbox.flush(std::chrono::seconds(30)));
    }

    std::istringstream written(buffer.str());
    std::vector<std::string> bodies;
    while (const std::optional<std::string> body = fb::lsp::read_message(written)) {
        bodies.push_back(*body);
    }

    CHECK(bodies == std::vector<std::string>{ "first", "a3", "b1", std::string(50, 'x') });
}

TEST_CASE("a write is tried again only when the file can take it later", "[lsp]") {
    int calls = 0;
    std::string written;
    fb::lsp::fd_buffer_t would_block([&](const char* text, const std::size_t size) {
        if (++calls % 2 == 1) {
            return fb::lsp::fd_buffer_t::wrote_t{ .again = true };
        }

        written.append(text, 1);
        static_cast<void>(size);
        return fb::lsp::fd_buffer_t::wrote_t{ .count = 1 };
    });
    CHECK(would_block.sputn("abc", 3) == 3);
    CHECK(written == "abc");

    // A full disk, or anything else, ends it, and a write of nothing that won't change does too.
    calls = 0;
    fb::lsp::fd_buffer_t failed([&](const char*, const std::size_t) {
        ++calls;
        return fb::lsp::fd_buffer_t::wrote_t{};
    });
    CHECK(failed.sputn("abc", 3) == 0);
    CHECK(calls == 1);
}

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_CASE("what writes to stdout doesn't reach the messages", "[lsp]") {
    const fb::lsp::test::folder_t folder({});
    const std::filesystem::path file = folder.path("stray.txt");
    const std::vector<std::string> arguments{ "file", fb::to_utf8(file) };
    const std::expected<fb::process_result_t, std::string> run =
        fb::run_process(FB_LSP_STDOUT_PROBE, arguments);
    REQUIRE(run);
    CHECK(run->output == "message");
    CHECK(read_file(file) == "stray");
}

TEST_CASE("with no stderr, what writes to stdout goes nowhere", "[lsp]") {
    const std::vector<std::string> arguments{ "closed" };
    const std::expected<fb::process_result_t, std::string> run =
        fb::run_process(FB_LSP_STDOUT_PROBE, arguments);
    REQUIRE(run);
    CHECK(run->exit_code == 0);
    CHECK(run->output == "message");
}

namespace {

/// What writes nothing, as a stdout that's closed does.
class closed_buffer_t : public std::streambuf {};

} // namespace

TEST_CASE("an outbox says once that what it writes to is gone, which isn't that it's full",
          "[lsp]") {
    closed_buffer_t buffer;
    std::ostream out(&buffer);
    std::atomic<int> gone = 0;
    std::atomic<int> full = 0;
    fb::lsp::outbox_t outbox(out, 1024, [&full] { ++full; }, [&gone] { ++gone; });
    outbox.push("one");
    outbox.push("two");
    CHECK(outbox.flush(std::chrono::seconds(30)));
    CHECK(gone == 1);
    CHECK(full == 0);
}

TEST_CASE("one message goes whatever its size, with nothing else waiting", "[lsp]") {
    std::atomic<int> full = 0;
    const std::string long_answer(100, 'x');
    SECTION("with a client that's reading") {
        std::ostringstream out;
        {
            fb::lsp::outbox_t outbox(out, 10, [&full] { ++full; });
            outbox.push(long_answer);
            CHECK(outbox.flush(std::chrono::seconds(30)));
            outbox.push(long_answer);
        }

        CHECK(full == 0);
        std::istringstream written(out.str());
        CHECK(fb::lsp::read_message(written) == long_answer);
        CHECK(fb::lsp::read_message(written) == long_answer);
    }

    SECTION("though what's behind it is held to the limit") {
        held_buffer_t buffer;
        std::ostream out(&buffer);
        {
            fb::lsp::outbox_t outbox(out, 10, [&full] { ++full; });
            // The first the writer may have, which the buffer holds, and the next waits.
            outbox.push(long_answer);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            outbox.push(long_answer);
            CHECK(full == 0);
            outbox.push(long_answer);
            CHECK(full == 1);
            // One that replaces all that's waiting is alone.
            outbox.push("a", "a");
            CHECK(full == 2);
            buffer.let_go();
        }
    }
}

namespace {

/// A stream's buffer that says the most it was asked for at once.
class asked_buffer_t : public std::stringbuf {
public:
    using std::stringbuf::stringbuf;

    [[nodiscard]] std::streamsize most_asked() const {
        return m_most_asked;
    }

protected:
    std::streamsize xsgetn(char* into, const std::streamsize count) override {
        m_most_asked = std::max(m_most_asked, count);
        return std::stringbuf::xsgetn(into, count);
    }

private:
    std::streamsize m_most_asked = 0;
};

} // namespace

TEST_CASE("a length no message has is the end of what can be read", "[lsp]") {
    std::istringstream too_long("Content-Length: 9223372036854775807\r\n\r\n");
    CHECK_FALSE(fb::lsp::read_message(too_long));

    // Past 1 GiB it's refused, reading nothing of the body; at it, it reads what there is.
    REQUIRE(fb::lsp::most_message == std::size_t{ 1 } << 30U);
    std::istringstream past(
        std::format("Content-Length: {}\r\n\r\nabc", fb::lsp::most_message + 1));
    CHECK_FALSE(fb::lsp::read_message(past));
    CHECK(past.get() == 'a');
    std::istringstream at(std::format("Content-Length: {}\r\n\r\nabc", fb::lsp::most_message));
    CHECK_FALSE(fb::lsp::read_message(at));
    CHECK(at.get() == std::istringstream::traits_type::eof());
}

TEST_CASE("a message's memory grows as its bytes come", "[lsp]") {
    // A length with no body behind it: nothing asks for it all at once, which takes the memory.
    asked_buffer_t no_body(std::format("Content-Length: {}\r\n\r\n", fb::lsp::most_message));
    std::istream no_body_in(&no_body);
    CHECK_FALSE(fb::lsp::read_message(no_body_in));
    CHECK(no_body.most_asked() <= static_cast<std::streamsize>(fb::lsp::most_read));

    // And one longer than a piece is whole.
    const std::string body = body_of(1, (fb::lsp::most_read * 2) + 3);
    asked_buffer_t whole(std::format("Content-Length: {}\r\n\r\n{}", body.size(), body));
    std::istream whole_in(&whole);
    CHECK((fb::lsp::read_message(whole_in) == std::optional(body)));
    CHECK(whole.most_asked() <= static_cast<std::streamsize>(fb::lsp::most_read));
}

TEST_CASE("a child doesn't inherit what the messages are written to", "[lsp]") {
    // Nothing reads its stderr, and a file would be held by the child after the test.
#ifdef _WIN32
    const std::vector<std::string> arguments{ "child", "NUL" };
#else
    const std::vector<std::string> arguments{ "child", "/dev/null" };
#endif
    const auto begun = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> run =
        fb::run_process(FB_LSP_STDOUT_PROBE, arguments);
    REQUIRE(run);
    CHECK(run->output == "message");
    // The child lives for 10 seconds, and the pipe would stay open with it.
    CHECK(std::chrono::steady_clock::now() - begun < std::chrono::seconds(7));
}

TEST_CASE("the message thread is going on until one message holds it for too long", "[lsp]") {
    fb::lsp::liveness_t liveness(std::chrono::milliseconds(50));
    CHECK(liveness.going());
    liveness.handling();
    CHECK(liveness.going());
    liveness.handled();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(liveness.going());
    liveness.handling();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(liveness.going());
    CHECK(liveness.held_for(std::chrono::milliseconds(50)));
    liveness.handled();
    CHECK(liveness.going());
    CHECK_FALSE(liveness.held_for(std::chrono::milliseconds(0)));
}
