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
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <condition_variable>
#include <mutex>
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

/// Runs the probe as `mode` says, its stderr going where its stdout does, or, with `only_errors`,
/// its stdout going nowhere, and gives what it printed and its exit code.
std::pair<std::string, int> run_probe(const std::string& mode, const bool only_errors = false) {
    probe_as(mode);
#ifdef _WIN32
    const char* nowhere = " 1>NUL";
#else
    const char* nowhere = " 1>/dev/null";
#endif
    const std::string command =
        std::string("\"") + FB_TEST_TERM_PROBE + "\" 2>&1" + (only_errors ? nowhere : "");
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
/// The term probe in a pseudoconsole of its own, as a terminal runs it, as `mode` says: what's
/// written to it is typed, and what it prints is read as it comes.
class terminal_t {
public:
    terminal_t(const SHORT columns, const SHORT rows, const std::string& mode = "keys") {
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
        probe_as(mode);
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
/// The term probe in a pty of its own, as a terminal runs it, as `mode` says: what's written to
/// it is typed, and what it prints is read as it comes.
class terminal_t {
public:
    terminal_t(const unsigned short columns, const unsigned short rows,
               const std::string& mode = "keys") {
        winsize size{};
        size.ws_col = columns;
        size.ws_row = rows;
        probe_as(mode);
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
    // Whether all of text went in, which another thread can ask without Catch's macros.
    const auto sent = [&](const std::string_view text) {
        DWORD written = 0;
        return WriteFile(write_end, text.data(), static_cast<DWORD>(text.size()), &written,
                         nullptr) != 0 &&
               written == text.size();
    };
    const auto end = [&] { CloseHandle(write_end); };
#else
    std::array<int, 2> ends{};
    REQUIRE(pipe(ends.data()) == 0);
    const int before = dup(STDIN_FILENO);
    dup2(ends[0], STDIN_FILENO);
    const auto sent = [&](const std::string_view text) {
        return write(ends[1], text.data(), text.size()) == static_cast<ssize_t>(text.size());
    };
    const auto end = [&] { close(ends[1]); };
#endif
    const auto send = [&](const std::string_view text) { REQUIRE(sent(text)); };

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
    // A timeout too long for the clock to reach waits as long as it takes.
    bool late_sent = false;
    std::thread late([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        late_sent = sent("z");
    });
    const std::int64_t late_count = fb_term_read(into, std::numeric_limits<std::int64_t>::max());
    late.join();
    CHECK(late_sent);
    REQUIRE(late_count == 1);
    CHECK(text_of(into, 1) == "z");
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

TEST_CASE("a read's deadline is its timeout from now, or none for one too long for the clock",
          "[term]") {
    const auto now = std::chrono::steady_clock::now();
    CHECK_FALSE(fb::term::deadline_of(now, -1).has_value());
    CHECK_FALSE(fb::term::deadline_of(now, std::numeric_limits<std::int64_t>::max()).has_value());
    const std::optional<std::chrono::steady_clock::time_point> soon =
        fb::term::deadline_of(now, 1'000'000);
    CHECK(soon.has_value());
    CHECK(std::chrono::duration_cast<std::chrono::nanoseconds>(soon.value_or(now) - now).count() ==
          1'000'000);
    const std::optional<std::chrono::steady_clock::time_point> at_once =
        fb::term::deadline_of(now, 0);
    CHECK(at_once.has_value());
    CHECK(at_once.value_or(now + std::chrono::seconds(1)) == now);
}

#ifndef _WIN32
TEST_CASE("raw mode reads each key as it's typed, Ctrl+\\ too, and Ctrl+C with 2", "[term]") {
    const auto flag = [](const auto value) { return static_cast<tcflag_t>(value); };
    termios before{};
    before.c_lflag = flag(ECHO) | flag(ICANON) | flag(IEXTEN) | flag(ISIG);
    before.c_iflag = flag(ICRNL) | flag(IXON);
    before.c_cc[VQUIT] = 0x1c;
    const termios raw = fb::term::raw_mode(before, 1);
    CHECK((raw.c_lflag & (flag(ECHO) | flag(ICANON) | flag(IEXTEN))) == 0);
    CHECK((raw.c_lflag & flag(ISIG)) != 0);
    CHECK((raw.c_iflag & (flag(ICRNL) | flag(IXON))) == 0);
    // Ctrl+\ is a key, not SIGQUIT, which would end the program with the terminal left raw.
    CHECK(raw.c_cc[VQUIT] == static_cast<cc_t>(_POSIX_VDISABLE));
    CHECK(raw.c_cc[VMIN] == 1);
    CHECK((fb::term::raw_mode(before, 3).c_lflag & flag(ISIG)) == 0);
}
#endif

TEST_CASE("a panic's message shows once the screen is put back, which is put back once", "[term]") {
    const auto [output, code] = run_probe("panic");
    CHECK(output == "before\nLEAVEpanic: boom\n");
    CHECK(code == 101);
}

TEST_CASE("what puts the screen back goes to the stream it was changed on", "[term]") {
    const auto [output, code] = run_probe("panic on stderr", true);
    CHECK(output == "LEAVEpanic: boom\n");
    CHECK(code == 101);
}

TEST_CASE("the terminal's size is the console's, wherever stdout and stderr go, and stdin's "
          "raw mode is its own",
          "[term]") {
    terminal_t terminal(80, 24, "streams");
    const bool shown = terminal.shows(
        [](const std::string& seen) { return seen.contains("streams 1 80x24 input 1 1\n"); });
    INFO(terminal.seen());
    CHECK(shown);
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
    // Ctrl+\ is a key, as Windows' console gives it, not SIGQUIT, which would end the program
    // with the terminal left raw.
    terminal.type("\x1c");
    expect(typing("1c"));
    terminal.type("q");
    expect(typing("71"));
    expect(has("done echo=1 lines=1\n"));
}

#ifdef _WIN32
namespace {

/// A key record that went down, as the console gives it.
fb::term::key_record_t key(const WORD virtual_key, const wchar_t unit, const DWORD state = 0,
                           const WORD repeat = 1, const bool down = true) {
    return {
        .key = virtual_key,
        .unit = static_cast<char16_t>(unit),
        .state = state,
        .repeat = repeat,
        .down = down,
    };
}

/// What the console gives for keys, with virtual terminal input on, or without it.
std::string keys(const std::initializer_list<fb::term::key_record_t> records,
                 const bool vt = false) {
    return fb::term::key_bytes(std::span(records.begin(), records.size()), vt);
}

/// A mouse record of the buttons down, its flags, where it is, and the control keys.
fb::term::mouse_record_t mouse(const DWORD buttons, const DWORD flags, const SHORT column,
                               const SHORT row, const DWORD state = 0) {
    return { .buttons = buttons, .flags = flags, .column = column, .row = row, .state = state };
}

std::string mice(const std::initializer_list<fb::term::mouse_record_t> records) {
    return fb::term::mouse_bytes(std::span(records.begin(), records.size()));
}

/// The wheel's turn in a mouse record's buttons, its high word.
DWORD wheel(const int turned) {
    return static_cast<DWORD>(static_cast<WORD>(static_cast<std::int16_t>(turned))) << 16U;
}

} // namespace

TEST_CASE("Windows' console's key and mouse records come as a terminal sends them", "[term]") {
    constexpr DWORD shift = SHIFT_PRESSED;
    constexpr DWORD alt = LEFT_ALT_PRESSED;
    constexpr DWORD control = LEFT_CTRL_PRESSED;
    // Where the console writes what a terminal sends, its characters are what come, and a key
    // without one, Shift alone too, gives nothing.
    CHECK(keys({ key(0, 0x1b), key(0, L'['), key(0, L'A'), key(VK_UP, 0), key(VK_SHIFT, 0, shift),
                 key(0, L'a', alt) },
               true) == "\x1b[Aa");
    // Otherwise, xterm's sequences, with the modifiers between.
    CHECK(keys({ key(VK_UP, 0), key(VK_DOWN, 0), key(VK_RIGHT, 0), key(VK_LEFT, 0) }) ==
          "\x1b[A\x1b[B\x1b[C\x1b[D");
    CHECK(keys({ key(VK_HOME, 0), key(VK_END, 0), key(VK_INSERT, 0), key(VK_DELETE, 0),
                 key(VK_PRIOR, 0), key(VK_NEXT, 0) }) ==
          "\x1b[H\x1b[F\x1b[2~\x1b[3~\x1b[5~\x1b[6~");
    CHECK(keys({ key(VK_F1, 0), key(VK_F4, 0), key(VK_F5, 0), key(VK_F6, 0), key(VK_F10, 0),
                 key(VK_F11, 0), key(VK_F12, 0) }) ==
          "\x1bOP\x1bOS\x1b[15~\x1b[17~\x1b[21~\x1b[23~\x1b[24~");
    CHECK(keys({ key(VK_LEFT, 0, control), key(VK_F1, 0, shift), key(VK_DELETE, 0, control | alt),
                 key(VK_UP, 0, shift | alt | control) }) == "\x1b[1;5D\x1b[1;2P\x1b[3;7~\x1b[1;8A");
    CHECK(keys({ key(VK_SHIFT, 0, shift), key(VK_CAPITAL, 0) }).empty());
    // A held key's repeats each come, and a key going up gives nothing.
    CHECK(keys({ key(VK_DOWN, 0, 0, 3), key(0, L'x', 0, 2), key(0, L'y', 0, 1, false) }) ==
          "\x1b[B\x1b[B\x1b[Bxx");
    // Alt before a character is an escape, but not AltGr, which is Ctrl and Alt; Shift+Tab is
    // xterm's.
    CHECK(keys({ key(0, L'a', alt), key(0, L'@', RIGHT_ALT_PRESSED | control),
                 key(VK_TAB, L'\t', shift), key(VK_TAB, L'\t') }) == "\x1b"
                                                                     "a@\x1b[Z\t");
    // A character past U+FFFF comes in two records, and a second half alone is nothing.
    CHECK(keys({ key(0, 0xD83D), key(0, 0xDE00), key(0, 0xDE00), key(0, L'\x4e2d') }) ==
          "\xf0\x9f\x98\x80\xe4\xb8\xad");
    // Ctrl+Space is NUL, which the console gives as no character.
    CHECK(keys({ key(VK_SPACE, 0, control) }) == std::string(1, '\0'));

    // The mouse, as SGR reports it: a press, a drag, and a release, with the buttons as the
    // records before them had them.
    CHECK(mice({ mouse(FROM_LEFT_1ST_BUTTON_PRESSED, 0, 4, 2),
                 mouse(FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED, 5, 2), mouse(0, 0, 5, 2) }) ==
          "\x1b[<0;5;3M\x1b[<32;6;3M\x1b[<0;6;3m");
    // A move with no button down isn't reported, nor what changes no button.
    CHECK(mice({ mouse(0, MOUSE_MOVED, 1, 1), mouse(0, 0, 1, 1) }).empty());
    CHECK(mice({ mouse(RIGHTMOST_BUTTON_PRESSED, 0, 0, 0),
                 mouse(RIGHTMOST_BUTTON_PRESSED | FROM_LEFT_2ND_BUTTON_PRESSED, 0, 0, 0),
                 mouse(FROM_LEFT_2ND_BUTTON_PRESSED, 0, 0, 0), mouse(0, 0, 0, 0) }) ==
          "\x1b[<2;1;1M\x1b[<1;1;1M\x1b[<2;1;1m\x1b[<1;1;1m");
    // A double click's second press is a press.
    CHECK(mice({ mouse(FROM_LEFT_1ST_BUTTON_PRESSED, 0, 2, 2), mouse(0, 0, 2, 2),
                 mouse(FROM_LEFT_1ST_BUTTON_PRESSED, DOUBLE_CLICK, 2, 2) }) ==
          "\x1b[<0;3;3M\x1b[<0;3;3m\x1b[<0;3;3M");
    // The wheel's turns, up, down, right, and left, and the control keys held.
    CHECK(mice({ mouse(wheel(120), MOUSE_WHEELED, 0, 0), mouse(wheel(-120), MOUSE_WHEELED, 0, 0),
                 mouse(wheel(120), MOUSE_HWHEELED, 0, 0),
                 mouse(wheel(-120), MOUSE_HWHEELED, 0, 0) }) ==
          "\x1b[<64;1;1M\x1b[<65;1;1M\x1b[<67;1;1M\x1b[<66;1;1M");
    CHECK(mice({ mouse(FROM_LEFT_1ST_BUTTON_PRESSED, 0, 9, 9, shift | control),
                 mouse(0, 0, 9, 9, alt) }) == "\x1b[<20;10;10M\x1b[<8;10;10m");
}
#endif
