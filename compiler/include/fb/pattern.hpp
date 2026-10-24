#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fb {

/// Why a regex pattern isn't valid: PCRE2's message, and the byte of the pattern it's at.
struct pattern_problem_t {
    std::string message;
    std::size_t offset = 0;
};

/// Compiles `pattern` as std.text.regex does, with its RegexFlags `flags`, and says why it isn't
/// valid, or nullopt when it is. The runtime's fb_regex_new (runtime/src/regex.cpp) takes the same
/// options.
[[nodiscard]] std::optional<pattern_problem_t> check_pattern(std::string_view pattern,
                                                             std::int64_t flags);

/// Where a character of a string literal is in its source: its first byte, and how many it
/// takes there, like an escape's.
struct literal_place_t {
    std::size_t begin = 0;
    std::size_t length = 0;
};

/// Where the byte `offset` of a string literal's text is in `source`, the literal as written
/// with its quotes, for a literal on one line: a raw one's bytes are its text's, and a regular
/// one's escapes, "{{", and "}}" each make one character. Past its end, it's the closing quote.
/// nullopt for a multi-line string or one with an interpolation.
[[nodiscard]] std::optional<literal_place_t> literal_place(std::string_view source,
                                                           std::size_t offset);

/// Whether a string literal, as written in `source` with its quotes, is one the lexer took
/// without an error: it ends in its closing quote, and each escape of a regular one is one.
[[nodiscard]] bool literal_is_whole(std::string_view source);

} // namespace fb
