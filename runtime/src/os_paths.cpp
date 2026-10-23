// The paths std.os's folders are made from, apart from the OS: os.cpp, over libuv, gives them
// what the OS says, and the tests what they choose, on every platform.

#include "fb/os.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace fb::os {

bool is_url(const std::string_view target) {
    const std::size_t colon = target.find(':');
    if (colon == std::string_view::npos || colon < 2) {
        return false;
    }

    const auto letter = [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    const auto digit = [](const char c) { return c >= '0' && c <= '9'; };
    return letter(target[0]) && std::ranges::all_of(target.substr(1, colon - 1), [&](const char c) {
               return letter(c) || digit(c) || c == '+' || c == '-' || c == '.';
           });
}

std::optional<std::string> xdg_folder(const std::optional<std::string>& value,
                                      const std::optional<std::string>& home,
                                      const std::string_view fallback) {
    if (value && value->starts_with('/')) {
        return value;
    }

    return home ? std::optional(*home + "/" + std::string(fallback)) : std::nullopt;
}

std::optional<std::string> user_dirs_documents(const std::string_view text,
                                               const std::string_view home) {
    constexpr std::string_view key = "XDG_DOCUMENTS_DIR=\"";
    std::optional<std::string> found;
    for (const auto part : std::views::split(text, '\n')) {
        std::string_view line(part.begin(), part.end());
        const std::size_t end = line.rfind('"');
        if (!line.starts_with(key) || end == std::string_view::npos || end < key.size()) {
            continue;
        }

        line = line.substr(key.size(), end - key.size());
        if (line == "$HOME" || line.starts_with("$HOME/")) {
            found = std::string(home) + std::string(line.substr(5));
        } else if (line.starts_with('/')) {
            found = std::string(line);
        }
    }

    return found;
}

} // namespace fb::os
