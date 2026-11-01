#pragma once

#include "fb/options.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fb {

enum class stream_t : std::uint8_t {
    STANDARD_OUTPUT,
    STANDARD_ERROR,
};

/// The arguments after the program's name, as UTF-8 on every platform.
[[nodiscard]] std::vector<std::string> command_line(int argc, char** argv);

/// Turns on ANSI colors and UTF-8 output in Windows consoles, and keeps "\n" from becoming
/// "\r\n". Call it before printing anything. The console, which the shell shares, gets its code
/// page and modes back as the process exits. Does nothing on other platforms.
void prepare_terminal();

/// Ends the process with `code` at once, as std::_Exit does, without waiting for its threads or
/// running its destructors, after flushing stdout and stderr and giving the console back what
/// prepare_terminal() changed.
[[noreturn]] void end_process(int code);

/// False when the stream goes to a pipe or a file.
[[nodiscard]] bool is_terminal(stream_t stream);

/// Whether to color what goes to `stream`. AUTO means color in a terminal, unless NO_COLOR is set.
[[nodiscard]] bool use_color(color_t choice, stream_t stream);

/// What diagnostics' columns count: UTF-16 code units when FB_COLUMNS is "utf-16", as an editor's
/// task asks for, else code points.
[[nodiscard]] column_unit_t diagnostic_columns();

} // namespace fb
