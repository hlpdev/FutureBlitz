#pragma once

#include "fb/source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fb {

/// An expression's index in its ast_t. Nodes refer to each other by id.
enum class expr_id_t : std::uint32_t {};
/// A statement's index in its ast_t.
enum class stmt_id_t : std::uint32_t {};
/// A type expression's index in its ast_t.
enum class type_expr_id_t : std::uint32_t {};

/// Statements in source order.
using block_t = std::vector<stmt_id_t>;

/// A name as written, like a declared variable or a loop label. Empty when it was missing; the
/// parser reported that, and later stages skip empty names.
struct name_t {
    std::string_view text;
    span_t span;
};

/// The prefix operators `-`, `!`, and `~`.
enum class unary_op_t : std::uint8_t {
    NEGATE,
    NOT,
    BIT_NOT,
};

/// The infix operators. The order matters: analysis groups them by range, arithmetic and
/// bitwise first, then the comparisons.
enum class binary_op_t : std::uint8_t {
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    /// `%`
    REMAINDER,
    /// `**`
    POWER,
    SHIFT_LEFT,
    SHIFT_RIGHT,
    BIT_AND,
    BIT_OR,
    BIT_XOR,
    EQUAL,
    NOT_EQUAL,
    LESS,
    LESS_EQUAL,
    GREATER,
    GREATER_EQUAL,
    /// `&&`, which only evaluates its right side when the left is True.
    AND,
    /// `||`, which only evaluates its right side when the left is False.
    OR,
    /// `??`, null-coalescing. Not supported in M1.
    COALESCE,
};

/// The operator as written in source, like "-" or "**".
[[nodiscard]] std::string_view spelling(unary_op_t op);
[[nodiscard]] std::string_view spelling(binary_op_t op);

// Expressions

/// Where an expression failed to parse. The error is already reported.
struct error_expr_t {};

/// An integer literal like "1_000" or "0xFF". A leading '-' is a unary_expr_t around it.
struct integer_expr_t {
    /// 0 when it's malformed or doesn't fit in 64 bits, which the lexer reported.
    std::uint64_t value;
};

/// A literal like "1.5" or "1e9".
struct decimal_expr_t {
    /// 0 when it's out of range, which the parser reported.
    double value;
};

/// A character literal like 'a', which analysis treats as an integer: its code point.
struct character_expr_t {
    /// U+FFFD when the literal is empty or invalid.
    char32_t value;
};

/// Decoded text, or the expression in a "{...}".
using string_part_t = std::variant<std::string, expr_id_t>;

/// A string literal, with escapes and indentation already decoded.
struct string_expr_t {
    /// Text and interpolated expressions in source order. Empty texts are left out, so ""
    /// has no parts.
    std::vector<string_part_t> parts;
};

struct bool_expr_t {
    bool value;
};

/// `Null`. Not supported in M1.
struct null_expr_t {};

/// A name used as a value or a callee, like `x` in "x + 1" or `Print` in "Print(x)".
struct name_expr_t {
    std::string_view name;
};

struct unary_expr_t {
    unary_op_t op;
    expr_id_t operand;
};

struct binary_expr_t {
    binary_op_t op;
    expr_id_t left;
    expr_id_t right;
};

/// "condition ? if_true : if_false"
struct ternary_expr_t {
    expr_id_t condition;
    expr_id_t if_true;
    expr_id_t if_false;
};

/// One argument of a call.
struct argument_t {
    /// Set for a named argument: `x` in "Move(x: 1)".
    std::optional<name_t> name;
    /// Written with ByRef: "Swap(ByRef a, ByRef b)".
    bool by_ref;
    expr_id_t value;
};

/// "callee(arguments)"
struct call_expr_t {
    expr_id_t callee;
    std::vector<argument_t> arguments;
};

/// "object.member", or "object?.member" when `null_safe`. Not supported in M1.
struct member_expr_t {
    expr_id_t object;
    name_t member;
    bool null_safe;
};

/// "object[indices]", like "grid[x, y]".
struct index_expr_t {
    expr_id_t object;
    std::vector<expr_id_t> indices;
};

/// "value As Type", or "value As? Type" when `checked`: a downcast that gives Null when it
/// fails, which isn't supported in M1.
struct cast_expr_t {
    expr_id_t value;
    type_expr_id_t type;
    bool checked;
};

/// "value Is Type", a type test. Not supported in M1.
struct is_expr_t {
    expr_id_t value;
    type_expr_id_t type;
};

/// "Self": the value a Method or New works on.
struct self_expr_t {};

/// "Super", which only comes before a call: "Super.Hurt(5)" calls the Method of the Class that
/// Self's Class extends, and "Super.New()" its New.
struct super_expr_t {};

/// "[1, 2, 3]", a new array of the elements.
struct array_expr_t {
    std::vector<expr_id_t> elements;
};

using expr_node_t =
    std::variant<error_expr_t, integer_expr_t, decimal_expr_t, character_expr_t, string_expr_t,
                 bool_expr_t, null_expr_t, name_expr_t, unary_expr_t, binary_expr_t, ternary_expr_t,
                 call_expr_t, member_expr_t, index_expr_t, cast_expr_t, is_expr_t, self_expr_t,
                 super_expr_t, array_expr_t>;

struct expr_t {
    span_t span;
    expr_node_t node;
};

// Types, as written in the source

/// Where a type failed to parse.
struct error_type_t {};

/// A type by name, like "Int" or "String".
struct named_type_t {
    std::string_view name;
};

/// "Type?". Not supported in M1.
struct nullable_type_t {
    type_expr_id_t inner;
};

/// "Type[]", or a fixed array "Type[N]" or "Type[W, H]", whose sizes are known at compile time.
struct array_type_t {
    type_expr_id_t element;
    /// Empty for "Type[]".
    std::vector<expr_id_t> sizes;
};

using type_expr_node_t = std::variant<error_type_t, named_type_t, nullable_type_t, array_type_t>;

struct type_expr_t {
    span_t span;
    type_expr_node_t node;
};

// Statements

/// A statement that failed to parse, or a construct after M1 that was reported and skipped.
struct error_stmt_t {};

/// An expression on its own line. Only calls do anything; other expressions are reported.
struct expression_stmt_t {
    expr_id_t expression;
};

/// "target = value", or a compound assignment like "target += value".
struct assign_stmt_t {
    /// The operator of a compound assignment: ADD for "+=". nullopt for a plain '='.
    std::optional<binary_op_t> op;
    expr_id_t target;
    expr_id_t value;
};

/// "target++", or "target--" when `decrement`.
struct increment_stmt_t {
    expr_id_t target;
    bool decrement;
};

/// The keyword that declares a variable: Local, Global, Const, or a Struct's Field.
enum class variable_kind_t : std::uint8_t {
    LOCAL,
    GLOBAL,
    CONSTANT,
    FIELD,
};

/// "Local name: Type = value". The type and the value are each optional, but analysis needs at
/// least one.
struct variable_stmt_t {
    variable_kind_t kind;
    name_t name;
    std::optional<type_expr_id_t> type;
    std::optional<expr_id_t> value;
};

/// "ByRef name: Type = default". ByRef and the default are optional.
struct parameter_t {
    name_t name;
    bool by_ref;
    type_expr_id_t type;
    std::optional<expr_id_t> default_value;
};

/// What a function_stmt_t declares.
enum class function_kind_t : std::uint8_t {
    FUNCTION,
    /// "Method Name()" in a Struct: `Self` is read-only.
    METHOD,
    /// "ByRef Method Name()" in a Struct: it can change `Self`.
    BYREF_METHOD,
    /// "New()" in a Struct: makes a new value. Its name is the "New" keyword.
    CONSTRUCTOR,
};

/// What Abstract, Final, and Override say about a Class or a Method.
struct modifiers_t {
    bool is_abstract = false;
    bool is_final = false;
    bool is_override = false;
};

/// "Function Name(parameters): Type ... End Function", or a Struct's Method or New.
struct function_stmt_t {
    function_kind_t kind = function_kind_t::FUNCTION;
    name_t name;
    std::vector<parameter_t> parameters;
    /// nullopt when the function returns nothing.
    std::optional<type_expr_id_t> return_type;
    /// For the one-line form, "Function Name() => value", a single Return of the value. An
    /// Abstract Method has none.
    block_t body;
    modifiers_t modifiers;
};

/// The If, or one ElseIf: a condition and the code it runs.
struct if_branch_t {
    expr_id_t condition;
    block_t body;
};

/// "If ... ElseIf ... Else ... End If".
struct if_stmt_t {
    /// The If, then each ElseIf, in order.
    std::vector<if_branch_t> branches;
    std::optional<block_t> else_body;
    /// Written on one line as "If condition Then statement Else statement". It has one branch,
    /// and each body holds one statement.
    bool single_line;
};

/// "While condition ... End While".
struct while_stmt_t {
    /// Set for a labeled loop, like "Outer: While ...", which Break and Continue can name.
    std::optional<name_t> label;
    expr_id_t condition;
    block_t body;
};

/// "Repeat ... Until condition". The body runs at least once.
struct repeat_stmt_t {
    std::optional<name_t> label;
    block_t body;
    expr_id_t condition;
};

/// "Loop ... End Loop", which only ends through Break or Return.
struct loop_stmt_t {
    std::optional<name_t> label;
    block_t body;
};

/// "For variable = start To end Step step ... End For", which declares `variable`.
struct for_stmt_t {
    std::optional<name_t> label;
    name_t variable;
    expr_id_t start;
    expr_id_t end;
    /// True for To, which includes `end`. False for Until, which stops before it.
    bool inclusive;
    /// nullopt without a Step, which counts by 1.
    std::optional<expr_id_t> step;
    block_t body;
};

/// "For value In sequence", or "For index, value In sequence".
struct for_in_stmt_t {
    std::optional<name_t> label;
    std::optional<name_t> index;
    name_t value;
    expr_id_t sequence;
    block_t body;
};

/// One value of a Case, or an inclusive range like "1 To 5" when `range_end` is set.
struct case_value_t {
    expr_id_t value;
    std::optional<expr_id_t> range_end;
};

/// "Case e As Boss": matches an object of a Class, or of one that extends it, and names it as
/// one in the Case's code.
struct type_case_t {
    name_t name;
    type_expr_id_t type;
};

/// "Case values", or a type match, and the code it runs.
struct case_t {
    std::vector<case_value_t> values;
    /// A type match, which has no values.
    std::optional<type_case_t> match;
    block_t body;
};

/// "Select subject ... End Select". The first Case that matches runs, or else Default.
struct select_stmt_t {
    expr_id_t subject;
    std::vector<case_t> cases;
    std::optional<block_t> default_body;
};

/// "Break", or "Break Outer" for the loop labeled Outer.
struct break_stmt_t {
    std::optional<name_t> label;
};

/// "Continue", or "Continue Outer" for the loop labeled Outer.
struct continue_stmt_t {
    std::optional<name_t> label;
};

/// "Return", with a value in a function that returns one.
struct return_stmt_t {
    std::optional<expr_id_t> value;
};

/// "Struct Name ... End Struct", or "Class Name Extends Base ... End Class". Its members are
/// Fields (variable_stmt_t) and Methods and News (function_stmt_t), in source order.
struct type_stmt_t {
    /// "Class" rather than "Struct".
    bool is_class = false;
    name_t name;
    block_t members;
    /// The Class after Extends.
    std::optional<name_t> base;
    modifiers_t modifiers;
};

using stmt_node_t =
    std::variant<error_stmt_t, expression_stmt_t, assign_stmt_t, increment_stmt_t, variable_stmt_t,
                 function_stmt_t, if_stmt_t, while_stmt_t, repeat_stmt_t, loop_stmt_t, for_stmt_t,
                 for_in_stmt_t, select_stmt_t, break_stmt_t, continue_stmt_t, return_stmt_t,
                 type_stmt_t>;

struct stmt_t {
    span_t span;
    stmt_node_t node;
};

/// A parsed file. It holds every node, and its items are the top-level statements in source
/// order. Names and spans refer to the source file's text.
class ast_t {
public:
    expr_id_t add(expr_t expr);
    stmt_id_t add(stmt_t stmt);
    type_expr_id_t add(type_expr_t type);

    /// `id` must come from this AST.
    [[nodiscard]] const expr_t& get(expr_id_t id) const;
    [[nodiscard]] const stmt_t& get(stmt_id_t id) const;
    [[nodiscard]] const type_expr_t& get(type_expr_id_t id) const;

    /// Ids run from 0 up to the count, so analysis can keep tables indexed by them.
    [[nodiscard]] std::size_t expr_count() const {
        return m_exprs.size();
    }

    [[nodiscard]] std::size_t stmt_count() const {
        return m_stmts.size();
    }

    [[nodiscard]] const block_t& items() const {
        return m_items;
    }

    void add_item(const stmt_id_t id) {
        m_items.push_back(id);
    }

private:
    std::vector<expr_t> m_exprs;
    std::vector<stmt_t> m_stmts;
    std::vector<type_expr_t> m_types;
    block_t m_items;
};

} // namespace fb
