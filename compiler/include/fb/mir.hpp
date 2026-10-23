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
#include <utility>
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
    /// "score", or "Enemy.count" for a Static Field, with its module first outside the main
    /// module: "std.core.limits.score".
    std::string name;
    type_id_t type = type_id_t::INVALID;
    /// Its value when the program starts: the initializer's when that's known at compile time,
    /// else zero, False, or "" until main sets it. nullopt for a type whose zero isn't a
    /// constant, like a Struct's, a Class's, a T[]'s, or a fixed array's, which starts at its
    /// zero.
    std::optional<const_value_t> initial;
    /// Main sets it with its initializer, first. A function that an earlier one's initializer
    /// calls could use it before that, which debug builds check (ready_t), and release builds
    /// too where its zero holds a null object.
    bool set_in_main = false;
    /// "ThreadLocal Global": each thread has its own, which starts at `initial` and which
    /// `initializer`, when it has one, sets the first time the thread uses it (ready_t). The
    /// runtime releases it as the thread ends.
    bool is_thread_local = false;
    /// A ThreadLocal's initializer, which sets the calling thread's value. Lowering makes it the
    /// first time code uses the Global; nullopt for one whose value is known at compile time, or
    /// that has no initializer.
    std::optional<function_id_t> initializer;
    /// Its place in the order main sets the Globals, the program's code's, where a Static Field
    /// is at its Class's, a generic Class's too. The program lets go of them in the other order.
    std::uint32_t order = 0;
};

/// One step inward from a place: into a field, or into an element of an array. From a Ptr,
/// its field is what it points at, and an element is that many T's on.
struct step_t {
    /// The field's position, for a Struct, a Class's object, or a T?'s value.
    std::uint32_t field = 0;
    /// For an element: the locals holding its indices, one per dimension. Lowering also goes
    /// through a fixed array of several dimensions with one index, which counts every element.
    /// Empty for a field.
    std::vector<local_id_t> indices;
    /// Where an index that's out of range panics, or a step from a Null Ptr in a debug build.
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
    /// Gives a Bool instead: whether integer + - * would overflow, or / would divide by zero or
    /// overflow. An integer's Checked Methods test with it.
    bool overflows = false;
};

/// Converts between numbers, or from a Bool to an integer (0 or 1), for 'As' and for implicit
/// conversions. Ptrs and CStrings convert to each other as they are, and to and from integers as
/// addresses. Integers are cut down or extended. A float to an integer drops the fraction,
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
    /// For a call through an Interface's object: the Interface. `slot` is the place in the table
    /// that the object's Class has for it, and `function` is the Interface's own, with the
    /// Interface for Self, which takes the same arguments.
    type_id_t interface = type_id_t::INVALID;
    /// For a @PanicsAtCaller function: where the call is, which its panics report. nullopt where
    /// no code calls it, as in a function value's code, so its panics report where they are.
    std::optional<span_t> place;
    /// Where the call is in the code, which compile-time evaluation's errors point at.
    std::optional<span_t> span;
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
    /// (Int64, String) to a new String: the integer's text by a format spec that fits it, the
    /// part after an interpolation's ':'.
    FORMAT_INT,
    /// (UInt64, String) to a new String, the same for an unsigned integer.
    FORMAT_UINT,
    /// (Float64, Bool, String) to a new String, the same for a float, a Float32 when the Bool is.
    FORMAT_FLOAT,
    /// (String, Bool, String) to a new String: text padded and cut by a format spec that fits
    /// it, a String's when the Bool is, which its debug form puts in quotes.
    FORMAT_TEXT,
    /// (String) to a new String: its debug form, in quotes with source code's escapes.
    DEBUG_STRING,
    /// (an object, Int32) to a new String: its debug form, by its own Class, at that indent, or
    /// on one line below 0.
    DEBUG_OBJECT,
    /// (Int32) to a new String: a line break and the indent, for a debug form on many lines.
    DEBUG_INDENT,
    /// (String) to a new CString: a copy of its text with a NUL after it, for C code to read
    /// during a call.
    CSTRING_NEW,
    /// (CString) to nothing: frees a copy that CSTRING_NEW made.
    CSTRING_FREE,
    /// (CString) to a new String?: a copy of C's text up to its NUL, or Null for a null one.
    STRING_FROM_CSTRING,
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
    /// (T[]) to its Capacity, an Int32: how many elements it has room for.
    ARRAY_CAPACITY,
    /// (T[], an integer) to nothing: makes room for that many elements in all.
    ARRAY_RESERVE,
    /// (T[]) to nothing: gives back the room beyond its Count.
    ARRAY_TRIM,
    /// (Error?) to nothing: prints an error that top-level code let out, which nothing caught, and
    /// ends the program with exit code 1.
    ERROR_UNCAUGHT,
    /// (an Error) to nothing: sets its Place to where `span` is, unless an earlier Throw did.
    ERROR_THROWN,
    /// () to Bool: whether this is a debug build, which codegen makes a constant, so a release
    /// build leaves out the code only a debug build runs.
    DEBUG_BUILD,
    /// (Weak<T>) to a T?: a new reference to its object, or Null once the object is gone.
    WEAK_LOAD,
    /// (T) to nothing: lets go of an object whose New threw. Its fields are released, and its
    /// Destroy doesn't run.
    OBJECT_DISCARD,
    /// (String, String) to an Int32: below 0, 0, or above 0 as the first comes before the
    /// second, is the same, or comes after, for `<` and the rest on Strings.
    COMPARE,
    /// (a number, Bool, Enum, String, or Class's object) to a UInt64 hash, the same for equal
    /// values.
    HASH,
    /// (UInt64, UInt64) to a UInt64: a hash so far, combined with the next part's.
    HASH_COMBINE,
    /// (String, Int32[]) to nothing: adds the String's code points to the array.
    STRING_CHARS,
    /// (String, UInt8[]) to nothing: adds the String's UTF-8 bytes to the array.
    STRING_BYTES,
    /// (Task) to nothing: starts its work on the pool.
    TASK_START,
    /// (Thread) to nothing: starts its work on a thread of its own.
    THREAD_START,
    /// (Task or Thread) to nothing: returns once its work is done.
    TASK_WAIT,
    /// (Task or Thread) to a Bool: whether its work is done.
    TASK_DONE,
    /// (Int64, Int64, a Parallel For's body) to nothing: runs the body's code for chunks of the
    /// indexes from the first up to the second, on the pool, and returns once they're done.
    PARALLEL_FOR,
    /// (Channel<T>, a place holding a T) to nothing: sends the T, with its references, waiting
    /// while the Channel is full. Panics at the span once it's closed.
    CHANNEL_SEND,
    /// (Channel<T>, a place holding a T) to a Bool: whether it sent the T, which it does only
    /// when there's room and the Channel is open.
    CHANNEL_TRY_SEND,
    /// (Channel<T>, a place for a T) to a Bool: puts the oldest T there, with its references,
    /// waiting while the Channel is empty, or gives False once it's closed and empty.
    CHANNEL_RECEIVE,
    /// (Channel<T>, a place for a T) to a Bool: like CHANNEL_RECEIVE, without waiting.
    CHANNEL_TRY_RECEIVE,
    /// (Channel<T>) to nothing: closes it.
    CHANNEL_CLOSE,
    /// (Task) to nothing: gives a new Task its completion, which an Async Function finishes.
    ASYNC_BEGIN,
    /// (Task) to nothing: the Async Function is done, which wakes what waits for it.
    ASYNC_FINISH,
    /// (Task, this Async Function's own Task) to nothing: the scheduler running it resumes this
    /// coroutine once the first Task is done. A suspend_t comes right after.
    AWAIT,
    /// (Task) to nothing: the scheduler running the code finishes it at its next tick.
    WAIT_FRAME,
    /// (Task, Int64 nanoseconds, Bool real) to nothing: the scheduler running the code finishes it
    /// once that much of its time, or the real clock's, has passed.
    WAIT_TIME,
    /// (Scheduler, Task) to nothing: the Scheduler runs the Task's coroutine from then on.
    SCHEDULER_RUN,
    /// (Scheduler, Float64 seconds) to nothing: moves its time on and runs its coroutines.
    SCHEDULER_TICK,
    /// (Scheduler) to a Bool: whether no coroutine waits on it.
    SCHEDULER_IDLE,
};

struct runtime_call_t {
    runtime_function_t function;
    std::vector<operand_t> arguments;
    /// Where the call is: where one that can panic, like Insert's or a Channel's Send, says it
    /// does, and what compile-time evaluation's errors point at.
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

/// Calls a function value: the code in slot 0 of its object's Method table, with the object
/// first, then the arguments.
struct invoke_t {
    /// A function value, whose code is in slot 0 of its object's Method table, or a Callback,
    /// which is called by the C ABI.
    operand_t callee;
    std::vector<operand_t> arguments;
    /// Where a debug build panics on a Null Callback.
    span_t span;
};

/// What an atomic_t does.
enum class atomic_op_t : std::uint8_t {
    /// (T) to a new Atomic<T> holding it.
    MAKE,
    /// () to the T it holds.
    LOAD,
    /// (T) to nothing: holds the T instead.
    STORE,
    /// (T) to the T it held before adding it, wrapping around.
    ADD,
    /// (T) to the T it held before subtracting it, wrapping around.
    SUBTRACT,
    /// (T) to the T it held before holding this one.
    EXCHANGE,
    /// (expected, desired) to a Bool: whether it held the expected T, and now holds the desired
    /// one.
    COMPARE_EXCHANGE,
};

/// An Atomic<T>'s operation on `target`, where it stays, which every thread sees in one order.
/// MAKE has no target, and gives the Atomic<T> to put in one.
struct atomic_t {
    atomic_op_t op;
    std::optional<place_t> target;
    std::vector<operand_t> operands;
    /// The Atomic<T>.
    type_id_t type;
};

/// A new Channel<T> of `type`, with room for `capacity` elements, an Int64, which panics at
/// `span` unless that's 1 or more; or a new Scheduler, whose `capacity` is 0.
struct channel_t {
    type_id_t type;
    operand_t capacity;
    span_t span;
};

/// A place's address, as a Ptr: what AddressOf gives.
struct address_of_t {
    place_t place;
};

/// A Callback of `type`: the address of a function by the C ABI that calls `function`, a
/// Function's code, or with `closure`, a lambda's, which gets a null closure, since it captures
/// nothing.
struct callback_t {
    function_id_t function;
    bool closure = false;
    type_id_t type = type_id_t::INVALID;
    /// The Function's or lambda's declaration, where an Enum C passes that isn't one of its
    /// values panics.
    span_t span;
};

/// "p + n" and "p - n", a Ptr n of its T's on or back, and "p - q", an Int64: how many T's there
/// are from q to p. The steps are the T's size.
struct pointer_t {
    binary_op_t op;
    operand_t left;
    operand_t right;
};

/// Takes the Mutex<T> at `target`, waiting while another thread holds it, or lets go of it.
/// Taking one that this thread holds already panics at `span`.
struct lock_t {
    place_t target;
    bool unlock = false;
    span_t span;
};

/// Something a statement computes.
using rvalue_t = std::variant<use_t, unary_t, binary_t, cast_t, call_t, runtime_call_t, aggregate_t,
                              array_t, zero_t, new_t, is_t, downcast_t, invoke_t, atomic_t, lock_t,
                              channel_t, address_of_t, pointer_t, callback_t>;

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

/// For a Global main sets: once its initializer has run, `mark` says so; before a function uses
/// it, a check that it has, since a function that an earlier Global's initializer calls could
/// get there first. Debug builds panic at `span` when it hasn't, and release builds do neither.
/// Before each use of a ThreadLocal Global, in every build, the thread makes its own the first
/// time, and panics at `span` where its initializer reaches it, or where it's used after the
/// thread has released it.
struct ready_t {
    global_id_t global;
    bool mark = false;
    span_t span;
};

/// Statements run in order, inside a block.
using statement_t = std::variant<assign_t, evaluate_t, retain_t, release_t, ready_t>;

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

/// In an Async Function: its coroutine stops here, and the scheduler resumes it at `resume`, or
/// destroys it at `cancel`, when its Scheduler goes first.
struct suspend_t {
    block_id_t resume;
    block_id_t cancel;
};

/// How a block ends: where control goes next.
using terminator_t = std::variant<goto_t, branch_t, return_t, panic_t, unreachable_t, suspend_t>;

/// Statements that run in order, then the terminator that leaves the block.
struct basic_block_t {
    std::vector<statement_t> statements;
    terminator_t terminator = unreachable_t{};
};

/// An Extern Function, which C code or the runtime defines: its symbol, and how it's called.
struct mir_extern_t {
    std::string symbol;
    /// C's convention takes and gives its values as C has them: Structs by the target's C ABI,
    /// and a String it gives as a CString, which the call copies. The runtime's take and give
    /// them as compiled code holds them. Both extend small integers and Bools as C does.
    bool c_abi = true;
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
    /// Written with @Inline: every call to it is inlined, in debug builds too.
    bool is_inline = false;
    /// Written with @PanicsAtCaller, or overriding one that is: codegen gives it its call's
    /// place as a hidden last parameter, which its panics report, and it passes that on to the
    /// @PanicsAtCaller functions it calls.
    bool panics_at_caller = false;
    /// An Async Function's code: a coroutine, whose Task is its first parameter, or the one after
    /// a Method's Self or a lambda's closure. Its Returns end the coroutine, and it gives LLVM its
    /// handle.
    bool is_async = false;
    /// An Extern Function: only its parameters, with no blocks, since other code defines it.
    std::optional<mir_extern_t> external;
};

/// An Interface that a Class's objects implement, and the Class's table for it: for each of the
/// Interface's Methods, and each Get and Set of its Properties, in order, the one they run.
struct mir_implementation_t {
    type_id_t interface;
    std::vector<function_id_t> methods;
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
    /// The Interfaces its objects implement that can be types, with its table for each.
    std::vector<mir_implementation_t> interfaces;
    /// Its objects' debug form, (object, Int32 indent) to a String, once the program shows one.
    std::optional<function_id_t> debug;
};

/// Why lowering stopped: code that would need a larger instance of generic code each time it
/// runs, without end, which analysis didn't catch, at the code that makes the instance.
struct mir_error_t {
    std::string message;
    span_t span;
};

/// A whole program, lowered from its modules' files. Names view the source text, so the files
/// must outlive the module. Names outside the main module start with their module, like
/// "std.collections.map.Map<Int32, String>.Add(Int32, String)", so two modules' names stay apart,
/// and the few that still come out alike, as module x's Class y's and module x.y's do, are
/// numbered: "x.y.Z()#2".
struct mir_module_t {
    std::vector<global_t> globals;
    std::vector<mir_class_t> classes;
    /// The array types whose objects go where an Interface is expected, with a table for each
    /// Interface they implement. They have no Destroy or Method table.
    std::vector<mir_class_t> array_classes;
    /// The T[] types whose debug form the program shows, with the function that makes it, which
    /// their fb_class_t has, as a Class's has its own.
    std::vector<std::pair<type_id_t, function_id_t>> array_debugs;
    /// The main module's functions that aren't generic, then the entry, then the other modules'
    /// functions and the instances of generic ones, as the program uses them.
    std::vector<mir_function_t> functions;
    /// "main": the top-level code, which runs when the program starts. Globals whose values
    /// aren't known at compile time are set at its start.
    function_id_t entry{};
    /// The types that locals, globals, and functions refer to: the analysis's, and the ones
    /// lowering made for the instances of generic code.
    std::shared_ptr<type_table_t> types;
    /// The main module's name, which a message showing two types whose names are the same puts
    /// before the main module's.
    std::string main_module;
    /// Set when lowering stopped, and the module can't be built.
    std::optional<mir_error_t> error;
};

/// The module as text, which `fb --show mir` prints.
[[nodiscard]] std::string dump(const mir_module_t& module, const style_t& style = {});

} // namespace fb
