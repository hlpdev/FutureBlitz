#pragma once

#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <span>

namespace fb {

/// Builds the AST of a file from its tokens, which must end with END_OF_FILE. It always returns
/// an AST: errors are reported, broken code becomes error nodes, and constructs that come after
/// M1 are reported and skipped. Names in the AST view `file`'s text, so the file must outlive it.
[[nodiscard]] ast_t parse(const source_file_t& file, std::span<const token_t> tokens,
                          diagnostic_bag_t& diagnostics);

/// Like parse(), but adds the file's nodes and items to `ast`, which may hold other files of the
/// same program already. In a library's interface file, `interface_file`, a declaration can be
/// its first line and "=> ...", or for a Global, a Static Field, or a Class's Field, its type and
/// "= ...", when the library's binary has its code, and @MainThreadOnly says what that code
/// does.
void parse_into(const source_file_t& file, std::span<const token_t> tokens,
                diagnostic_bag_t& diagnostics, ast_t& ast, bool interface_file = false);

} // namespace fb
