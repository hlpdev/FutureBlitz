#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fb {

/// What a piece of terminal text is, which picks its color.
enum class tone_t : std::uint8_t {
    PLAIN,
    BOLD,
    ERROR,   // bold red
    WARNING, // bold yellow
    NOTE,    // bold cyan
    SUCCESS, // bold green: status lines like "Compiling"
    GUTTER,  // bold blue: diagnostic gutters and view headers
    DIM,     // gray: positions and punctuation
    KEYWORD, // magenta
    NAME,    // cyan
    LITERAL, // green
    TYPE,    // yellow
};

/// Colors text with ANSI escape codes, or leaves it plain when `color` is false.
struct style_t {
    bool color = false;

    /// `text` colored for `tone`. Plain and empty text come back unchanged.
    [[nodiscard]] std::string paint(std::string_view text, tone_t tone) const;
};

} // namespace fb
