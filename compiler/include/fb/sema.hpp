#pragma once

#include "fb/ast.hpp"
#include "fb/const_value.hpp"
#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {

/// A symbol's index in analysis_t::symbols.
enum class symbol_id_t : std::uint32_t {};

enum class symbol_kind_t : std::uint8_t {
    /// A Function declared in the source.
    FUNCTION,
    /// A function the runtime provides, like Print. It has no declaration.
    BUILTIN_FUNCTION,
    GLOBAL,
    /// A Const, at the top level or in a function.
    CONSTANT,
    /// A Local, at the top level or in a function.
    LOCAL,
    PARAMETER,
    /// A For loop's variable, which can't be assigned.
    LOOP_VARIABLE,
    /// A Struct. Its `type` is the Struct type itself.
    STRUCT,
    /// One of a Struct's fields.
    FIELD,
    /// A Struct's Method or "ByRef Method". Self isn't among its parameters.
    METHOD,
    /// A Struct's New, or the constructor a Struct without one gets, whose parameters are its
    /// fields. Its `type` is the Struct.
    CONSTRUCTOR,
};

/// Something a name can refer to: a function, a variable, a Const, a parameter, a Struct, or one
/// of a Struct's members.
struct symbol_t {
    symbol_kind_t kind = symbol_kind_t::LOCAL;
    /// Views the source text, or static text for builtins.
    std::string_view name;
    /// The name where it's declared. Empty for builtins.
    span_t span;
    /// A variable's type, or a function's return type (NONE when it returns nothing). INVALID
    /// when it couldn't be worked out, which was reported.
    type_id_t type = type_id_t::INVALID;
    /// A parameter declared ByRef, or a "ByRef Method".
    bool by_ref = false;
    /// A parameter that calls can leave out. ByRef parameters never have a default. A field
    /// always has one, since it's a parameter of the constructor a Struct gets.
    bool has_default = false;
    /// A Const's value, or a parameter's or field's default value, when it's known at compile
    /// time. A field without one starts at zero.
    std::optional<const_value_t> value;
    /// A function's parameters, in order.
    std::vector<symbol_id_t> parameters;
    /// The statement that declares it. nullopt for parameters, builtins, and the constructor a
    /// Struct gets.
    std::optional<stmt_id_t> declaration;
    /// A field's, Method's, or constructor's Struct.
    type_id_t owner = type_id_t::INVALID;
    /// A Method's or New's Self: a parameter in a Method (ByRef in a "ByRef Method"), and a
    /// Local in New, which returns it.
    std::optional<symbol_id_t> self;
    /// A Struct's fields in order, then its Methods and constructors.
    std::vector<symbol_id_t> members;
    /// A field's position among its Struct's fields.
    std::uint32_t field = 0;
};

/// What analysis found out about one expression.
struct expr_info_t {
    /// The expression's own type. A literal takes the type its context expects when its value
    /// fits, like Int64 for the `1` in "x + 1" with x an Int64. NONE for a call that returns
    /// nothing, and INVALID after an error.
    type_id_t type = type_id_t::INVALID;
    /// The type the context converts it to, without loss: Int64 for an Int32 passed to an Int64
    /// parameter. The same as `type` when nothing converts it; lowering casts when they differ.
    type_id_t converted = type_id_t::INVALID;
    /// The value when it's known at compile time, already converted to `converted`. Literals,
    /// Consts, and operations on known values have one.
    std::optional<const_value_t> value;
    /// For a name: the variable, Const, or parameter it refers to. For a call and its callee:
    /// the function, after choosing among overloads.
    std::optional<symbol_id_t> symbol;

    /// For a call: one entry per parameter of the function, in parameter order. Each is the
    /// argument's expression, or nullopt when the parameter takes its default value. Empty
    /// when the arguments couldn't be matched to the parameters.
    std::vector<std::optional<expr_id_t>> arguments;
};

/// What analysis found out about one statement.
struct stmt_info_t {
    /// What it declares: a Function's symbol, a Local's, Global's, or Const's, or a For loop's
    /// variable.
    std::optional<symbol_id_t> symbol;
    /// For Break and Continue: the loop statement they leave or continue.
    std::optional<stmt_id_t> loop;
};

/// Everything analysis found out about a file, for lowering and tools. `exprs` and `stmts` have
/// one entry per AST node, indexed by its id; nodes in code with errors may keep the defaults.
struct analysis_t {
    /// Indexed by symbol_id_t. The builtins come first.
    std::vector<symbol_t> symbols;
    std::vector<expr_info_t> exprs;
    std::vector<stmt_info_t> stmts;
    /// The Functions, Methods, and News declared in the source, in source order. Builtins and
    /// the constructors Structs get aren't included.
    std::vector<symbol_id_t> functions;
    /// The top-level Globals and Consts, in source order.
    std::vector<symbol_id_t> globals;
    /// Every type the program uses, the built-ins first.
    type_table_t types;

    [[nodiscard]] const symbol_t& symbol(const symbol_id_t id) const {
        return symbols[std::to_underlying(id)];
    }

    [[nodiscard]] const expr_info_t& expr(const expr_id_t id) const {
        return exprs[std::to_underlying(id)];
    }

    [[nodiscard]] const stmt_info_t& stmt(const stmt_id_t id) const {
        return stmts[std::to_underlying(id)];
    }
};

/// Resolves names, checks types and that variables are assigned before they're read, folds
/// constants, and reports errors to `diagnostics`. It always returns a full analysis, but only
/// one without errors can be lowered.
[[nodiscard]] analysis_t analyze(const source_file_t& file, const ast_t& ast,
                                 diagnostic_bag_t& diagnostics);

} // namespace fb
