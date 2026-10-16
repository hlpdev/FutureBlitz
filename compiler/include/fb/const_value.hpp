#pragma once

#include "fb/ast.hpp"
#include "fb/types.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace fb {

/// A value known at compile time. `data` holds a bool for Bool, an int64_t for signed integers,
/// a uint64_t for unsigned ones, a double for floats (a Float32 rounded to float precision),
/// and a std::string for String. A T? holds what its T would, or nullptr for Null.
struct const_value_t {
    type_id_t type = type_id_t::INVALID;
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string, std::nullptr_t> data;

    bool operator==(const const_value_t&) const = default;
};

/// A folded value, or the message for why it can't be computed, like "division by zero".
using fold_result_t = std::expected<const_value_t, std::string>;

/// An integer literal as a number of `type`: its magnitude, negated when `negative`. nullopt
/// when `type` isn't numeric or can't hold the value exactly.
[[nodiscard]] std::optional<const_value_t> make_number(std::uint64_t magnitude, bool negative,
                                                       type_id_t type);

/// A decimal literal as Float32 or Float64. nullopt when it's too big for the type.
[[nodiscard]] std::optional<const_value_t> make_decimal(double value, type_id_t type);

[[nodiscard]] const_value_t make_bool(bool value);
[[nodiscard]] const_value_t make_string(std::string value);

/// `op` must work on the operand's type. Fails when negating the most negative integer.
[[nodiscard]] fold_result_t fold_unary(unary_op_t op, const const_value_t& operand);

/// `op` must work on the operands, which have the same type, except that a shift's amount can
/// be any integer. Fails on integer overflow, division by zero, a shift amount out of range, or
/// a float result that isn't finite. Bitwise operators and shifts never overflow.
[[nodiscard]] fold_result_t fold_binary(binary_op_t op, const const_value_t& left,
                                        const const_value_t& right);

/// Between numbers, or from a Bool to an integer (0 or 1). Integer to integer keeps the low
/// bits, and float to integer drops the fraction. Fails when a float doesn't fit the new type.
[[nodiscard]] fold_result_t fold_cast(const const_value_t& value, type_id_t type);

/// The value as string interpolation shows it, like "True" or "1.5". A String is its own text,
/// unquoted, and Null is "Null".
[[nodiscard]] std::string to_text(const const_value_t& value);

/// Orders two integers of the same signedness.
[[nodiscard]] std::strong_ordering compare(const const_value_t& left, const const_value_t& right);

} // namespace fb
