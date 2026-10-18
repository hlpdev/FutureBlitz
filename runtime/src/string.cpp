#include "fb/runtime.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

namespace {

/// Strings allocated and not yet freed, for the leak check in fb_finish. Any thread can make
/// one.
std::atomic<std::int64_t> live_strings = 0;

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

void fb_string_retain(fb_string_t* string) {
    if (string == nullptr) {
        return;
    }

    // Literals are static, with a refcount of -1.
    const std::atomic_ref refcount(string->refcount);
    if (refcount.load(std::memory_order_relaxed) >= 0) {
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
    constexpr std::uint64_t prime = 0x100000001b3ULL;
    std::uint64_t hash = offset;
    for (const char byte : std::string_view(bytes_of(string), size_of(string))) {
        hash = (hash ^ static_cast<unsigned char>(byte)) * prime;
    }

    return fb_hash_integer(hash);
}

void fb_string_chars(const fb_string_t* string, fb_array_t* into) {
    // A String is always valid UTF-8: its lead byte says how many continuation bytes follow,
    // each with 6 bits of the code point.
    constexpr std::uint32_t four = 0xF0;
    constexpr std::uint32_t three = 0xE0;
    constexpr std::uint32_t two = 0xC0;
    constexpr std::uint32_t continuation_bits = 0x3F;
    constexpr std::uint32_t bits_per_continuation = 6;
    const std::string_view text(bytes_of(string), size_of(string));
    const auto byte_at = [&](const std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(text[at]));
    };

    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t lead = byte_at(i);
        std::size_t length = 1;
        std::uint32_t point = lead;
        if (lead >= four) {
            length = 4;
            point = lead & 0x07U;
        } else if (lead >= three) {
            length = 3;
            point = lead & 0x0FU;
        } else if (lead >= two) {
            length = 2;
            point = lead & 0x1FU;
        }

        for (std::size_t j = 1; j < length; ++j) {
            point = (point << bits_per_continuation) | (byte_at(i + j) & continuation_bits);
        }

        const auto value = static_cast<std::int32_t>(point);
        std::memcpy(fb_array_push(into, sizeof value), &value, sizeof value);
        i += length;
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
