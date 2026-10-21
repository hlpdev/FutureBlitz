#include "fb/style.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fb {
namespace {

/// The ANSI SGR parameters for `tone`, like "1;31" for bold red.
std::string_view code(const tone_t tone) {
    switch (tone) {
    case tone_t::PLAIN:
        return "";
    case tone_t::BOLD:
        return "1";
    case tone_t::ERROR:
        return "1;31";
    case tone_t::WARNING:
        return "1;33";
    case tone_t::NOTE:
        return "1;36";
    case tone_t::SUCCESS:
        return "1;32";
    case tone_t::GUTTER:
        return "1;34";
    case tone_t::DIM:
        return "90";
    case tone_t::KEYWORD:
        return "35";
    case tone_t::NAME:
        return "36";
    case tone_t::LITERAL:
        return "32";
    case tone_t::TYPE:
        return "33";
    }

    std::unreachable();
}

} // namespace

std::string style_t::paint(const std::string_view text, const tone_t tone) const {
    if (!color || tone == tone_t::PLAIN || text.empty()) {
        return std::string(text);
    }

    return std::format("\x1b[{}m{}\x1b[0m", code(tone), text);
}

} // namespace fb
