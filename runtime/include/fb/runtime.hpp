#pragma once

#include <cstdint>

// The C ABI that every compiled program links against. compiler/src/codegen.cpp declares these
// functions again in LLVM IR, so a signature change goes in both places.
extern "C" {

/// The header of a String. Its `length` bytes of UTF-8 follow right after it, with no
/// terminating NUL. Strings are immutable and shared by counting references.
struct fb_string_t {
    /// How many owners the String has. A literal is a static block with -1, which retain and
    /// release leave alone, so it's never freed.
    std::int64_t refcount;
    /// In bytes, not characters.
    std::int64_t length;
};

/// Where in the source a panic happened, as compiled code passes it to the runtime.
struct fb_location_t {
    /// The source file's path, NUL-terminated.
    const char* file;
    /// 1-based.
    std::uint32_t line;
    /// 1-based, counted in code points.
    std::uint32_t column;
};

/// The first thing a program's main calls. On Windows, it switches the console to UTF-8, keeps
/// "\n" from becoming "\r\n", and turns on colors for panics.
void fb_start();

/// The last thing a program's main calls, after releasing the Globals' Strings. Prints a leak
/// report to stderr when any String was never released.
void fb_finish();

/// Adds an owner to `string`. Compiled code calls it when a String gets a new holder, like a
/// variable. Does nothing for a literal.
void fb_string_retain(fb_string_t* string);
/// Removes an owner from `string`, and frees it when that was the last one. Compiled code calls
/// it when a holder lets go, like a variable leaving its scope. Does nothing for a literal.
void fb_string_release(fb_string_t* string);
/// A new String with `left`'s bytes, then `right`'s, for `+` on Strings. The caller owns the
/// result and must release it. The arguments are only borrowed.
fb_string_t* fb_string_concat(const fb_string_t* left, const fb_string_t* right);
/// A new String with the `count` Strings in `parts` one after another, for interpolated strings.
/// The caller owns the result and must release it. The parts are only borrowed.
fb_string_t* fb_string_join(const fb_string_t* const* parts, std::int64_t count);
/// 1 when `left` and `right` hold the same bytes, and 0 otherwise, for `==` and `!=` on Strings.
std::int32_t fb_string_equal(const fb_string_t* left, const fb_string_t* right);

/// `value` as decimal text, like "-42", for interpolated strings. The caller owns the result.
fb_string_t* fb_string_from_int(std::int64_t value);
/// `value` as decimal text, for interpolated strings. The caller owns the result.
fb_string_t* fb_string_from_uint(std::uint64_t value);
/// The shortest text that reads back as `value`, like "0.1", for interpolated strings. It
/// matches the compiler's constant folding. The caller owns the result.
fb_string_t* fb_string_from_float32(float value);
/// The shortest text that reads back as `value`, like "0.1", for interpolated strings. It
/// matches the compiler's constant folding. The caller owns the result.
fb_string_t* fb_string_from_float64(double value);
/// "True" when `value` isn't 0, and "False" otherwise, for interpolated strings. The caller owns
/// the result.
fb_string_t* fb_string_from_bool(std::int32_t value);

/// `base` to the power `exponent`, for `**` on a signed integer type that's `bits` wide (8, 16,
/// 32, or 64). Compiled code widens the operands to 64 bits, and the result comes back
/// sign-extended. When `checked` isn't 0 (debug builds), overflow panics; otherwise it wraps. A
/// negative exponent always panics. `location` is where a panic reports, and can be null.
std::int64_t fb_power_int(std::int64_t base, std::int64_t exponent, std::uint32_t bits,
                          std::int32_t checked, const fb_location_t* location);
/// `base` to the power `exponent`, for `**` on an unsigned integer type that's `bits` wide (8,
/// 16, 32, or 64). Compiled code widens the operands to 64 bits. When `checked` isn't 0 (debug
/// builds), overflow panics; otherwise it wraps. `location` is where a panic reports, and can be
/// null.
std::uint64_t fb_power_uint(std::uint64_t base, std::uint64_t exponent, std::uint32_t bits,
                            std::int32_t checked, const fb_location_t* location);

/// Writes `string` and a newline to stdout, for the built-in Print. The String is only borrowed.
void fb_print(const fb_string_t* string);

/// Prints "panic: <message> at <file>:<line>:<column>" to stderr and exits with code 101.
/// Compiled code calls it when a run-time check fails. `location` can be null, which leaves out
/// the " at ..." part.
[[noreturn]] void fb_panic(const char* message, const fb_location_t* location);
}
