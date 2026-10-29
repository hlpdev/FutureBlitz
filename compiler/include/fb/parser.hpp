#pragma once

#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace fb {

/// Builds the AST of a file from its tokens, which must end with END_OF_FILE. It always returns
/// an AST: errors are reported, broken code becomes error nodes, and constructs that come after
/// M1 are reported and skipped. Names in the AST view `file`'s text, so the file must outlive it.
[[nodiscard]] ast_t parse(const source_file_t& file, std::span<const token_t> tokens,
                          diagnostic_bag_t& diagnostics);

/// Like parse(), but adds the file's nodes and items to `ast`, which may hold other files of the
/// same program already. In a library's interface file, `interface_file`, a declaration can be
/// its first line and "=> ...", or for a Global or a Static Field, its type and "= ...", when the
/// library's binary has its code, and @MainThreadOnly says what that code does.
void parse_into(const source_file_t& file, std::span<const token_t> tokens,
                diagnostic_bag_t& diagnostics, ast_t& ast, bool interface_file = false);

/// What a token is to the formatter, where its spelling alone doesn't say.
enum class token_role_t : std::uint8_t {
    /// A binary operator, or a piece of one written as two, like ">>".
    BINARY,
    /// A prefix operator: '-', '!', or '~'.
    UNARY,
    /// The '<' or '>' around type arguments or type parameters.
    GENERIC,
    /// The '?' or ':' of a ternary.
    TERNARY,
};

/// What the formatter learns from a parse besides the AST. Tokens are named by where they begin,
/// and a '>' of a ">=" is the ">=" token's begin.
struct layout_t {
    /// Each line's first token: how many blocks it's in, where a Select's Cases and a Property's
    /// Get and Set are a block's, and a Where line is in its declaration's.
    std::unordered_map<std::uint32_t, std::uint32_t> depths;
    /// The NEWLINE tokens that end a statement. The others only go on with one.
    std::unordered_set<std::uint32_t> line_ends;
    /// Where the tokens that a line break may follow outside brackets end: an operator, '=', ',',
    /// or "=>". Inside brackets, one may come anywhere.
    std::unordered_set<std::uint32_t> breaks;
    std::unordered_map<std::uint32_t, token_role_t> roles;
};

/// Like parse(), and fills `layout`.
[[nodiscard]] ast_t parse_layout(const source_file_t& file, std::span<const token_t> tokens,
                                 diagnostic_bag_t& diagnostics, layout_t& layout);

} // namespace fb
