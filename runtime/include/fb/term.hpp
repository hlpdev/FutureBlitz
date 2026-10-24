#pragma once

#include "fb/runtime.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#ifndef _WIN32
#include <termios.h>
#endif

// std.term's functions in the runtime: what the console's streams are, its size and colors, and
// its input read a key at a time, over the OS's own calls: the console API with virtual terminal
// sequences on Windows, and termios and SIGWINCH elsewhere. A stream is 0 for stdin, 1 for
// stdout, and 2 for stderr. Whatever the program changes, the console gets back as the program
// ends, however it ends, and before a panic's or an uncaught error's message shows. Strings are
// only borrowed.

extern "C" {

/// 1 when `stream` is a terminal, a console on Windows, and 0 when it's a pipe, a file, or
/// closed.
std::int32_t fb_term_is_terminal(std::int32_t stream);
/// The size of the terminal that stdout, stderr, or stdin is, the first that's one, through
/// `columns` and `rows`, and 1, or 0 when none is.
std::int32_t fb_term_size(std::int32_t* columns, std::int32_t* rows);
/// 1 when `stream`, stdout or stderr, is a terminal that takes escape sequences, which Windows'
/// console is once it's asked to, or stdin is a terminal, and 0 when it isn't.
std::int32_t fb_term_start(std::int32_t stream);
/// The colors `stream` shows, as fb::term::color_level() works them out from the environment: 0
/// for none, 1 for the 16 basic ones, 2 for 256, and 3 for 24-bit color.
std::int32_t fb_term_colors(std::int32_t stream);
/// Sets how stdin reads, `mode` being 0 for as it was, or else 1 for each key as it's typed,
/// without echo, with Ctrl+C still ending the program, plus 2 for Ctrl+C read as a key, and 4
/// for the mouse's reports too. Gives 1, or 0 when stdin isn't a terminal.
std::int32_t fb_term_set_mode(std::int32_t mode);
/// Reads what's come from stdin into `into`, a UInt8[], at most its Count bytes, waiting for
/// `timeout` nanoseconds at most, or for as long as it takes when that's negative. Gives how many
/// bytes it read, 0 when none came in time, -1 at the end of the input, and -2, before any
/// more, when the terminal's size has changed. A key comes as a terminal sends it, Windows'
/// console's too, and so does the mouse.
std::int64_t fb_term_read(const fb_array_t* into, std::int64_t timeout);
/// What to write to `stream`, stdout or stderr, the one the screen was changed on, as the program
/// ends, or stops for a while, while it has the screen changed: `leave`, like leaving the
/// alternate screen and showing the cursor, and `enter`, which changes it again when the program
/// goes on after Ctrl+Z. Each is 255 bytes at most, and "" for nothing.
void fb_term_set_screen(const fb_string_t* enter, const fb_string_t* leave, std::int32_t stream);
}

namespace fb::term {

/// When a read that starts at `now` and waits `timeout` nanoseconds gives up, as fb_term_read()
/// works it out, or nullopt for a negative timeout, or one too long for the clock to reach,
/// which is as long as it takes.
[[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
deadline_of(std::chrono::steady_clock::time_point now, std::int64_t timeout);

#ifndef _WIN32
/// The termios fb_term_set_mode() gives stdin for `mode`, not 0, from what it had `before`: no
/// echo or lines, Enter as "\r", and Ctrl+S, Ctrl+Q, and Ctrl+\ as keys, and Ctrl+C too with 2.
[[nodiscard]] termios raw_mode(const termios& before, int mode);
#endif

/// What a terminal's environment says it shows, as fb_term_colors() reads it: none where it
/// isn't a terminal, NO_COLOR has a value, or TERM is "dumb"; 24-bit color where COLORTERM is
/// "truecolor" or "24bit", or on Windows' console; 256 where TERM has "256color" in it; and the
/// basic 16 otherwise. Any of the texts can be null, for a variable that isn't set.
[[nodiscard]] int color_level(bool terminal, const char* no_color, const char* term,
                              const char* color_term, bool windows);

#ifdef _WIN32
/// One of Windows' console's key records, as fb_term_read() gets them: its virtual key, its UTF-16
/// code unit, or 0, the control keys' state, how many times it repeats, and whether it went down.
struct key_record_t {
    std::uint16_t key;
    char16_t unit;
    std::uint32_t state;
    std::uint16_t repeat;
    bool down;
};

/// One of its mouse records: the buttons down, its event flags, where it is, and the control
/// keys' state.
struct mouse_record_t {
    std::uint32_t buttons;
    std::uint32_t flags;
    std::int16_t column;
    std::int16_t row;
    std::uint32_t state;
};

/// What fb_term_read() gives for the key records, in order, as a terminal would send them: where
/// `vt` says the console writes that itself, their characters; otherwise, xterm's sequences for
/// the keys without one, and Alt and Shift+Tab as xterm sends them.
[[nodiscard]] std::string key_bytes(std::span<const key_record_t> records, bool vt);

/// What fb_term_read() gives for the mouse records, in order, as SGR's reports, with the console's
/// window at the top of its buffer.
[[nodiscard]] std::string mouse_bytes(std::span<const mouse_record_t> records);
#endif

} // namespace fb::term
