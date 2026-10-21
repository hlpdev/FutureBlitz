#pragma once

#include "fb/ast.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/style.hpp"
#include "fb/token.hpp"

#include <span>
#include <string>
#include <string_view>

// Text views of the compiler's stages, for `fb --show` and for tests. MIR has its own dump() in
// mir.hpp.

namespace fb {

/// One token per line: its position, its kind, and its text. What `--show tokens` prints.
[[nodiscard]] std::string dump_tokens(const source_file_t& file, std::span<const token_t> tokens,
                                      const style_t& style = {});

/// The AST as S-expressions like "(if ready (block (call Go)))", one top-level statement per
/// line, without color. Parser tests compare against it.
[[nodiscard]] std::string dump_ast(const ast_t& ast);

/// The main module's AST as an indented tree, with each statement of a block on its own line.
/// Literals are plain, like "1" instead of "(int 1)". What `--show ast` prints. With `package`,
/// it's every module of the main module's package, as for a library, whose main module is a
/// placeholder.
[[nodiscard]] std::string dump_tree(const ast_t& ast, const style_t& style = style_t(),
                                    bool package = false);

/// dump_tree() with each expression's type, any conversion, and any known value, like
/// "x: Int32 → Int64 = 3". What `--show types` prints.
[[nodiscard]] std::string dump_types(const ast_t& ast, const analysis_t& analysis,
                                     const style_t& style = style_t(), bool package = false);

/// Colors LLVM IR text. Without color it comes back unchanged.
[[nodiscard]] std::string highlight_llvm(std::string_view ir, const style_t& style = {});

} // namespace fb
