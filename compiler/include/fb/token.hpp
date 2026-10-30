#pragma once

#include "fb/source.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace fb {

/// Every kind in token_kinds.def, in its order.
enum class token_kind_t : std::uint8_t {
#define FB_TOKEN(name, description) name,
#include "fb/token_kinds.def" // NOLINT(readability-trailing-comma)
};

/// A token's text is the source its span covers.
struct token_t {
    token_kind_t kind = token_kind_t::END_OF_FILE;
    span_t span;
};

/// How messages name a kind: "identifier", "'('", "'End'".
[[nodiscard]] std::string_view describe(token_kind_t kind);

/// The keyword spelled exactly `text`, or nullopt for anything else. Keywords are
/// case-sensitive, so "if" is an identifier.
[[nodiscard]] std::optional<token_kind_t> keyword_kind(std::string_view text);

/// The keyword `text` is but for its letter case, as written, like "If" for "if", or nullopt when
/// it's no keyword's or the keyword's own spelling.
[[nodiscard]] std::optional<std::string_view> miscased_keyword(std::string_view text);

} // namespace fb
