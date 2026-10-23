#include "fb/c_abi.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

[[nodiscard]] std::uint64_t aligned(const std::uint64_t offset, const std::uint64_t align) {
    return (offset + align - 1) / align * align;
}

/// A scalar's layout: one field of `size` bytes, aligned to its size.
[[nodiscard]] c_layout_t scalar(const std::uint64_t size, const c_scalar_t kind) {
    return { .size = size, .align = size, .fields = { { .size = size, .kind = kind } } };
}

/// AArch64's homogeneous floating-point aggregate: one to four floats, or one to four doubles,
/// and nothing else, which go in that many vector registers. The part, and how many.
[[nodiscard]] std::optional<std::pair<c_part_t, std::size_t>>
homogeneous_floats(const c_layout_t& layout) {
    const std::size_t count = layout.fields.size();
    if (count == 0 || count > 4) {
        return std::nullopt;
    }

    const c_scalar_t kind = layout.fields.front().kind;
    const bool alike = std::ranges::all_of(
        layout.fields, [&](const c_field_t& field) { return field.kind == kind; });
    const std::uint64_t size = kind == c_scalar_t::DOUBLE ? 8 : 4;
    if (kind == c_scalar_t::INTEGER || !alike || layout.size != count * size) {
        return std::nullopt;
    }

    return std::pair{ kind == c_scalar_t::DOUBLE ? c_part_t::DOUBLE : c_part_t::FLOAT, count };
}

/// How AArch64 passes a Struct, as clang does: a homogeneous floating-point aggregate as an
/// array of its floats or doubles; any other of 16 bytes or less as an i64, or two together
/// (a result of 8 bytes or less as an integer its size); and anything bigger through a pointer
/// to a copy the caller makes, or for a result, memory the caller passes.
[[nodiscard]] c_passing_t aapcs64(const c_layout_t& layout, const bool result) {
    if (const auto floats = homogeneous_floats(layout)) {
        const auto& [part, count] = *floats;
        return { .pieces = std::vector<c_piece_t>(count, { .part = part }), .together = true };
    }

    if (layout.size > 16) {
        return { .pass = result ? c_pass_t::MEMORY : c_pass_t::INDIRECT };
    }

    if (layout.size <= 8) {
        return { .pieces = { { .part = c_part_t::INTEGER, .size = result ? layout.size : 8 } } };
    }

    return {
        .pieces = {
            { .part = c_part_t::INTEGER, .size = 8 },
            { .part = c_part_t::INTEGER, .size = 8 },
        },
        .together = true,
    };
}

/// The piece an SSE eightbyte goes in: a double, a float, or two floats.
[[nodiscard]] c_piece_t sse_piece(const std::vector<c_field_t>& fields) {
    if (fields.size() == 2) {
        return { .part = c_part_t::TWO_FLOATS };
    }

    return fields.front().kind == c_scalar_t::DOUBLE ? c_piece_t{ .part = c_part_t::DOUBLE }
                                                     : c_piece_t{ .part = c_part_t::FLOAT };
}

} // namespace

c_layout_t c_layout(const type_table_t& types, const type_id_t type) {
    switch (type) {
    case type_id_t::BOOL:
    case type_id_t::INT8:
    case type_id_t::UINT8:
        return scalar(1, c_scalar_t::INTEGER);
    case type_id_t::INT16:
    case type_id_t::UINT16:
        return scalar(2, c_scalar_t::INTEGER);
    case type_id_t::INT32:
    case type_id_t::UINT32:
    case type_id_t::CHAR:
        return scalar(4, c_scalar_t::INTEGER);
    case type_id_t::FLOAT32:
        return scalar(4, c_scalar_t::FLOAT);
    case type_id_t::FLOAT64:
        return scalar(8, c_scalar_t::DOUBLE);
    case type_id_t::INT64:
    case type_id_t::UINT64:
    case type_id_t::CSTRING:
        return scalar(8, c_scalar_t::INTEGER);
    default:
        break;
    }

    const type_t& info = types.get(type);
    switch (info.kind) {
    case type_kind_t::ENUM:
        return c_layout(types, info.inner);
    case type_kind_t::PTR:
    case type_kind_t::CALLBACK:
        return scalar(8, c_scalar_t::INTEGER);
    case type_kind_t::FIXED_ARRAY: {
        // Its elements one after another.
        const c_layout_t element = c_layout(types, info.inner);
        c_layout_t layout{ .align = element.align };
        const std::uint64_t count = types.element_count(type);
        // Past what a std::uint64_t counts, it's as big as one: more than any machine has.
        const bool huge = element.size != 0 && count > UINT64_MAX / element.size;
        layout.size = huge ? UINT64_MAX : element.size * count;
        // Only a value of 32 bytes or less goes in registers, AArch64's four doubles at most, so
        // only its fields matter.
        if (layout.size <= 32) {
            for (std::uint64_t i = 0; i < count && !element.fields.empty(); ++i) {
                for (c_field_t field : element.fields) {
                    field.offset += i * element.size;
                    layout.fields.push_back(field);
                }
            }
        }

        return layout;
    }
    default:
        break;
    }

    // A Struct's fields each at its own alignment after the last, and its size a multiple of
    // its largest field's.
    c_layout_t layout;
    for (const field_t& part : info.fields) {
        const c_layout_t inner = c_layout(types, part.type);
        if (layout.size > UINT64_MAX / 4 || inner.size > UINT64_MAX / 4) {
            return { .size = UINT64_MAX, .align = std::max(layout.align, inner.align) };
        }

        const std::uint64_t offset = aligned(layout.size, inner.align);
        for (c_field_t field : inner.fields) {
            field.offset += offset;
            layout.fields.push_back(field);
        }

        layout.size = offset + inner.size;
        layout.align = std::max(layout.align, inner.align);
    }

    layout.size = aligned(layout.size, layout.align);
    return layout;
}

c_abi_t c_abi_of(const std::string_view triple) {
    if (triple.starts_with("aarch64") || triple.starts_with("arm64")) {
        return c_abi_t::AAPCS64;
    }

    if (!triple.starts_with("x86_64") && !triple.starts_with("amd64")) {
        return c_abi_t::OTHER;
    }

    const bool windows = triple.contains("windows") || triple.contains("mingw") ||
                         triple.contains("win32") || triple.contains("cygwin");
    return windows ? c_abi_t::WIN64 : c_abi_t::SYSV;
}

c_passing_t classify(const c_layout_t& layout, const c_abi_t abi, const bool result,
                     c_registers_t& registers) {
    // Win64 passes a Struct of 1, 2, 4, or 8 bytes as an integer, floats or not, and any other
    // through a pointer: a copy the caller makes for an argument, and for a result, memory the
    // caller passes.
    if (abi == c_abi_t::WIN64) {
        const std::uint64_t size = layout.size;
        if (size == 1 || size == 2 || size == 4 || size == 8) {
            return { .pieces = { { .part = c_part_t::INTEGER, .size = size } } };
        }

        return { .pass = result ? c_pass_t::MEMORY : c_pass_t::INDIRECT };
    }

    if (abi == c_abi_t::AAPCS64 && layout.size > 0) {
        return aapcs64(layout, result);
    }

    if (abi == c_abi_t::OTHER || layout.size == 0 || layout.size > 16) {
        return { .pass = c_pass_t::MEMORY };
    }

    // SysV classifies each eightbyte: INTEGER when anything in it is, and SSE when it has only
    // floats. An INTEGER piece covers the bytes up to its last field's end.
    c_passing_t passing;
    unsigned integers = 0;
    unsigned sse = 0;
    for (std::uint64_t begin = 0; begin < layout.size; begin += 8) {
        std::vector<c_field_t> inside;
        std::ranges::copy_if(layout.fields, std::back_inserter(inside), [&](const c_field_t& f) {
            return f.offset >= begin && f.offset < begin + 8;
        });
        if (inside.empty()) {
            continue;
        }

        const bool integer = std::ranges::any_of(
            inside, [](const c_field_t& f) { return f.kind == c_scalar_t::INTEGER; });
        if (integer) {
            const c_field_t& last = inside.back();
            passing.pieces.push_back(
                { .part = c_part_t::INTEGER, .size = last.offset + last.size - begin });
            ++integers;
        } else {
            passing.pieces.push_back(sse_piece(inside));
            ++sse;
        }
    }

    // An argument goes in memory when its pieces don't all fit in the registers left.
    if (!result) {
        if (integers > registers.integers || sse > registers.sse) {
            return { .pass = c_pass_t::MEMORY };
        }

        registers.integers -= integers;
        registers.sse -= sse;
    }

    return passing;
}

} // namespace fb
