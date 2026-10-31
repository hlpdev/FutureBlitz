#pragma once

#include "fb/driver.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fb::lsp {

/// A published diagnostic as the LSP has it, with its notes after its message, and its labels
/// as related information.
[[nodiscard]] json_t diagnostic_json(const compilation_t& compilation,
                                     const diagnostic_t& diagnostic, const positions_t& positions);

/// textDocument/hover: what the name at `offset` is, with its docs, or the type of the
/// expression there. Null when there's nothing.
[[nodiscard]] json_t hover(const query_t& query, std::uint32_t offset,
                           const positions_t& positions);

/// textDocument/definition: where what's named at `offset` is declared, or the file an Import
/// there names. Null when it's nowhere.
[[nodiscard]] json_t definition(const query_t& query, std::uint32_t offset,
                                const positions_t& positions);

/// Where each of `compilations` names the symbols `keys`, their declarations with them when
/// `declaration`, each once, as LSP Locations, in the files `keep` says to.
[[nodiscard]] json_t references(std::span<const std::shared_ptr<const compilation_t>> compilations,
                                std::span<const symbol_key_t> keys, bool declaration,
                                const std::function<bool(const std::filesystem::path&)>& keep,
                                const positions_t& positions);

/// What a rename of `key` changes with it, found in `compilations`: the Methods and Properties
/// that override it or that it overrides, and those that implement the Interface member it is or
/// implements, each way, with `key` itself, and the files they're declared in, which other
/// compilations may have more of. Making its set can allocate, which clang-tidy reports as an
/// exception escaping.
struct rename_set_t { // NOLINT(bugprone-exception-escape)
    std::vector<symbol_key_t> keys;
    std::set<std::filesystem::path> files;
};

[[nodiscard]] rename_set_t
rename_set(std::span<const std::shared_ptr<const compilation_t>> compilations,
           const symbol_key_t& key);

/// Why renaming `keys` to `name` would make code mean something else, or not check: another
/// declaration of `name` where they're declared, a member of `name` in their type, or a name
/// `name` that code where they're used sees. nullopt when nothing would.
[[nodiscard]] std::optional<std::string>
rename_conflict(std::span<const std::shared_ptr<const compilation_t>> compilations,
                std::span<const symbol_key_t> keys, const std::string& name);

/// The names code at `offset` sees: its variables, its file's and Imports' declarations, the
/// builtins, and the built-in types.
[[nodiscard]] std::set<std::string> visible_names(const query_t& query, std::uint32_t offset);

/// textDocument/rename's WorkspaceEdit: each of `references`, LSP Locations, becomes `name`.
[[nodiscard]] json_t rename_edit(const json_t& references, const std::string& name);

/// Whether `name` can be a declaration's name: an identifier that isn't a keyword.
[[nodiscard]] bool is_identifier(const std::string& name);

/// textDocument/documentSymbol: the file's declarations, as DocumentSymbols, each type's members
/// inside it. Only the file is parsed, so it's there before a check.
[[nodiscard]] json_t document_symbols(const source_file_t& file, const positions_t& positions);

/// workspace/symbol's SymbolInformation for a file's declarations whose names have `query`'s
/// letters in order, as fuzzy_match() says, as `uri`.
[[nodiscard]] json_t workspace_symbols(const source_file_t& file, const std::string& uri,
                                       const std::string& query, const positions_t& positions);

/// Whether `name` has `query`'s letters in order, ignoring ASCII case.
[[nodiscard]] bool fuzzy_match(std::string_view name, std::string_view query);

/// A Public name a module declares at its top level, which another file can import the module
/// for.
struct export_t {
    std::string name;
    /// What an Import names to bring it in, like "std.collections" or "geo.shapes".
    std::string module;
    /// The keyword it's declared with, as std_name_t's.
    std::string kind;
};

/// The innermost call whose parentheses a position is in: the call, which of its arguments the
/// position is in, and the functions it can be, with the one it is, or the first that takes that
/// many arguments.
struct call_at_t {
    expr_id_t call{};
    std::uint32_t active = 0;
    std::vector<symbol_id_t> candidates;
    std::size_t chosen = 0;
};

/// The call at `offset`, nullopt when it's in none.
[[nodiscard]] std::optional<call_at_t> call_at(const query_t& query, std::uint32_t offset);

/// textDocument/completion at `offset` of `text`, the document as it is, whose last check is
/// `query`: a member after a '.', a module after Import, or the names in scope and keywords.
/// `modules` gives the modules an Import can name, which it asks for only at an Import, and
/// `importable` the names the file could import a module for, which an item adds the Import of.
[[nodiscard]] json_t completion(const query_t& query, std::uint32_t offset,
                                const std::function<std::vector<std::string>()>& modules,
                                const std::function<std::vector<export_t>()>& importable,
                                const positions_t& positions);

/// textDocument/signatureHelp for the call `offset` is in.
[[nodiscard]] json_t signature_help(const query_t& query, std::uint32_t offset,
                                    const positions_t& positions);

/// The legend semantic_tokens() uses, for the server's capabilities.
[[nodiscard]] json_t semantic_tokens_legend();

/// textDocument/semanticTokens/full: the file's names, as what they name, and its keywords,
/// strings, numbers, and comments.
[[nodiscard]] json_t semantic_tokens(const query_t& query, const positions_t& positions);

/// textDocument/codeAction: a quick fix for each diagnostic in the file that has fix-its, of
/// those that `range`, offsets in the file, touches.
[[nodiscard]] json_t code_actions(const query_t& query, span_t range, const positions_t& positions);

/// textDocument/formatting's edits: the whole document as format_source() has it, or none when
/// it's formatted already or doesn't parse.
[[nodiscard]] json_t formatting(const source_file_t& file, std::uint32_t width,
                                const positions_t& positions);

/// A Location in a compilation's file, or nullopt for one that isn't on disk. A prebuilt
/// library's module is in its interface file, at the same lines, a module as a whole at its first.
[[nodiscard]] std::optional<json_t> location_of(const compilation_t& compilation, span_t span,
                                                const positions_t& positions);

/// textDocument/codeLens, unresolved: one at each declaration's name, whose data is `uri` and its
/// position, which codeLens/resolve counts the references of. Only the file is parsed.
[[nodiscard]] json_t code_lenses(const source_file_t& file, const std::string& uri,
                                 const positions_t& positions);

/// textDocument/foldingRange: a file's blocks, an If's branches, a Select's Cases, and a Try's
/// Catches each on its own, its lambdas, arrays, and Maps over more than a line, its runs of
/// comments and of Imports, and its #If blocks, each from its first line to the one before its
/// End. Only the file is parsed, so it's there before a check.
[[nodiscard]] json_t folding_ranges(const source_file_t& file);

/// textDocument/selectionRange: for each of `offsets`, what a selection there grows to, from its
/// word out to the whole file through each expression, statement, and block around it. Only the
/// file is parsed.
[[nodiscard]] json_t selection_ranges(const source_file_t& file,
                                      std::span<const std::uint32_t> offsets,
                                      const positions_t& positions);

/// textDocument/documentHighlight: the file's names of what's named at `offset`, each a Write
/// where it's declared or assigned, and a Read elsewhere.
[[nodiscard]] json_t document_highlights(const query_t& query, std::uint32_t offset,
                                         const positions_t& positions);

/// Which inlay hints the client wants, which its settings say.
struct inlay_options_t {
    /// The types of Locals, a For's variables, and a lambda's parameters, where none is written.
    bool types = true;
    /// The parameters' names at a call's arguments, but where the argument or the function says it.
    bool parameters = true;
};

/// textDocument/inlayHint for the code that starts in `range`.
[[nodiscard]] json_t inlay_hints(const query_t& query, span_t range, const inlay_options_t& options,
                                 const positions_t& positions);

/// textDocument/typeDefinition: where the type of what's at `offset` is declared, the T of a T?, a
/// T[], or a Weak<T>. Null for a built-in type, or none.
[[nodiscard]] json_t type_definition(const query_t& query, std::uint32_t offset,
                                     const positions_t& positions);

/// textDocument/implementation: in `compilations`, what implements or extends the Interface or
/// Class `key` names, or implements or overrides the member it names, as Locations.
[[nodiscard]] json_t
implementations(std::span<const std::shared_ptr<const compilation_t>> compilations,
                const symbol_key_t& key, const positions_t& positions);

/// textDocument/prepareCallHierarchy: the Function, Method, or New named at `offset`, as a
/// CallHierarchyItem whose data is its symbol_key_t. Null for anything else.
[[nodiscard]] json_t prepare_call_hierarchy(const query_t& query, std::uint32_t offset,
                                            const positions_t& positions);

/// callHierarchy/incomingCalls: in `compilations`, the functions, and files' top-level code, that
/// call `key`'s, each with where.
[[nodiscard]] json_t
incoming_calls(std::span<const std::shared_ptr<const compilation_t>> compilations,
               const symbol_key_t& key, const positions_t& positions);

/// callHierarchy/outgoingCalls: what `key`'s function calls, or with `top`, what its file's
/// top-level code does, each with where, in `query`, a check of its file.
[[nodiscard]] json_t outgoing_calls(const query_t& query, const symbol_key_t& key, bool top,
                                    const positions_t& positions);

/// textDocument/prepareTypeHierarchy: the Struct, Class, Interface, or Enum named at `offset`, as
/// a TypeHierarchyItem whose data is its symbol_key_t. Null for anything else.
[[nodiscard]] json_t prepare_type_hierarchy(const query_t& query, std::uint32_t offset,
                                            const positions_t& positions);

/// typeHierarchy/supertypes: the Class `key`'s type extends and the Interfaces it lists, in
/// `query`, a check of its file.
[[nodiscard]] json_t supertypes(const query_t& query, const symbol_key_t& key,
                                const positions_t& positions);

/// typeHierarchy/subtypes: in `compilations`, the types that extend `key`'s or list it.
[[nodiscard]] json_t subtypes(std::span<const std::shared_ptr<const compilation_t>> compilations,
                              const symbol_key_t& key, const positions_t& positions);

} // namespace fb::lsp
