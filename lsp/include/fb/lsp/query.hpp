#pragma once

#include "fb/ast.hpp"
#include "fb/driver.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fb::lsp {

/// A place in a file that names a symbol: a use of it, or where it's declared.
struct named_t {
    /// Just the name.
    span_t span;
    symbol_id_t symbol{};
    bool declaration = false;
};

/// What tells a symbol apart in any check: where it's declared, so a generic one's instances,
/// and the same declaration in another compilation, are one. A builtin is its name.
struct symbol_key_t {
    std::string file;
    std::uint32_t begin = 0;
    std::string name;

    bool operator==(const symbol_key_t&) const = default;
};

/// Where a statement starts, with the attributes and modifiers a declaration has before its
/// keyword.
[[nodiscard]] std::uint32_t declaration_begin(const stmt_t& statement);

/// What the LSP's features look up in a check: the symbols a file names, what they are, and
/// what's at a position.
class query_t {
public:
    query_t(const compilation_t& compilation, file_id_t file);

    [[nodiscard]] const compilation_t& compilation() const {
        return m_compilation;
    }

    [[nodiscard]] const analysis_t& analysis() const {
        return m_compilation.analysis;
    }

    [[nodiscard]] const ast_t& ast() const {
        return m_compilation.ast;
    }

    [[nodiscard]] const source_file_t& source() const {
        return m_compilation.sources.get(m_file);
    }

    [[nodiscard]] file_id_t file() const {
        return m_file;
    }

    /// The symbol of a Struct, Class, Interface, or Enum, or of a generic one's instance.
    [[nodiscard]] std::optional<symbol_id_t> type_symbol(type_id_t type) const;

    /// Every place in `file`, this one by default, that names a symbol, in order. A use that
    /// names a constructor by its type's name, as "Point(1, 2)" does, names the type.
    [[nodiscard]] std::vector<named_t> names(std::optional<file_id_t> file = std::nullopt) const;

    /// The name at `offset`, or just before it, as at the end of a word.
    [[nodiscard]] std::optional<named_t> name_at(std::uint32_t offset) const;

    [[nodiscard]] symbol_key_t key(symbol_id_t id) const;

    /// The symbols here that `key` names: its own, and its generic copies'.
    [[nodiscard]] std::vector<symbol_id_t> symbols_of(const symbol_key_t& key) const;

    /// Where `use` names its symbol, as names() has it, or nullopt when its text doesn't name it,
    /// as an operator's doesn't.
    [[nodiscard]] std::optional<named_t> name_of(const symbol_use_t& use) const;

    /// A symbol's declaration as code shows it, like "Function Area(p: Point): Int32".
    [[nodiscard]] std::string signature(symbol_id_t id) const;

    /// The documentation on a symbol's declaration.
    [[nodiscard]] std::string docs(symbol_id_t id) const;

    /// Where a symbol is declared on disk. nullopt for a builtin, or one in no file of its own.
    [[nodiscard]] std::optional<std::filesystem::path> declared_in(symbol_id_t id) const;

    /// The innermost expression in this file that `offset` is in or at the end of.
    [[nodiscard]] std::optional<expr_id_t> expr_at(std::uint32_t offset) const;

    /// A type as code names it.
    [[nodiscard]] std::string type_name(type_id_t type) const;

    /// The module this file is.
    [[nodiscard]] std::optional<module_id_t> module() const;

private:
    const compilation_t& m_compilation;
    file_id_t m_file;
    /// The symbol of each Struct, Class, Interface, and Enum, by its type.
    std::unordered_map<type_id_t, symbol_id_t> m_types;
};

} // namespace fb::lsp
