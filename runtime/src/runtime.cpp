#include "fb/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

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

/// Lets tests and tools tell a panic from a normal exit.
constexpr int panic_exit_code = 101;

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

void fb_print(const fb_string_t* string) {
    const auto* bytes = reinterpret_cast<const char*>(string + 1);
    std::fwrite(bytes, 1, static_cast<std::size_t>(string->length), stdout);
    std::fputc('\n', stdout);
}

void fb_panic(const char* message, const fb_location_t* location) {
    // Whatever the program printed shows up before the panic.
    std::fflush(stdout);
    const char* label = color_panics() ? "\x1b[1;31mpanic\x1b[0m" : "panic";
    if (location != nullptr) {
        std::fprintf(stderr, "%s: %s at %s:%u:%u\n", label, message, location->file, location->line,
                     location->column);
    } else {
        std::fprintf(stderr, "%s: %s\n", label, message);
    }

    std::exit(panic_exit_code);
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
}
