#pragma once

#include "fb/compress.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the runtime's parts for std.compress share, each in its own archive over its library, so
// it's all inline: the results they give, and how their bytes go into the UInt8[]s std.compress
// makes for them.
namespace fb::compress {

/// What a decoder or an archive's read gives.
enum result_t : std::uint8_t {
    RESULT_DONE = 0,
    /// The bytes aren't the format, or are broken.
    RESULT_INVALID = 1,
    /// They stop before the format says they end.
    RESULT_ENDS_TOO_SOON = 2,
    /// They come out to more bytes than the limit.
    RESULT_TOO_LARGE = 3,
    /// They're the format, but use what isn't supported, like a method or encryption.
    RESULT_UNSUPPORTED = 4,
};

/// What an archive's entry is, as std.compress's ArchiveEntryKind numbers them.
enum entry_kind_t : std::uint8_t {
    KIND_FILE = 0,
    KIND_FOLDER = 1,
    KIND_LINK = 2,
};

/// When an archive's entry was last changed: its seconds since 1970-01-01T00:00:00Z, below 0
/// before it, and the nanoseconds after those, from 0 to 999999999.
struct moment_t {
    std::int64_t seconds;
    std::int32_t nanos;
};

/// The most bytes a UInt8[] holds, which an Int counts.
constexpr std::int64_t most_bytes = std::numeric_limits<std::int32_t>::max();

/// How many bytes a codec makes at a time, before they go into the output.
constexpr std::size_t chunk_size = std::size_t{ 64 } * 1024;

inline std::span<const std::uint8_t> bytes_of(const fb_array_t* array) {
    return { static_cast<const std::uint8_t*>(array->data),
             static_cast<std::size_t>(array->count) };
}

inline std::string_view text_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

inline fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

/// Bytes going into a UInt8[], which it empties first, up to `limit` of them.
class output_t {
public:
    output_t(fb_array_t* array, const std::int64_t limit) : m_array(array), m_limit(limit) {
        m_array->count = 0;
    }

    /// Adds `bytes` at the end, or gives false, adding none, when they'd go past the limit.
    [[nodiscard]] bool add(const std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return true;
        }

        const auto size = static_cast<std::int64_t>(bytes.size());
        if (size > m_limit - m_array->count) {
            return false;
        }

        const std::int64_t count = m_array->count + size;
        if (count > m_array->capacity) {
            const std::int64_t doubled = std::min(most_bytes, m_array->capacity * 2);
            fb_array_reserve(m_array, 1, std::max(count, doubled), nullptr);
        }

        std::memcpy(static_cast<std::uint8_t*>(m_array->data) + m_array->count, bytes.data(),
                    bytes.size());
        m_array->count = count;
        return true;
    }

    /// Adds `bytes` at the end, panicking when they'd go past the most an array holds, which only
    /// an encoder's output can.
    void add_all(const std::span<const std::uint8_t> bytes) {
        if (!add(bytes)) {
            fb_panic("the compressed bytes are more than a UInt8[] holds", nullptr);
        }
    }

private:
    fb_array_t* m_array;
    std::int64_t m_limit;
};

/// Appends `code`'s UTF-8 to `text`.
inline void append_utf8(std::string& text, const char32_t code) {
    if (code < 0x80U) {
        text += static_cast<char>(code);
    } else if (code < 0x800U) {
        text += static_cast<char>(0xc0U | (code >> 6U));
        text += static_cast<char>(0x80U | (code & 0x3fU));
    } else if (code < 0x10000U) {
        text += static_cast<char>(0xe0U | (code >> 12U));
        text += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
        text += static_cast<char>(0x80U | (code & 0x3fU));
    } else {
        text += static_cast<char>(0xf0U | (code >> 18U));
        text += static_cast<char>(0x80U | ((code >> 12U) & 0x3fU));
        text += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
        text += static_cast<char>(0x80U | (code & 0x3fU));
    }
}

/// How many bytes the well-formed UTF-8 character at the start of `text` takes, or 0 when it
/// isn't one: no overlong forms, surrogates, or code points past U+10FFFF.
inline std::size_t utf8_length(const std::string_view text) {
    const auto byte = [&](const std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[at]));
    };
    const std::uint32_t first = byte(0);
    if (first < 0x80U) {
        return 1;
    }

    std::size_t length = 0;
    char32_t code = 0;
    if ((first & 0xe0U) == 0xc0U) {
        length = 2;
        code = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
        length = 3;
        code = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
        length = 4;
        code = first & 0x07U;
    } else {
        return 0;
    }

    if (text.size() < length) {
        return 0;
    }

    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(i) & 0xc0U) != 0x80U) {
            return 0;
        }

        code = (code << 6U) | (byte(i) & 0x3fU);
    }

    constexpr std::array<char32_t, 5> least{ 0, 0, 0x80U, 0x800U, 0x10000U };
    const bool surrogate = code >= 0xd800U && code <= 0xdfffU;
    return code < least[length] || surrogate || code > 0x10ffffU ? 0 : length;
}

/// Whether `text` is well-formed UTF-8 throughout.
inline bool is_utf8(std::string_view text) {
    while (!text.empty()) {
        const std::size_t length = utf8_length(text);
        if (length == 0) {
            return false;
        }

        text.remove_prefix(length);
    }

    return true;
}

/// `text`, with U+FFFD in place of each byte that doesn't start well-formed UTF-8, as the
/// runtime reads input.
inline std::string utf8_or_replaced(std::string_view text) {
    std::string made;
    made.reserve(text.size());
    while (!text.empty()) {
        const std::size_t length = utf8_length(text);
        if (length == 0) {
            append_utf8(made, 0xfffd);
            text.remove_prefix(1);
        } else {
            made += text.substr(0, length);
            text.remove_prefix(length);
        }
    }

    return made;
}

/// What a decoder keeps once it fails: why, which it gives each time it's asked again.
struct failure_t {
    std::int32_t result = RESULT_DONE;
    std::string message;

    [[nodiscard]] std::int32_t fail(const std::int32_t why, const std::string_view text) {
        result = why;
        message = text;
        return why;
    }
};

} // namespace fb::compress
