#include "fb/lsp/protocol.hpp"

#include "fb/disk.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace fb::lsp {
namespace {

/// `text` with ASCII letters in lower case.
std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return text;
}

/// The value of a hexadecimal digit, or nullopt for another character.
std::optional<int> hex_value(const char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return std::nullopt;
}

/// Whether a URI's path holds `c` as it is: the unreserved characters, '/', and ':'.
bool kept(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
}

/// A Windows share's path without the two separators it starts with, as server/share/x. MinGW's
/// std::filesystem has no root name for one: absolute() puts it on the current drive, and
/// lexically_normal() makes it a path from the drive's root.
std::optional<std::string> share_of(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto is_separator = [](const wchar_t c) { return c == L'/' || c == L'\\'; };
    const std::wstring_view native = path.native();
    if (native.size() > 2 && is_separator(native[0]) && is_separator(native[1]) &&
        !is_separator(native[2])) {
        return to_utf8(std::filesystem::path(native.substr(2)).lexically_normal());
    }
#else
    static_cast<void>(path);
#endif
    return std::nullopt;
}

/// A URI's path, escaped.
std::string escaped(const std::string_view text) {
    std::string uri;
    for (const char c : text) {
        if (kept(c)) {
            uri += c;
        } else {
            uri += std::format("%{:02X}", static_cast<unsigned char>(c));
        }
    }

    return uri;
}

} // namespace

std::optional<std::string> read_message(std::istream& in) {
    std::optional<std::size_t> length;
    std::string line;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }

        // A blank line ends the header.
        if (line.empty()) {
            if (!length) {
                return std::nullopt;
            }

            // More than any document, which would otherwise take all the memory there is, or
            // throw on the reader's thread: the stream's lost its place.
            if (*length > most_message) {
                return std::nullopt;
            }

            // It grows as its bytes come, so a length that's wrong takes no memory for them.
            std::string body;
            while (body.size() < *length) {
                const std::size_t size = std::min(most_read, *length - body.size());
                const std::size_t at = body.size();
                body.resize(at + size);
                in.read(body.data() + at, static_cast<std::streamsize>(size));
                if (std::cmp_not_equal(in.gcount(), size)) {
                    return std::nullopt;
                }
            }

            return body;
        }

        constexpr std::string_view name = "content-length:";
        if (lowered(line.substr(0, std::min(line.size(), name.size()))) == name) {
            std::string_view value = std::string_view(line).substr(name.size());
            while (value.starts_with(' ')) {
                value.remove_prefix(1);
            }

            std::size_t parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error == std::errc{} && end == value.data() + value.size()) {
                length = parsed;
            }
        }
    }

    return std::nullopt;
}

void write_message(std::ostream& out, const std::string_view body) {
    // In one piece, the header with its body.
    std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    frame += body;
    out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
    out.flush();
}

std::string uri_of(const std::filesystem::path& path) {
    // A share's server is the URI's authority, as VS Code has it.
    if (const std::optional<std::string> share = share_of(path)) {
        return "file://" + escaped(*share);
    }

    std::error_code error;
    const std::string text = to_utf8(std::filesystem::absolute(path, error).lexically_normal());
    // A Windows path, like C:/x, starts its URI's path with a '/'.
    return std::string(text.starts_with('/') ? "file://" : "file:///") + escaped(text);
}

std::optional<std::filesystem::path> path_of(const std::string_view uri) {
    constexpr std::string_view scheme = "file:";
    if (lowered(std::string(uri.substr(0, scheme.size()))) != scheme) {
        return std::nullopt;
    }

    // With an authority, or without one, as file:/x is the same as file:///x, and
    // file://localhost/x is too, as localhost is this machine.
    std::string_view rest = uri.substr(scheme.size());
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        constexpr std::string_view here = "localhost";
        const std::string_view authority = rest.substr(0, rest.find('/'));
        if (lowered(std::string(authority)) == here) {
            rest.remove_prefix(authority.size());
        }
    } else if (!rest.starts_with('/')) {
        return std::nullopt;
    }

    if (rest.empty()) {
        return std::nullopt;
    }

    std::string text;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const std::optional<int> high = hex_value(rest[i + 1]);
            const std::optional<int> low = hex_value(rest[i + 2]);
            if (high && low) {
                text += static_cast<char>((*high * 16) + *low);
                i += 2;
                continue;
            }
        }

        text += rest[i];
    }

    // "file:///C:/x" is C:/x, and "file://server/share" is a Windows share's \\server\share.
    if (text.size() >= 3 && text[0] == '/' && text[2] == ':' &&
        ((text[1] >= 'a' && text[1] <= 'z') || (text[1] >= 'A' && text[1] <= 'Z'))) {
        text.erase(0, 1);
    } else if (!text.starts_with('/')) {
        text = "//" + text;
    }

    return path_from_utf8(text);
}

namespace {

/// What file_key() asked the disk, until forget_file_keys().
disk_cache_t& key_answers() {
    static disk_cache_t answers;
    return answers;
}

/// The keys file_key() gave, by the absolute paths it was given, until forget_file_keys(), and
/// how many times that was, as a key worked out before it isn't kept.
struct known_keys_t {
    std::mutex mutex;
    std::unordered_map<std::filesystem::path::string_type, std::string> keys;
    std::uint64_t forgotten = 0;
};

known_keys_t& known_keys() {
    static known_keys_t known;
    return known;
}

/// A file's key, as the disk says where it is.
std::string key_from_disk(const std::filesystem::path& path) {
    const disk_scope_t scope(&key_answers());
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    std::filesystem::path canonical = disk::weakly_canonical(absolute, error);
    if (error) {
        canonical = absolute.lexically_normal();
    }

    const std::string text = to_utf8(canonical);
#if defined(_WIN32) || defined(__APPLE__)
    return lowered(text);
#else
    return text;
#endif
}

} // namespace

std::string file_key(const std::filesystem::path& path) {
    // Working it out asks the disk, slowly on Windows, and the LSP asks for a file's key often,
    // each module's and its imports' after each check.
    known_keys_t& known = known_keys();
    std::uint64_t asked_at = 0;
    if (path.is_absolute()) {
        const std::scoped_lock lock(known.mutex);
        if (const auto found = known.keys.find(path.native()); found != known.keys.end()) {
            return found->second;
        }

        asked_at = known.forgotten;
    }

    std::string key = key_from_disk(path);
    if (path.is_absolute()) {
        const std::scoped_lock lock(known.mutex);
        if (known.forgotten == asked_at) {
            known.keys.try_emplace(path.native(), key);
        }
    }

    return key;
}

void forget_file_keys() {
    // The disk's answers first: a key worked out from them before the keys are forgotten isn't
    // kept, and one worked out after is from new ones.
    key_answers().clear();
    known_keys_t& known = known_keys();
    const std::scoped_lock lock(known.mutex);
    known.keys.clear();
    ++known.forgotten;
}

json_t positions_t::position(const source_file_t& file, const std::uint32_t offset) const {
    const location_t location = file.location_of(offset, unit);
    return { { "line", location.line }, { "character", location.column } };
}

json_t positions_t::range(const source_file_t& file, const span_t span) const {
    return { { "start", position(file, span.begin) }, { "end", position(file, span.end) } };
}

std::uint32_t positions_t::offset(const source_file_t& file, const json_t& position) const {
    const auto number = [&](const char* name) -> std::uint32_t {
        const auto found = position.find(name);
        if (found == position.end() || !found->is_number_integer() ||
            found->get<std::int64_t>() < 0) {
            return 0;
        }

        return static_cast<std::uint32_t>(
            std::min<std::int64_t>(found->get<std::int64_t>(), INT32_MAX));
    };
    return file.offset_of({ .line = number("line"), .column = number("character") }, unit);
}

std::uint32_t positions_t::length(const std::string_view text) const {
    if (unit == column_unit_t::UTF8) {
        return static_cast<std::uint32_t>(text.size());
    }

    std::uint32_t count = 0;
    for (std::size_t at = 0; at < text.size();) {
        const decoded_t decoded = decode_utf8(text, at);
        count += unit == column_unit_t::UTF16 && decoded.code_point > 0xFFFF ? 2 : 1;
        at += decoded.length;
    }

    return count;
}

} // namespace fb::lsp
