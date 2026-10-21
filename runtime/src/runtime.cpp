#include "fb/runtime.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

/// splitmix64's finalizer: each bit of `value` changes about half the bits of the result.
std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

/// Lets tests and tools tell a panic from a normal exit.
constexpr int panic_exit_code = 101;
/// A program that let an error out failed, like a command that couldn't do its job.
constexpr int error_exit_code = 1;

/// Keeps the low `bits` bits of `value`, sign-extending them when `is_signed`.
std::uint64_t truncate(const std::uint64_t value, const std::uint32_t bits, const bool is_signed) {
    if (bits == 64) {
        return value;
    }

    const std::uint64_t low = value & ((std::uint64_t{ 1 } << bits) - 1);
    if (!is_signed) {
        return low;
    }

    // Flipping the sign bit and then subtracting it copies the sign into the high bits.
    const std::uint64_t sign = std::uint64_t{ 1 } << (bits - 1);
    return (low ^ sign) - sign;
}

/// `base` to the power `exponent` modulo 2^64, by squaring. The low bits are right for signed
/// bases too.
std::uint64_t wrapping_power(std::uint64_t base, std::uint64_t exponent) {
    std::uint64_t result = 1;
    while (exponent > 0) {
        if ((exponent & 1U) != 0) {
            result *= base;
        }

        exponent >>= 1U;
        base *= base;
    }

    return result;
}

/// Sets `result` to `base` to the power `exponent`, by squaring, and returns true when that
/// doesn't fit in `Int`.
template <typename Int>
bool power_overflows(Int base, std::uint64_t exponent, Int& result) {
    result = 1;
    while (exponent > 0) {
        if ((exponent & 1U) != 0 && __builtin_mul_overflow(result, base, &result)) {
            return true;
        }

        // The last square is never used, so it mustn't count as an overflow.
        exponent >>= 1U;
        if (exponent > 0 && __builtin_mul_overflow(base, base, &base)) {
            return true;
        }
    }

    return false;
}

/// Held while a line goes to stdout, so lines from different threads don't mix.
std::mutex& output() {
    static std::mutex instance;
    return instance;
}

/// Held by the first thread that ends the program, so another that panics meanwhile waits.
std::mutex& ending() {
    static std::mutex instance;
    return instance;
}

/// Ends the program with `code` at once, with what it printed flushed. Exiting normally would
/// destroy what other threads may still be using, like the pool.
[[noreturn]] void end_program(const int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

/// Panics are colored when stderr is a terminal that shows colors, unless NO_COLOR is set.
bool color_panics() {
    const char* no_color = std::getenv("NO_COLOR");
    if (no_color != nullptr && *no_color != '\0') {
        return false;
    }

#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0 &&
           (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

/// Ends the program with "panic: <message> at <file>:<line>:<column>" on stderr, the message
/// whole, NULs too.
[[noreturn]] void panic_with(const std::string_view message, const fb_location_t* location) {
    // The lock is never let go: the program ends with it held.
    ending().lock();
    // Whatever the program printed shows up before the panic, without a line cut in half.
    output().lock();
    std::fflush(stdout);
    const char* label = color_panics() ? "\x1b[1;31mpanic\x1b[0m" : "panic";
    std::fprintf(stderr, "%s: ", label);
    std::fwrite(message.data(), 1, message.size(), stderr);
    if (location != nullptr) {
        std::fprintf(stderr, " at %s:%u:%u", location->file, location->line, location->column);
    }

    std::fputc('\n', stderr);
    end_program(panic_exit_code);
}

} // namespace

extern "C" {

void fb_start() {
#ifdef _WIN32
    // Strings are UTF-8, and binary mode keeps "\n" from becoming "\r\n".
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    // Lets panics print in color.
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(error, &mode) != 0) {
        SetConsoleMode(error, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

void* fb_mem_alloc(const std::uint64_t bytes) {
    return std::calloc(1, std::max<std::uint64_t>(bytes, 1));
}

void* fb_mem_resize(void* memory, const std::uint64_t bytes) {
    return std::realloc(memory, std::max<std::uint64_t>(bytes, 1));
}

void fb_mem_free(void* memory) {
    std::free(memory);
}

void fb_mem_copy(void* destination, const void* source, const std::uint64_t bytes) {
    std::memmove(destination, source, bytes);
}

void fb_mem_fill(void* destination, const std::uint8_t value, const std::uint64_t bytes) {
    std::memset(destination, value, bytes);
}

std::int32_t fb_mem_compare(const void* a, const void* b, const std::uint64_t bytes) {
    return std::memcmp(a, b, bytes);
}

void fb_print(const fb_string_t* string) {
    const auto* bytes = reinterpret_cast<const char*>(string + 1);
    const std::scoped_lock lock(output());
    std::fwrite(bytes, 1, static_cast<std::size_t>(string->length), stdout);
    std::fputc('\n', stdout);
}

void fb_panic(const char* message, const fb_location_t* location) {
    panic_with(message, location);
}

void fb_panic_string(const fb_string_t* message, const fb_location_t* location) {
    const auto* bytes = reinterpret_cast<const char*>(message + 1);
    panic_with({ bytes, static_cast<std::size_t>(message->length) }, location);
}

void fb_error_uncaught(const fb_object_t* error, const fb_location_t* location) {
    ending().lock();
    output().lock();
    std::fflush(stdout);
    // Every error's Class extends Error, so its first field is the Message.
    const fb_string_t* message = *reinterpret_cast<fb_string_t* const*>(error + 1);
    const char* label = color_panics() ? "\x1b[1;31muncaught\x1b[0m" : "uncaught";
    std::fprintf(stderr, "%s %s", label, error->type->name);
    if (message != nullptr && message->length != 0) {
        std::fputs(": ", stderr);
        std::fwrite(message + 1, 1, static_cast<std::size_t>(message->length), stderr);
    }

    if (location != nullptr) {
        std::fprintf(stderr, " at %s:%u:%u", location->file, location->line, location->column);
    }

    std::fputc('\n', stderr);
    end_program(error_exit_code);
}

std::int64_t fb_power_int(const std::int64_t base, const std::int64_t exponent,
                          const std::uint32_t bits, const std::int32_t checked,
                          const fb_location_t* location) {
    if (exponent < 0) {
        fb_panic("an integer power needs an exponent of 0 or more", location);
    }

    const auto unsigned_exponent = static_cast<std::uint64_t>(exponent);
    if (checked != 0) {
        // The largest value of a `bits`-wide signed integer.
        const auto max =
            static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() >> (65 - bits));
        std::int64_t result = 0;
        if (power_overflows(base, unsigned_exponent, result) || result > max || result < -max - 1) {
            fb_panic("integer overflow in '**'", location);
        }

        return result;
    }

    const std::uint64_t result =
        wrapping_power(static_cast<std::uint64_t>(base), unsigned_exponent);
    return static_cast<std::int64_t>(truncate(result, bits, true));
}

std::uint64_t fb_power_uint(const std::uint64_t base, const std::uint64_t exponent,
                            const std::uint32_t bits, const std::int32_t checked,
                            const fb_location_t* location) {
    if (checked != 0) {
        const std::uint64_t max = std::numeric_limits<std::uint64_t>::max() >> (64 - bits);
        std::uint64_t result = 0;
        if (power_overflows(base, exponent, result) || result > max) {
            fb_panic("integer overflow in '**'", location);
        }

        return result;
    }

    return truncate(wrapping_power(base, exponent), bits, false);
}

std::uint64_t fb_hash_integer(const std::uint64_t bits) {
    return mix(bits);
}

std::uint64_t fb_hash_float(double value) {
    // 0.0 and -0.0 are equal, so they hash the same.
    if (value == 0.0) {
        value = 0.0;
    }

    return mix(std::bit_cast<std::uint64_t>(value));
}

std::uint64_t fb_hash_combine(const std::uint64_t seed, const std::uint64_t hash) {
    return mix(seed ^ (hash + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U)));
}
}
