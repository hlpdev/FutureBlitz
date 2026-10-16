#include "fb/runtime.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

namespace {

/// Strings allocated and not yet freed, for the leak check in fb_finish.
std::int64_t live_strings = 0;

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
    ++live_strings;
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

void fb_finish() {
    if (live_strings != 0) {
        std::fprintf(stderr, "leak: %lld Strings were never released\n",
                     static_cast<long long>(live_strings));
    }
}

void fb_string_retain(fb_string_t* string) {
    // Literals are static, with a refcount of -1.
    if (string->refcount >= 0) {
        ++string->refcount;
    }
}

void fb_string_release(fb_string_t* string) {
    if (string->refcount < 0) {
        return;
    }

    --string->refcount;
    if (string->refcount == 0) {
        std::free(string);
        --live_strings;
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
