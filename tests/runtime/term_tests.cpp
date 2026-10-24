// The pseudoconsole the probe runs in on Windows is Windows 10 1809's.
#if defined(_WIN32) && !defined(NTDDI_VERSION)
#define NTDDI_VERSION 0x0A000006
#endif

#include "fb/runtime.hpp"
#include "fb/term.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace {

void free_bytes(fb_object_t* object) {
    fb_array_free(reinterpret_cast<fb_array_t*>(object));
}

void visit_nothing(const fb_object_t* /*object*/, fb_visitor_t /*visitor*/, void* /*context*/) {}

/// The Class of a UInt8[], whose elements need no releasing.
fb_class_t bytes_class() {
    fb_class_t type{};
    type.name = "UInt8[]";
    type.size = sizeof(fb_array_t);
    type.release = free_bytes;
    type.visit = visit_nothing;
    return type;
}

/// The first count of a UInt8[]'s bytes.
std::string text_of(const fb_array_t* bytes, const std::int64_t count) {
    return { static_cast<const char*>(bytes->data), static_cast<std::size_t>(count) };
}

/// What the probe printed that isn't a terminal's escape codes, with "\r\n" as "\n", as a
/// pseudoconsole draws it.
std::string plain(const std::string_view output) {
    std::string kept;
    for (std::size_t at = 0; at < output.size(); ++at) {
        const char c = output[at];
        if (c == '\x1b' && at + 1 < output.size() && output[at + 1] == '[') {
            at += 2;
            while (at < output.size() && (output[at] < 0x40 || output[at] > 0x7E)) {
                ++at;
            }
        } else if (c == '\x1b' && at + 1 < output.size() && output[at + 1] == ']') {
            while (at < output.size() && output[at] != '\x07') {
                ++at;
            }
        } else if (c != '\r') {
            kept += c;
        }
    }

    return kept;
}

/// What the probe has printed is enough to go on.
using seen_t = std::function<bool(const std::string&)>;

/// The bytes the probe's "bytes" lines say it read, in hex, a space between each two, however
/// its reads split them.
std::string typed(const std::string_view seen) {
    std::string bytes;
    for (std::size_t at = seen.find("bytes "); at != std::string_view::npos;
         at = seen.find("bytes ", at)) {
        at += 6;
        const std::size_t end = seen.find('\n', at);
        if (end == std::string_view::npos) {
            break;
        }

        bytes += (bytes.empty() ? "" : " ") + std::string(seen.substr(at, end - at));
        at = end;
    }

    return bytes;
}

/// Has the probes this process starts run as `mode` says.
void probe_as(const std::string& mode) {
#ifdef _WIN32
    _putenv_s("FB_TERM_PROBE", mode.c_str());
#else
    setenv("FB_TERM_PROBE", mode.c_str(), 1);
#endif
}

/// Runs the probe as `mode` says, its stderr going where its stdout does, and gives what it
/// printed and its exit code.
std::pair<std::string, int> run_probe(const std::string& mode) {
    probe_as(mode);
    const std::string command = std::string("\"") + FB_TEST_TERM_PROBE + "\" 2>&1";
#ifdef _WIN32
    std::FILE* pipe = _popen(command.c_str(), "rb"); // NOLINT(bugprone-command-processor)
#else
    std::FILE* pipe = popen(command.c_str(), "r"); // NOLINT(bugprone-command-processor)
#endif
    REQUIRE(pipe != nullptr);
    std::string output;
    std::array<char, 256> chunk{};
    for (std::size_t count = 0; (count = std::fread(chunk.data(), 1, chunk.size(), pipe)) > 0;) {
        output.append(chunk.data(), count);
    }

#ifdef _WIN32
    const int code = _pclose(pipe);
#else
    const int status = pclose(pipe);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return { output, code };
}

#ifdef _WIN32
/// The term probe in a pseudoconsole of its own, as a terminal runs it: what's written to it is
/// typed, and what it prints is read as it comes.
class terminal_t {
public:
    terminal_t(const SHORT columns, const SHORT rows) {
        HANDLE input_read = nullptr;
        HANDLE output_write = nullptr;
        REQUIRE(CreatePipe(&input_read, &m_input, nullptr, 0) != 0);
        REQUIRE(CreatePipe(&m_output, &output_write, nullptr, 0) != 0);
        REQUIRE(CreatePseudoConsole({ .X = columns, .Y = rows }, input_read, output_write, 0,
                                    &m_console) == S_OK);
        // The pseudoconsole has its own copies of its ends.
        CloseHandle(input_read);
        CloseHandle(output_write);

        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        m_attributes.resize(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attributes.data());
        REQUIRE(InitializeProcThreadAttributeList(attributes, 1, 0, &size) != 0);
        REQUIRE(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                          m_console, sizeof(m_console), nullptr, nullptr) != 0);
        STARTUPINFOEXA startup{};
        startup.StartupInfo.cb = sizeof(STARTUPINFOEXA);
        startup.lpAttributeList = attributes;
        // No std handles of this process's, which ctest has in pipes, so the probe's stdin and
        // stdout are the pseudoconsole's.
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        probe_as("keys");
        std::string command = std::string("\"") + FB_TEST_TERM_PROBE + "\"";
        REQUIRE(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                               EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                               &m_process) != 0);
        m_reader = std::thread([this] { read(); });
    }

    terminal_t(const terminal_t&) = delete;
    terminal_t& operator=(const terminal_t&) = delete;

    ~terminal_t() {
        if (WaitForSingleObject(m_process.hProcess, 10000) != WAIT_OBJECT_0) {
            TerminateProcess(m_process.hProcess, 1);
        }

        // Closing the pseudoconsole ends its output, which ends the reader.
        ClosePseudoConsole(m_console);
        CloseHandle(m_input);
        m_reader.join();
        CloseHandle(m_output);
        CloseHandle(m_process.hProcess);
        CloseHandle(m_process.hThread);
        DeleteProcThreadAttributeList(
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attributes.data()));
    }

    /// Types `text`.
    void type(const std::string_view text) {
        DWORD written = 0;
        REQUIRE(WriteFile(m_input, text.data(), static_cast<DWORD>(text.size()), &written,
                          nullptr) != 0);
    }

    void resize(const SHORT columns, const SHORT rows) {
        REQUIRE(ResizePseudoConsole(m_console, { .X = columns, .Y = rows }) == S_OK);
    }

    /// Whether what the probe printed, without escape codes, is `enough` within ten seconds.
    bool shows(const seen_t& enough) {
        std::unique_lock lock(m_lock);
        return m_changed.wait_for(lock, std::chrono::seconds(10),
                                  [&] { return enough(plain(m_seen)); });
    }

    [[nodiscard]] std::string seen() {
        const std::scoped_lock lock(m_lock);
        return plain(m_seen);
    }

private:
    HPCON m_console = nullptr;
    HANDLE m_input = nullptr;
    HANDLE m_output = nullptr;
    PROCESS_INFORMATION m_process{};
    std::vector<std::byte> m_attributes;
    std::thread m_reader;
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::string m_seen;

    void read() {
        std::array<char, 4096> chunk{};
        DWORD count = 0;
        while (ReadFile(m_output, chunk.data(), static_cast<DWORD>(chunk.size()), &count,
                        nullptr) != 0 &&
               count > 0) {
            const std::scoped_lock lock(m_lock);
            m_seen.append(chunk.data(), count);
            m_changed.notify_all();
        }
    }
};
#else
/// The term probe in a pty of its own, as a terminal runs it: what's written to it is typed, and
/// what it prints is read as it comes.
class terminal_t {
public:
    terminal_t(const unsigned short columns, const unsigned short rows) {
        winsize size{};
        size.ws_col = columns;
        size.ws_row = rows;
        probe_as("keys");
        m_child = forkpty(&m_terminal, nullptr, nullptr, &size);
        REQUIRE(m_child >= 0);
        if (m_child == 0) {
            execl(FB_TEST_TERM_PROBE, FB_TEST_TERM_PROBE, nullptr);
            _exit(127);
        }
    }

    terminal_t(const terminal_t&) = delete;
    terminal_t& operator=(const terminal_t&) = delete;

    ~terminal_t() {
        int status = 0;
        if (waitpid(m_child, &status, WNOHANG) == 0) {
            kill(m_child, SIGKILL);
            waitpid(m_child, &status, 0);
        }

        close(m_terminal);
    }

    void type(const std::string_view text) {
        REQUIRE(write(m_terminal, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    }

    void resize(const unsigned short columns, const unsigned short rows) {
        winsize size{};
        size.ws_col = columns;
        size.ws_row = rows;
        REQUIRE(ioctl(m_terminal, TIOCSWINSZ, &size) == 0);
    }

    bool shows(const seen_t& enough) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!enough(plain(m_seen))) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }

            // select, since macOS's poll doesn't take a terminal.
            fd_set watched{};
            FD_ZERO(&watched);
            FD_SET(m_terminal, &watched);
            timeval wait{};
            wait.tv_sec = static_cast<decltype(wait.tv_sec)>(left.count() / 1000);
            wait.tv_usec = static_cast<decltype(wait.tv_usec)>(left.count() % 1000 * 1000);
            if (select(m_terminal + 1, &watched, nullptr, nullptr, &wait) <= 0) {
                continue;
            }

            std::array<char, 4096> chunk{};
            const ssize_t count = read(m_terminal, chunk.data(), chunk.size());
            if (count <= 0) {
                return enough(plain(m_seen));
            }

            m_seen.append(chunk.data(), static_cast<std::size_t>(count));
        }

        return true;
    }

    [[nodiscard]] std::string seen() const {
        return plain(m_seen);
    }

private:
    pid_t m_child = -1;
    int m_terminal = -1;
    std::string m_seen;
};
#endif

} // namespace

TEST_CASE("a terminal's colors are what its environment says", "[term]") {
    // Not a terminal, NO_COLOR with a value, or a dumb terminal: none.
    CHECK(fb::term::color_level(false, nullptr, "xterm-256color", "truecolor", false) == 0);
    CHECK(fb::term::color_level(false, nullptr, nullptr, nullptr, true) == 0);
    CHECK(fb::term::color_level(true, "1", "xterm-256color", "truecolor", false) == 0);
    CHECK(fb::term::color_level(true, "1", nullptr, nullptr, true) == 0);
    CHECK(fb::term::color_level(true, nullptr, "dumb", "truecolor", false) == 0);
    // An empty NO_COLOR is as if it weren't set.
    CHECK(fb::term::color_level(true, "", "xterm", nullptr, false) == 1);
    CHECK(fb::term::color_level(true, nullptr, "xterm", "truecolor", false) == 3);
    CHECK(fb::term::color_level(true, nullptr, "xterm", "24bit", false) == 3);
    CHECK(fb::term::color_level(true, nullptr, "xterm-256color", nullptr, false) == 2);
    CHECK(fb::term::color_level(true, nullptr, "screen-256color", "yes", false) == 2);
    CHECK(fb::term::color_level(true, nullptr, "xterm", nullptr, false) == 1);
    CHECK(fb::term::color_level(true, nullptr, nullptr, nullptr, false) == 1);
    // Windows' console shows 24-bit color, unless TERM says less.
    CHECK(fb::term::color_level(true, nullptr, nullptr, nullptr, true) == 3);
    CHECK(fb::term::color_level(true, nullptr, "xterm-256color", nullptr, true) == 2);
}

TEST_CASE("stdin that's a pipe gives what's come, waits as long as it's told, and ends", "[term]") {
    fb_class_t type = bytes_class();
    const fb_array_t* into = fb_array_new(&type, 1, 8);
#ifdef _WIN32
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    REQUIRE(CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    HANDLE before = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, read_end);
    const auto send = [&](const std::string_view text) {
        DWORD written = 0;
        REQUIRE(WriteFile(write_end, text.data(), static_cast<DWORD>(text.size()), &written,
                          nullptr) != 0);
    };
    const auto end = [&] { CloseHandle(write_end); };
#else
    std::array<int, 2> ends{};
    REQUIRE(pipe(ends.data()) == 0);
    const int before = dup(STDIN_FILENO);
    dup2(ends[0], STDIN_FILENO);
    const auto send = [&](const std::string_view text) {
        REQUIRE(write(ends[1], text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    };
    const auto end = [&] { close(ends[1]); };
#endif

    // A pipe isn't a terminal, so it has no raw mode.
    CHECK(fb_term_is_terminal(0) == 0);
    CHECK(fb_term_set_mode(1) == 0);
    const auto start = std::chrono::steady_clock::now();
    CHECK(fb_term_read(into, 30'000'000) == 0);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(30));
    send("abc");
    REQUIRE(fb_term_read(into, 1'000'000'000) == 3);
    CHECK(text_of(into, 3) == "abc");
    // Up to its room at a time, and the rest after.
    send("0123456789");
    REQUIRE(fb_term_read(into, -1) == 8);
    CHECK(text_of(into, 8) == "01234567");
    REQUIRE(fb_term_read(into, 0) == 2);
    CHECK(text_of(into, 2) == "89");
    end();
    CHECK(fb_term_read(into, -1) == -1);
    CHECK(fb_term_read(into, 0) == -1);

#ifdef _WIN32
    SetStdHandle(STD_INPUT_HANDLE, before);
    CloseHandle(read_end);
#else
    dup2(before, STDIN_FILENO);
    close(before);
    close(ends[0]);
#endif
    fb_array_free(into);
}

TEST_CASE("a panic's message shows once the screen is put back, which is put back once", "[term]") {
    const auto [output, code] = run_probe("panic");
    CHECK(output == "before\nLEAVEpanic: boom\n");
    CHECK(code == 101);
}

TEST_CASE("a terminal's keys come in raw mode as it sends them, and so does a resize", "[term]") {
    terminal_t terminal(80, 24);
    const auto expect = [&](const seen_t& enough) {
        const bool shown = terminal.shows(enough);
        INFO(terminal.seen());
        REQUIRE(shown);
    };
    const auto has = [](const std::string_view text) {
        return [text](const std::string& seen) { return seen.contains(text); };
    };
    // The bytes the keys typed, however the reads split them.
    const auto typing = [](const std::string_view bytes) {
        return [bytes](const std::string& seen) { return typed(seen).ends_with(bytes); };
    };
    expect(has("ready 80x24\n"));
    // Each key as it's typed, without its echo.
    terminal.type("a");
    expect(has("ready 80x24\nbytes 61\n"));
    terminal.type("\x1b[A");
    expect(typing("61 1b 5b 41"));
    terminal.resize(100, 30);
    expect(has("resize 100x30\n"));
#ifndef _WIN32
    // Ctrl+Z puts the terminal back while the program stops, and changes it again after, which
    // the program hears of as a resize. A program in a pty of its own doesn't stop.
    terminal.type("\x1a");
    expect(has("resize 100x30\nresize 100x30\n"));
    terminal.type("b");
    expect(typing("41 62"));
#endif
    terminal.type("q");
    expect(typing("71"));
    expect(has("done echo=1 lines=1\n"));
}
