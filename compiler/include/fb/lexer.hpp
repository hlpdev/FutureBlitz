#pragma once

#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace fb {

/// Splits a file into tokens, skipping spaces and comments, and reports errors to
/// `diagnostics`. It always finishes: bad input gives INVALID tokens or reported errors, and
/// the last token is END_OF_FILE. The lines an #If leaves out give no tokens; `defined` has the
/// symbols that are true.
[[nodiscard]] std::vector<token_t> lex(const source_file_t& file, diagnostic_bag_t& diagnostics,
                                       std::span<const std::string_view> defined = {});

} // namespace fb
