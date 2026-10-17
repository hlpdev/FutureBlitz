#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

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
    FLOAT32,
    FLOAT64,
    STRING,
    /// Not a type: the first id a type table gives out.
    BUILTIN_COUNT,
};

/// A built-in type as the source spells it, like "Int32" or "String". Only for built-ins:
/// type_table_t::name() names every type.
[[nodiscard]] std::string_view builtin_name(type_id_t type);

/// The type a name stands for, aliases included: Int is Int32, Long is Int64, Byte is UInt8,
/// Float is Float32, and Double is Float64. nullopt for anything else.
[[nodiscard]] std::optional<type_id_t> builtin_type(std::string_view name);

[[nodiscard]] bool is_integer(type_id_t type);

/// Signed integers and floats.
[[nodiscard]] bool is_signed(type_id_t type);

[[nodiscard]] bool is_float(type_id_t type);

[[nodiscard]] bool is_numeric(type_id_t type);

/// 0 for types that aren't numbers.
[[nodiscard]] std::uint32_t bit_width(type_id_t type);

/// Whether every value of `from` is exactly a value of `to`, so it converts without an 'As'.
/// Float32 holds integers of up to 16 bits, and Float64 up to 32.
[[nodiscard]] bool converts_losslessly(type_id_t from, type_id_t to);

/// What a type in the table is.
enum class type_kind_t : std::uint8_t {
    /// INVALID through STRING.
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
    /// An Enum the program declares: a value of an integer type, among the named ones.
    ENUM,
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

/// One entry in a type table.
struct type_t {
    type_kind_t kind = type_kind_t::BUILTIN;
    /// As errors and dumps show it, like "Int32", "String[]", or "Int32[]?".
    std::string name;
    /// NULLABLE, ARRAY, and FIXED_ARRAY: the type inside. ENUM: its integer type.
    type_id_t inner = type_id_t::INVALID;
    /// FIXED_ARRAY: each dimension's size. The elements are stored in one run, the last
    /// dimension's changing fastest.
    std::vector<std::uint32_t> sizes;
    /// STRUCT and CLASS: the fields in declaration order, which is also their order in memory.
    /// NULLABLE: one field, "value", the T inside. Places reach it once the T? is known to hold
    /// one. A Class's start with the fields of the Class it extends.
    std::vector<field_t> fields;
    /// CLASS: the Class it extends, or INVALID.
    type_id_t base = type_id_t::INVALID;
    /// ENUM: its values, in declaration order.
    std::vector<enum_value_t> values;
    /// ENUM: written with @Flags, so its values are bits that combine.
    bool flags = false;
};

/// Every type one program uses. The built-ins are always there, with their fixed ids. A type
/// made from others, like "Int32?", is added the first time it's asked for, so equal types
/// always have the same id and can be compared with ==.
class type_table_t {
public:
    type_table_t();

    [[nodiscard]] const type_t& get(type_id_t id) const;

    /// Like "Int32" or "String[]". Stays valid as more types are added.
    [[nodiscard]] std::string_view name(type_id_t id) const {
        return get(id).name;
    }

    [[nodiscard]] std::size_t size() const {
        return m_entries.size();
    }

    /// "T?". A nullable of INVALID is INVALID, so one mistake stays one mistake.
    [[nodiscard]] type_id_t nullable(type_id_t inner);

    /// "T[]". An array of INVALID is INVALID.
    [[nodiscard]] type_id_t array(type_id_t element);

    /// "T[N]", or "T[W, H]" with two sizes. Of INVALID, it's INVALID.
    [[nodiscard]] type_id_t fixed_array(type_id_t element, const std::vector<std::uint32_t>& sizes);

    /// A fixed array's number of elements, in all its dimensions.
    [[nodiscard]] std::uint64_t element_count(type_id_t type) const;

    /// A new Struct type. Its fields come later, from set_fields(), once every type they might
    /// use exists.
    [[nodiscard]] type_id_t add_struct(std::string name);

    /// A new Class type, whose fields come later like a Struct's.
    [[nodiscard]] type_id_t add_class(std::string name);

    void set_fields(type_id_t type, std::vector<field_t> fields);

    /// A new Enum type. Its integer type and values come later, from set_values().
    [[nodiscard]] type_id_t add_enum(std::string name);

    void set_values(type_id_t type, type_id_t integer, std::vector<enum_value_t> values,
                    bool flags);

    /// An Enum's zero: no flags for @Flags, and else its first value.
    [[nodiscard]] std::uint64_t enum_zero(type_id_t type) const;

    /// Makes a Class extend another. The caller makes sure that doesn't make a cycle.
    void set_base(type_id_t type, type_id_t base);

    /// Whether `type` is the Class `base`, or extends it directly or through others.
    [[nodiscard]] bool extends(type_id_t type, type_id_t base) const;

    /// Whether a value holds references that need retain and release: a String, a Class, a T[],
    /// or a Struct, T?, or fixed array with one inside.
    [[nodiscard]] bool has_references(type_id_t type) const;

    /// Whether a variable of the type has a value before it's assigned: zero, False, "", Null, or
    /// a Struct or fixed array of those. A Class or T[] has none, since it can't be Null, and
    /// neither does a Struct with a field of one.
    [[nodiscard]] bool has_zero(type_id_t type) const;

private:
    struct entry_t {
        /// On the heap, so the type and its name never move.
        std::unique_ptr<type_t> type;
        /// This type's "T?" and "T[]", once they've been made. INVALID until then.
        type_id_t nullable = type_id_t::INVALID;
        type_id_t array = type_id_t::INVALID;
    };

    /// Indexed by type_id_t.
    std::vector<entry_t> m_entries;
    /// Every fixed array made so far, to find one again.
    std::vector<type_id_t> m_fixed_arrays;

    type_id_t add(type_t type);
    type_id_t make(type_kind_t kind, type_id_t inner, std::string_view suffix);
};

} // namespace fb
