#include "fb/types.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace fb {
namespace {

struct named_type_t {
    std::string_view name;
    type_id_t type;
};

/// Every built-in type's own name, then the aliases ("Int" for Int32 and so on).
/// builtin_name() takes the first match, so the own names must come first.
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

std::string_view builtin_name(const type_id_t type) {
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

type_table_t::type_table_t() {
    for (std::uint32_t i = 0; i < std::to_underlying(type_id_t::BUILTIN_COUNT); ++i) {
        add({ .name = std::string(builtin_name(static_cast<type_id_t>(i))) });
    }
}

const type_t& type_table_t::get(const type_id_t id) const {
    assert(std::to_underlying(id) < m_entries.size());
    return *m_entries[std::to_underlying(id)].type;
}

type_id_t type_table_t::nullable(const type_id_t inner) {
    return make(type_kind_t::NULLABLE, inner, "?");
}

type_id_t type_table_t::array(const type_id_t element) {
    return make(type_kind_t::ARRAY, element, "[]");
}

type_id_t type_table_t::make(const type_kind_t kind, const type_id_t inner,
                             const std::string_view suffix) {
    if (inner == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    // Found by index each time, since adding a type can move the entries.
    const auto made = [&] -> type_id_t& {
        entry_t& entry = m_entries[std::to_underlying(inner)];
        return kind == type_kind_t::NULLABLE ? entry.nullable : entry.array;
    };

    if (made() == type_id_t::INVALID) {
        std::string full_name = std::format("{}{}", name(inner), suffix);
        type_t type{ .kind = kind, .name = std::move(full_name), .inner = inner };
        if (kind == type_kind_t::NULLABLE) {
            type.fields.push_back({ .name = "value", .type = inner });
        }

        made() = add(std::move(type));
    }

    return made();
}

type_id_t type_table_t::fixed_array(const type_id_t element,
                                    const std::vector<std::uint32_t>& sizes) {
    if (element == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    for (const type_id_t made : m_fixed_arrays) {
        if (get(made).inner == element && get(made).sizes == sizes) {
            return made;
        }
    }

    std::string text = std::format("{}[", name(element));
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        text += std::format("{}{}", i == 0 ? "" : ", ", sizes[i]);
    }

    const type_id_t id = add({
        .kind = type_kind_t::FIXED_ARRAY,
        .name = text + "]",
        .inner = element,
        .sizes = sizes,
    });
    m_fixed_arrays.push_back(id);
    return id;
}

std::uint64_t type_table_t::element_count(const type_id_t type) const {
    std::uint64_t count = 1;
    for (const std::uint32_t size : get(type).sizes) {
        count *= size;
    }

    return count;
}

type_id_t type_table_t::add_struct(std::string name) {
    return add({ .kind = type_kind_t::STRUCT, .name = std::move(name) });
}

type_id_t type_table_t::add_class(std::string name) {
    return add({ .kind = type_kind_t::CLASS, .name = std::move(name) });
}

void type_table_t::set_fields(const type_id_t type, std::vector<field_t> fields) {
    assert(get(type).kind == type_kind_t::STRUCT || get(type).kind == type_kind_t::CLASS);
    m_entries[std::to_underlying(type)].type->fields = std::move(fields);
}

type_id_t type_table_t::add_enum(std::string name) {
    return add({ .kind = type_kind_t::ENUM, .name = std::move(name), .inner = type_id_t::INT32 });
}

void type_table_t::set_values(const type_id_t type, const type_id_t integer,
                              std::vector<enum_value_t> values, const bool flags) {
    assert(get(type).kind == type_kind_t::ENUM);
    type_t& entry = *m_entries[std::to_underlying(type)].type;
    entry.inner = integer;
    entry.values = std::move(values);
    entry.flags = flags;
}

std::uint64_t type_table_t::enum_zero(const type_id_t type) const {
    const type_t& entry = get(type);
    return entry.flags || entry.values.empty() ? 0 : entry.values.front().bits;
}

void type_table_t::set_base(const type_id_t type, const type_id_t base) {
    assert(get(type).kind == type_kind_t::CLASS && get(base).kind == type_kind_t::CLASS);
    m_entries[std::to_underlying(type)].type->base = base;
}

bool type_table_t::extends(const type_id_t type, const type_id_t base) const {
    for (type_id_t current = type; current != type_id_t::INVALID; current = get(current).base) {
        if (current == base) {
            return true;
        }
    }

    return false;
}

bool type_table_t::has_references(const type_id_t type) const {
    const type_kind_t kind = get(type).kind;
    if (type == type_id_t::STRING || kind == type_kind_t::CLASS || kind == type_kind_t::ARRAY) {
        return true;
    }

    if (kind == type_kind_t::FIXED_ARRAY) {
        return has_references(get(type).inner);
    }

    return std::ranges::any_of(get(type).fields,
                               [this](const field_t& field) { return has_references(field.type); });
}

bool type_table_t::has_zero(const type_id_t type) const {
    const type_kind_t kind = get(type).kind;
    if (kind == type_kind_t::CLASS || kind == type_kind_t::ARRAY) {
        return false;
    }

    if (kind == type_kind_t::FIXED_ARRAY) {
        return has_zero(get(type).inner);
    }

    // A T?'s zero is Null, whatever the T.
    if (get(type).kind != type_kind_t::STRUCT) {
        return true;
    }

    return std::ranges::all_of(get(type).fields,
                               [this](const field_t& field) { return has_zero(field.type); });
}

type_id_t type_table_t::add(type_t type) {
    const auto id = static_cast<type_id_t>(m_entries.size());
    m_entries.push_back({ .type = std::make_unique<type_t>(std::move(type)) });
    return id;
}

} // namespace fb
