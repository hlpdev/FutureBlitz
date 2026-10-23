#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.math.big's functions in the runtime, each over the bundled C library its prefix names, which
// a program links only when it calls one: fb_bigint_ over libtommath, for BigInt, and fb_decimal_
// over mpdecimal, for Decimal. Each number is an object of the library's, which a void* holds:
// what gives one gives a new one, which the caller frees with fb_bigint_free or fb_decimal_free,
// and they never change. Arrays and Strings are only borrowed. std.math.big checks what panics
// first, like dividing by 0, so a bad argument here is a bug, which panics.

extern "C" {

/// A new BigInt of `value`.
void* fb_bigint_from_int(std::int64_t value);
/// A new BigInt of `value`'s whole part, which is finite.
void* fb_bigint_from_double(double value);
/// A new BigInt of `text` in `radix`, from 2 to 36: a sign or none, then one or more digits, the
/// ones past 9 letters of either case; or null when it isn't one.
void* fb_bigint_parse(const fb_string_t* text, std::int32_t radix);
/// A new BigInt of `bytes` (a UInt8[]), big-endian, 0 or more.
void* fb_bigint_from_bytes(const fb_array_t* bytes);
void fb_bigint_free(void* a);

/// `a` in `radix`, from 2 to 36, with lowercase letters past 9, and "-" first when negative.
fb_string_t* fb_bigint_to_string(const void* a, std::int32_t radix);
/// `a`'s magnitude, big-endian, in as few bytes as hold it, none for 0, into `output`.
void fb_bigint_to_bytes(const void* a, fb_array_t* output);
/// 1 when `a` is an Int64's, and 0 otherwise.
std::int32_t fb_bigint_fits_int64(const void* a);
/// `a`, which fits an Int64.
std::int64_t fb_bigint_to_int64(const void* a);
/// The Double nearest `a`, ties to even, or an infinity past the largest.
double fb_bigint_to_double(const void* a);
/// A hash of `a`, the same for equal values.
std::uint64_t fb_bigint_hash(const void* a);

/// -1, 0, or 1 as `a` is below, equal to, or above `b`.
std::int32_t fb_bigint_compare(const void* a, const void* b);
/// -1, 0, or 1 as `a` is below, equal to, or above 0.
std::int32_t fb_bigint_sign(const void* a);
/// How many bits `a`'s magnitude takes, 0 for 0.
std::int64_t fb_bigint_bit_length(const void* a);

void* fb_bigint_add(const void* a, const void* b);
void* fb_bigint_subtract(const void* a, const void* b);
void* fb_bigint_multiply(const void* a, const void* b);
/// `a / b` rounded toward 0, as an Int's `/` is; `b` isn't 0.
void* fb_bigint_divide(const void* a, const void* b);
/// What's left of `a / b`, with `a`'s sign, as an Int's `%` is; `b` isn't 0.
void* fb_bigint_remainder(const void* a, const void* b);
void* fb_bigint_negate(const void* a);
void* fb_bigint_abs(const void* a);
/// The bitwise operators, on two's complement with as many bits as the operands need, as an
/// Int's are on 64.
void* fb_bigint_and(const void* a, const void* b);
void* fb_bigint_or(const void* a, const void* b);
void* fb_bigint_xor(const void* a, const void* b);
void* fb_bigint_not(const void* a);
/// `a` times 2 to the power `count`, which is 0 or more.
void* fb_bigint_shift_left(const void* a, std::int64_t count);
/// `a` over 2 to the power `count`, which is 0 or more, rounded down, as an Int's `>>` is.
void* fb_bigint_shift_right(const void* a, std::int64_t count);
/// `a` to the power `exponent`, which is 0 or more.
void* fb_bigint_power(const void* a, std::int64_t exponent);
/// `base` to the power `exponent`, 0 or more, modulo `modulus`, above 0: from 0 to below it.
void* fb_bigint_mod_power(const void* base, const void* exponent, const void* modulus);
/// The number from 0 to below `modulus`, above 1, that times `a` is 1 modulo it, or null when
/// there's none.
void* fb_bigint_mod_inverse(const void* a, const void* modulus);
/// The greatest common divisor of `a` and `b`, 0 or more, and their least common multiple.
void* fb_bigint_gcd(const void* a, const void* b);
void* fb_bigint_lcm(const void* a, const void* b);
/// The square root of `a`, 0 or more, rounded down.
void* fb_bigint_sqrt(const void* a);
/// 1 when `a` is prime by the Baillie-PSW test, which no composite number is known to pass, and
/// 0 otherwise.
std::int32_t fb_bigint_is_probable_prime(const void* a);

/// A new Decimal of `value`.
void* fb_decimal_from_int(std::int64_t value);
/// A new Decimal of the shortest text that reads back as `value`, which is finite.
void* fb_decimal_from_double(double value);
/// A new Decimal of `text`, exactly: a sign or none, then digits with a "." or not, then an
/// exponent or none, as "E" or "e", a sign or none, and digits; or null when it isn't one, or is
/// too large or small to hold.
void* fb_decimal_parse(const fb_string_t* text);
void fb_decimal_free(void* a);

/// `a` as Python's str gives it: plain digits, unless the exponent is above 0 or very small, as
/// in "1.5E+3".
fb_string_t* fb_decimal_to_string(const void* a);
/// `a` with no exponent, in all its digits, as in "1500".
fb_string_t* fb_decimal_to_plain_string(const void* a);
/// The Double nearest `a`, ties to even, an infinity past the largest, or 0 below the smallest.
double fb_decimal_to_double(const void* a);
/// A hash of `a`, the same for equal values, like 1.0 and 1.00.
std::uint64_t fb_decimal_hash(const void* a);

/// -1, 0, or 1 as `a` is below, equal to, or above `b`.
std::int32_t fb_decimal_compare(const void* a, const void* b);
/// -1, 0, or 1 as `a` is below, equal to, or above 0.
std::int32_t fb_decimal_sign(const void* a);
/// 1 when `a` is a whole number, and 0 otherwise.
std::int32_t fb_decimal_is_integer(const void* a);
/// 1 when `a` is a whole number in an Int64's range, and 0 otherwise.
std::int32_t fb_decimal_fits_int64(const void* a);
/// `a`, a whole number that fits an Int64.
std::int64_t fb_decimal_to_int64(const void* a);

/// The arithmetic, in 28 significant digits rounded half to even, with exponents from -999999 to
/// 999999, as Python's decimal does by default; each gives null when the result is past that,
/// or isn't a number. `b` isn't 0 for `/`.
void* fb_decimal_add(const void* a, const void* b);
void* fb_decimal_subtract(const void* a, const void* b);
void* fb_decimal_multiply(const void* a, const void* b);
void* fb_decimal_divide(const void* a, const void* b);
void* fb_decimal_power(const void* a, const void* exponent);
void* fb_decimal_sqrt(const void* a);
void* fb_decimal_exp(const void* a);
void* fb_decimal_ln(const void* a);
void* fb_decimal_log10(const void* a);
/// What's left of `a / b`, its quotient rounded toward 0, with `a`'s sign, exactly; or null when
/// that quotient has more digits than 28 or the operands', whichever is more. `b` isn't 0.
void* fb_decimal_remainder(const void* a, const void* b);
void* fb_decimal_negate(const void* a);
void* fb_decimal_abs(const void* a);
/// `a` rounded to `places` digits after the point, or before it when negative, by `mode`: 0 half
/// to even, 1 half away from 0, 2 half toward 0, 3 away from 0, 4 toward 0, 5 toward positive
/// infinity, or 6 toward negative infinity. It gives null when the result has more digits than
/// 28 or `a`'s, whichever is more.
void* fb_decimal_round(const void* a, std::int64_t places, std::int32_t mode);
/// `a` rounded to a whole number by `mode`, keeping an exponent above 0.
void* fb_decimal_round_whole(const void* a, std::int32_t mode);
}
