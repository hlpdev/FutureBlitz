#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fb {

/// The most elements a fixed array's dimension holds.
constexpr std::int64_t max_count = 2'147'483'647;

/// A type: one of the built-ins named here, or from BUILTIN_COUNT on, a type that a type_table_t
/// added. The order of the built-ins matters: is_integer() and is_signed() test ranges of it.
enum class type_id_t : std::uint32_t { // NOLINT(performance-enum-size)
    /// The type of something with an error. Checks pass quietly on it, so one mistake isn't
    /// reported over and over.
    INVALID,
    /// No value: what a function without a return type gives back. Shown as "nothing".
    NONE,
    BOOL,
    INT8,
    INT16,
    INT32,
    INT64,
    UINT8,
    UINT16,
    UINT32,
    UINT64,
    /// C's `long`, `unsigned long`, and `wchar_t`, integers whose sizes follow the target, as
    /// on_target() has them. At compile time a CLong's or CWChar's value is held as an Int64's,
    /// and a CULong's as a UInt64's.
    CLONG,
    CULONG,
    CWCHAR,
    FLOAT32,
    FLOAT64,
    STRING,
    /// A C string, `const char*`: what C code takes and gives for text. Its zero is null, and only
    /// Unsafe code reads it.
    CSTRING,
    /// A Unicode scalar value: a code point from U+0000 to U+10FFFF that isn't a surrogate, which
    /// a literal like 'a' makes. It's 32 bits, compares by its code, and isn't a number.
    CHAR,
    /// Not a type: the first id a type table gives out.
    BUILTIN_COUNT,
};

/// A built-in type as the source spells it, like "Int32" or "String". Only for built-ins:
/// type_table_t::name() names every type.
[[nodiscard]] std::string_view builtin_name(type_id_t type);

/// The type a name stands for, aliases included: Int is Int32, Long is Int64, Byte is UInt8,
/// Float is Float32, and Double is Float64. nullopt for anything else.
[[nodiscard]] std::optional<type_id_t> builtin_type(std::string_view name);

/// The fixed-size integers, and C's, whose sizes follow the target.
[[nodiscard]] bool is_integer(type_id_t type);

/// Signed integers and floats. CWChar is one, since its value at compile time is held as an
/// Int64's; at run time it's signed or not as the target's wchar_t is.
[[nodiscard]] bool is_signed(type_id_t type);

[[nodiscard]] bool is_float(type_id_t type);

[[nodiscard]] bool is_numeric(type_id_t type);

/// CLong, CULong, and CWChar.
[[nodiscard]] bool is_c_integer(type_id_t type);

/// 0 for types that aren't numbers, and for C's integers, whose width is the target's.
[[nodiscard]] std::uint32_t bit_width(type_id_t type);

/// What C's `long`, `unsigned long`, and `wchar_t` are on a target.
struct c_integers_t {
    type_id_t c_long = type_id_t::INT64;
    type_id_t c_ulong = type_id_t::UINT64;
    type_id_t c_wchar = type_id_t::INT32;
};

/// C's integers on each kind of target FutureBlitz has: Windows, where they're 32, 32, and 16
/// bits, wchar_t unsigned; elsewhere, where they're 64, 64, and 32 bits, wchar_t unsigned on
/// AArch64 but Apple's; and Cygwin, where they're 64, 64, and an unsigned 16.
[[nodiscard]] std::span<const c_integers_t> every_c_integers();

/// The integer `type` is on a target whose C integers are `integers`: CLong's, CULong's, or
/// CWChar's, and any other type itself.
[[nodiscard]] type_id_t on_target(type_id_t type, const c_integers_t& integers);

/// Whether every value of `from` is exactly a value of `to`, so it converts without an 'As'.
/// Float32 holds integers of up to 16 bits, and Float64 up to 32. With one of C's integers, it
/// has to on every target.
[[nodiscard]] bool converts_losslessly(type_id_t from, type_id_t to);

/// What a type in the table is.
enum class type_kind_t : std::uint8_t {
    /// INVALID through CSTRING.
    BUILTIN,
    /// "T?": a T, or Null. A String? or a Class? is a reference that's null for Null; any other
    /// T? is a flag and a T.
    NULLABLE,
    /// "T[]": a dynamic array of T, which is a reference to an object holding its elements.
    ARRAY,
    /// "T[N]" or "T[W, H]": a fixed number of T, stored inline like a Struct's fields.
    FIXED_ARRAY,
    /// A Struct the program declares: a value made of fields.
    STRUCT,
    /// A Class the program declares: a reference to an object of fields, which counts its
    /// owners.
    CLASS,
    /// An Interface the program declares: what the Structs and Classes that implement it have.
    /// One that needs no Statics or Operators, and doesn't use Self in its members' types, is a
    /// type of its own: a reference to an object of a Class that implements it.
    INTERFACE,
    /// An Enum the program declares: a value of an integer type, among the named ones.
    ENUM,
    /// "(A, B)": a value made of its elements, like a Struct without a name. Its fields are named
    /// "0", "1", and so on.
    TUPLE,
    /// "Func(A, B): R": a function value, which refers to a closure: an object of a Class that
    /// holds the variables it captures, with its code in the Method table.
    FUNCTION,
    /// A generic Function's, Struct's, or Class's type parameter, like the T in "Box<T>". Generic
    /// code is checked with it, and lowering puts each type argument in its place.
    PARAMETER,
    /// "Weak<T>": a reference to an object of the Class T that doesn't keep it. Reading one gives
    /// a T?, which is Null once the object is gone.
    WEAK,
    /// A Const type parameter's argument: an integer known at compile time, like the 64 in
    /// "Grid<Float32, 64, 64>". It's never a value's type.
    VALUE,
    /// "Atomic<T>": an integer or a Bool that threads change with atomic operations. It stays
    /// where it's made, in a Global, a Static Field, or a Class's Field, so it's never copied.
    ATOMIC,
    /// "Mutex<T>": a Sendable T and a lock, which a Lock block holds while it uses the value. It
    /// stays where it's made, like an Atomic. Its fields are the lock's state and the number of
    /// the thread holding it, then the value.
    MUTEX,
    /// "Task<T>": work that a Spawn runs on the pool, and then its result, the `inner`. "Task"
    /// alone is work that gives nothing. It's a reference to an object whose fields are the job
    /// running it, the closure doing the work, and the result.
    TASK,
    /// "Thread": work that Thread.Start runs on a thread of its own, an object like a Task's.
    THREAD,
    /// "Channel<T>": a queue of Sendable T that threads send to and receive from, a reference to
    /// an object of the runtime's fb_channel_t.
    CHANNEL,
    /// "Scheduler": runs the coroutines given to it when it's ticked, on the main thread. A
    /// reference to an object of the runtime's fb_scheduler_t.
    SCHEDULER,
    /// "Ptr<T>": the address of a T, as C's T*, which only Unsafe code reads or writes through.
    /// Its one field, "Value", is what it points at. "Ptr<Void>", whose inner is NONE, points at
    /// anything, as C's void*, and has no field.
    PTR,
    /// "Callback(A, B): R": a C function pointer, which C code calls, taking the `parameters`
    /// and giving the `inner`, NONE for nothing.
    CALLBACK,
};

/// One of a Struct's or Class's fields.
struct field_t {
    std::string name;
    type_id_t type = type_id_t::INVALID;
};

/// One of an Enum's values, and its name.
struct enum_value_t {
    std::string name;
    /// The value's bits, in the Enum's integer type.
    std::uint64_t bits = 0;
};

/// How a VALUE type is computed from two others, like the N * 2 of "Grid<N * 2>". NONE for a
/// value that's known. DIVIDE and REMAINDER round toward 0, as an Int's / and % do.
enum class value_op_t : std::uint8_t {
    NONE,
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    REMAINDER,
};

/// One entry in a type table.
struct type_t {
    type_kind_t kind = type_kind_t::BUILTIN;
    /// As errors and dumps show it, like "Int32", "String[]", or "Int32[]?".
    std::string name;
    /// A Struct, Class, Interface, or Enum declared outside the main module: that module, like
    /// "std.collections.map", which its qualified name starts with. Empty for the main module's,
    /// and for the other kinds. An instance has its generic type's.
    std::string module;
    /// NULLABLE, ARRAY, FIXED_ARRAY, and WEAK: the type inside. ENUM: its integer type. A
    /// PARAMETER that's Const: the type of its value. VALUE: Int64, whatever its parameter's
    /// type, since each instance works it out in one and checks it fits.
    /// FUNCTION: the type it returns, NONE when it returns nothing. INTERFACE: Self, the type
    /// parameter that stands for the type implementing it, which a generic one's instances share.
    type_id_t inner = type_id_t::INVALID;
    /// FIXED_ARRAY: each dimension's size. The elements are stored in one run, the last
    /// dimension's changing fastest.
    std::vector<std::uint32_t> sizes;
    /// STRUCT, CLASS, and TUPLE: the fields in order, which is also their order in memory.
    /// NULLABLE: one field, "value", the T inside. Places reach it once the T? is known to hold
    /// one. A Class's start with the fields of the Class it extends.
    std::vector<field_t> fields;
    /// CLASS: the Class it extends, or INVALID.
    type_id_t base = type_id_t::INVALID;
    /// STRUCT and CLASS: the Interfaces after Implements. INTERFACE: the ones it extends. An
    /// instance's have its type arguments in them, followed by what an Extend of that instance
    /// alone lists.
    std::vector<type_id_t> interfaces;
    /// ENUM: its values, in declaration order.
    std::vector<enum_value_t> values;
    /// ENUM: written with @Flags, so its values are bits that combine.
    bool flags = false;
    /// VALUE: how one computed from others is worked out, or NONE for a number.
    value_op_t operation = value_op_t::NONE;
    /// CLASS: a Shared Class, whose objects count their owners atomically.
    bool shared = false;
    /// STRUCT: a @CLayout Struct, which C code sees.
    bool c_layout = false;
    /// Made by the compiler, not declared: the Cell<T> a captured Local lives in, and a lambda's
    /// Class. It belongs to no module, so its qualified name has none, though its parts may.
    bool made = false;
    /// FUNCTION: its parameters' types. A generic STRUCT, CLASS, or INTERFACE: its type
    /// parameters. FIXED_ARRAY: for each size that's a Const type parameter, that parameter, with
    /// 0 in `sizes`, and INVALID for the others; empty when every size is known.
    std::vector<type_id_t> parameters;
    /// An instance of a generic Struct, Class, or Interface, like "Box<Int32>": the generic one,
    /// and a type argument for each of its type parameters. Its fields are the generic one's,
    /// with the arguments in place of the parameters.
    type_id_t generic = type_id_t::INVALID;
    std::vector<type_id_t> arguments;
    /// VALUE: the value. One computed from others has 0, and `parameters` has the two it's
    /// computed from, a Const type parameter or another VALUE each, which each instance works out
    /// with `operation`. Two known ones that can't be worked out, since they divide by 0 or pass
    /// what an Int64 holds, stay that way, for lowering to report where the instance is made.
    std::int64_t number = 0;
    /// TASK: the Classes that its Async Function's errors extend, which Wait and Await re-throw.
    /// Its last field, "error", holds the error, or Null. A type parameter among them stands for
    /// one.
    std::vector<type_id_t> throws;
    /// FUNCTION: a Sendable Func, which other threads can call: its closure captures only
    /// Sendable values and counts its owners atomically.
    bool sendable = false;
};

/// Every type one program uses. The built-ins are always there, with their fixed ids. A type
/// made from others, like "Int32?", is added the first time it's asked for, so equal types
/// always have the same id and can be compared with ==.
class type_table_t {
public:
    type_table_t();
    /// A copy has the same types at the same ids, and adds its own after them.
    type_table_t(const type_table_t& other);
    type_table_t& operator=(const type_table_t& other);
    type_table_t(type_table_t&& other) noexcept = default;
    type_table_t& operator=(type_table_t&& other) noexcept = default;
    ~type_table_t() = default;

    [[nodiscard]] const type_t& get(type_id_t id) const;

    /// Like "Int32" or "String[]". Stays valid as more types are added.
    [[nodiscard]] std::string_view name(type_id_t id) const {
        return get(id).name;
    }

    /// As MIR and LLVM name it: like name(), but each Struct, Class, Interface, and Enum in it
    /// from outside the main module starts with its module, so two modules' types stay apart, as
    /// in "std.collections.map.Map<Int32, Node>". With `main`, the main module's name, the main
    /// module's start with it too, as a message shows two types whose names are the same.
    [[nodiscard]] std::string qualified_name(type_id_t id, std::string_view main = {}) const;

    [[nodiscard]] std::size_t size() const {
        return m_entries.size();
    }

    /// "T?". A nullable of INVALID is INVALID, so one mistake stays one mistake.
    [[nodiscard]] type_id_t nullable(type_id_t inner);

    /// "T[]". An array of INVALID is INVALID.
    [[nodiscard]] type_id_t array(type_id_t element);

    /// "Weak<T>", for a Class T. It makes "T?" too, which reading one gives.
    [[nodiscard]] type_id_t weak(type_id_t inner);

    /// "Atomic<T>", for an integer or Bool T.
    [[nodiscard]] type_id_t atomic(type_id_t inner);

    /// "Mutex<T>", for a Sendable T.
    [[nodiscard]] type_id_t mutex(type_id_t inner);

    /// "Task<T>", or "Task" for a `result` of NONE.
    [[nodiscard]] type_id_t task(type_id_t result);

    /// "Task<T> Throws A, B": the Task of an Async Function that throws, whose "error" field is
    /// an `error`, the Error? type. Without `throws`, it's task(result).
    [[nodiscard]] type_id_t task(type_id_t result, const std::vector<type_id_t>& throws,
                                 type_id_t error);

    /// "Thread".
    [[nodiscard]] type_id_t thread();

    /// "Channel<T>", for a Sendable T.
    [[nodiscard]] type_id_t channel(type_id_t element);

    /// "Scheduler".
    [[nodiscard]] type_id_t scheduler();

    /// "Ptr<T>", or "Ptr<Void>" for NONE.
    [[nodiscard]] type_id_t pointer(type_id_t inner);

    /// "T?" when it's been made already, as for any T that has a Weak<T>. INVALID otherwise.
    [[nodiscard]] type_id_t find_nullable(type_id_t inner) const;

    /// "T[N]", or "T[W, H]" with two sizes. Of INVALID, it's INVALID. A size that's a Const type
    /// parameter is 0, with the parameter at its place in `dimensions`.
    [[nodiscard]] type_id_t fixed_array(type_id_t element, const std::vector<std::uint32_t>& sizes,
                                        const std::vector<type_id_t>& dimensions = {});

    /// "N * 2": a value computed from two others, each a VALUE or a Const type parameter. Two
    /// known values give the value they compute, where there is one.
    [[nodiscard]] type_id_t value_expression(value_op_t op, type_id_t left, type_id_t right);

    /// "(A, B)", of two or more elements. With an INVALID element, it's INVALID.
    [[nodiscard]] type_id_t tuple(const std::vector<type_id_t>& elements);

    /// "Func(A, B): R", with a `result` of NONE for one that returns nothing, or "Sendable Func(A,
    /// B): R". With an INVALID part, it's INVALID.
    [[nodiscard]] type_id_t function(const std::vector<type_id_t>& parameters, type_id_t result,
                                     bool sendable = false);

    /// "Callback(A, B): R", or "Callback(A)" for one that gives nothing.
    [[nodiscard]] type_id_t callback(const std::vector<type_id_t>& parameters, type_id_t result);

    /// A new type parameter, like the T in "Box<T>".
    [[nodiscard]] type_id_t add_parameter(std::string name);

    /// A new Const type parameter, like the W in "Grid<T, Const W: Int>", with its value's type.
    [[nodiscard]] type_id_t add_value_parameter(std::string name, type_id_t type);

    /// A Const type parameter's argument, like the 64 in "Grid<Float32, 64, 64>": the same type
    /// for the same number, whatever the parameter's type.
    [[nodiscard]] type_id_t value(std::int64_t number);

    /// Makes a Struct, Class, or Interface generic, with these type parameters.
    void set_parameters(type_id_t type, std::vector<type_id_t> parameters);

    /// A generic Struct, Class, or Interface with these type arguments, like "Box<Int32>", made
    /// once for each set of them. With its own type parameters as the arguments, it's the generic
    /// one itself. INVALID with an INVALID argument, past a limit on how deep instances nest
    /// while they're filled in, or with an argument made of more types than max_size.
    [[nodiscard]] type_id_t instance(type_id_t generic, const std::vector<type_id_t>& arguments);

    /// The most types an instance's argument can be made of, as one that doubles each time it's
    /// made, like S<(T, T)>, gets to quickly.
    static constexpr std::uint32_t max_size = 65'536;

    /// How deep a type nests, and how many types it's made of, itself included, each counted no
    /// higher than a little past max_size.
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> measure(type_id_t type) const;

    /// `type` with each of `parameters` replaced by the argument at the same place, in it or in
    /// the types it's made of.
    [[nodiscard]] type_id_t substitute(type_id_t type, const std::vector<type_id_t>& parameters,
                                       const std::vector<type_id_t>& arguments);

    /// A fixed array's number of elements, in all its dimensions.
    [[nodiscard]] std::uint64_t element_count(type_id_t type) const;

    /// A new Struct type, declared in `module`, or in the main module when it's empty. Its fields
    /// come later, from set_fields(), once every type they might use exists.
    [[nodiscard]] type_id_t add_struct(std::string name, std::string module = {});

    /// A new Class type, whose fields come later like a Struct's.
    [[nodiscard]] type_id_t add_class(std::string name, std::string module = {});

    void set_fields(type_id_t type, std::vector<field_t> fields);

    /// A new Interface type, with a type parameter of its own for Self.
    [[nodiscard]] type_id_t add_interface(std::string name, std::string module = {});

    /// The Interfaces a Struct or Class implements, or that an Interface extends. The caller makes
    /// sure an Interface doesn't extend itself.
    void set_interfaces(type_id_t type, std::vector<type_id_t> interfaces);

    /// A new Enum type. Its integer type and values come later, from set_values().
    [[nodiscard]] type_id_t add_enum(std::string name, std::string module = {});

    void set_values(type_id_t type, type_id_t integer, std::vector<enum_value_t> values,
                    bool flags);

    /// An Enum's zero: no flags for @Flags, and else its first value.
    [[nodiscard]] std::uint64_t enum_zero(type_id_t type) const;

    /// Makes a Class extend another. The caller makes sure that doesn't make a cycle.
    void set_base(type_id_t type, type_id_t base);

    /// Marks a Class as the compiler's own, before any of its instances are made.
    void set_made(type_id_t type);

    /// Makes a Class a Shared Class, before any of its instances are made.
    void set_shared(type_id_t type);

    /// Makes a Struct without type parameters a @CLayout Struct.
    void set_c_layout(type_id_t type);

    /// Whether `type` is the Class `base`, or extends it directly or through others. For an
    /// Interface, whether it's `base`, or extends that Interface.
    [[nodiscard]] bool extends(type_id_t type, type_id_t base) const;

    /// Whether a value holds references that need retain and release: a String, a Class, an
    /// Interface, a T[], a Weak<T>, a function value, or a Struct, T?, tuple, or fixed array with
    /// one inside.
    [[nodiscard]] bool has_references(type_id_t type) const;

    /// Whether a variable of the type has a value before it's assigned: zero, False, "", Null, or
    /// a Struct, tuple, or fixed array of those. A Class, Interface, T[], or function value has
    /// none, since it can't be Null, and neither does a Struct with a field of one.
    [[nodiscard]] bool has_zero(type_id_t type) const;

private:
    /// A value computed with + and -, and * by a known number: a known number, and how many
    /// times it has each other part.
    struct linear_t;

    /// "N * 2" as it's written.
    [[nodiscard]] type_id_t computed(value_op_t op, type_id_t left, type_id_t right);
    /// A VALUE or Const type parameter as a linear_t, whose parts are Const type parameters and
    /// values computed otherwise.
    [[nodiscard]] linear_t linear(type_id_t type) const;
    /// Two linear_ts added, subtracted, or multiplied, or nullopt where the result isn't one, or
    /// would drop a part that can fail, or its numbers pass what an Int64 holds.
    [[nodiscard]] std::optional<linear_t> combine(value_op_t op, const linear_t& left,
                                                  const linear_t& right) const;
    /// The value a linear_t is, the same one for equal ones.
    [[nodiscard]] type_id_t from_linear(const linear_t& value);

    struct entry_t {
        /// On the heap, so the type and its name never move.
        std::unique_ptr<type_t> type;
        /// This type's "T?", "T[]", "Weak<T>", "Atomic<T>", "Mutex<T>", "Task<T>", "Channel<T>",
        /// and "Ptr<T>", once they've been made. INVALID until then.
        type_id_t nullable = type_id_t::INVALID;
        type_id_t array = type_id_t::INVALID;
        type_id_t weak = type_id_t::INVALID;
        type_id_t atomic = type_id_t::INVALID;
        type_id_t mutex = type_id_t::INVALID;
        type_id_t task = type_id_t::INVALID;
        type_id_t channel = type_id_t::INVALID;
        type_id_t pointer = type_id_t::INVALID;
    };

    /// Indexed by type_id_t.
    std::vector<entry_t> m_entries;
    /// Every fixed array, tuple, Func, and instance made so far, to find one again.
    std::vector<type_id_t> m_fixed_arrays;
    std::vector<type_id_t> m_tuples;
    std::vector<type_id_t> m_functions;
    std::vector<type_id_t> m_callbacks;
    /// The Tasks of Async Functions that throw.
    std::vector<type_id_t> m_throwing_tasks;
    std::vector<type_id_t> m_instances;
    std::vector<type_id_t> m_values;
    /// "Thread" and "Scheduler", once they've been made.
    type_id_t m_thread = type_id_t::INVALID;
    type_id_t m_scheduler = type_id_t::INVALID;
    /// How many instances are being filled in, one inside another.
    std::uint32_t m_depth = 0;
    /// The instances whose fill stopped at the limit on m_depth, which instance() fills again
    /// when it's asked for one outside any other.
    std::unordered_set<type_id_t> m_incomplete;
    /// Each type's measure(), once it's asked for.
    mutable std::unordered_map<type_id_t, std::pair<std::uint32_t, std::uint32_t>> m_measures;

    type_id_t add(type_t type);
    type_id_t make(type_kind_t kind, type_id_t inner, std::string full_name);
    /// A type's name before a "?" or "[]": a Func that returns something goes in parentheses,
    /// so "(Func(): Int32)?" can't be read as returning an Int32?, and so does a Task with a
    /// Throws, as in "(Task<Int32> Throws E)[]". With `qualified`, it's the qualified name, with
    /// `main` as qualified_name() takes it.
    [[nodiscard]] std::string suffixed(type_id_t type, bool qualified = false,
                                       std::string_view main = {}) const;
    /// A type's name in a list of them, a tuple's, a Func's parameters, or type arguments: a
    /// Task with a Throws, or a Func that gives one, goes in parentheses, so the ',' after it
    /// doesn't read as its next Class.
    [[nodiscard]] std::string listed(type_id_t type, bool qualified = false,
                                     std::string_view main = {}) const;
    /// Gives an instance its generic type's fields, base, and Interfaces, with its type arguments
    /// in them, keeping the Interfaces an Extend of the instance itself lists.
    void fill(type_id_t instance);
};

} // namespace fb
