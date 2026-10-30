#include "fb/token.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace fb {
namespace {

/// Indexed by token_kind_t, which lists the kinds in the same order.
constexpr std::array descriptions = {
#define FB_TOKEN(name, description) std::string_view(description),
#include "fb/token_kinds.def"
};

struct keyword_t {
    std::string_view spelling;
    token_kind_t kind;
};

/// Sorted by spelling, so keyword_kind() can binary-search it.
constexpr std::array keywords = {
#define FB_KEYWORD(name, spelling) keyword_t{ spelling, token_kind_t::name },
#include "fb/token_kinds.def"
};

static_assert(std::ranges::is_sorted(keywords, {}, &keyword_t::spelling),
              "keywords in token_kinds.def must stay sorted");

} // namespace

std::string_view describe(const token_kind_t kind) {
    return descriptions[std::to_underlying(kind)];
}

std::optional<token_kind_t> keyword_kind(const std::string_view text) {
    const auto it = std::ranges::lower_bound(keywords, text, {}, &keyword_t::spelling);
    if (it == keywords.end() || it->spelling != text) {
        return std::nullopt;
    }

    return it->kind;
}

std::optional<std::string_view> miscased_keyword(const std::string_view text) {
    const auto lower = [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    for (const keyword_t& keyword : keywords) {
        if (keyword.spelling != text && keyword.spelling.size() == text.size() &&
            std::ranges::equal(keyword.spelling, text, {}, lower, lower)) {
            return keyword.spelling;
        }
    }

    return std::nullopt;
}

} // namespace fb
