// std.core's Value's text: a String in quotes, as JSON writes one, and bytes in hex; the steps
// of a path to a Value inside one; and what Load<T> reads a String as, a Bool, a number, or an
// Enum's member.

#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::string_view hex_digits = "0123456789abcdef";

fb_string_t* string_of(const std::string& text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

std::string_view view_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

/// Whether two names are the same, letter for letter, in either case, as ASCII has them.
bool same_name(const std::string_view a, const std::string_view b) {
    return std::ranges::equal(a, b, [](const char x, const char y) {
        const auto lower = [](const char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return lower(x) == lower(y);
    });
}

/// The bits of the member `text` names, as it is or else in another case, or nullopt.
std::optional<std::uint64_t> member_bits(const std::string_view text, const fb_array_t* names,
                                         const fb_array_t* values) {
    const auto* strings = static_cast<const fb_string_t* const*>(names->data);
    const auto* bits = static_cast<const std::uint64_t*>(values->data);
    for (std::int64_t i = 0; i < names->count; ++i) {
        if (view_of(strings[i]) == text) {
            return bits[i];
        }
    }

    for (std::int64_t i = 0; i < names->count; ++i) {
        if (same_name(view_of(strings[i]), text)) {
            return bits[i];
        }
    }

    return std::nullopt;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }

    return text;
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
        *static_cast<fb_string_t**>(fb_array_push(keys, sizeof(fb_string_t*), nullptr)) =
            fb_string_from_bytes(key.data(), static_cast<std::int64_t>(key.size()));
        *static_cast<std::int64_t*>(fb_array_push(indices, sizeof(std::int64_t), nullptr)) = index;
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

fb_string_t* fb_value_joined(const fb_array_t* parts) {
    const auto* strings = static_cast<const fb_string_t* const*>(parts->data);
    std::size_t size = 0;
    for (std::int64_t i = 0; i < parts->count; ++i) {
        size += static_cast<std::size_t>(strings[i]->length);
    }

    std::string text;
    text.reserve(size);
    for (std::int64_t i = 0; i < parts->count; ++i) {
        text.append(reinterpret_cast<const char*>(strings[i] + 1),
                    static_cast<std::size_t>(strings[i]->length));
    }

    return string_of(text);
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

std::int32_t fb_value_scalar(const fb_string_t* text, std::uint64_t* bits, double* real) {
    const std::string_view whole = view_of(text);
    if (same_name(whole, "true") || same_name(whole, "false")) {
        *bits = same_name(whole, "true") ? 1 : 0;
        return 1;
    }

    // A sign, then digits, is an integer, unless it's past a UInt64's or below an Int64's, and
    // then it's a Double. A '+' goes before a number too, though JSON has none, but not before
    // another sign.
    std::string_view number = whole;
    const bool plus = number.starts_with('+');
    if (plus) {
        number.remove_prefix(1);
    }

    if (number.empty() || (plus && (number.front() == '+' || number.front() == '-'))) {
        return 0;
    }

    const char* end = number.data() + number.size();
    const bool negative = number.starts_with('-');
    const std::string_view digits = negative ? number.substr(1) : number;
    if (!digits.empty() &&
        std::ranges::all_of(digits, [](const char c) { return c >= '0' && c <= '9'; })) {
        if (std::int64_t signed_value = 0;
            std::from_chars(number.data(), end, signed_value).ec == std::errc()) {
            *bits = static_cast<std::uint64_t>(signed_value);
            return 2;
        }

        if (std::uint64_t unsigned_value = 0;
            !negative && std::from_chars(number.data(), end, unsigned_value).ec == std::errc()) {
            *bits = unsigned_value;
            return 3;
        }
    }

    // A number past a Double's range is an infinity, or 0, as strtod reads it, which from_chars
    // leaves unread.
    double value = 0;
    const auto [stop, error] = std::from_chars(number.data(), end, value);
    if (stop != end || (error != std::errc() && error != std::errc::result_out_of_range)) {
        return 0;
    }

    *real = error == std::errc() ? value : std::strtod(std::string(number).c_str(), nullptr);
    return 4;
}

std::int32_t fb_value_enum(const fb_string_t* text, const fb_array_t* names,
                           const fb_array_t* values, const std::int32_t flags,
                           std::uint64_t* bits) {
    const std::string_view whole = view_of(text);
    if (flags == 0) {
        const std::optional<std::uint64_t> found = member_bits(trimmed(whole), names, values);
        if (found) {
            *bits = *found;
        }

        return found ? 1 : 0;
    }

    // A @Flags Enum's names are joined by '|', with spaces around it or not.
    std::uint64_t combined = 0;
    std::size_t start = 0;
    while (start <= whole.size()) {
        const std::size_t bar = std::min(whole.find('|', start), whole.size());
        const std::optional<std::uint64_t> found =
            member_bits(trimmed(whole.substr(start, bar - start)), names, values);
        if (!found) {
            return 0;
        }

        combined |= *found;
        start = bar + 1;
    }

    *bits = combined;
    return 1;
}

double fb_value_float(const float value) {
    std::array<char, 32> text{};
    const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
    double wide = value;
    if (error == std::errc()) {
        std::from_chars(text.data(), end, wide);
    }

    return wide;
}
}
