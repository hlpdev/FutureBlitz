#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.data's functions in the runtime: JSON's tokens, read one at a time from text, as RFC 8259
// has them, and the text JSON writes for a String and a Double. Arrays and Strings are only
// borrowed.
//
// A token is 0 at the end, 1 an object's start and 2 its end, 3 an array's start and 4 its end, 5
// a key, 6 a string, 7 an integer that fits an Int64, 8 one past an Int64 that fits a UInt64, 9
// any other number, as a Double, 10 true, 11 false, and 12 null; or, when the text isn't JSON,
// -1 for what can't be there, -2 for text that ends too soon, and -3 for a number past a
// Double's range, or arrays and objects nested past `depth`.

extern "C" {

/// A reader of `text`'s JSON, or of `bytes`', which must be UTF-8, after a byte order mark if
/// there's one, that fb_json_reader_free lets go of. It reads arrays and objects nested at most
/// `depth` deep.
void* fb_json_reader_new(const fb_string_t* text, std::int64_t depth);
void* fb_json_reader_bytes(const fb_array_t* bytes, std::int64_t depth);
void fb_json_reader_free(void* reader);
/// The next token, and the same one again at the end, or after the text turned out not to be
/// JSON.
std::int32_t fb_json_next(void* reader);
/// The token's text, a key's or a string's, its integer or Double, and how many arrays and
/// objects hold it.
fb_string_t* fb_json_text(void* reader);
std::int64_t fb_json_int(void* reader);
std::uint64_t fb_json_uint(void* reader);
double fb_json_double(void* reader);
std::int64_t fb_json_depth(void* reader);
/// Where the token starts, or what isn't JSON is, from 1, its column in code points, and what's
/// wrong there.
std::int64_t fb_json_line(void* reader);
std::int64_t fb_json_column(void* reader);
fb_string_t* fb_json_error(void* reader);

/// `text` as a JSON string: in quotes, with `"`, `\`, and each control character escaped.
fb_string_t* fb_json_quote(const fb_string_t* text);
/// `value` as JSON's shortest text that reads back as it, with ".0" after one that would read as
/// an integer, or null for NaN or an infinity, which JSON doesn't have.
fb_string_t* fb_json_number(double value);

/// `bytes` in base64 (RFC 4648's alphabet, with its '=' padding), as JSON writes bytes.
fb_string_t* fb_data_base64(const fb_array_t* bytes);
/// The `length` bytes of `bytes` from `start` as a new String, or null when they aren't
/// well-formed UTF-8, for MessagePack's strings.
fb_string_t* fb_data_utf8(const fb_array_t* bytes, std::int64_t start, std::int64_t length);
}
