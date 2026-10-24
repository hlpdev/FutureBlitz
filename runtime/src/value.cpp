// std.core's Value's text: a String in quotes, as JSON writes one, and bytes in hex; and the
// steps of a path to a Value inside one.

#include "fb/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::string_view hex_digits = "0123456789abcdef";

fb_string_t* string_of(const std::string& text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

} // namespace

extern "C" {

fb_string_t* fb_value_quoted(const fb_string_t* text) {
    const std::string_view bytes(reinterpret_cast<const char*>(text + 1),
                                 static_cast<std::size_t>(text->length));
    std::string quoted = "\"";
    quoted.reserve(bytes.size() + 2);
    for (const char c : bytes) {
        switch (c) {
        case '"':
            quoted += "\\\"";
            break;
        case '\\':
            quoted += "\\\\";
            break;
        case '\b':
            quoted += "\\b";
            break;
        case '\f':
            quoted += "\\f";
            break;
        case '\n':
            quoted += "\\n";
            break;
        case '\r':
            quoted += "\\r";
            break;
        case '\t':
            quoted += "\\t";
            break;
        default:
            // JSON escapes the rest of the control characters by their code; everything else,
            // UTF-8's multibyte characters too, is as it is.
            if (static_cast<unsigned char>(c) < 0x20U) {
                const auto code = static_cast<unsigned char>(c);
                quoted += "\\u00";
                quoted += hex_digits[code >> 4U];
                quoted += hex_digits[code & 15U];
            } else {
                quoted += c;
            }
        }
    }

    quoted += '"';
    return string_of(quoted);
}

std::int32_t fb_value_path(const fb_string_t* path, fb_array_t* keys, fb_array_t* indices) {
    const std::string_view text(reinterpret_cast<const char*>(path + 1),
                                static_cast<std::size_t>(path->length));
    const auto add = [&](const std::string_view key, const std::int64_t index) {
        *static_cast<fb_string_t**>(fb_array_push(keys, sizeof(fb_string_t*))) =
            fb_string_from_bytes(key.data(), static_cast<std::int64_t>(key.size()));
        *static_cast<std::int64_t*>(fb_array_push(indices, sizeof(std::int64_t))) = index;
    };
    std::size_t at = 0;
    while (at < text.size()) {
        if (text[at] == '[') {
            const std::size_t close = text.find(']', at);
            if (close == std::string_view::npos) {
                return 0;
            }

            const std::string_view digits = text.substr(at + 1, close - at - 1);
            std::int64_t index = 0;
            const auto [end, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (digits.empty() || digits.front() == '-' || error != std::errc() ||
                end != digits.data() + digits.size()) {
                return 0;
            }

            add({}, index);
            at = close + 1;
            continue;
        }

        // A key: the first one as it is, and each after it after a dot.
        if (at > 0 && text[at] != '.') {
            return 0;
        }

        const std::size_t start = at > 0 ? at + 1 : 0;
        const std::size_t end = std::min(text.find_first_of(".[]", start), text.size());
        if (end == start || (end < text.size() && text[end] == ']')) {
            return 0;
        }

        add(text.substr(start, end - start), -1);
        at = end;
    }

    return 1;
}

fb_string_t* fb_value_hex(const fb_array_t* bytes) {
    const auto* data = static_cast<const std::uint8_t*>(bytes->data);
    std::string text;
    text.reserve(static_cast<std::size_t>(bytes->count) * 2);
    for (std::int64_t i = 0; i < bytes->count; ++i) {
        text += hex_digits[data[i] >> 4U];
        text += hex_digits[data[i] & 15U];
    }

    return string_of(text);
}
}
