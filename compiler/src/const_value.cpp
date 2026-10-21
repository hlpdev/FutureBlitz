#include "fb/const_value.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

// Compile-time evaluation for sema. Overflow, division by zero, and out-of-range casts are compile
// errors here, in debug and release builds alike (docs/spec/13-compile-time.md).

namespace fb {
namespace {

constexpr std::uint64_t unsigned_max(const std::uint32_t bits) {
    return bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                      : (std::uint64_t{ 1 } << bits) - 1;
}

constexpr std::int64_t signed_max(const std::uint32_t bits) {
    return static_cast<std::int64_t>(unsigned_max(bits) >> 1U);
}

constexpr std::int64_t signed_min(const std::uint32_t bits) {
    return -signed_max(bits) - 1;
}

/// A Float32 value is the nearest float, kept as a double. Out of range it's infinite: from
/// halfway between the largest float and 2^128, which rounds to 2^128, as IEEE rounding has it,
/// so 3.4028235e38, the largest float's shortest spelling, is that float.
double round_to(const double value, const type_id_t type) {
    if (type != type_id_t::FLOAT32) {
        return value;
    }

    constexpr double overflows = 0x1.ffffffp127;
    if (std::abs(value) >= overflows) {
        return std::numeric_limits<double>::infinity();
    }

    return static_cast<double>(static_cast<float>(value));
}

/// An integer's two's complement bits.
std::uint64_t raw_bits(const const_value_t& value) {
    if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
        return std::bit_cast<std::uint64_t>(*number);
    }

    return std::get<std::uint64_t>(value.data);
}

/// The low bits of `raw` as a value of the integer `type`, sign-extended for signed types.
const_value_t wrap(const std::uint64_t raw, const type_id_t type) {
    const std::uint32_t bits = bit_width(type);
    const std::uint64_t low = raw & unsigned_max(bits);
    if (!is_signed(type)) {
        return { .type = type, .data = low };
    }

    // Flipping the sign bit and then subtracting it sign-extends to 64 bits.
    const std::uint64_t sign = std::uint64_t{ 1 } << (bits - 1);
    return { .type = type, .data = std::bit_cast<std::int64_t>((low ^ sign) - sign) };
}

std::optional<std::int64_t> checked_add(const std::int64_t a, const std::int64_t b) {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t min = std::numeric_limits<std::int64_t>::min();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) {
        return std::nullopt;
    }

    return a + b;
}

std::optional<std::int64_t> checked_sub(const std::int64_t a, const std::int64_t b) {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t min = std::numeric_limits<std::int64_t>::min();
    if ((b < 0 && a > max + b) || (b > 0 && a < min + b)) {
        return std::nullopt;
    }

    return a - b;
}

std::optional<std::int64_t> checked_mul(const std::int64_t a, const std::int64_t b) {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t min = std::numeric_limits<std::int64_t>::min();
    if (a == 0 || b == 0) {
        return 0;
    }

    bool overflows = false;
    if (a > 0) {
        overflows = b > 0 ? a > max / b : b < min / a;
    } else {
        overflows = b > 0 ? a < min / b : b < max / a;
    }

    if (overflows) {
        return std::nullopt;
    }

    return a * b;
}

std::optional<std::uint64_t> checked_add(const std::uint64_t a, const std::uint64_t b) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return std::nullopt;
    }

    return a + b;
}

std::optional<std::uint64_t> checked_sub(const std::uint64_t a, const std::uint64_t b) {
    if (a < b) {
        return std::nullopt;
    }

    return a - b;
}

std::optional<std::uint64_t> checked_mul(const std::uint64_t a, const std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::nullopt;
    }

    return a * b;
}

/// Exponentiation by squaring, so huge exponents finish quickly.
template <typename Int>
std::optional<Int> checked_pow(Int base, std::uint64_t exponent) {
    Int result = 1;
    while (exponent > 0) {
        if ((exponent & 1U) != 0) {
            const std::optional<Int> product = checked_mul(result, base);
            if (!product) {
                return std::nullopt;
            }

            result = *product;
        }

        exponent >>= 1U;
        if (exponent > 0) {
            const std::optional<Int> square = checked_mul(base, base);
            if (!square) {
                return std::nullopt;
            }

            base = *square;
        }
    }

    return result;
}

/// The error for a result of `op` that doesn't fit in `type`.
std::string overflow(const binary_op_t op, const type_id_t type) {
    return std::format("the result of '{}' overflows {}", spelling(op), builtin_name(type));
}

/// Arithmetic on signed integers of any width. Operands are held in 64 bits; the exact result has
/// to fit in 64 bits and then in `type`. Unlike a release build at run time, nothing wraps.
fold_result_t fold_signed(const binary_op_t op, const std::int64_t a, const std::int64_t b,
                          const type_id_t type) {
    std::optional<std::int64_t> result;
    switch (op) {
    case binary_op_t::ADD:
        result = checked_add(a, b);
        break;
    case binary_op_t::SUBTRACT:
        result = checked_sub(a, b);
        break;
    case binary_op_t::MULTIPLY:
        result = checked_mul(a, b);
        break;
    case binary_op_t::DIVIDE:
        if (b == 0) {
            return std::unexpected("division by zero");
        }

        // Int64's minimum / -1 overflows 64 bits; leaving the result empty reports it.
        if (b != -1 || a != std::numeric_limits<std::int64_t>::min()) {
            result = a / b;
        }

        break;
    case binary_op_t::REMAINDER:
        if (b == 0) {
            return std::unexpected("division by zero");
        }

        // x % -1 is always 0, but Int64's minimum % -1 is undefined behavior in C++.
        result = b == -1 ? 0 : a % b;
        break;
    case binary_op_t::POWER:
        if (b < 0) {
            return std::unexpected("an integer power needs an exponent of 0 or more");
        }

        result = checked_pow(a, static_cast<std::uint64_t>(b));
        break;
    default:
        std::unreachable();
    }

    const std::uint32_t bits = bit_width(type);
    if (!result || *result < signed_min(bits) || *result > signed_max(bits)) {
        return std::unexpected(overflow(op, type));
    }

    return const_value_t{ .type = type, .data = *result };
}

/// Arithmetic on unsigned integers of any width, checked like fold_signed(). Going below 0 is an
/// overflow too.
fold_result_t fold_unsigned(const binary_op_t op, const std::uint64_t a, const std::uint64_t b,
                            const type_id_t type) {
    std::optional<std::uint64_t> result;
    switch (op) {
    case binary_op_t::ADD:
        result = checked_add(a, b);
        break;
    case binary_op_t::SUBTRACT:
        result = checked_sub(a, b);
        break;
    case binary_op_t::MULTIPLY:
        result = checked_mul(a, b);
        break;
    case binary_op_t::DIVIDE:
    case binary_op_t::REMAINDER:
        if (b == 0) {
            return std::unexpected("division by zero");
        }

        result = op == binary_op_t::DIVIDE ? a / b : a % b;
        break;
    case binary_op_t::POWER:
        result = checked_pow(a, b);
        break;
    default:
        std::unreachable();
    }

    if (!result || *result > unsigned_max(bit_width(type))) {
        return std::unexpected(overflow(op, type));
    }

    return const_value_t{ .type = type, .data = *result };
}

/// Float arithmetic, done in double precision and then rounded to `type`. Constants are always
/// finite, so a result that overflows to infinity or is NaN is an error.
fold_result_t fold_float(const binary_op_t op, const double a, const double b,
                         const type_id_t type) {
    double result = 0;
    switch (op) {
    case binary_op_t::ADD:
        result = a + b;
        break;
    case binary_op_t::SUBTRACT:
        result = a - b;
        break;
    case binary_op_t::MULTIPLY:
        result = a * b;
        break;
    case binary_op_t::DIVIDE:
        result = a / b;
        break;
    case binary_op_t::REMAINDER:
        result = std::fmod(a, b);
        break;
    case binary_op_t::POWER:
        result = std::pow(a, b);
        break;
    default:
        std::unreachable();
    }

    // For + - * /, rounding the double result gives exactly what float arithmetic would.
    result = round_to(result, type);

    if (!std::isfinite(result)) {
        // x / 0 and x % 0 come out infinite or NaN; name the real cause.
        if (b == 0 && (op == binary_op_t::DIVIDE || op == binary_op_t::REMAINDER)) {
            return std::unexpected("division by zero");
        }

        return std::unexpected(
            std::format("the result of '{}' isn't a finite {}", spelling(op), builtin_name(type)));
    }

    return const_value_t{ .type = type, .data = result };
}

/// The amount can be any integer type, but has to be from 0 to the left side's width minus 1
/// (docs/spec/03-operators.md). Bits shifted out are dropped; that's never an overflow.
fold_result_t fold_shift(const binary_op_t op, const const_value_t& left,
                         const const_value_t& right) {
    const std::uint32_t bits = bit_width(left.type);
    // A negative amount is out of range like one that's too big.
    std::uint64_t amount = 0;
    if (const auto* signed_amount = std::get_if<std::int64_t>(&right.data)) {
        amount = *signed_amount < 0 ? bits : static_cast<std::uint64_t>(*signed_amount);
    } else {
        amount = std::get<std::uint64_t>(right.data);
    }

    if (amount >= bits) {
        return std::unexpected(std::format("the shift amount must be from 0 to {}", bits - 1));
    }

    const std::uint64_t raw = raw_bits(left);
    if (op == binary_op_t::SHIFT_LEFT) {
        return wrap(raw << amount, left.type);
    }

    // Shifting a negative number right brings in ones, so it stays negative.
    const auto* number = std::get_if<std::int64_t>(&left.data);
    const bool negative = number != nullptr && *number < 0;
    const std::uint64_t fill = negative ? ~(~std::uint64_t{ 0 } >> amount) : 0;
    return wrap((raw >> amount) | fill, left.type);
}

/// Orders two numbers of the same type: floats as doubles, integers with compare().
std::partial_ordering order(const const_value_t& left, const const_value_t& right) {
    // Strings compare by their UTF-8 bytes, which orders them by code point.
    if (const auto* text = std::get_if<std::string>(&left.data)) {
        return *text <=> std::get<std::string>(right.data);
    }

    if (is_float(left.type)) {
        return std::get<double>(left.data) <=> std::get<double>(right.data);
    }

    return compare(left, right);
}

} // namespace

std::optional<const_value_t> make_number(const std::uint64_t magnitude, const bool negative,
                                         const type_id_t type) {
    // An integer literal converts to a float only when the float holds it exactly. Near 2^64 the
    // double rounds up to 2^64, which can't be cast back, so that's checked first.
    if (is_float(type)) {
        const auto value = static_cast<double>(magnitude);
        constexpr double two_to_64 = 18446744073709551616.0;
        const bool exact =
            value < two_to_64 && static_cast<std::uint64_t>(value) == magnitude &&
            (type == type_id_t::FLOAT64 || static_cast<double>(static_cast<float>(value)) == value);
        if (!exact) {
            return std::nullopt;
        }

        return const_value_t{ .type = type, .data = negative ? -value : value };
    }

    if (!is_integer(type)) {
        return std::nullopt;
    }

    const std::uint32_t bits = bit_width(type);
    // "-0" is the only negative value an unsigned type holds.
    if (!is_signed(type)) {
        if ((negative && magnitude != 0) || magnitude > unsigned_max(bits)) {
            return std::nullopt;
        }

        return const_value_t{ .type = type, .data = magnitude };
    }

    // A negative value can go one past the maximum: Int8 holds -128 but not 128.
    const auto limit = static_cast<std::uint64_t>(signed_max(bits)) + (negative ? 1U : 0U);
    if (magnitude > limit) {
        return std::nullopt;
    }

    // Negate in unsigned arithmetic, so -9223372036854775808 doesn't overflow.
    const std::uint64_t bits_value = negative ? ~magnitude + 1 : magnitude;
    return const_value_t{ .type = type, .data = std::bit_cast<std::int64_t>(bits_value) };
}

std::optional<const_value_t> make_decimal(const double value, const type_id_t type) {
    const double rounded = round_to(value, type);
    if (!std::isfinite(rounded)) {
        return std::nullopt;
    }

    return const_value_t{ .type = type, .data = rounded };
}

const_value_t make_bool(const bool value) {
    return { .type = type_id_t::BOOL, .data = value };
}

const_value_t make_string(std::string value) {
    return { .type = type_id_t::STRING, .data = std::move(value) };
}

fold_result_t fold_unary(const unary_op_t op, const const_value_t& operand) {
    switch (op) {
    case unary_op_t::NOT:
        return make_bool(!std::get<bool>(operand.data));
    case unary_op_t::BIT_NOT:
        return wrap(~raw_bits(operand), operand.type);
    case unary_op_t::NEGATE:
        // Sema allows '-' only on floats and signed integers. Of those, only the most negative
        // integer overflows: -(-128) doesn't fit in an Int8.
        if (const auto* number = std::get_if<double>(&operand.data)) {
            return const_value_t{ .type = operand.type, .data = -*number };
        }

        if (std::get<std::int64_t>(operand.data) == signed_min(bit_width(operand.type))) {
            return std::unexpected(
                std::format("the result of '-' overflows {}", builtin_name(operand.type)));
        }

        return const_value_t{ .type = operand.type, .data = -std::get<std::int64_t>(operand.data) };
    }

    std::unreachable();
}

fold_result_t fold_binary(const binary_op_t op, const const_value_t& left,
                          const const_value_t& right) {
    // Sema converts both sides to one type first, so their data compares directly. Only a
    // shift's amount can have a different integer type.
    switch (op) {
    case binary_op_t::EQUAL:
        return make_bool(left.data == right.data);
    case binary_op_t::NOT_EQUAL:
        return make_bool(left.data != right.data);
    case binary_op_t::LESS:
        return make_bool(order(left, right) < 0);
    case binary_op_t::LESS_EQUAL:
        return make_bool(order(left, right) <= 0);
    case binary_op_t::GREATER:
        return make_bool(order(left, right) > 0);
    case binary_op_t::GREATER_EQUAL:
        return make_bool(order(left, right) >= 0);
    case binary_op_t::AND:
        return make_bool(std::get<bool>(left.data) && std::get<bool>(right.data));
    case binary_op_t::OR:
        return make_bool(std::get<bool>(left.data) || std::get<bool>(right.data));
    case binary_op_t::BIT_AND:
        return wrap(raw_bits(left) & raw_bits(right), left.type);
    case binary_op_t::BIT_OR:
        return wrap(raw_bits(left) | raw_bits(right), left.type);
    case binary_op_t::BIT_XOR:
        return wrap(raw_bits(left) ^ raw_bits(right), left.type);
    case binary_op_t::SHIFT_LEFT:
    case binary_op_t::SHIFT_RIGHT:
        return fold_shift(op, left, right);
    case binary_op_t::COALESCE:
        // Sema folds '??' itself.
        std::unreachable();
    default:
        break;
    }

    // The rest is arithmetic. On Strings, only '+' is allowed, and it joins them.
    if (left.type == type_id_t::STRING) {
        return make_string(std::get<std::string>(left.data) + std::get<std::string>(right.data));
    }

    if (is_float(left.type)) {
        return fold_float(op, std::get<double>(left.data), std::get<double>(right.data), left.type);
    }

    if (is_signed(left.type)) {
        return fold_signed(op, std::get<std::int64_t>(left.data),
                           std::get<std::int64_t>(right.data), left.type);
    }

    return fold_unsigned(op, std::get<std::uint64_t>(left.data),
                         std::get<std::uint64_t>(right.data), left.type);
}

fold_result_t fold_cast(const const_value_t& value, const type_id_t type) {
    if (value.type == type) {
        return value;
    }

    if (const auto* flag = std::get_if<bool>(&value.data)) {
        return wrap(*flag ? 1U : 0U, type);
    }

    // Between integers, As keeps the low bits: 300 As UInt8 is 44 (docs/spec/02-types.md).
    if (is_integer(value.type) && is_integer(type)) {
        return wrap(raw_bits(value), type);
    }

    // Integer to float can't fail: every 64-bit integer is within Float32's range.
    if (is_integer(value.type)) {
        const double number = is_signed(value.type)
                                  ? static_cast<double>(std::get<std::int64_t>(value.data))
                                  : static_cast<double>(std::get<std::uint64_t>(value.data));
        return const_value_t{ .type = type, .data = round_to(number, type) };
    }

    const double number = std::get<double>(value.data);
    const auto too_big = [&] {
        return std::unexpected(
            std::format("{} doesn't fit in {}", to_text(value), builtin_name(type)));
    };

    // Float64 to Float32 rounds. Beyond Float32's range is an error, not infinity.
    if (is_float(type)) {
        const double rounded = round_to(number, type);
        if (!std::isfinite(rounded)) {
            return too_big();
        }

        return const_value_t{ .type = type, .data = rounded };
    }

    // Float to integer drops the fraction; the rest has to fit.
    const double whole = std::trunc(number);
    const std::uint32_t bits = bit_width(type);
    // The limits are powers of two, which doubles hold exactly. Int64's maximum as a double would
    // round up to 2^63 and let 2^63 through.
    if (is_signed(type)) {
        const double limit = std::ldexp(1.0, static_cast<int>(bits) - 1);
        if (whole < -limit || whole >= limit) {
            return too_big();
        }

        return const_value_t{ .type = type, .data = static_cast<std::int64_t>(whole) };
    }

    const double limit = std::ldexp(1.0, static_cast<int>(bits));
    if (whole <= -1.0 || whole >= limit) {
        return too_big();
    }

    return const_value_t{ .type = type, .data = static_cast<std::uint64_t>(whole) };
}

std::string to_text(const const_value_t& value) {
    if (const auto* flag = std::get_if<bool>(&value.data)) {
        return *flag ? "True" : "False";
    }

    if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
        return std::format("{}", *number);
    }

    if (const auto* number = std::get_if<std::uint64_t>(&value.data)) {
        return std::format("{}", *number);
    }

    // std::format prints the shortest text that reads back exactly, so a Float32 is printed as a
    // float: "0.1", not "0.10000000149011612".
    if (const auto* number = std::get_if<double>(&value.data)) {
        if (value.type == type_id_t::FLOAT32) {
            return std::format("{}", static_cast<float>(*number));
        }

        return std::format("{}", *number);
    }

    if (std::holds_alternative<std::nullptr_t>(value.data)) {
        return "Null";
    }

    return std::get<std::string>(value.data);
}

std::strong_ordering compare(const const_value_t& left, const const_value_t& right) {
    if (const auto* number = std::get_if<std::int64_t>(&left.data)) {
        return *number <=> std::get<std::int64_t>(right.data);
    }

    return std::get<std::uint64_t>(left.data) <=> std::get<std::uint64_t>(right.data);
}

std::optional<integer_method_t> integer_method(const std::string_view name) {
    const bool checked = name.starts_with("Checked");
    if (!checked && !name.starts_with("Wrapping")) {
        return std::nullopt;
    }

    const std::string_view operation = name.substr(checked ? 7 : 8);
    if (operation == "Negate") {
        return integer_method_t{ .op = binary_op_t::SUBTRACT, .checked = checked, .negate = true };
    }

    constexpr auto operations = std::to_array<std::pair<std::string_view, binary_op_t>>({
        { "Add", binary_op_t::ADD },
        { "Subtract", binary_op_t::SUBTRACT },
        { "Multiply", binary_op_t::MULTIPLY },
        { "Divide", binary_op_t::DIVIDE },
    });
    for (const auto& [spelling, op] : operations) {
        // Only Checked divides: a Wrapping divide would still panic on zero.
        if (operation == spelling && (checked || op != binary_op_t::DIVIDE)) {
            return integer_method_t{ .op = op, .checked = checked };
        }
    }

    return std::nullopt;
}

std::optional<const_value_t> fold_integer_method(const integer_method_t& method,
                                                 const const_value_t& self,
                                                 const const_value_t& other) {
    const const_value_t zero =
        *fold_cast({ .type = type_id_t::UINT64, .data = std::uint64_t{ 0 } }, self.type);
    const const_value_t& left = method.negate ? zero : self;
    const const_value_t& right = method.negate ? self : other;
    if (method.checked) {
        const fold_result_t result = fold_binary(method.op, left, right);
        return result ? std::optional(*result) : std::nullopt;
    }

    // Wrapping: the low bits of the result.
    const auto bits = [](const const_value_t& value) {
        if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
            return std::bit_cast<std::uint64_t>(*number);
        }

        return std::get<std::uint64_t>(value.data);
    };

    std::uint64_t raw = bits(left) * bits(right);
    if (method.op == binary_op_t::ADD) {
        raw = bits(left) + bits(right);
    } else if (method.op == binary_op_t::SUBTRACT) {
        raw = bits(left) - bits(right);
    }

    return fold_cast({ .type = type_id_t::UINT64, .data = raw }, self.type).value();
}

} // namespace fb
