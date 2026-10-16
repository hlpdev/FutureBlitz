#pragma once

#include "fb/options.hpp"

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
/// "\r\n". Call it before printing anything. Does nothing on other platforms.
void prepare_terminal();

/// False when the stream goes to a pipe or a file.
[[nodiscard]] bool is_terminal(stream_t stream);

/// Whether to color what goes to `stream`. AUTO means color in a terminal, unless NO_COLOR is set.
[[nodiscard]] bool use_color(color_t choice, stream_t stream);

} // namespace fb
