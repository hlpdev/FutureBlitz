#include "fb/pattern.hpp"

#include "fb/literal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace fb {

namespace {

/// How many bytes `point` takes in UTF-8.
std::size_t utf8_length(const char32_t point) {
    if (point < 0x80) {
        return 1;
    }

    if (point < 0x800) {
        return 2;
    }

    return point < 0x10000 ? 3 : 4;
}

/// How many bytes the UTF-8 character that starts with `lead` takes.
std::size_t sequence_length(const char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    if (byte < 0xC0) {
        return 1;
    }

    if (byte < 0xE0) {
        return 2;
    }

    return byte < 0xF0 ? 3 : 4;
}

} // namespace

std::optional<pattern_problem_t> check_pattern(const std::string_view pattern,
                                               const std::int64_t flags) {
    // std.text.regex's RegexFlags, as runtime/src/regex.cpp takes them.
    std::uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C;
    const auto bits = static_cast<std::uint64_t>(flags);
    if ((bits & 1U) != 0) {
        options |= PCRE2_CASELESS;
    }

    if ((bits & 2U) != 0) {
        options |= PCRE2_MULTILINE;
    }

    if ((bits & 4U) != 0) {
        options |= PCRE2_DOTALL;
    }

    if ((bits & 8U) != 0) {
        options |= PCRE2_EXTENDED;
    }

    pcre2_compile_context* context = pcre2_compile_context_create(nullptr);
    pcre2_set_newline(context, PCRE2_NEWLINE_ANYCRLF);
    int error = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
                                     options, &error, &offset, context);
    pcre2_compile_context_free(context);
    if (code != nullptr) {
        pcre2_code_free(code);
        return std::nullopt;
    }

    std::array<PCRE2_UCHAR, 256> message{};
    const int length = pcre2_get_error_message(error, message.data(), message.size());
    return pattern_problem_t{
        .message = std::string(reinterpret_cast<const char*>(message.data()),
                               static_cast<std::size_t>(length < 0 ? 0 : length)),
        .offset = offset,
    };
}

std::optional<literal_place_t> literal_place(const std::string_view source,
                                             const std::size_t offset) {
    const bool raw = source.starts_with('r');
    const std::size_t open = raw ? 2 : 1;
    if (source.size() < open + 1 || source.substr(open - 1).starts_with(R"(""")")) {
        return std::nullopt;
    }

    // Each character's place in the source, and how many bytes it makes in the text, up to the
    // one that holds `offset`. One beyond ASCII is its UTF-8 bytes in both.
    const std::size_t close = source.size() - 1;
    std::size_t at = open;
    std::size_t text = 0;
    while (at < close) {
        std::size_t length = sequence_length(source[at]);
        std::size_t made = length;
        if (!raw && source[at] == '\\') {
            const escape_t escape = decode_escape(source, at);
            length = escape.length;
            made = utf8_length(escape.code_point);
        } else if (!raw && (source[at] == '{' || source[at] == '}')) {
            if (at + 1 == close || source[at + 1] != source[at]) {
                return std::nullopt;
            }

            length = 2;
            made = 1;
        }

        if (offset < text + made) {
            return literal_place_t{ .begin = at, .length = length };
        }

        at += length;
        text += made;
    }

    return literal_place_t{ .begin = close, .length = 1 };
}

} // namespace fb
