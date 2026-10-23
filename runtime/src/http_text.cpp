// std.net.http's text that needs no libcurl: header names and values, percent-encoding for
// forms, and a body's bytes as a String, in the runtime's own archive, so a program that only
// makes Headers or a Body links none of libcurl.

#include "fb/http.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

std::string_view view_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

char lower(const char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_token_character(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("!#$%&'*+-.^_`|~").contains(c);
}

} // namespace

extern "C" {

fb_string_t* fb_string_from_utf8(const fb_array_t* bytes) {
    return fb_string_from_bytes(static_cast<const char*>(bytes->data), bytes->count);
}

std::int32_t fb_headers_same_name(const fb_string_t* a, const fb_string_t* b) {
    const std::string_view x = view_of(a);
    const std::string_view y = view_of(b);
    return x.size() == y.size() &&
                   std::ranges::equal(
                       x, y, [](const char p, const char q) { return lower(p) == lower(q); })
               ? 1
               : 0;
}

std::int32_t fb_headers_is_name(const fb_string_t* name) {
    const std::string_view text = view_of(name);
    return !text.empty() && std::ranges::all_of(text, is_token_character) ? 1 : 0;
}

std::int32_t fb_headers_is_value(const fb_string_t* value) {
    // RFC 9110's field values: no control characters but tabs.
    return std::ranges::none_of(view_of(value),
                                [](const char c) {
                                    const auto code = static_cast<unsigned char>(c);
                                    return (code < 0x20 && code != '\t') || code == 0x7F;
                                })
               ? 1
               : 0;
}

fb_string_t* fb_percent_encode(const fb_string_t* text) {
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::string encoded;
    for (const char c : view_of(text)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            encoded += c;
        } else {
            const auto code = static_cast<unsigned char>(c);
            encoded += '%';
            encoded += digits[code >> 4U];
            encoded += digits[code & 15U];
        }
    }

    return fb_string_from_bytes(encoded.data(), static_cast<std::int64_t>(encoded.size()));
}
}
