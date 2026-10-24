#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

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
/// console is once it's asked to, and 0 when it isn't.
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
/// What to write to stdout as the program ends, or stops for a while, while it has the screen
/// changed: `leave`, like leaving the alternate screen and showing the cursor, and `enter`, which
/// changes it again when the program goes on after Ctrl+Z. Each is 255 bytes at most, and ""
/// for nothing.
void fb_term_set_screen(const fb_string_t* enter, const fb_string_t* leave);
}

namespace fb::term {

/// What a terminal's environment says it shows, as fb_term_colors() reads it: none where it
/// isn't a terminal, NO_COLOR has a value, or TERM is "dumb"; 24-bit color where COLORTERM is
/// "truecolor" or "24bit", or on Windows' console; 256 where TERM has "256color" in it; and the
/// basic 16 otherwise. Any of the texts can be null, for a variable that isn't set.
[[nodiscard]] int color_level(bool terminal, const char* no_color, const char* term,
                              const char* color_term, bool windows);

} // namespace fb::term
