#pragma once

#include "fb/ast.hpp"
#include "fb/const_value.hpp"
#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
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
    /// A Struct, Class, Interface, or Enum. Its `type` is the type itself.
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
    /// A For Field's or For Method's variable: one of the fields or Methods of its `owner`,
    /// which each copy of the loop's body has its own of. Its `type` is the type parameter for
    /// the field's type, or NONE for a Method.
    MEMBER_VARIABLE,
    /// The qualifier an Import gives, like "collections" or an As name: it stands for its
    /// `modules` before a '.', as in "collections.Stack<Int>()". It isn't a value.
    MODULE,
};

/// A constraint, as analysis keeps it: a type parameter, and an Interface that its type argument
/// implements.
struct bound_t {
    type_id_t parameter = type_id_t::INVALID;
    type_id_t interface = type_id_t::INVALID;
    /// For a For Field loop's Where line: the type parameter for each field's type. Then the
    /// constraint is on `parameter`'s fields, each of whose types implements `interface`, with
    /// the field's type in place of this. A For Property's or For Static Field's is on those.
    type_id_t field = type_id_t::INVALID;
    member_kind_t members = member_kind_t::FIELD;

    friend bool operator==(const bound_t&, const bound_t&) = default;
};

/// An attribute that an Attribute declaration declares, on a declaration: its type, a Struct of
/// its parameters, and their values.
struct attribute_value_t {
    type_id_t type = type_id_t::INVALID;
    std::vector<const_value_t> values;
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
    /// A parameter written "nums: Int...". Its type is the Int[] that a call packs its last
    /// arguments into.
    bool variadic = false;
    /// An Extern "runtime" Function that gives a Task<T>: its call makes the Task, which the
    /// runtime takes before the arguments and finishes later, its result right after its
    /// fb_task_t.
    bool gives_task = false;
    /// A variadic parameter written "values: Printable...", with std's Printable: a call shows
    /// each of its last arguments, of any type a string can show, as "{x}" does, so it's a
    /// String[] of their texts.
    bool shows = false;
    /// A parameter that calls can leave out. ByRef parameters never have a default. A field is
    /// a parameter of the constructor a Struct gets, so it has one, its own or zero, unless its
    /// type has no zero.
    bool has_default = false;
    /// An Extern "C" Function's CString parameter, which a String argument becomes for the call.
    bool converts_string = false;
    /// An Extern "C" Function's CString parameter that a String can't stand for, since the
    /// Function gives a CString, or takes one ByRef, which could point into the String's copy.
    bool string_escapes = false;
    /// A Const whose initializer had an error that left its value unknown, which is quiet where
    /// it's used.
    bool failed = false;
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
    /// An Interface that needs Statics or Operators, or uses Self in its members' types: it's
    /// only a constraint, never a type of its own.
    bool constraint_only = false;
    /// A member that an Extend adds. It isn't in the Method table, so nothing overrides it, and
    /// it overrides nothing.
    bool extended = false;
    /// A Property's Get and Set. A Property that overrides another may leave out its Set, and
    /// then uses that one's.
    std::optional<symbol_id_t> getter;
    std::optional<symbol_id_t> setter;
    /// A Method that a Struct or Class gets from an Interface's default: that Interface's
    /// Method, whose code it runs, with the Struct or Class for Self.
    std::optional<symbol_id_t> default_method;
    /// With a default_method: the Interface it implements, an instance like "Iterator<Int32>"
    /// for a generic one, whose type arguments the default's code runs with.
    type_id_t default_interface = type_id_t::INVALID;
    /// A Local that a lambda captures: the Class of the cell it lives in, which the lambda shares
    /// with the code around it.
    type_id_t cell = type_id_t::INVALID;
    /// A generic Function's or Method's own type parameters. A generic Struct's or Class's are in
    /// its type.
    std::vector<type_id_t> type_parameters;
    /// A generic Struct's, Class's, or function's constraints. A Method's own include the ones
    /// its Where lines put on its type's type parameters, which only some instances meet.
    std::vector<bound_t> constraints;
    /// The attributes that Attribute declarations declare, on its declaration, in order.
    std::vector<attribute_value_t> attributes;
    /// A MODULE's modules: the one its Import names, or each file of a folder.
    std::vector<module_id_t> modules;
};

/// How a Struct or Class implements an Interface, directly or through one it lists that extends
/// it.
struct conformance_t {
    /// The Struct or Class. A generic one's instances implement the Interface the same way.
    type_id_t type;
    /// The Interface, an instance for a generic one, like "Iterator<T>" for a "Range<T>". A type
    /// implements one instance of a generic Interface.
    type_id_t interface;
    /// For each of the Interface's own members, in order: the member of `type` that implements
    /// it, its own, one it inherits, or one it got from the Interface's default.
    std::vector<symbol_id_t> members;
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
    /// A Parallel For's body: the For, whose code goes over a chunk of its indexes.
    std::optional<stmt_id_t> body;
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
    /// For a name: "Count" or "Capacity" alone in an Extend of T[], which is Self's. `symbol`
    /// is Self.
    bool count = false;
    /// For "game.score" or "collections.Stack": a name after a module's qualifier, which works
    /// as the name alone would. The qualifier's `symbol` is its MODULE.
    bool qualified = false;
    /// For "AddressOf(x)": a Ptr to its argument's place.
    bool address_of = false;
    /// For "TypeName<T>()" where T has type parameters in it: the type whose name, with the
    /// instance's type arguments, the call gives.
    std::optional<type_id_t> type_name;
    /// For a Function's name or a lambda where a Callback is expected: a C function pointer to
    /// its code, the Function `symbol` names or the lambda's `closure`.
    bool callback = false;
    /// For "T.Has(A)" or "T.Attribute(A)": T, whose own attributes it reads. `tested` is A.
    type_id_t reflected = type_id_t::INVALID;

    /// For 'Is': the Class it tests for.
    type_id_t tested = type_id_t::INVALID;
    /// For the name of a Const type parameter, like the W in "Grid<T, Const W: Int>": the
    /// parameter, whose value each instance has.
    type_id_t parameter = type_id_t::INVALID;

    /// For a call: one entry per parameter of the function, in parameter order. Each is the
    /// argument's expression, or nullopt when the parameter takes its default value. Empty
    /// when the arguments couldn't be matched to the parameters. For an operator that calls an
    /// Operator, whose symbol is `symbol`: its operands.
    std::vector<std::optional<expr_id_t>> arguments;
    /// For a call with a variadic parameter whose entry in `arguments` is nullopt: the
    /// arguments packed into its new array, maybe none.
    std::vector<expr_id_t> packed;

    /// For a lambda, or a Function's name used as a value: its closure's index in
    /// analysis_t::closures.
    std::optional<std::size_t> closure;

    /// For a call of a generic Function or Method, or a generic Function used as a value: a type
    /// argument for each of the function's own type parameters. Those of a generic Struct or
    /// Class come from the type of the object, or of the one New makes. For an operator that
    /// calls an Operator of a generic type, its type's. For a call of an Interface's member, or
    /// a Static Property used through a type parameter: the type for Self, the one it's used on,
    /// then a generic Interface's type arguments.
    std::vector<type_id_t> type_arguments;
};

/// What analysis found out about one statement.
struct stmt_info_t {
    /// What it declares: a Function's symbol, a Local's, Global's, or Const's, or a For loop's
    /// variable.
    std::optional<symbol_id_t> symbol;
    /// A Parallel For's body, as a closure, by its index in `analysis_t::closures`.
    std::optional<std::size_t> closure;
    /// For Break and Continue: the loop statement they leave or continue.
    std::optional<stmt_id_t> loop;
    /// For a Select of type matches, or a Try's Catches: each one's variable, in order. For a
    /// Local that takes a tuple apart, each new Local, without the "_"s.
    std::vector<symbol_id_t> matches;
    /// For a "For index, value In": the index's variable. `symbol` is the value's. Over an
    /// Iterator, they're the two parts of each element.
    std::optional<symbol_id_t> index;
    /// For a "For ... In" over an Iterator: its Next, which each pass calls, and the Iterate
    /// Method that gives the Iterator, when the sequence isn't one itself.
    std::optional<symbol_id_t> next;
    std::optional<symbol_id_t> iterate;
    /// For a Select on an Enum without a Default: whether its Cases cover every value, so no
    /// path goes past them all.
    bool exhaustive = false;
    /// For "x op= y" with an Operator, whose symbol is `symbol`: a type argument for each type
    /// parameter of its Struct or Class. For a loop over a type's members, whose variable is
    /// `symbol`: the type it goes over, then the type parameter for each member's type, then for
    /// a For Method, the Func type of the Methods it goes over.
    std::vector<type_id_t> type_arguments;
};

/// An Interface that an Extend makes a type implement: a built-in type, an Enum, every T[], or
/// the instances of a generic Struct or Class whose type arguments meet the Extend's
/// constraints. A Struct's or Class's that has no constraints is among its type's interfaces
/// instead.
struct extension_interface_t {
    /// The type, or T[], or the generic Struct or Class.
    type_id_t target = type_id_t::INVALID;
    type_id_t interface = type_id_t::INVALID;
    /// The type parameters its constraints are on: T[]'s T, or the generic type's own.
    std::vector<type_id_t> parameters;
    std::vector<bound_t> bounds;
};

/// The Interfaces in std/core that the language's operators, strings, and loops go through.
/// Each is INVALID without the standard library.
struct builtin_interfaces_t {
    type_id_t equatable = type_id_t::INVALID;
    type_id_t comparable = type_id_t::INVALID;
    type_id_t hashable = type_id_t::INVALID;
    type_id_t printable = type_id_t::INVALID;
    type_id_t numeric = type_id_t::INVALID;
    /// What makes a copy of itself, which arrays are with their own Clone.
    type_id_t cloneable = type_id_t::INVALID;
    /// What has a value to start from, which the numbers, Bool, String, and Enums are with their
    /// zero.
    type_id_t defaultable = type_id_t::INVALID;
    /// Iterator<T>, from std/core/iterator.fb, which 'For ... In' goes over.
    type_id_t iterator = type_id_t::INVALID;
    /// What can go to another thread, which the compiler decides from a type's fields.
    type_id_t sendable = type_id_t::INVALID;
};

/// Everything analysis found out about a file, for lowering and tools. `exprs` and `stmts` have
/// one entry per AST node, indexed by its id; nodes in code with errors may keep the defaults.
/// Making its deque can allocate, which clang-tidy reports as an exception escaping.
struct analysis_t { // NOLINT(bugprone-exception-escape)
    /// Indexed by symbol_id_t. The builtins come first. A deque, so a reference to a symbol
    /// stays valid while analysis adds more, like a lambda's parameters inside a call.
    std::deque<symbol_t> symbols;
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
    builtin_interfaces_t interfaces;
    /// std's Boxed<S, T>, from std/core/iterator.fb: the Class a Struct that implements
    /// Iterator<T> goes in where a Method of the Interface it implements gives an Iterator<T>,
    /// as a collection's Iterate() does for Iterable<T>'s. INVALID without the standard library.
    type_id_t boxed_iterator = type_id_t::INVALID;
    /// A CString's ToString, which copies its text into a String in Unsafe code, once a program
    /// uses it.
    std::optional<symbol_id_t> cstring_text;
    /// The function values the program makes. The expression that makes each one, a lambda or
    /// a Function's name, has its index.
    std::vector<closure_t> closures;
    /// How each Struct and Class implements each of its Interfaces, and each type an Extend
    /// makes implement one.
    std::vector<conformance_t> conformances;
    /// What Extends make types implement, besides a Struct's or Class's own Interfaces.
    std::vector<extension_interface_t> extension_interfaces;
    /// The Interfaces that are only constraints, as symbol_t::constraint_only marks them, which
    /// a Class doesn't have from a Class it extends: its Self would be the other Class.
    std::vector<type_id_t> constraint_only;
    /// What Extends of one instance of a generic Struct or Class add to it, by instance, like
    /// Box<Int32>'s: its Methods, Operators, and Properties, and the defaults of the Interfaces
    /// they list. The generic type's own members have what Extends of all its instances add.
    std::unordered_map<type_id_t, std::vector<symbol_id_t>> instance_members;

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

/// Whether `type`, with no type parameters in it, implements `interface`: by listing it or
/// one that extends it, through a Class it extends, as a built-in type, or through an Extend
/// whose constraints its type arguments meet. Lowering asks it of the instances it makes, for
/// their Interfaces' tables. `types` is the type table those instances are in.
[[nodiscard]] bool implements(const analysis_t& analysis, type_table_t& types, type_id_t type,
                              type_id_t interface);

/// Resolves names, checks types and that variables are assigned before they're read, folds
/// constants, and reports errors to `diagnostics`. Each of the modules in `ast` has its own
/// top-level names, and sees those of the modules it imports after them, as the module table
/// says; `sources` holds their files. It always returns a full analysis, but only one without
/// errors can be lowered.
[[nodiscard]] analysis_t analyze(const source_manager_t& sources, const ast_t& ast,
                                 diagnostic_bag_t& diagnostics);

} // namespace fb
