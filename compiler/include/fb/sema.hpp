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
    /// A Struct, Class, or Enum. Its `type` is the type itself.
    TYPE,
    /// One of a Struct's or Class's fields.
    FIELD,
    /// A Method, or a Struct's "ByRef Method". Self isn't among its parameters.
    METHOD,
    /// A New, or the one a type without a New gets: a Struct's takes its fields, and a Class's
    /// takes nothing. Its `type` is the Struct or Class.
    CONSTRUCTOR,
    /// A Struct's or Class's Property. Its `type` is its value's, and its Get and Set are
    /// Methods, which aren't members themselves.
    PROPERTY,
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
    /// A parameter that calls can leave out. ByRef parameters never have a default. A field is
    /// a parameter of the constructor a Struct gets, so it has one, its own or zero, unless its
    /// type has no zero.
    bool has_default = false;
    /// A Const's value, or a parameter's or field's default value, when it's known at compile
    /// time. A field without one starts at zero.
    std::optional<const_value_t> value;
    /// A function's parameters, in order.
    std::vector<symbol_id_t> parameters;
    /// A function's Throws: the Classes its errors extend. Empty when it can't throw.
    std::vector<type_id_t> throws;
    /// The statement that declares it. nullopt for parameters, builtins, and the constructor a
    /// Struct gets.
    std::optional<stmt_id_t> declaration;
    /// A field's, Method's, or constructor's Struct or Class.
    type_id_t owner = type_id_t::INVALID;
    /// A Method's or New's Self: a parameter in a Method (ByRef in a "ByRef Method"), and in a
    /// Struct's New, a Local that New returns. A Class's Self is a reference, which can't be
    /// replaced, though its object's fields can change.
    std::optional<symbol_id_t> self;
    /// A Struct's or Class's fields in order, then its Methods and constructors. What a Class
    /// inherits isn't among them.
    std::vector<symbol_id_t> members;
    /// A field's position among its type's fields, which start with the inherited ones.
    std::uint32_t field = 0;
    /// A Class's, Method's, Field's, or Property's modifiers. A Static Field is a GLOBAL whose
    /// owner is its type.
    modifiers_t modifiers;
    /// A Method that overrides one of the Class it extends: that one.
    std::optional<symbol_id_t> overrides;
    /// A Class's New that doesn't start with Super.New: the New of the Class it extends that
    /// runs first, without arguments.
    std::optional<symbol_id_t> base_new;
    /// A parameter or field whose default is "[]", which gives each use a new empty array.
    bool empty_default = false;
    /// A Property's Get and Set. A Property that overrides another may leave out its Set, and
    /// then uses that one's.
    std::optional<symbol_id_t> getter;
    std::optional<symbol_id_t> setter;
    /// A Local that a lambda captures: the Class of the cell it lives in, which the lambda shares
    /// with the code around it.
    type_id_t cell = type_id_t::INVALID;
    /// A generic Function's or Method's own type parameters. A generic Struct's or Class's are in
    /// its type.
    std::vector<type_id_t> type_parameters;
};

/// How a closure keeps a variable from around its lambda.
enum class capture_kind_t : std::uint8_t {
    /// A copy, of a variable that can't change: a parameter, a loop variable, a Const, or Self.
    VALUE,
    /// The cell a Local lives in, which the closure shares with the code around it.
    CELL,
    /// "[Weak x]": a Weak<T> of the object the variable refers to when the closure is made.
    WEAK,
};

/// A variable that a lambda uses from around it.
struct capture_t {
    /// The variable outside the lambda.
    symbol_id_t variable;
    /// What the lambda's code uses: the variable itself, or for a Weak capture, a read-only
    /// Weak<T> named like it.
    symbol_id_t inside;
    capture_kind_t kind = capture_kind_t::VALUE;
};

/// A function value: a lambda, or a Function used by name.
struct closure_t {
    /// The lambda's symbol, which has its parameters and return type, or the Function.
    symbol_id_t function;
    /// The Class of its objects: a field for each capture, in order.
    type_id_t type;
    /// Its Func type.
    type_id_t func;
    std::vector<capture_t> captures;
    /// The lambda. nullopt for a Function used by name.
    std::optional<expr_id_t> lambda;
};

/// What analysis found out about one expression.
struct expr_info_t {
    /// The expression's own type. A literal takes the type its context expects when its value
    /// fits, like Int64 for the `1` in "x + 1" with x an Int64. A nullable variable that a null
    /// check narrowed has its T. NONE for a call that returns nothing, and INVALID after an
    /// error.
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

    /// For 'Is': the Class it tests for.
    type_id_t tested = type_id_t::INVALID;

    /// For a call: one entry per parameter of the function, in parameter order. Each is the
    /// argument's expression, or nullopt when the parameter takes its default value. Empty
    /// when the arguments couldn't be matched to the parameters. For an operator that calls an
    /// Operator, whose symbol is `symbol`: its operands.
    std::vector<std::optional<expr_id_t>> arguments;

    /// For a lambda, or a Function's name used as a value: its closure's index in
    /// analysis_t::closures.
    std::optional<std::size_t> closure;

    /// For a call of a generic Function or Method, or a generic Function used as a value: a type
    /// argument for each of the function's own type parameters. Those of a generic Struct or
    /// Class come from the type of the object, or of the one New makes. For an operator that
    /// calls an Operator of a generic type, its type's.
    std::vector<type_id_t> type_arguments;
};

/// What analysis found out about one statement.
struct stmt_info_t {
    /// What it declares: a Function's symbol, a Local's, Global's, or Const's, or a For loop's
    /// variable.
    std::optional<symbol_id_t> symbol;
    /// For Break and Continue: the loop statement they leave or continue.
    std::optional<stmt_id_t> loop;
    /// For a Select of type matches, or a Try's Catches: each one's variable, in order. For a
    /// Local that takes a tuple apart, each new Local, without the "_"s.
    std::vector<symbol_id_t> matches;
    /// For a "For index, value In": the index's variable. `symbol` is the value's.
    std::optional<symbol_id_t> index;
    /// For a Select on an Enum without a Default: whether its Cases cover every value, so no
    /// path goes past them all.
    bool exhaustive = false;
    /// For "x op= y" with an Operator, whose symbol is `symbol`: a type argument for each type
    /// parameter of its Struct or Class.
    std::vector<type_id_t> type_arguments;
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
    /// The Error Class from std/core/error.fb, which every error extends, and Error?, which
    /// holds the error a call throws, or Null.
    type_id_t error_class = type_id_t::INVALID;
    type_id_t nullable_error = type_id_t::INVALID;
    /// The function values the program makes. The expression that makes each one, a lambda or
    /// a Function's name, has its index.
    std::vector<closure_t> closures;

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
/// constants, and reports errors to `diagnostics`. Every file in `ast` shares one namespace, and
/// `sources` holds them all. It always returns a full analysis, but only
/// one without errors can be lowered.
[[nodiscard]] analysis_t analyze(const source_manager_t& sources, const ast_t& ast,
                                 diagnostic_bag_t& diagnostics);

} // namespace fb
