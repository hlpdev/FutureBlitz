#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// What std.text's String Methods, StringBuilder, and encodings need from a String's bytes. Each
// takes byte positions that std has checked: inside the String, and between its characters.

using fb::runtime::decode_utf8;
using fb::runtime::decoded_t;
using fb::runtime::new_string;
using fb::runtime::text_of;

namespace {

/// The elements of a UInt8[], from `start`, `count` of them.
std::string_view bytes_in(const fb_array_t* bytes, const std::int64_t start,
                          const std::int64_t count) {
    return { static_cast<const char*>(bytes->data) + start, static_cast<std::size_t>(count) };
}

} // namespace

extern "C" {

std::int64_t fb_text_length(const fb_string_t* text) {
    return text->length;
}

std::uint8_t fb_text_byte(const fb_string_t* text, const std::int64_t at) {
    return static_cast<std::uint8_t>(text_of(text)[static_cast<std::size_t>(at)]);
}

std::uint32_t fb_text_char_at(const fb_string_t* text, const std::int64_t at) {
    return decode_utf8(text_of(text), static_cast<std::size_t>(at)).point;
}

fb_string_t* fb_text_slice(const fb_string_t* text, const std::int64_t start,
                           const std::int64_t finish) {
    return new_string(text_of(text).substr(static_cast<std::size_t>(start),
                                           static_cast<std::size_t>(finish - start)));
}

std::int64_t fb_text_find(const fb_string_t* text, const fb_string_t* part,
                          const std::int64_t from) {
    const std::size_t found = text_of(text).find(text_of(part), static_cast<std::size_t>(from));
    return found == std::string_view::npos ? -1 : static_cast<std::int64_t>(found);
}

std::int64_t fb_text_find_last(const fb_string_t* text, const fb_string_t* part,
                               const std::int64_t before) {
    // The last one that ends at `before` or sooner starts no later than `before` less its length.
    const std::string_view whole = text_of(text);
    const std::string_view wanted = text_of(part);
    if (static_cast<std::size_t>(before) < wanted.size()) {
        return -1;
    }

    const std::size_t found = whole.substr(0, static_cast<std::size_t>(before)).rfind(wanted);
    return found == std::string_view::npos ? -1 : static_cast<std::int64_t>(found);
}

std::int32_t fb_text_matches_at(const fb_string_t* text, const fb_string_t* part,
                                const std::int64_t at) {
    const std::string_view whole = text_of(text);
    const std::string_view wanted = text_of(part);
    const auto start = static_cast<std::size_t>(at);
    return start <= whole.size() && wanted.size() <= whole.size() - start &&
                   whole.substr(start, wanted.size()) == wanted
               ? 1
               : 0;
}

std::int64_t fb_text_char_count(const fb_string_t* text, const std::int64_t start,
                                const std::int64_t finish) {
    // A character's bytes after its first are 10xxxxxx, so the rest start one each.
    const std::string_view bytes = text_of(text).substr(static_cast<std::size_t>(start),
                                                        static_cast<std::size_t>(finish - start));
    return std::ranges::count_if(
        bytes, [](const char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U; });
}

fb_string_t* fb_text_from_utf8(const fb_array_t* bytes, const std::int64_t start,
                               const std::int64_t count) {
    // Null unless every byte is part of a well-formed character. A U+FFFD that's written there
    // decodes to 3 bytes, unlike a byte that isn't UTF-8.
    const std::string_view text = bytes_in(bytes, start, count);
    for (std::size_t i = 0; i < text.size();) {
        const decoded_t decoded = decode_utf8(text, i);
        if (decoded.point == 0xFFFD && decoded.length == 1) {
            return nullptr;
        }

        i += decoded.length;
    }

    return new_string(text);
}

void fb_text_append(fb_array_t* into, const fb_string_t* text) {
    for (const char byte : text_of(text)) {
        *static_cast<char*>(fb_array_push(into, 1, nullptr)) = byte;
    }
}

fb_string_t* fb_text_join(const fb_array_t* parts, const fb_string_t* separator) {
    const auto* const* texts = static_cast<const fb_string_t* const*>(parts->data);
    const std::string_view between = text_of(separator);
    std::string joined;
    for (std::int64_t i = 0; i < parts->count; ++i) {
        if (i > 0) {
            joined += between;
        }

        joined += text_of(texts[i]);
    }

    return new_string(joined);
}

fb_string_t* fb_text_repeat(const fb_string_t* text, const std::int64_t count) {
    // Nothing repeated is "", however many times.
    const std::string_view once = text_of(text);
    if (once.empty() || count <= 0) {
        return new_string({});
    }

    // A String past the most one holds panics before its bytes are asked for.
    if (static_cast<std::uint64_t>(count) > INT32_MAX / once.size()) {
        fb_panic("a String holds at most 2147483647 bytes", nullptr);
    }

    std::string repeated;
    repeated.reserve(once.size() * static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        repeated += once;
    }

    return new_string(repeated);
}
}
