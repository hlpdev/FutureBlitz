// For std.term's runtime tests, a program that uses the console as std.term does, as its
// FB_TERM_PROBE environment variable says. "keys" puts stdin in raw mode, with the mouse's
// reports, prints "ready <columns>x<rows>", then a line for each read, "bytes" and the bytes in
// hex, or "resize <columns>x<rows>", until a "q" comes or the input ends, and prints "done" once
// stdin is back as it was, with whether it echoes and reads lines, "echo=1 lines=1". "panic"
// keeps "LEAVE" for the console to write if the program ends, prints "before", and panics.

#include "fb/runtime.hpp"
#include "fb/term.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

/// fb_term_set_mode's modes: each key as it's typed, and the mouse's reports.
constexpr std::int32_t raw_keys = 1;
constexpr std::int32_t mouse_reports = 4;

/// A String the program owns, as a literal is.
struct literal_t {
    fb_string_t header;
    std::array<char, 16> bytes;
};

literal_t literal(const std::string_view text) {
    literal_t made{};
    made.header.refcount = -1;
    made.header.length = static_cast<std::int64_t>(text.size());
    text.copy(made.bytes.data(), made.bytes.size());
    return made;
}

void free_bytes(fb_object_t* object) {
    fb_array_free(reinterpret_cast<fb_array_t*>(object));
}

void visit_nothing(const fb_object_t* /*object*/, fb_visitor_t /*visitor*/, void* /*context*/) {}

/// Writes a line, flushed, so a test sees it as soon as it's written.
void say(const std::string& line) {
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

int keys() {
    fb_class_t type{};
    type.name = "UInt8[]";
    type.size = sizeof(fb_array_t);
    type.release = free_bytes;
    type.visit = visit_nothing;
    const fb_array_t* bytes = fb_array_new(&type, 1, 256);
    std::int32_t columns = 0;
    std::int32_t rows = 0;
    fb_term_size(&columns, &rows);
    fb_term_set_mode(raw_keys + mouse_reports);
    say(std::format("ready {}x{}", columns, rows));
    bool quit = false;
    while (!quit) {
        const std::int64_t count = fb_term_read(bytes, 20'000'000'000);
        if (count == -2) {
            fb_term_size(&columns, &rows);
            say(std::format("resize {}x{}", columns, rows));
            continue;
        }

        if (count <= 0) {
            say(std::format("ended {}", count));
            break;
        }

        std::string line = "bytes";
        for (std::int64_t i = 0; i < count; ++i) {
            const auto byte = static_cast<const unsigned char*>(bytes->data)[i];
            line += std::format(" {:02x}", byte);
            quit = quit || byte == 'q';
        }

        say(line);
    }

    fb_array_free(bytes);
    fb_term_set_mode(0);
#ifdef _WIN32
    DWORD now = 0;
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &now);
    const bool echo = (now & static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0;
    const bool lines = (now & static_cast<DWORD>(ENABLE_LINE_INPUT)) != 0;
#else
    termios now{};
    tcgetattr(STDIN_FILENO, &now);
    const bool echo = (now.c_lflag & static_cast<tcflag_t>(ECHO)) != 0;
    const bool lines = (now.c_lflag & static_cast<tcflag_t>(ICANON)) != 0;
#endif
    say(std::format("done echo={} lines={}", echo ? 1 : 0, lines ? 1 : 0));
    return 0;
}

[[noreturn]] void panic() {
    const literal_t enter = literal("");
    const literal_t leave = literal("LEAVE");
    fb_term_set_screen(&enter.header, &leave.header);
    std::fputs("before\n", stdout);
    fb_panic("boom", nullptr);
}

} // namespace

int main() {
    try {
        fb_start();
        const char* mode = std::getenv("FB_TERM_PROBE");
        const std::string_view given = mode == nullptr ? "" : mode;
        if (given == "keys") {
            return keys();
        }

        if (given == "panic") {
            panic();
        }

        std::fputs("FB_TERM_PROBE is keys or panic\n", stderr);
        return 2;
    } catch (...) {
        std::fputs("the term probe failed\n", stderr);
        return 1;
    }
}
