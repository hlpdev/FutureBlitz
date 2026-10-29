#pragma once

#include "fb/source.hpp"

#include <cstdint>
#include <string>

namespace fb::lsp {

/// The documentation of the declaration that starts at `begin` in `file`, as Markdown: the
/// comments on the lines right above it, "//" and "///" lines or a "/* */" block, without their
/// markers, or without any, one after the code on its line. Empty when it has none.
///
/// This is the one place the LSP reads docs from, so step 8's `///` comments, which the AST will
/// hold, can take its place.
[[nodiscard]] std::string doc_comment(const source_file_t& file, std::uint32_t begin);

} // namespace fb::lsp
