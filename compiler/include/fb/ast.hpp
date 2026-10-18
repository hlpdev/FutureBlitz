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
    /// The type arguments in "Max<Float>(1, 2.5)" or "Box<Int>(5)".
    std::vector<type_expr_id_t> type_arguments;
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

/// "object.member", or "object?.member" when `null_safe`. A tuple's element is a member named
/// by its number, as in "pair.0".
struct member_expr_t {
    expr_id_t object;
    name_t member;
    bool null_safe;
    /// The type arguments in "box.Map<String>(f)".
    std::vector<type_expr_id_t> type_arguments;
};

/// "object[indices]", like "grid[x, y]".
struct index_expr_t {
    expr_id_t object;
    std::vector<expr_id_t> indices;
};

/// "object[from..to]": a new array of the elements from `from` up to `to`, without the one at
/// `to`. Either can be left out, as in "a[2..]" or "a[..n]", for the start or the end.
struct slice_expr_t {
    expr_id_t object;
    std::optional<expr_id_t> from;
    std::optional<expr_id_t> to;
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

/// "key: value" in a Map literal.
struct map_entry_t {
    expr_id_t key;
    expr_id_t value;
};

/// "{"a": 1, "b": 2}", a new Map of the entries.
struct map_expr_t {
    std::vector<map_entry_t> entries;
};

/// "(a, b)", a tuple of two or more values. As an assignment's target, each element is assigned:
/// "a, b = (b, a)".
struct tuple_expr_t {
    std::vector<expr_id_t> elements;
};

/// One of a lambda's parameters: "x", or "x: Int". Without a type, it takes the one the Func
/// expected where the lambda is has.
struct lambda_parameter_t {
    name_t name;
    std::optional<type_expr_id_t> type;
};

/// "Function(x: Int) => x * 2", or a block lambda: "Function(x)", its body, and "End Function". It
/// makes a function value, which can use the variables around it. "Function[Weak Self]()" lists
/// the ones it refers to weakly.
struct lambda_expr_t {
    /// The names in "[Weak Self, Weak node]": Self, or a variable.
    std::vector<name_t> weak;
    std::vector<lambda_parameter_t> parameters;
    std::optional<type_expr_id_t> return_type;
    /// For "=> value": the value it returns, or the call it makes.
    std::optional<expr_id_t> value;
    /// A block lambda's statements.
    block_t body;
};

/// "Try operand" lets the errors of the calls in `operand` go on to the code around it. "Try?
/// operand" gives a T? instead, which is Null when one of them throws.
struct try_expr_t {
    expr_id_t operand;
    /// Written "Try?".
    bool optional;
};

using expr_node_t =
    std::variant<error_expr_t, integer_expr_t, decimal_expr_t, character_expr_t, string_expr_t,
                 bool_expr_t, null_expr_t, name_expr_t, unary_expr_t, binary_expr_t, ternary_expr_t,
                 call_expr_t, member_expr_t, index_expr_t, slice_expr_t, cast_expr_t, is_expr_t,
                 self_expr_t, super_expr_t, array_expr_t, map_expr_t, try_expr_t, tuple_expr_t,
                 lambda_expr_t>;

struct expr_t {
    span_t span;
    expr_node_t node;
};

// Types, as written in the source

/// Where a type failed to parse.
struct error_type_t {};

/// A type by name, like "Int" or "String", or a generic one with its type arguments, like
/// "Box<Int>".
struct named_type_t {
    std::string_view name;
    std::vector<type_expr_id_t> arguments;
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

/// "Weak<T>", a reference that doesn't keep its object. M2 has no other generic types.
struct weak_type_t {
    type_expr_id_t inner;
};

/// "(A, B)", a tuple of two or more types. "(T)" is just T.
struct tuple_type_t {
    std::vector<type_expr_id_t> elements;
};

/// "Func(A, B): R", or "Func(A)" for a function that returns nothing.
struct function_type_t {
    std::vector<type_expr_id_t> parameters;
    std::optional<type_expr_id_t> result;
};

/// A value where a type argument goes, for a Const type parameter: the 64 in
/// "Grid<Float, 64, 64>".
struct value_type_t {
    expr_id_t value;
};

using type_expr_node_t = std::variant<error_type_t, named_type_t, nullable_type_t, array_type_t,
                                      weak_type_t, tuple_type_t, function_type_t, value_type_t>;

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

/// "@Rename("hp")": an attribute that an Attribute declaration declares, with its arguments,
/// which can be named, like a call's.
struct attribute_t {
    name_t name;
    std::vector<argument_t> arguments;
};

/// What Abstract, Final, Override, Static, and ReadOnly say about a declaration: a Class, a
/// Method, a Field, or a Property.
struct modifiers_t {
    bool is_abstract = false;
    bool is_final = false;
    bool is_override = false;
    /// "Static": a Field or Method of the type itself, rather than of each value or object.
    bool is_static = false;
    /// "ReadOnly": a Field that only its Class's New can change.
    bool is_read_only = false;
    /// "Const Function": the compiler runs a call whose arguments are known at compile time.
    bool is_const = false;
    /// "@Inline": every call to it is inlined.
    bool is_inline = false;
    /// "@Deprecated", with its message, which may be empty: each use gets a warning.
    std::optional<std::string> deprecated;
    /// The attributes that Attribute declarations declare, which reflection reads.
    std::vector<attribute_t> attributes;
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
    /// Static and ReadOnly, which only a Field can have.
    modifiers_t modifiers;
};

/// "Local lo, hi = value": takes a tuple apart into new Locals, one for each element, which
/// take the elements' types. A name "_" skips its element.
struct destructure_stmt_t {
    std::vector<name_t> names;
    expr_id_t value;
};

/// "ByRef name: Type = default". ByRef and the default are optional.
struct parameter_t {
    name_t name;
    bool by_ref;
    type_expr_id_t type;
    /// "nums: Int...": the last parameter, which takes the rest of a call's arguments as an
    /// Int[].
    bool variadic;
    std::optional<expr_id_t> default_value;
};

/// "T: Hashable & Equatable": the Interfaces a type parameter implements, written in the type
/// parameters, as in "<T: Comparable>", or on a Where line at the top of the body.
struct constraint_t {
    name_t parameter;
    std::vector<type_expr_id_t> interfaces;
    /// Written on a "Where" line.
    bool where = false;
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
    /// "Operator +(a: Money, b: Money): Money" in a Struct or Class: what the operator does on
    /// its type. It's Static, and its name is the operator, like "+".
    OPERATOR,
};

/// "Function Name(parameters): Type ... End Function", or a Struct's Method or New.
struct function_stmt_t {
    function_kind_t kind = function_kind_t::FUNCTION;
    name_t name;
    /// The names in "Function First<T>(...)": a generic function's type parameters.
    std::vector<name_t> type_parameters;
    /// For each type parameter written "Const N: Int", the type of its value, at the same place
    /// as its name. Empty when none is Const.
    std::vector<std::optional<type_expr_id_t>> value_types;
    /// What its type parameters implement. On a Method's Where line, one for its Struct's or
    /// Class's type parameter makes the Method conditional: it's only there when the type
    /// argument implements those Interfaces.
    std::vector<constraint_t> constraints;
    std::vector<parameter_t> parameters;
    /// nullopt when the function returns nothing.
    std::optional<type_expr_id_t> return_type;
    /// The Classes after Throws: the errors it can throw extend them.
    std::vector<type_expr_id_t> throws;
    /// For the one-line form, "Function Name() => value", a single Return of the value. An
    /// Abstract Method has none.
    block_t body;
    modifiers_t modifiers;
    /// In an Interface: written as its first line alone, without a body, so each type that
    /// implements the Interface gives it.
    bool required = false;
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

/// What a loop over a type's members goes over.
enum class member_kind_t : std::uint8_t {
    FIELD,
    METHOD,
    PROPERTY,
    STATIC_FIELD,
};

/// "For Field f: F In T", "For Method m In T", "For Property p: P In T", or "For Static Field
/// s: S In T": a loop over a type's members, which the compiler unrolls into a copy of the body
/// for each one.
struct for_member_stmt_t {
    std::optional<name_t> label;
    member_kind_t kind = member_kind_t::FIELD;
    name_t variable;
    /// The "F" in "For Field f: F In T": a name for each member's type in the body, or for a
    /// Method's, the Func type its Where line gives.
    std::optional<name_t> type_name;
    type_expr_id_t type;
    /// The Where lines at the top of the body, on the member's type.
    std::vector<constraint_t> constraints;
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

/// "Struct Name ... End Struct", "Class Name Extends Base Implements A, B ... End Class", or
/// "Interface Name Extends A, B ... End Interface". Its members are Fields (variable_stmt_t),
/// Methods, Operators, and News (function_stmt_t), and Properties, in source order.
struct type_stmt_t {
    /// "Class" rather than "Struct".
    bool is_class = false;
    /// "Interface": what the types that implement it have. Its members without a body are
    /// required, and the others are defaults.
    bool is_interface = false;
    /// "Attribute Rename(Name: String)": a Struct whose fields are its parameters, which goes on
    /// declarations as "@Rename("hp")" for reflection to read.
    bool is_attribute = false;
    name_t name;
    /// The names in "Class Stack<T>": a generic type's type parameters.
    std::vector<name_t> type_parameters;
    /// For each type parameter written "Const W: Int", the type of its value, at the same place
    /// as its name. Empty when none is Const.
    std::vector<std::optional<type_expr_id_t>> value_types;
    /// What its type parameters implement, from "<T: Printable>" and the Where lines at the top
    /// of its body.
    std::vector<constraint_t> constraints;
    block_t members;
    /// The Class after Extends, and its type arguments when it's generic: "Extends Base<Int>".
    std::optional<name_t> base;
    std::vector<type_expr_id_t> base_arguments;
    /// The Interfaces after Implements, or for an Interface, after Extends.
    std::vector<type_expr_id_t> interfaces;
    modifiers_t modifiers;
};

/// "Property Name: Type", with "Get => value" or a Get block, and maybe "Set(value)" and its
/// block. Each part is a Method the parser makes: the Get takes nothing and returns the value,
/// and the Set takes it as its one parameter.
struct property_stmt_t {
    name_t name;
    type_expr_id_t type;
    /// A function_stmt_t. nullopt when it's missing, which the parser reported.
    std::optional<stmt_id_t> getter;
    std::optional<stmt_id_t> setter;
    modifiers_t modifiers;
    /// Written as its first line alone, in an Interface or as Abstract: each type that implements
    /// the Interface, or Class that extends this one, gives its Get, and its Set unless it's
    /// ReadOnly.
    bool required = false;
};

/// "Defer statement": runs the statement when the enclosing block exits, however it exits.
struct defer_stmt_t {
    stmt_id_t statement;
};

/// "Throw error": the code here ends with an error, an object of a Class that extends Error.
struct throw_stmt_t {
    expr_id_t error;
};

/// "Catch e As FileError", or "Catch e" for any error, and the code it runs.
struct catch_t {
    name_t name;
    /// nullopt for "Catch e", which catches every Error.
    std::optional<type_expr_id_t> type;
    block_t body;
};

/// "Try ... Catch ... Finally ... End Try". An error in the body goes to the first Catch that
/// matches it, and the Finally runs however the Try ends. It has a Catch or a Finally.
struct try_stmt_t {
    block_t body;
    std::vector<catch_t> catches;
    std::optional<block_t> finally_body;
};

/// "Extend Type Implements A, B ... End Extend": Methods, Operators, Properties, and Interfaces
/// added to a type that's declared already. Its type is a Struct, Class, Enum, or built-in
/// type, a generic one written with its own type parameters, like "Box<T>", or "T[]" for every
/// array.
struct extend_stmt_t {
    type_expr_id_t type;
    std::vector<type_expr_id_t> interfaces;
    /// From the Where lines at the top of its body, which only some instances, or arrays, meet.
    std::vector<constraint_t> constraints;
    block_t members;
};

/// One of an Enum's members: "Name", or "Name = value".
struct enum_member_t {
    name_t name;
    std::optional<expr_id_t> value;
};

/// "Enum Name: Type ... End Enum", or "@Flags Enum Name ... End Enum". Members are separated by
/// commas or lines.
struct enum_stmt_t {
    name_t name;
    /// The integer type after ':'. nullopt for Int.
    std::optional<type_expr_id_t> type;
    std::vector<enum_member_t> members;
    /// Written with @Flags: the values are bits, which combine.
    bool flags = false;
    /// Its attributes, like @Deprecated.
    modifiers_t modifiers;
};

using stmt_node_t =
    std::variant<error_stmt_t, expression_stmt_t, assign_stmt_t, increment_stmt_t, variable_stmt_t,
                 function_stmt_t, if_stmt_t, while_stmt_t, repeat_stmt_t, loop_stmt_t, for_stmt_t,
                 for_in_stmt_t, select_stmt_t, break_stmt_t, continue_stmt_t, return_stmt_t,
                 type_stmt_t, enum_stmt_t, defer_stmt_t, throw_stmt_t, try_stmt_t, property_stmt_t,
                 destructure_stmt_t, extend_stmt_t, for_member_stmt_t>;

struct stmt_t {
    span_t span;
    stmt_node_t node;
};

/// A parsed program: the main file and the library files the compiler adds, in one AST. It holds
/// every node, and its items are the top-level statements in source order, the library files'
/// first. Names and spans refer to the source files' text.
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

    /// The program's main file, whose top-level code runs. Any other file is a library file,
    /// which only declares things.
    void set_main_file(const file_id_t file) {
        m_main = file;
    }

    /// Whether a statement is in a library file rather than the main file.
    [[nodiscard]] bool from_library(const stmt_id_t id) const {
        return get(id).span.file != m_main;
    }

private:
    std::vector<expr_t> m_exprs;
    std::vector<stmt_t> m_stmts;
    std::vector<type_expr_t> m_types;
    block_t m_items;
    file_id_t m_main{};
};

} // namespace fb
