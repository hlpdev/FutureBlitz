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

/// The characters Unicode counts as cased beyond Lu, Ll, and Lt, which utf8proc doesn't say:
/// Other_Lowercase, like U+00AA and the modifier letters, and Other_Uppercase, like the Roman
/// numerals and circled letters, from Unicode's PropList.txt: each range's first and last
/// characters, in turn.
constexpr std::u32string_view other_cased =
    U"\u00AA\u00AA\u00BA\u00BA\u02B0\u02B8\u02C0\u02C1\u02E0\u02E4\u0345\u0345\u037A\u037A"
    U"\u10FC\u10FC\u1D2C\u1D6A\u1D78\u1D78\u1D9B\u1DBF\u2071\u2071\u207F\u207F\u2090\u209C"
    U"\u2160\u217F\u24B6\u24E9\u2C7C\u2C7D\uA69C\uA69D\uA770\uA770\uA7F2\uA7F4\uA7F8\uA7F9"
    U"\uAB5C\uAB5F\uAB69\uAB69\U00010780\U00010780\U00010783\U00010785\U00010787\U000107B0"
    U"\U000107B2\U000107BA\U0001E030\U0001E06D\U0001F130\U0001F149\U0001F150\U0001F169"
    U"\U0001F170\U0001F189";

/// Whether a character has case, as Unicode's Cased has it: an uppercase, lowercase, or
/// titlecase letter, or one of the others it counts, for the final sigma.
bool is_cased(const std::int32_t point) {
    const utf8proc_category_t category = category_of(point);
    if (category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL ||
        category == UTF8PROC_CATEGORY_LT) {
        return true;
    }

    for (std::size_t i = 0; i < other_cased.size(); i += 2) {
        if (static_cast<char32_t>(point) >= other_cased[i] &&
            static_cast<char32_t>(point) <= other_cased[i + 1]) {
            return true;
        }
    }

    return false;
}

/// Whether case mapping looks past a character to the letter before or after it, as Unicode's
/// Case_Ignorable has them: a mark, a format character, a modifier, or what Word_Break counts as
/// inside a word, an apostrophe, a period, or a colon, in each script's own forms.
bool is_case_ignorable(const std::int32_t point) {
    switch (category_of(point)) {
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_CF:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_SK:
        return true;
    default:
        break;
    }

    constexpr std::u32string_view inside_words =
        U"'.:\u00B7\u0387\u055F\u05F4\u2018\u2019\u2024\u2027\uFE13\uFE52\uFE55\uFF07\uFF0E\uFF1A";
    return inside_words.contains(static_cast<char32_t>(point));
}

/// Whether the capital sigma at `at` ends a word, so it lowers to final sigma, U+03C2, as
/// Unicode's Final_Sigma says: a cased character comes before it, and none after it, past what
/// case mapping looks past. A character that's both, like U+02B0, is looked past, as ICU and
/// Python do.
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

/// The columns a grapheme cluster takes: its widest character's, so a letter with its marks, or
/// emoji joined into one, is one character. A flag, two regional indicators, takes 2, and so
/// does an emoji presentation sequence, a pictograph or a keycap's 0 to 9, #, or * with U+FE0F
/// after it, which terminals draw as emoji though the character alone is 1 wide.
int cluster_width(const std::span<const std::int32_t> cluster) {
    constexpr std::int32_t first_indicator = 0x1F1E6;
    constexpr std::int32_t last_indicator = 0x1F1FF;
    constexpr std::int32_t emoji_selector = 0xFE0F;
    constexpr std::int32_t keycap = 0x20E3;
    int widest = 0;
    int indicators = 0;
    bool emoji = false;
    bool capped = false;
    for (const std::int32_t point : cluster) {
        widest = std::max(widest, utf8proc_charwidth(point));
        indicators += point >= first_indicator && point <= last_indicator ? 1 : 0;
        emoji = emoji || point == emoji_selector;
        capped = capped || point == keycap;
    }

    const std::int32_t base = cluster.front();
    const auto bound = static_cast<utf8proc_boundclass_t>(utf8proc_get_property(base)->boundclass);
    const bool pictograph =
        bound == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC || bound == UTF8PROC_BOUNDCLASS_E_ZWG;
    const bool keycap_base = (base >= '0' && base <= '9') || base == '#' || base == '*';
    if (indicators >= 2 || (emoji && (pictograph || (keycap_base && capped)))) {
        return std::max(widest, 2);
    }

    return widest;
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
    const std::vector<std::int32_t> points = points_of(text);
    std::int64_t width = 0;
    std::size_t start = 0;
    std::int32_t state = 0;
    for (std::size_t i = 1; i <= points.size(); ++i) {
        if (i == points.size() ||
            utf8proc_grapheme_break_stateful(points[i - 1], points[i], &state)) {
            width += cluster_width(std::span(points).subspan(start, i - start));
            start = i;
        }
    }

    return width;
}
}
