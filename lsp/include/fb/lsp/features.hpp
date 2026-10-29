#pragma once

#include "fb/driver.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

/// Where each of `compilations` names the symbol `key`, its declaration with them when
/// `declaration`, each once, as LSP Locations, in the files `keep` says to.
[[nodiscard]] json_t references(std::span<const std::shared_ptr<const compilation_t>> compilations,
                                const symbol_key_t& key, bool declaration,
                                const std::function<bool(const std::filesystem::path&)>& keep,
                                const positions_t& positions);

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

/// textDocument/completion at `offset` of `text`, the document as it is, whose last check is
/// `query`: a member after a '.', a module after Import, or the names in scope and keywords.
/// `modules` are the modules an Import can name.
[[nodiscard]] json_t completion(const query_t& query, std::uint32_t offset,
                                const std::vector<std::string>& modules,
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

} // namespace fb::lsp
