#pragma once

#include "fb/diagnostic.hpp"
#include "fb/lsp/query.hpp"
#include "fb/source.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fb::lsp {

/// A refactoring a selection offers: what the editor calls it, its code action's kind, and its
/// edits, each a span of a file of the check and its new text.
struct refactoring_t {
    std::string title;
    std::string kind;
    std::vector<fix_t> edits;
};

/// What `selection` in the query's file can be made into, each only where the code does the same
/// after it: the expression it is, extracted into a Local before its statement; the statements
/// it covers, extracted into a Function, or a Method of its Method's type; and the Local or
/// Function named where it starts, inlined.
[[nodiscard]] std::vector<refactoring_t> refactorings(const query_t& query, span_t selection);

/// The edits that leave the query's file's Imports sorted, std's first and quoted paths last,
/// without what its check's warnings say nothing uses. None when they are, or when a comment or
/// an #If is among them.
[[nodiscard]] std::vector<fix_t> organized_imports(const query_t& query);

/// The parameters of the Function or Method `function` as its declaration writes them, "a: Int,
/// b: Int = 2", for its signature to be changed, or why it can't be: what overrides or
/// implements another, is overridden, or is used as a value takes its parameters from elsewhere.
[[nodiscard]] std::expected<std::string, std::string> signature_parameters(const query_t& query,
                                                                           symbol_id_t function);

/// The edits, in the query's check, that give the function `key` names the parameters `written`
/// lists, as signature_parameters() writes them: a parameter by its name, kept, maybe moved; one
/// written "new: Type <- old", renamed; and a new one written "name: Type = value", whose value
/// every call passes. What isn't listed goes, with its arguments. Every call gets its arguments
/// in the new order, named after one that takes its default, and the function's code the new
/// names. Why not, when an argument that goes or moves past another may do something.
[[nodiscard]] std::expected<std::vector<fix_t>, std::string>
change_signature(const query_t& query, const symbol_key_t& key, std::string_view written);

} // namespace fb::lsp
