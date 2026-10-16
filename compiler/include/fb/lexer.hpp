#pragma once

#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <vector>

namespace fb {

/// Splits a file into tokens, skipping spaces and comments, and reports errors to
/// `diagnostics`. It always finishes: bad input gives INVALID tokens or reported errors, and
/// the last token is END_OF_FILE.
[[nodiscard]] std::vector<token_t> lex(const source_file_t& file, diagnostic_bag_t& diagnostics);

} // namespace fb
