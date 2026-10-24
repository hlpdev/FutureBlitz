// std.data's functions in the runtime: text from part of a UInt8[], and a Double from text, by
// from_chars, which rounds to the nearest and doesn't depend on the locale.

#include "fb/data.hpp"

#include "fb/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>

namespace {

/// Whether the decimal number in [first, last), which from_chars found out of a Double's range,
/// is past its largest, rather than below its smallest: whether its first digit that isn't 0 is
/// left of the point once its exponent moves it.
bool too_large(const char* first, const char* last) {
    std::int64_t digits = 0;
    std::int64_t whole = 0;
    std::int64_t leading = -1;
    bool after_point = false;
    const char* at = first;
    for (; at != last && *at != 'e' && *at != 'E'; ++at) {
        if (*at == '.') {
            after_point = true;
        } else {
            if (leading == -1 && *at != '0') {
                leading = digits;
            }
            ++digits;
            if (!after_point) {
                ++whole;
            }
        }
    }

    std::int64_t exponent = 0;
    if (at != last) {
        ++at;
        const bool negative = at != last && *at == '-';
        if (at != last && (*at == '-' || *at == '+')) {
            ++at;
        }
        for (; at != last; ++at) {
            exponent = std::min<std::int64_t>((exponent * 10) + (*at - '0'), 1'000'000'000);
        }
        if (negative) {
            exponent = -exponent;
        }
    }

    // The leading digit is worth 10 to the power of the digits before the point, less one, less
    // how many digits come before it, then plus the exponent.
    return leading != -1 && whole - 1 - leading + exponent >= 0;
}

} // namespace

extern "C" {

fb_string_t* fb_data_text(const fb_array_t* bytes, const std::int64_t start,
                          const std::int64_t finish) {
    const auto* data = static_cast<const char*>(bytes->data);
    return fb_string_from_bytes(data + start, finish - start);
}

std::int32_t fb_data_float(const fb_array_t* bytes, const std::int64_t start,
                           const std::int64_t finish, double* value) {
    const auto* first = static_cast<const char*>(bytes->data) + start;
    const auto* last = static_cast<const char*>(bytes->data) + finish;
    const bool negative = first != last && *first == '-';
    const std::from_chars_result read = std::from_chars(first, last, *value);
    if (read.ptr != last) {
        return 0;
    }

    if (read.ec == std::errc::result_out_of_range) {
        const double size = too_large(first + (negative ? 1 : 0), last)
                                ? std::numeric_limits<double>::infinity()
                                : 0.0;
        *value = negative ? -size : size;
        return 1;
    }
    return read.ec == std::errc() ? 1 : 0;
}
}
