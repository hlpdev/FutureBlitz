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

/// What lex() skips that the formatter places: the comments, and the lines from each outermost
/// #If to its #End If, which it leaves as they are.
struct trivia_t {
    /// Each comment, "//" up to its line break or "/*" through its "*/", in order. The ones in
    /// `conditionals` aren't here.
    std::vector<span_t> comments;
    /// From the start of an outermost #If's line to the end of its #End If's line, before the
    /// line break, or to the end of the file when it has none.
    std::vector<span_t> conditionals;
};

/// Like lex(), for the formatter: no branch of an #If gives tokens, whatever is defined, so the
/// tokens are the same on every platform, and `trivia` gets the comments and the #If lines.
[[nodiscard]] std::vector<token_t> lex_layout(const source_file_t& file,
                                              diagnostic_bag_t& diagnostics, trivia_t& trivia);

} // namespace fb
