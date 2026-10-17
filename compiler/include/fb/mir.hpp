#pragma once

#include "fb/ast.hpp"
#include "fb/const_value.hpp"
#include "fb/source.hpp"
#include "fb/style.hpp"
#include "fb/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fb {

/// An index into a function's locals.
enum class local_id_t : std::uint32_t {};
/// An index into the module's globals.
enum class global_id_t : std::uint32_t {};
/// An index into the module's functions.
enum class function_id_t : std::uint32_t {};
/// An index into a function's blocks.
enum class block_id_t : std::uint32_t {};

/// A variable in a function: a parameter, a Local, a loop variable, or a temporary that
/// lowering made.
struct local_t {
    type_id_t type = type_id_t::INVALID;
    /// Empty for temporaries.
    std::string_view name;
    /// A ByRef parameter. It stands for the caller's variable, so assigning it changes theirs.
    bool by_ref = false;
};

/// A top-level Global or Const, or a Static Field.
struct global_t {
    /// "score", or "Enemy.count" for a Static Field.
    std::string name;
    type_id_t type = type_id_t::INVALID;
    /// Its value when the program starts: the initializer's when that's known at compile time,
    /// else zero, False, or "" until main sets it. nullopt for a Struct, which starts with every
    /// field at zero.
    std::optional<const_value_t> initial;
};

/// One step inward from a place: into a field, or into an element of an array.
struct step_t {
    /// The field's position, for a Struct, a Class's object, or a T?'s value.
    std::uint32_t field = 0;
    /// For an element: the locals holding its indices, one per dimension. Lowering also goes
    /// through a fixed array of several dimensions with one index, which counts every element.
    /// Empty for a field.
    std::vector<local_id_t> indices;
    /// Where an index that's out of range panics.
    span_t span;
};

/// Somewhere a value is stored: a local or a global, or a field or element inside one, like
/// "_1.pos.x" or "_2[_3]".
struct place_t {
    std::variant<local_id_t, global_id_t> root;
    /// The steps from the root inward.
    std::vector<step_t> steps;

    // Implicit, so a local or global can be used wherever a place goes.
    place_t(const local_id_t local) : root(local) {}
    place_t(const global_id_t global) : root(global) {}
};

/// A value an instruction reads: a constant, or what a place holds.
using operand_t = std::variant<const_value_t, place_t>;

/// An operand's value, as it is.
struct use_t {
    operand_t value;
};

/// "-x", "!x", or "~x". A debug build panics at `span` when '-' overflows.
struct unary_t {
    unary_op_t op;
    operand_t operand;
    span_t span;
};

/// An arithmetic, bitwise, comparison, or Bool operator. Both operands have the same type,
/// except that a shift's amount can be any integer. Comparisons give a Bool, and the rest give
/// the left operand's type. && and || evaluate both sides here; short-circuiting already
/// happened with blocks. Strings use runtime calls instead. A T? only has == and !=: a String?
/// or Class? compares references, and any other T? only with a Null constant, which tells
/// whether it holds a value.
struct binary_t {
    binary_op_t op;
    operand_t left;
    operand_t right;
    /// Where a panic points: overflow, division by zero, or a shift amount out of range.
    span_t span;
    /// Integer + - * wrap around on overflow instead of panicking. Release builds always wrap;
    /// this makes debug builds wrap too. For loops use it where wrapping is intended.
    bool wrapping = false;
};

/// Converts between numbers, or from a Bool to an integer (0 or 1), for 'As' and for implicit
/// conversions. Integers are cut down or extended. A float to an integer drops the fraction,
/// and panics at `span` when the rest doesn't fit. A T, or a T?, converts to a U? whose U the
/// T converts to.
struct cast_t {
    operand_t value;
    type_id_t type;
    span_t span;
};

struct call_argument_t {
    operand_t value;
    /// `value` is a place, and the function gets its address.
    bool by_ref = false;
};

/// Calls a function in this module and gives its result.
struct call_t {
    function_id_t function;
    /// One per parameter, in order, with default values already filled in.
    std::vector<call_argument_t> arguments;
    /// For a Method that a Class could override: its slot in the Method table of the object's
    /// Class, which has the one to call. `function` is the one the receiver's type has, which
    /// takes the same arguments. A Class's Self takes an object of any Class extending it.
    std::optional<std::uint32_t> slot;
};

/// The runtime library functions MIR calls; see runtime/include/fb/runtime.hpp. A String they
/// return is new, and its holder must release it.
enum class runtime_function_t : std::uint8_t {
    /// (String, String) to a new String.
    CONCAT,
    /// (String...) to a new String: the parts of an interpolated string, joined.
    JOIN,
    /// (String, String) to Bool.
    EQUAL,
    /// (a number or Bool) to a new String, as interpolation shows it.
    TO_TEXT,
    /// (String) to nothing: prints it and a line break.
    PRINT,
    /// (T[]) to its Count, an Int32.
    ARRAY_COUNT,
    /// (T[], T) to nothing: adds the value at the end, taking over its references.
    ARRAY_ADD,
    /// (T[], an integer, T) to nothing: puts the value at the index, taking over its references.
    ARRAY_INSERT,
    /// (T[], an integer) to nothing: releases the element at the index and takes it out.
    ARRAY_REMOVE_AT,
    /// (T[]) to nothing: releases every element, leaving none.
    ARRAY_CLEAR,
    /// (T[]) to a new T[] with the same elements, retained.
    ARRAY_CLONE,
    /// (Error?) to nothing: prints an error that top-level code let out, which nothing caught, and
    /// ends the program with exit code 1.
    ERROR_UNCAUGHT,
    /// (Weak<T>) to a T?: a new reference to its object, or Null once the object is gone.
    WEAK_LOAD,
    /// (T) to nothing: lets go of an object whose New threw. Its fields are released, and its
    /// Destroy doesn't run.
    OBJECT_DISCARD,
};

struct runtime_call_t {
    runtime_function_t function;
    std::vector<operand_t> arguments;
    /// For Insert and RemoveAt: where an index out of range panics.
    std::optional<span_t> span;
};

/// A Struct value made of one operand per field, in order. It holds the fields' references,
/// which lowering has already given it.
struct aggregate_t {
    type_id_t type;
    std::vector<operand_t> fields;
};

/// A new T[] holding the elements, which have their references already, or a fixed array's
/// value made of them.
struct array_t {
    type_id_t type;
    std::vector<operand_t> elements;
};

/// The value every variable of `type` starts with: zero, False, "", or Null, and the same for
/// each field of a Struct. A Class's is null, which only a variable that isn't assigned yet
/// holds.
struct zero_t {
    type_id_t type;
};

/// A new object of a Class, with one owner and every field zeroed. Lowering sets the fields
/// that need another value next.
struct new_t {
    type_id_t type;
};

/// "x Is T": a Bool, whether `value`'s object is of the Class `type`, or of one that extends it.
/// False for Null.
struct is_t {
    operand_t value;
    type_id_t type;
};

/// An object as a Class that extends its type: the same reference, as a `type`. It panics at
/// `span` unless the object is of that Class, or one extending it. Null panics too, unless
/// `type` is a T?. With `or_null`, for 'As?', it gives Null instead of panicking.
struct downcast_t {
    operand_t value;
    type_id_t type;
    span_t span;
    bool or_null = false;
};

/// Something a statement computes.
/// Calls a function value: the code in slot 0 of its object's Method table, with the object
/// first, then the arguments.
struct invoke_t {
    operand_t callee;
    std::vector<operand_t> arguments;
};

using rvalue_t = std::variant<use_t, unary_t, binary_t, cast_t, call_t, runtime_call_t, aggregate_t,
                              array_t, zero_t, new_t, is_t, downcast_t, invoke_t>;

/// "target = value". It doesn't count references; lowering adds retain_t and release_t for
/// Strings, and for Structs that hold them.
struct assign_t {
    place_t target;
    rvalue_t value;
};

/// Computes `value` for its effects and drops the result, like a call that returns nothing.
struct evaluate_t {
    rvalue_t value;
};

/// Adds a reference to a String for a new holder, or to every String in a Struct. String
/// constants are static, and retain and release ignore them.
struct retain_t {
    operand_t value;
};

/// Drops the reference a String place holds. The String is freed with its last reference.
struct release_t {
    place_t target;
};

/// Statements run in order, inside a block.
using statement_t = std::variant<assign_t, evaluate_t, retain_t, release_t>;

struct goto_t {
    block_id_t target;
};

/// Goes to `if_true` or `if_false` on a Bool.
struct branch_t {
    operand_t condition;
    block_id_t if_true;
    block_id_t if_false;
};

/// `value` is nullopt in a function that returns nothing.
struct return_t {
    std::optional<operand_t> value;
};

/// Stops the program, printing `message` and the line and column of `span`. For loops use it
/// for a Step that's 0 at run time, and 'As' for an integer that isn't one of an Enum's values.
struct panic_t {
    std::string message;
    span_t span;
    /// For Panic and Assert: a String the program made, shown instead of `message`.
    std::optional<operand_t> text;
};

/// Ends a path that can't run, like the end of a function that returns on every path. A block
/// has it until it gets a real terminator.
struct unreachable_t {};

/// How a block ends: where control goes next.
using terminator_t = std::variant<goto_t, branch_t, return_t, panic_t, unreachable_t>;

/// Statements that run in order, then the terminator that leaves the block.
struct basic_block_t {
    std::vector<statement_t> statements;
    terminator_t terminator = unreachable_t{};
};

/// A Function, or "main" for the top-level code.
struct mir_function_t {
    /// The name with its parameter types, like "Add(Int32, Int32)", so overloads differ.
    std::string name;
    /// The parameters come first.
    std::vector<local_t> locals;
    std::size_t parameter_count = 0;
    /// NONE when it returns nothing.
    type_id_t return_type = type_id_t::NONE;
    /// The function starts at blocks[0].
    std::vector<basic_block_t> blocks;
};

/// A Class, for the runtime: codegen makes its fb_class_t, and the destroy that runs its
/// Destroy() and releases its fields.
struct mir_class_t {
    type_id_t type;
    /// The Class's own Destroy() Method, if it has one. The ones of the Classes it extends run
    /// after it.
    std::optional<function_id_t> destroy;
    /// The Method table: for each slot, the Method its objects run.
    std::vector<function_id_t> methods;
};

/// A whole program, lowered from one file. Names view the source text, so the file must
/// outlive the module.
struct mir_module_t {
    std::vector<global_t> globals;
    std::vector<mir_class_t> classes;
    /// The Functions in source order, then the entry.
    std::vector<mir_function_t> functions;
    /// "main": the top-level code, which runs when the program starts. Globals whose values
    /// aren't known at compile time are set at its start.
    function_id_t entry{};
    /// The types that locals, globals, and functions refer to: the analysis's, and the ones
    /// lowering made for the instances of generic code.
    std::shared_ptr<type_table_t> types;
};

/// The module as text, which `fb --show mir` prints.
[[nodiscard]] std::string dump(const mir_module_t& module, const style_t& style = {});

} // namespace fb
