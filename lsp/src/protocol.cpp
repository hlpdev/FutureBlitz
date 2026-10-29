#include "fb/lsp/protocol.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
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

            std::string body(*length, '\0');
            in.read(body.data(), static_cast<std::streamsize>(*length));
            if (std::cmp_not_equal(in.gcount(), *length)) {
                return std::nullopt;
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
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

std::string uri_of(const std::filesystem::path& path) {
    std::error_code error;
    const std::string text = to_utf8(std::filesystem::absolute(path, error).lexically_normal());
    std::string uri = "file://";
    // A Windows path, like C:/x, starts its URI's path with a '/'.
    if (!text.starts_with('/')) {
        uri += '/';
    }

    for (const char c : text) {
        if (kept(c)) {
            uri += c;
        } else {
            uri += std::format("%{:02X}", static_cast<unsigned char>(c));
        }
    }

    return uri;
}

std::optional<std::filesystem::path> path_of(const std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    if (lowered(std::string(uri.substr(0, scheme.size()))) != scheme) {
        return std::nullopt;
    }

    std::string text;
    const std::string_view rest = uri.substr(scheme.size());
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

std::string file_key(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
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
