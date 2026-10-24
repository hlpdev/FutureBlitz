#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.data's functions in the runtime, for its formats' readers and writers, which work on a
// text's UTF-8 bytes. Arrays are only borrowed. std.data checks that `start` and `finish` are in
// the array, with `start` not past `finish`.

extern "C" {

/// A new String of the bytes from `start` up to `finish` of `bytes`, a UInt8[], with U+FFFD for
/// each byte that doesn't start well-formed UTF-8, as the runtime reads text from outside the
/// program.
fb_string_t* fb_data_text(const fb_array_t* bytes, std::int64_t start, std::int64_t finish);
/// The Double that the bytes from `start` up to `finish` of `bytes` write, the nearest one, in
/// `value`, and 1; or 0 when they aren't all a decimal number: an optional '-', digits with an
/// optional '.' and digits after, and an optional exponent, or "inf" or "nan". A number past
/// the largest Double is infinite, and one below the smallest is 0.
std::int32_t fb_data_float(const fb_array_t* bytes, std::int64_t start, std::int64_t finish,
                           double* value);
}
