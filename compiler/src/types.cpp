#include "fb/types.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace fb {
namespace {

struct named_type_t {
    std::string_view name;
    type_id_t type;
};

/// Every built-in type's own name, then the aliases ("Int" for Int32 and so on). type_name()
/// takes the first match, so the own names must come first.
constexpr auto type_names = std::to_array<named_type_t>({
    { .name = "Bool", .type = type_id_t::BOOL },
    { .name = "Int8", .type = type_id_t::INT8 },
    { .name = "Int16", .type = type_id_t::INT16 },
    { .name = "Int32", .type = type_id_t::INT32 },
    { .name = "Int64", .type = type_id_t::INT64 },
    { .name = "UInt8", .type = type_id_t::UINT8 },
    { .name = "UInt16", .type = type_id_t::UINT16 },
    { .name = "UInt32", .type = type_id_t::UINT32 },
    { .name = "UInt64", .type = type_id_t::UINT64 },
    { .name = "Float32", .type = type_id_t::FLOAT32 },
    { .name = "Float64", .type = type_id_t::FLOAT64 },
    { .name = "String", .type = type_id_t::STRING },
    { .name = "Byte", .type = type_id_t::UINT8 },
    { .name = "Int", .type = type_id_t::INT32 },
    { .name = "Long", .type = type_id_t::INT64 },
    { .name = "Float", .type = type_id_t::FLOAT32 },
    { .name = "Double", .type = type_id_t::FLOAT64 },
});

} // namespace

std::string_view type_name(const type_id_t type) {
    if (type == type_id_t::INVALID) {
        return "<invalid>";
    }

    if (type == type_id_t::NONE) {
        return "nothing";
    }

    for (const named_type_t& entry : type_names) {
        if (entry.type == type) {
            return entry.name;
        }
    }

    std::unreachable();
}

std::optional<type_id_t> builtin_type(const std::string_view name) {
    for (const named_type_t& entry : type_names) {
        if (entry.name == name) {
            return entry.type;
        }
    }

    return std::nullopt;
}

// These ranges rely on type_id_t listing the signed integers, then the unsigned ones.
bool is_integer(const type_id_t type) {
    return type >= type_id_t::INT8 && type <= type_id_t::UINT64;
}

bool is_signed(const type_id_t type) {
    return (type >= type_id_t::INT8 && type <= type_id_t::INT64) || is_float(type);
}

bool is_float(const type_id_t type) {
    return type == type_id_t::FLOAT32 || type == type_id_t::FLOAT64;
}

bool is_numeric(const type_id_t type) {
    return is_integer(type) || is_float(type);
}

std::uint32_t bit_width(const type_id_t type) {
    switch (type) {
    case type_id_t::INT8:
    case type_id_t::UINT8:
        return 8;
    case type_id_t::INT16:
    case type_id_t::UINT16:
        return 16;
    case type_id_t::INT32:
    case type_id_t::UINT32:
    case type_id_t::FLOAT32:
        return 32;
    case type_id_t::INT64:
    case type_id_t::UINT64:
    case type_id_t::FLOAT64:
        return 64;
    default:
        return 0;
    }
}

bool converts_losslessly(const type_id_t from, const type_id_t to) {
    // INVALID was already reported. Accepting it avoids a second error.
    if (from == to || from == type_id_t::INVALID || to == type_id_t::INVALID) {
        return true;
    }

    if (is_integer(from) && is_integer(to)) {
        if (is_signed(from) == is_signed(to)) {
            return bit_width(to) >= bit_width(from);
        }

        // Unsigned to signed needs a wider type. Signed to unsigned loses the negatives.
        return !is_signed(from) && bit_width(to) > bit_width(from);
    }

    // A Float32 holds every integer of up to 24 bits exactly and a Float64 up to 53, so
    // 16-bit and 32-bit integers are the widest that fit.
    if (is_integer(from) && is_float(to)) {
        return bit_width(from) <= (to == type_id_t::FLOAT32 ? 16U : 32U);
    }

    return from == type_id_t::FLOAT32 && to == type_id_t::FLOAT64;
}

} // namespace fb
