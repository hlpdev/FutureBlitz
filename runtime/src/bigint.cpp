// std.math.big's BigInt over libtommath: integers of any size, each an mp_int of its own, which
// never changes once it's made.

#include "fb/big.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tommath.h>

namespace {

/// Panics when libtommath fails, which it does only when memory runs out or a number gets too
/// large for it.
void check(const mp_err result) {
    if (result == MP_MEM) {
        fb_panic("out of memory", nullptr);
    }
    if (result != MP_OKAY) {
        fb_panic("a BigInt got too large", nullptr);
    }
}

/// An mp_int for working something out, cleared when it goes.
struct number_t {
    mp_int value{};

    number_t() {
        check(mp_init(&value));
    }

    ~number_t() {
        mp_clear(&value);
    }

    number_t(const number_t&) = delete;
    number_t& operator=(const number_t&) = delete;
};

/// A new mp_int of 0.
mp_int* make() {
    auto* a = new mp_int{};
    check(mp_init(a));
    return a;
}

const mp_int* from(const void* a) {
    return static_cast<const mp_int*>(a);
}

/// A new mp_int that `function` sets from `a`, or from `a` and `b`, as libtommath's do.
void* apply(mp_err (*function)(const mp_int*, mp_int*), const void* a) {
    mp_int* result = make();
    check(function(from(a), result));
    return result;
}

void* apply(mp_err (*function)(const mp_int*, const mp_int*, mp_int*), const void* a,
            const void* b) {
    mp_int* result = make();
    check(function(from(a), from(b), result));
    return result;
}

/// The value of `digit` as a digit in a radix up to 36, or 36 when it isn't one.
int digit_value(const char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'z') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'Z') {
        return digit - 'A' + 10;
    }
    return 36;
}

/// Gives `output` `count` bytes, which replace its old ones, and returns where they are.
std::uint8_t* resize(fb_array_t* output, const std::int64_t count) {
    fb_array_reserve(output, 1, count, nullptr);
    output->count = count;
    return static_cast<std::uint8_t*>(output->data);
}

/// The primes up to 101, which the Baillie-PSW test tries dividing by first.
constexpr std::array<mp_digit, 26> small_primes = {
    2,  3,  5,  7,  11, 13, 17, 19, 23, 29, 31, 37, 41,
    43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101,
};

} // namespace

extern "C" {

void* fb_bigint_from_int(const std::int64_t value) {
    mp_int* result = make();
    mp_set_i64(result, value);
    return result;
}

void* fb_bigint_from_double(const double value) {
    // The whole part is `fraction` times 2 to the power `exponent`, and `fraction`'s 53 bits are
    // a whole number once moved up by 53.
    int exponent = 0;
    const double fraction = std::frexp(std::trunc(value), &exponent);
    mp_int* result = make();
    mp_set_i64(result, static_cast<std::int64_t>(std::ldexp(fraction, 53)));
    if (exponent >= 53) {
        check(mp_mul_2d(result, exponent - 53, result));
    } else {
        check(mp_div_2d(result, 53 - exponent, result, nullptr));
    }
    return result;
}

void* fb_bigint_parse(const fb_string_t* text, const std::int32_t radix) {
    const std::string_view view(reinterpret_cast<const char*>(text + 1),
                                static_cast<std::size_t>(text->length));
    const bool signed_text = !view.empty() && (view.front() == '-' || view.front() == '+');
    const std::string_view digits = view.substr(signed_text ? 1 : 0);
    if (digits.empty()) {
        return nullptr;
    }
    for (const char digit : digits) {
        if (digit_value(digit) >= radix) {
            return nullptr;
        }
    }

    // libtommath reads a "-" but not a "+".
    const std::string copy(view.front() == '-' ? view : digits);
    mp_int* result = make();
    check(mp_read_radix(result, copy.c_str(), radix));
    return result;
}

void* fb_bigint_from_bytes(const fb_array_t* bytes) {
    mp_int* result = make();
    check(mp_from_ubin(result, static_cast<const std::uint8_t*>(bytes->data),
                       static_cast<std::size_t>(bytes->count)));
    return result;
}

void fb_bigint_free(void* a) {
    auto* number = static_cast<mp_int*>(a);
    mp_clear(number);
    delete number;
}

fb_string_t* fb_bigint_to_string(const void* a, const std::int32_t radix) {
    // Each digit holds at least the bits of the largest power of 2 not above the radix, and the
    // text has a "-" and a NUL besides.
    const int bits_per_digit = std::bit_width(static_cast<unsigned>(radix)) - 1;
    std::string text(static_cast<std::size_t>((mp_count_bits(from(a)) / bits_per_digit) + 3), '\0');
    std::size_t written = 0;
    check(mp_to_radix(from(a), text.data(), text.size(), &written, radix));
    text.resize(written - 1);
    for (char& digit : text) {
        if (digit >= 'A' && digit <= 'Z') {
            digit = static_cast<char>(digit - 'A' + 'a');
        }
    }
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

void fb_bigint_to_bytes(const void* a, fb_array_t* output) {
    const std::size_t count = mp_ubin_size(from(a));
    std::uint8_t* bytes = resize(output, static_cast<std::int64_t>(count));
    if (count > 0) {
        check(mp_to_ubin(from(a), bytes, count, nullptr));
    }
}

std::int32_t fb_bigint_fits_int64(const void* a) {
    // An Int64 holds magnitudes of 63 bits, and -2^63, whose 64 bits are 1 and then 63 0s.
    const int bits = mp_count_bits(from(a));
    const bool lowest = bits == 64 && mp_isneg(from(a)) == MP_YES && mp_cnt_lsb(from(a)) == 63;
    return bits <= 63 || lowest ? 1 : 0;
}

std::int64_t fb_bigint_to_int64(const void* a) {
    return mp_get_i64(from(a));
}

double fb_bigint_to_double(const void* a) {
    const mp_int* number = from(a);
    const int bits = mp_count_bits(number);
    if (bits <= 63) {
        return static_cast<double>(mp_get_i64(number));
    }

    // The top 55 bits, the lowest of them 1 when any bit below them is, round to a Double's 53
    // as the whole number does, ties to even.
    number_t top;
    number_t rest;
    check(mp_abs(number, &top.value));
    check(mp_div_2d(&top.value, bits - 55, &top.value, &rest.value));
    const std::uint64_t sticky = mp_iszero(&rest.value) == MP_YES ? 0 : 1;
    const auto mantissa = static_cast<std::int64_t>(mp_get_mag_u64(&top.value) | sticky);
    const double magnitude = std::ldexp(static_cast<double>(mantissa), bits - 55);
    return mp_isneg(number) == MP_YES ? -magnitude : magnitude;
}

std::uint64_t fb_bigint_hash(const void* a) {
    const mp_int* number = from(a);
    std::uint64_t hash = fb_hash_integer(mp_isneg(number) == MP_YES ? 1 : 0);
    for (int i = 0; i < number->used; i++) {
        hash = fb_hash_combine(hash, fb_hash_integer(number->dp[i]));
    }
    return hash;
}

std::int32_t fb_bigint_compare(const void* a, const void* b) {
    return mp_cmp(from(a), from(b));
}

std::int32_t fb_bigint_sign(const void* a) {
    if (mp_iszero(from(a)) == MP_YES) {
        return 0;
    }
    return mp_isneg(from(a)) == MP_YES ? -1 : 1;
}

std::int64_t fb_bigint_bit_length(const void* a) {
    return mp_count_bits(from(a));
}

void* fb_bigint_add(const void* a, const void* b) {
    return apply(mp_add, a, b);
}

void* fb_bigint_subtract(const void* a, const void* b) {
    return apply(mp_sub, a, b);
}

void* fb_bigint_multiply(const void* a, const void* b) {
    return apply(mp_mul, a, b);
}

void* fb_bigint_divide(const void* a, const void* b) {
    mp_int* result = make();
    check(mp_div(from(a), from(b), result, nullptr));
    return result;
}

void* fb_bigint_remainder(const void* a, const void* b) {
    mp_int* result = make();
    check(mp_div(from(a), from(b), nullptr, result));
    return result;
}

void* fb_bigint_negate(const void* a) {
    return apply(mp_neg, a);
}

void* fb_bigint_abs(const void* a) {
    return apply(mp_abs, a);
}

void* fb_bigint_and(const void* a, const void* b) {
    return apply(mp_and, a, b);
}

void* fb_bigint_or(const void* a, const void* b) {
    return apply(mp_or, a, b);
}

void* fb_bigint_xor(const void* a, const void* b) {
    return apply(mp_xor, a, b);
}

void* fb_bigint_not(const void* a) {
    return apply(mp_complement, a);
}

void* fb_bigint_shift_left(const void* a, const std::int64_t count) {
    if (count > INT_MAX) {
        if (mp_iszero(from(a)) == MP_YES) {
            return make();
        }
        fb_panic("a BigInt got too large", nullptr);
    }

    mp_int* result = make();
    check(mp_mul_2d(from(a), static_cast<int>(count), result));
    return result;
}

void* fb_bigint_shift_right(const void* a, const std::int64_t count) {
    // Past all of its bits, what's left is 0, or -1 below 0.
    if (count >= mp_count_bits(from(a))) {
        return fb_bigint_from_int(mp_isneg(from(a)) == MP_YES ? -1 : 0);
    }

    mp_int* result = make();
    check(mp_signed_rsh(from(a), static_cast<int>(count), result));
    return result;
}

void* fb_bigint_power(const void* a, const std::int64_t exponent) {
    // Past an int's exponents, only 0, 1, and -1 aren't too large, and -1 to an even power is 1.
    if (exponent > INT_MAX) {
        if (mp_iszero(from(a)) == MP_YES) {
            return make();
        }
        if (mp_count_bits(from(a)) != 1) {
            fb_panic("a BigInt got too large", nullptr);
        }
        return fb_bigint_from_int(mp_isneg(from(a)) == MP_YES && exponent % 2 == 1 ? -1 : 1);
    }

    mp_int* result = make();
    check(mp_expt_n(from(a), static_cast<int>(exponent), result));
    return result;
}

void* fb_bigint_mod_power(const void* base, const void* exponent, const void* modulus) {
    // Every number is 0 modulo 1, but libtommath gives 1 for an exponent of 0.
    if (mp_cmp_d(from(modulus), 1) == MP_EQ) {
        return make();
    }

    // The base goes from 0 to below the modulus first, as libtommath's Montgomery code wants.
    number_t reduced;
    check(mp_mod(from(base), from(modulus), &reduced.value));
    mp_int* result = make();
    check(mp_exptmod(&reduced.value, from(exponent), from(modulus), result));
    return result;
}

void* fb_bigint_mod_inverse(const void* a, const void* modulus) {
    // Every number is 0 modulo 1, and 0 times anything is 1 modulo 1.
    if (mp_cmp_d(from(modulus), 1) == MP_EQ) {
        return make();
    }

    number_t reduced;
    check(mp_mod(from(a), from(modulus), &reduced.value));
    mp_int* result = make();
    const mp_err error = mp_invmod(&reduced.value, from(modulus), result);
    if (error == MP_VAL) {
        fb_bigint_free(result);
        return nullptr;
    }

    check(error);
    return result;
}

void* fb_bigint_gcd(const void* a, const void* b) {
    return apply(mp_gcd, a, b);
}

void* fb_bigint_lcm(const void* a, const void* b) {
    // libtommath divides by the gcd, which is 0 for two 0s.
    if (mp_iszero(from(a)) == MP_YES || mp_iszero(from(b)) == MP_YES) {
        return make();
    }
    return apply(mp_lcm, a, b);
}

void* fb_bigint_sqrt(const void* a) {
    return apply(mp_sqrt, a);
}

std::int32_t fb_bigint_is_probable_prime(const void* a) {
    const mp_int* number = from(a);
    if (mp_cmp_d(number, 2) == MP_LT) {
        return 0;
    }
    for (const mp_digit prime : small_primes) {
        if (mp_cmp_d(number, prime) == MP_EQ) {
            return 1;
        }

        mp_digit remainder = 0;
        check(mp_div_d(number, prime, nullptr, &remainder));
        if (remainder == 0) {
            return 0;
        }
    }

    // The Lucas test never ends on a square, which isn't prime anyway.
    mp_bool square = MP_NO;
    check(mp_is_square(number, &square));
    if (square == MP_YES) {
        return 0;
    }

    // A strong probable prime to base 2 that's a strong Lucas probable prime too.
    number_t two;
    mp_set(&two.value, 2);
    mp_bool prime = MP_NO;
    check(mp_prime_miller_rabin(number, &two.value, &prime));
    if (prime == MP_YES) {
        check(mp_prime_strong_lucas_selfridge(number, &prime));
    }
    return prime == MP_YES ? 1 : 0;
}
}
