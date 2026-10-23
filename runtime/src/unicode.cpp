#include "fb/runtime.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utf8proc.h>
#include <vector>

// What std.text needs of Unicode's data, over utf8proc: categories, case, normalization,
// grapheme clusters, and the columns a character takes. The fb_unicode_ functions are an archive
// of their own, so only a program that calls them links utf8proc.

using fb::runtime::decode_utf8;
using fb::runtime::decoded_t;
using fb::runtime::encode_utf8;
using fb::runtime::new_string;
using fb::runtime::text_of;

namespace {

/// A String's code points. Each String is UTF-8, so none is U+FFFD unless it's written there.
std::vector<std::int32_t> points_of(const fb_string_t* text) {
    const std::string_view bytes = text_of(text);
    std::vector<std::int32_t> points;
    points.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        const decoded_t decoded = decode_utf8(bytes, i);
        points.push_back(static_cast<std::int32_t>(decoded.point));
        i += decoded.length;
    }

    return points;
}

/// A new String of `points` in UTF-8.
fb_string_t* string_of(const std::span<const std::int32_t> points) {
    std::string text;
    text.reserve(points.size());
    for (const std::int32_t point : points) {
        text += encode_utf8(static_cast<std::uint32_t>(point));
    }

    return new_string(text);
}

utf8proc_category_t category_of(const std::int32_t point) {
    return utf8proc_category(point);
}

/// What `point` folds to, for case-insensitive comparison: one code point, or more, as U+00DF's
/// "ss" is.
std::vector<std::int32_t> folded(const std::int32_t point) {
    constexpr std::size_t longest = 4;
    std::array<std::int32_t, longest> sequence{};
    std::int32_t boundary = 0;
    const utf8proc_ssize_t count =
        utf8proc_decompose_char(point, sequence.data(), longest, UTF8PROC_CASEFOLD, &boundary);
    if (count <= 0) {
        return { point };
    }

    return { sequence.begin(), sequence.begin() + count };
}

/// Whether a letter has case: an uppercase, lowercase, or titlecase one, for the final sigma.
bool is_cased(const std::int32_t point) {
    const utf8proc_category_t category = category_of(point);
    return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL ||
           category == UTF8PROC_CATEGORY_LT;
}

/// Whether case mapping looks past a character to the letter before or after it: a mark, a
/// format character, a modifier, or an apostrophe or period inside a word, as Unicode's
/// Case_Ignorable has them.
bool is_case_ignorable(const std::int32_t point) {
    switch (category_of(point)) {
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_CF:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_SK:
        return true;
    default:
        return point == '\'' || point == '.' || point == ':' || point == 0x00B7 ||
               point == 0x2018 || point == 0x2019 || point == 0x2024 || point == 0x2027;
    }
}

/// Whether the capital sigma at `at` ends a word, so it lowers to final sigma, U+03C2: a cased
/// letter comes before it, and none after it, past what case mapping looks past.
bool is_final_sigma(const std::span<const std::int32_t> points, const std::size_t at) {
    std::size_t before = at;
    while (before > 0 && is_case_ignorable(points[before - 1])) {
        --before;
    }

    if (before == 0 || !is_cased(points[before - 1])) {
        return false;
    }

    std::size_t after = at + 1;
    while (after < points.size() && is_case_ignorable(points[after])) {
        ++after;
    }

    return after == points.size() || !is_cased(points[after]);
}

} // namespace

extern "C" {

std::int32_t fb_unicode_category(const std::uint32_t point) {
    return category_of(static_cast<std::int32_t>(point));
}

std::uint32_t fb_unicode_upper(const std::uint32_t point) {
    // utf8proc gives U+00DF the capital U+1E9E, as musl does, but Unicode's own data gives it
    // none: its uppercase is "SS", which one code point can't hold.
    constexpr std::uint32_t sharp_s = 0x00DF;
    if (point == sharp_s) {
        return point;
    }

    return static_cast<std::uint32_t>(utf8proc_toupper(static_cast<std::int32_t>(point)));
}

std::uint32_t fb_unicode_lower(const std::uint32_t point) {
    return static_cast<std::uint32_t>(utf8proc_tolower(static_cast<std::int32_t>(point)));
}

fb_string_t* fb_unicode_to_upper(const fb_string_t* text) {
    // utf8proc has each character's own uppercase, one code point. The ones that become more,
    // like U+00DF's "SS" or U+FB01's "FI", are the lowercase and titlecase letters that fold to
    // more: their uppercase is that of what they fold to, as Unicode's SpecialCasing has it, even
    // where utf8proc gives U+00DF the capital U+1E9E of its own.
    std::vector<std::int32_t> upper;
    for (const std::int32_t point : points_of(text)) {
        const std::vector<std::int32_t> fold = folded(point);
        if (fold.size() == 1 || category_of(point) == UTF8PROC_CATEGORY_LU) {
            upper.push_back(utf8proc_toupper(point));
            continue;
        }

        for (const std::int32_t part : fold) {
            upper.push_back(utf8proc_toupper(part));
        }
    }

    return string_of(upper);
}

fb_string_t* fb_unicode_to_lower(const fb_string_t* text) {
    // Beside each character's own lowercase, U+0130's is "i" with a dot above, and a capital
    // sigma that ends a word lowers to the final sigma.
    constexpr std::int32_t capital_i_dot = 0x0130;
    constexpr std::int32_t capital_sigma = 0x03A3;
    constexpr std::int32_t final_sigma = 0x03C2;
    constexpr std::int32_t dot_above = 0x0307;
    const std::vector<std::int32_t> points = points_of(text);
    std::vector<std::int32_t> lower;
    lower.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i] == capital_i_dot) {
            lower.insert(lower.end(), { 'i', dot_above });
        } else if (points[i] == capital_sigma && is_final_sigma(points, i)) {
            lower.push_back(final_sigma);
        } else {
            lower.push_back(utf8proc_tolower(points[i]));
        }
    }

    return string_of(lower);
}

fb_string_t* fb_unicode_case_fold(const fb_string_t* text) {
    std::vector<std::int32_t> fold;
    for (const std::int32_t point : points_of(text)) {
        const std::vector<std::int32_t> parts = folded(point);
        fold.insert(fold.end(), parts.begin(), parts.end());
    }

    return string_of(fold);
}

fb_string_t* fb_unicode_normalize(const fb_string_t* text, const std::int32_t form) {
    // NFC, NFD, NFKC, and NFKD, in std.text.unicode's NormalForm's order.
    constexpr auto stable = static_cast<unsigned>(UTF8PROC_STABLE);
    constexpr auto compose = static_cast<unsigned>(UTF8PROC_COMPOSE);
    constexpr auto decompose = static_cast<unsigned>(UTF8PROC_DECOMPOSE);
    constexpr auto compat = static_cast<unsigned>(UTF8PROC_COMPAT);
    constexpr std::array<unsigned, 4> options{
        stable | compose,
        stable | decompose,
        stable | compose | compat,
        stable | decompose | compat,
    };
    const std::string_view bytes = text_of(text);
    utf8proc_uint8_t* normal = nullptr;
    const utf8proc_ssize_t length =
        utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(bytes.data()),
                     static_cast<utf8proc_ssize_t>(bytes.size()), &normal,
                     static_cast<utf8proc_option_t>(options.at(static_cast<std::size_t>(form))));
    if (length < 0) {
        fb_panic("out of memory", nullptr);
    }

    fb_string_t* result =
        new_string({ reinterpret_cast<const char*>(normal), static_cast<std::size_t>(length) });
    utf8proc_free(normal);
    return result;
}

std::int64_t fb_unicode_next_grapheme(const fb_string_t* text, const std::int64_t at) {
    // Where the grapheme cluster that starts at `at` ends, by UAX #29's rules, whose state goes
    // along from one pair of characters to the next.
    const std::string_view bytes = text_of(text);
    auto end = static_cast<std::size_t>(at);
    std::int32_t state = 0;
    decoded_t previous = decode_utf8(bytes, end);
    end += previous.length;
    while (end < bytes.size()) {
        const decoded_t next = decode_utf8(bytes, end);
        if (utf8proc_grapheme_break_stateful(static_cast<std::int32_t>(previous.point),
                                             static_cast<std::int32_t>(next.point), &state)) {
            break;
        }

        end += next.length;
        previous = next;
    }

    return static_cast<std::int64_t>(end);
}

std::int64_t fb_unicode_grapheme_count(const fb_string_t* text) {
    const std::vector<std::int32_t> points = points_of(text);
    std::int64_t count = points.empty() ? 0 : 1;
    std::int32_t state = 0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (utf8proc_grapheme_break_stateful(points[i - 1], points[i], &state)) {
            ++count;
        }
    }

    return count;
}

std::int32_t fb_unicode_char_width(const std::uint32_t point) {
    return utf8proc_charwidth(static_cast<std::int32_t>(point));
}

std::int64_t fb_unicode_display_width(const fb_string_t* text) {
    // A grapheme cluster takes the columns of its widest character, so an emoji that joins
    // others, or a letter with its marks, is one wide character; a flag, two regional
    // indicators, is two columns too.
    constexpr std::int32_t first_indicator = 0x1F1E6;
    constexpr std::int32_t last_indicator = 0x1F1FF;
    const std::vector<std::int32_t> points = points_of(text);
    std::int64_t width = 0;
    int cluster = 0;
    std::int32_t state = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i > 0 && utf8proc_grapheme_break_stateful(points[i - 1], points[i], &state)) {
            width += cluster;
            cluster = 0;
        }

        const bool indicator = points[i] >= first_indicator && points[i] <= last_indicator;
        cluster = std::max(cluster, indicator ? 2 : utf8proc_charwidth(points[i]));
    }

    return width + cluster;
}
}
