// std.math.big's Decimal over mpdecimal: decimal numbers that hold what they're made from
// exactly, like 0.1, and work out the rest in 28 significant digits, as Python's decimal does by
// default. Each is an mpd_t of its own, always finite, which never changes once it's made.

#include "fb/big.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fast_float/fast_float.h>
#include <limits>
#include <mpdecimal.h>
#include <string>
#include <string_view>
#include <system_error>

namespace {

/// What the arithmetic works in: Python's decimal's default context, 28 digits rounded half to
/// even and exponents from -999999 to 999999.
mpd_context_t arithmetic() {
    mpd_context_t context{};
    mpd_defaultcontext(&context);
    context.prec = 28;
    context.emax = 999999;
    context.emin = -999999;
    context.round = MPD_ROUND_HALF_EVEN;
    return context;
}

/// What exact work is done in: as many digits and as large an exponent as mpdecimal holds, so
/// nothing rounds.
mpd_context_t exact() {
    mpd_context_t context{};
    mpd_maxcontext(&context);
    return context;
}

/// The digits that rounding to fit `a`, or `a` and `b`, keeps: 28, or more when they have more.
mpd_context_t fitting(const mpd_t* a, const mpd_t* b = nullptr) {
    mpd_context_t context = exact();
    context.prec = std::max<mpd_ssize_t>({ 28, a->digits, b != nullptr ? b->digits : 0 });
    return context;
}

/// mpdecimal's rounding for each of std.math.big's RoundingModes.
constexpr std::array<int, 7> roundings = {
    MPD_ROUND_HALF_EVEN, MPD_ROUND_HALF_UP, MPD_ROUND_HALF_DOWN, MPD_ROUND_UP,
    MPD_ROUND_DOWN,      MPD_ROUND_CEILING, MPD_ROUND_FLOOR,
};

mpd_t* make() {
    mpd_t* result = mpd_qnew();
    if (result == nullptr) {
        fb_panic("out of memory", nullptr);
    }
    return result;
}

const mpd_t* from(const void* a) {
    return static_cast<const mpd_t*>(a);
}

/// Panics when `status` says memory ran out.
void check(const std::uint32_t status) {
    if ((status & MPD_Malloc_error) != 0) {
        fb_panic("out of memory", nullptr);
    }
}

/// `result`, or null when `status` says it isn't a finite number in range, after freeing it.
void* finish(mpd_t* result, const std::uint32_t status) {
    check(status);
    constexpr std::uint32_t failed =
        MPD_IEEE_Invalid_operation | MPD_Division_by_zero | MPD_Overflow;
    if ((status & failed) != 0 || mpd_isfinite(result) == 0) {
        mpd_del(result);
        return nullptr;
    }
    return result;
}

using unary_t = void (*)(mpd_t*, const mpd_t*, const mpd_context_t*, std::uint32_t*);
using binary_t = void (*)(mpd_t*, const mpd_t*, const mpd_t*, const mpd_context_t*, std::uint32_t*);

/// A new mpd_t that `function` sets from `a`, or from `a` and `b`, in `context`, or null when it
/// isn't a finite number in range.
void* apply(const unary_t function, const void* a, const mpd_context_t& context) {
    mpd_t* result = make();
    std::uint32_t status = 0;
    function(result, from(a), &context, &status);
    return finish(result, status);
}

void* apply(const binary_t function, const void* a, const void* b, const mpd_context_t& context) {
    mpd_t* result = make();
    std::uint32_t status = 0;
    function(result, from(a), from(b), &context, &status);
    return finish(result, status);
}

/// A new mpd_t of `text`, exactly, or null when it isn't a finite number that fits.
void* parse(const char* text) {
    mpd_t* result = make();
    std::uint32_t status = 0;
    mpd_qset_string_exact(result, text, &status);
    return finish(result, status);
}

/// A new String of `text`, which mpdecimal made, and which this frees.
fb_string_t* take(char* text) {
    if (text == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    fb_string_t* string = fb_string_from_cstring(text);
    mpd_free(text);
    return string;
}

} // namespace

extern "C" {

void* fb_decimal_from_int(const std::int64_t value) {
    mpd_t* result = make();
    std::uint32_t status = 0;
    mpd_qset_i64_exact(result, value, &status);
    check(status);
    return result;
}

void* fb_decimal_from_double(const double value) {
    std::array<char, 64> text{};
    std::to_chars(text.data(), text.data() + text.size() - 1, value);
    return parse(text.data());
}

void* fb_decimal_parse(const fb_string_t* text) {
    // mpdecimal reads up to a NUL, so text with one in it isn't a number.
    const std::string_view view(reinterpret_cast<const char*>(text + 1),
                                static_cast<std::size_t>(text->length));
    if (view.contains('\0')) {
        return nullptr;
    }

    const std::string copy(view);
    return parse(copy.c_str());
}

void fb_decimal_free(void* a) {
    mpd_del(static_cast<mpd_t*>(a));
}

fb_string_t* fb_decimal_to_string(const void* a) {
    return take(mpd_to_sci(from(a), 1));
}

fb_string_t* fb_decimal_to_plain_string(const void* a) {
    const mpd_context_t context = exact();
    std::uint32_t status = 0;
    char* text = mpd_qformat(from(a), "f", &context, &status);
    check(status);
    return take(text);
}

double fb_decimal_to_double(const void* a) {
    // The text reads as the Double nearest it, unless that's past the largest Double or below the
    // smallest above 0. fast_float reads it, as std::from_chars would where the C++ library has it
    // for floats; Apple's libc++ doesn't.
    char* text = mpd_to_sci(from(a), 1);
    if (text == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    const std::string_view view(text);
    double value = 0;
    if (fast_float::from_chars(view.data(), view.data() + view.size(), value).ec ==
        std::errc::result_out_of_range) {
        value = mpd_adjexp(from(a)) > 0 ? std::numeric_limits<double>::infinity() : 0.0;
        value = mpd_isnegative(from(a)) != 0 ? -value : value;
    }
    mpd_free(text);
    return value;
}

std::uint64_t fb_decimal_hash(const void* a) {
    // Equal Decimals, like 1.0 and 1.00, reduce to one form, its trailing 0s gone, whose sign,
    // exponent, and digits go into the hash. Every 0 is equal, whatever its sign or exponent.
    if (mpd_iszero(from(a)) != 0) {
        return fb_hash_integer(0);
    }

    mpd_t* reduced = make();
    const mpd_context_t context = exact();
    std::uint32_t status = 0;
    mpd_qreduce(reduced, from(a), &context, &status);
    check(status);
    std::uint64_t hash = fb_hash_combine(fb_hash_integer(mpd_isnegative(reduced) != 0 ? 1 : 0),
                                         fb_hash_integer(static_cast<std::uint64_t>(reduced->exp)));
    for (mpd_ssize_t i = 0; i < reduced->len; i++) {
        hash = fb_hash_combine(hash, fb_hash_integer(reduced->data[i]));
    }
    mpd_del(reduced);
    return hash;
}

std::int32_t fb_decimal_compare(const void* a, const void* b) {
    std::uint32_t status = 0;
    return mpd_qcmp(from(a), from(b), &status);
}

std::int32_t fb_decimal_sign(const void* a) {
    if (mpd_iszero(from(a)) != 0) {
        return 0;
    }
    return mpd_isnegative(from(a)) != 0 ? -1 : 1;
}

std::int32_t fb_decimal_is_integer(const void* a) {
    return mpd_isinteger(from(a)) != 0 ? 1 : 0;
}

std::int32_t fb_decimal_fits_int64(const void* a) {
    std::uint32_t status = 0;
    static_cast<void>(mpd_qget_i64(from(a), &status));
    return status == 0 ? 1 : 0;
}

std::int64_t fb_decimal_to_int64(const void* a) {
    std::uint32_t status = 0;
    return mpd_qget_i64(from(a), &status);
}

void* fb_decimal_add(const void* a, const void* b) {
    return apply(mpd_qadd, a, b, arithmetic());
}

void* fb_decimal_subtract(const void* a, const void* b) {
    return apply(mpd_qsub, a, b, arithmetic());
}

void* fb_decimal_multiply(const void* a, const void* b) {
    return apply(mpd_qmul, a, b, arithmetic());
}

void* fb_decimal_divide(const void* a, const void* b) {
    return apply(mpd_qdiv, a, b, arithmetic());
}

void* fb_decimal_power(const void* a, const void* exponent) {
    return apply(mpd_qpow, a, exponent, arithmetic());
}

void* fb_decimal_sqrt(const void* a) {
    return apply(mpd_qsqrt, a, arithmetic());
}

void* fb_decimal_exp(const void* a) {
    return apply(mpd_qexp, a, arithmetic());
}

void* fb_decimal_ln(const void* a) {
    return apply(mpd_qln, a, arithmetic());
}

void* fb_decimal_log10(const void* a) {
    return apply(mpd_qlog10, a, arithmetic());
}

void* fb_decimal_remainder(const void* a, const void* b) {
    return apply(mpd_qrem, a, b, fitting(from(a), from(b)));
}

void* fb_decimal_negate(const void* a) {
    return apply(mpd_qminus, a, exact());
}

void* fb_decimal_abs(const void* a) {
    return apply(mpd_qabs, a, exact());
}

void* fb_decimal_round(const void* a, const std::int64_t places, const std::int32_t mode) {
    if (places > MPD_MAX_EMAX || places < -MPD_MAX_EMAX) {
        return nullptr;
    }

    // Going to more places pads the coefficient with 0s, which may not take it past the digits
    // kept, as Python's quantize says. mpdecimal's rescale doesn't check that, and would make
    // them all.
    mpd_context_t context = fitting(from(a));
    const mpd_t* number = from(a);
    if (mpd_iszero(number) == 0 && number->exp + places > 0 &&
        number->digits + number->exp + places > context.prec) {
        return nullptr;
    }

    context.round = roundings.at(static_cast<std::size_t>(mode));
    mpd_t* result = make();
    std::uint32_t status = 0;
    mpd_qrescale(result, from(a), -places, &context, &status);
    return finish(result, status);
}

void* fb_decimal_round_whole(const void* a, const std::int32_t mode) {
    mpd_context_t context = exact();
    context.round = roundings.at(static_cast<std::size_t>(mode));
    return apply(mpd_qround_to_int, a, context);
}
}
