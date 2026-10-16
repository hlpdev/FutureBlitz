#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace fb {

/// The built-in types. The order matters: is_integer() and is_signed() test ranges of it.
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
    BUILTIN_COUNT,
};

/// As the source spells it, like "Int32" or "String".
[[nodiscard]] std::string_view type_name(type_id_t type);

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

} // namespace fb
