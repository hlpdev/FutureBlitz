#include "fb/runtime.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

/// Strings allocated and not yet freed, for the leak check in fb_finish. Any thread can make
/// one.
std::atomic<std::int64_t> live_strings = 0;

/// fb_cstring_new's copies not yet freed, for the leak check too, and so that freeing anything
/// else panics. Any thread can make one.
struct cstrings_t {
    std::mutex mutex;
    std::unordered_set<const char*> live;
};

cstrings_t& cstrings() {
    static cstrings_t instance;
    return instance;
}

/// A String's bytes, which follow its header.
char* bytes_of(fb_string_t* string) {
    return reinterpret_cast<char*>(string + 1);
}

const char* bytes_of(const fb_string_t* string) {
    return reinterpret_cast<const char*>(string + 1);
}

std::size_t size_of(const fb_string_t* string) {
    return static_cast<std::size_t>(string->length);
}

/// A new String with room for `length` bytes, which the caller fills, and one owner.
fb_string_t* allocate(const std::size_t length) {
    auto* string = static_cast<fb_string_t*>(std::malloc(sizeof(fb_string_t) + length));
    if (string == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    string->refcount = 1;
    string->length = static_cast<std::int64_t>(length);
    live_strings.fetch_add(1, std::memory_order_relaxed);
    return string;
}

/// A new String holding a copy of `text`.
fb_string_t* make(const std::string_view text) {
    fb_string_t* string = allocate(text.size());
    std::memcpy(bytes_of(string), text.data(), text.size());
    return string;
}

/// A code point in UTF-8, and how many bytes it takes.
struct decoded_t {
    std::uint32_t point;
    std::size_t length;
};

/// U+FFFD, which stands for what isn't well-formed UTF-8.
constexpr std::uint32_t replacement = 0xFFFD;

/// The code point that starts at `at` in `text`, or U+FFFD and 1 for a byte that doesn't start a
/// well-formed one: a stray continuation byte, a lead byte without all its continuations, an
/// overlong form, a surrogate, or one past U+10FFFF.
decoded_t decode(const std::string_view text, const std::size_t at) {
    constexpr std::uint32_t continuation_bits = 0x3F;
    constexpr std::uint32_t bits_per_continuation = 6;
    const auto byte_at = [&](const std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(text[index]));
    };
    const std::uint32_t lead = byte_at(at);
    if (lead < 0x80) {
        return { .point = lead, .length = 1 };
    }

    std::size_t length = 0;
    std::uint32_t point = 0;
    std::uint32_t smallest = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        point = lead & 0x1FU;
        smallest = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        point = lead & 0x0FU;
        smallest = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        point = lead & 0x07U;
        smallest = 0x10000;
    } else {
        return { .point = replacement, .length = 1 };
    }

    if (at + length > text.size()) {
        return { .point = replacement, .length = 1 };
    }

    for (std::size_t j = 1; j < length; ++j) {
        const std::uint32_t next = byte_at(at + j);
        if ((next & 0xC0U) != 0x80U) {
            return { .point = replacement, .length = 1 };
        }

        point = (point << bits_per_continuation) | (next & continuation_bits);
    }

    if (point < smallest || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
        return { .point = replacement, .length = 1 };
    }

    return { .point = point, .length = length };
}

/// `value` as std::to_chars writes it by default: the same text as std::format("{}"), which the
/// compiler uses when it folds constants. Floats get the shortest text that reads back the same.
template <typename Number>
fb_string_t* from_number(const Number value) {
    std::array<char, 64> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return make({ buffer.data(), result.ptr });
}

} // namespace

extern "C" {

std::int64_t fb_string_live_count() {
    return live_strings.load(std::memory_order_relaxed);
}

char* fb_cstring_new(const fb_string_t* string) {
    const std::size_t length = size_of(string);
    auto* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    std::memcpy(copy, bytes_of(string), length);
    copy[length] = '\0';
    cstrings_t& known = cstrings();
    const std::scoped_lock lock(known.mutex);
    known.live.insert(copy);
    return copy;
}

void fb_cstring_free(char* copy) {
    {
        cstrings_t& known = cstrings();
        const std::scoped_lock lock(known.mutex);
        if (known.live.erase(copy) == 0) {
            fb_panic("Free gives back a CString that CString.From made, and this one isn't, or "
                     "it was given back already",
                     nullptr);
        }
    }

    std::free(copy);
}

std::uint64_t fb_cstring_length(const char* text) {
    return std::strlen(text);
}

std::int64_t fb_cstring_live_count() {
    cstrings_t& known = cstrings();
    const std::scoped_lock lock(known.mutex);
    return static_cast<std::int64_t>(known.live.size());
}

fb_string_t* fb_string_from_cstring(const char* text) {
    if (text == nullptr) {
        return nullptr;
    }

    // A String holds UTF-8, so each byte of C's text that doesn't start a well-formed code point,
    // like a code page's 'é', becomes U+FFFD.
    const std::string_view bytes(text);
    std::string valid;
    valid.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        const decoded_t decoded = decode(bytes, i);
        if (decoded.point == replacement && decoded.length == 1) {
            valid += "\xEF\xBF\xBD";
        } else {
            valid += bytes.substr(i, decoded.length);
        }

        i += decoded.length;
    }

    return make(valid);
}

void fb_string_retain(fb_string_t* string) {
    if (string == nullptr) {
        return;
    }

    // Literals are static, with a refcount of -1.
    const std::atomic_ref refcount(string->refcount);
    if (refcount.load(std::memory_order_relaxed) >= 0) {
        // ReSharper disable once CppExpressionWithoutSideEffects
        refcount.fetch_add(1, std::memory_order_relaxed);
    }
}

void fb_string_release(fb_string_t* string) {
    if (string == nullptr) {
        return;
    }

    const std::atomic_ref refcount(string->refcount);
    if (refcount.load(std::memory_order_relaxed) < 0) {
        return;
    }

    // Whichever thread lets go last frees it, after every other owner's last use.
    if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::free(string);
        live_strings.fetch_sub(1, std::memory_order_relaxed);
    }
}

fb_string_t* fb_string_concat(const fb_string_t* left, const fb_string_t* right) {
    fb_string_t* string = allocate(size_of(left) + size_of(right));
    std::memcpy(bytes_of(string), bytes_of(left), size_of(left));
    std::memcpy(bytes_of(string) + size_of(left), bytes_of(right), size_of(right));
    return string;
}

fb_string_t* fb_string_join(const fb_string_t* const* parts, const std::int64_t count) {
    const std::span<const fb_string_t* const> all(parts, static_cast<std::size_t>(count));
    std::size_t length = 0;
    for (const fb_string_t* part : all) {
        length += size_of(part);
    }

    fb_string_t* string = allocate(length);
    char* out = bytes_of(string);
    for (const fb_string_t* part : all) {
        std::memcpy(out, bytes_of(part), size_of(part));
        out += size_of(part);
    }

    return string;
}

std::int32_t fb_string_equal(const fb_string_t* left, const fb_string_t* right) {
    const bool equal = left->length == right->length &&
                       std::memcmp(bytes_of(left), bytes_of(right), size_of(left)) == 0;
    return equal ? 1 : 0;
}

std::int32_t fb_string_compare(const fb_string_t* left, const fb_string_t* right) {
    const std::string_view a(bytes_of(left), size_of(left));
    const std::string_view b(bytes_of(right), size_of(right));
    const int order = a.compare(b);
    if (order == 0) {
        return 0;
    }

    return order < 0 ? -1 : 1;
}

std::uint64_t fb_string_hash(const fb_string_t* string) {
    // FNV-1a, whose result fb_hash_integer spreads out.
    constexpr std::uint64_t offset = 0xcbf29ce484222325ULL;
    // ReSharper disable once CppTooWideScope
    constexpr std::uint64_t prime = 0x100000001b3ULL;
    std::uint64_t hash = offset;
    for (const char byte : std::string_view(bytes_of(string), size_of(string))) {
        hash = (hash ^ static_cast<unsigned char>(byte)) * prime;
    }

    return fb_hash_integer(hash);
}

void fb_string_chars(const fb_string_t* string, fb_array_t* into) {
    // A String is valid UTF-8, but decoding checks it anyway, so nothing reads past its end.
    const std::string_view text(bytes_of(string), size_of(string));
    for (std::size_t i = 0; i < text.size();) {
        const decoded_t decoded = decode(text, i);
        const auto value = static_cast<std::int32_t>(decoded.point);
        std::memcpy(fb_array_push(into, sizeof value), &value, sizeof value);
        i += decoded.length;
    }
}

void fb_string_bytes(const fb_string_t* string, fb_array_t* into) {
    for (const char byte : std::string_view(bytes_of(string), size_of(string))) {
        *static_cast<char*>(fb_array_push(into, 1)) = byte;
    }
}

fb_string_t* fb_string_from_int(const std::int64_t value) {
    return from_number(value);
}

fb_string_t* fb_string_from_uint(const std::uint64_t value) {
    return from_number(value);
}

fb_string_t* fb_string_from_float32(const float value) {
    return from_number(value);
}

fb_string_t* fb_string_from_float64(const double value) {
    return from_number(value);
}

fb_string_t* fb_string_from_bool(const std::int32_t value) {
    return make(value != 0 ? "True" : "False");
}
}
