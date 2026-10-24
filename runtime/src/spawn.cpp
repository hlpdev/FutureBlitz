// What starting a child takes apart from the OS: process.cpp, over libuv, starts it with them, and
// the tests check them on every platform.

#include "fb/os_process.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Whether `c` starts or ends a quoted folder in a PATH.
bool is_quote(const char c) {
    return c == '"' || c == '\'';
}

/// Whether cmd.exe takes `c` as it is, outside quotes: what it reads as nothing else.
bool is_plain(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view(R"(#$*+-./:?@\_)").contains(c);
}

/// Adds `text` to a cmd.exe line, with every % turned to %%cd:~,%, which cmd.exe makes an empty
/// part of the current folder's name and a %, so no %VAR% after it expands.
void add_escaped(std::string& line, const std::string_view text) {
    for (const char c : text) {
        if (c == '%') {
            line += "%%cd:~,";
        }

        line += c;
    }
}

/// Adds `argument` to a batch file's cmd.exe line, as Rust's append_bat_arg does, or gives false
/// for one with a line break.
bool add_batch_argument(std::string& line, const std::string_view argument) {
    if (argument.contains('\n') || argument.contains('\r')) {
        return false;
    }

    // An empty one goes in quotes, so it's there, and so does one ending in a backslash, which
    // would otherwise take the quote of a batch file's "%~1" after it.
    bool quote = argument.empty() || argument.ends_with('\\');
    for (std::size_t i = 0; i < argument.size(); ++i) {
        const auto c = static_cast<unsigned char>(argument[i]);
        // UTF-8's C1 controls, U+0080 to U+009F, are 0xC2 and then 0x80 to 0x9F.
        const bool control = c < 0x20 || c == 0x7F ||
                             (c == 0xC2 && i + 1 < argument.size() &&
                              static_cast<unsigned char>(argument[i + 1]) <= 0x9F &&
                              static_cast<unsigned char>(argument[i + 1]) >= 0x80);
        if (control || (c < 0x80 && !is_plain(argument[i]))) {
            quote = true;
        }
    }

    if (quote) {
        line += '"';
    }

    // A quote is doubled, and the backslashes before it doubled too, as before the closing one.
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
        } else {
            if (c == '"') {
                line.append(backslashes, '\\');
                line += '"';
            } else if (c == '%') {
                line += "%%cd:~,";
            }

            backslashes = 0;
        }

        line += c;
    }

    if (quote) {
        line.append(backslashes, '\\');
        line += '"';
    }

    return true;
}

} // namespace

namespace fb::os {

bool is_batch_file(std::string_view path) {
    while (!path.empty() && (path.back() == '.' || path.back() == ' ')) {
        path.remove_suffix(1);
    }

    if (path.size() < 4) {
        return false;
    }

    std::string extension(path.substr(path.size() - 4));
    std::ranges::transform(extension, extension.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return extension == ".bat" || extension == ".cmd";
}

std::optional<std::string> batch_arguments(const std::string_view script,
                                           const std::span<const std::string> arguments) {
    if (script.contains('"') || script.ends_with('\\')) {
        return std::nullopt;
    }

    // cmd.exe runs what's after /c without its first and last quotes: the script's path in
    // quotes of its own, then the arguments.
    std::string line = "\"\"";
    add_escaped(line, script);
    line += '"';
    for (const std::string& argument : arguments) {
        line += ' ';
        if (!add_batch_argument(line, argument)) {
            return std::nullopt;
        }
    }

    line += '"';
    return line;
}

std::vector<std::string> path_entries(const std::string_view path) {
    std::vector<std::string> entries;
    std::size_t at = 0;
    while (at < path.size()) {
        // A folder in quotes can have a semicolon in it.
        std::size_t end = at;
        if (is_quote(path[at])) {
            end = std::min(path.find(path[at], at + 1), path.size());
        }

        end = std::min(path.find(';', end), path.size());
        std::string_view entry = path.substr(at, end - at);
        if (!entry.empty() && is_quote(entry.front())) {
            entry.remove_prefix(1);
        }

        if (!entry.empty() && is_quote(entry.back())) {
            entry.remove_suffix(1);
        }

        if (!entry.empty()) {
            entries.emplace_back(entry);
        }

        at = end + 1;
    }

    return entries;
}

void byte_queue_t::append(const std::string_view bytes) {
    m_bytes.append(bytes);
}

std::size_t byte_queue_t::take(const std::span<std::uint8_t> into) {
    const std::size_t count = std::min(size(), into.size());
    std::memcpy(into.data(), m_bytes.data() + m_taken, count);
    m_taken += count;
    // What's left moves to the front only once more has been taken than is left, so each byte
    // moves less often than bytes are taken.
    if (m_taken == m_bytes.size()) {
        m_bytes.clear();
        m_taken = 0;
    } else if (m_taken > m_bytes.size() / 2) {
        m_bytes.erase(0, m_taken);
        m_taken = 0;
    }

    return count;
}

} // namespace fb::os
