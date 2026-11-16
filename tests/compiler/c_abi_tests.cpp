#include "fb/c_abi.hpp"
#include "fb/codegen.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <format>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fb::c_layout_t;
using fb::c_part_t;
using fb::c_pass_t;
using fb::c_passing_t;
using fb::c_scalar_t;

constexpr c_scalar_t integer = c_scalar_t::INTEGER;
constexpr c_scalar_t single = c_scalar_t::FLOAT;
constexpr c_scalar_t wide = c_scalar_t::DOUBLE;

/// A layout of scalars one after another, each at its own alignment.
c_layout_t layout(const std::vector<std::pair<std::uint64_t, c_scalar_t>>& scalars) {
    c_layout_t out;
    for (const auto& [size, kind] : scalars) {
        const std::uint64_t offset = (out.size + size - 1) / size * size;
        out.fields.push_back({ .offset = offset, .size = size, .kind = kind });
        out.size = offset + size;
        out.align = std::max(out.align, size);
    }

    out.size = (out.size + out.align - 1) / out.align * out.align;
    return out;
}

c_passing_t argument(const c_layout_t& of, const fb::c_abi_t abi) {
    fb::c_registers_t registers;
    return classify(of, abi, false, registers);
}

c_passing_t result(const c_layout_t& of, const fb::c_abi_t abi) {
    fb::c_registers_t registers;
    return classify(of, abi, true, registers);
}

c_passing_t direct(std::vector<fb::c_piece_t> pieces) {
    return { .pieces = std::move(pieces) };
}

/// Pieces that go as one array.
c_passing_t together(std::vector<fb::c_piece_t> pieces) {
    return { .pieces = std::move(pieces), .together = true };
}

constexpr fb::c_abi_t win64 = fb::c_abi_t::WIN64;
constexpr fb::c_abi_t sysv = fb::c_abi_t::SYSV;
constexpr fb::c_abi_t aapcs64 = fb::c_abi_t::AAPCS64;

} // namespace

TEST_CASE("a target's triple says its C ABI", "[c_abi]") {
    CHECK(fb::c_abi_of("x86_64-w64-windows-gnu") == win64);
    CHECK(fb::c_abi_of("x86_64-pc-windows-msvc") == win64);
    CHECK(fb::c_abi_of("x86_64-unknown-linux-gnu") == sysv);
    CHECK(fb::c_abi_of("x86_64-apple-macosx14.0.0") == sysv);
    CHECK(fb::c_abi_of("aarch64-apple-macosx14.0.0") == aapcs64);
    CHECK(fb::c_abi_of("arm64-apple-macosx14.0.0") == aapcs64);
    CHECK(fb::c_abi_of("aarch64-unknown-linux-gnu") == aapcs64);
    CHECK(fb::c_abi_of("aarch64-pc-windows-msvc") == aapcs64);
    CHECK(fb::c_abi_of("riscv64-unknown-linux-gnu") == fb::c_abi_t::OTHER);
}

TEST_CASE("a target's triple says what C's long, unsigned long, and wchar_t are", "[c_abi]") {
    using fb::type_id_t;
    // As clang has them: 32 bits on Windows, and wchar_t unsigned on AArch64 but Apple's.
    const auto integers = [](const std::string_view triple) {
        const fb::c_integers_t found = fb::c_integers_of(triple);
        return std::vector{ found.c_long, found.c_ulong, found.c_wchar };
    };
    const std::vector windows{ type_id_t::INT32, type_id_t::UINT32, type_id_t::UINT16 };
    CHECK(integers("x86_64-w64-windows-gnu") == windows);
    CHECK(integers("x86_64-pc-windows-msvc") == windows);
    CHECK(integers("aarch64-pc-windows-msvc") == windows);
    const std::vector signed_wchar{ type_id_t::INT64, type_id_t::UINT64, type_id_t::INT32 };
    CHECK(integers("x86_64-unknown-linux-gnu") == signed_wchar);
    CHECK(integers("x86_64-apple-macosx14.0.0") == signed_wchar);
    CHECK(integers("arm64-apple-macosx14.0.0") == signed_wchar);
    CHECK(integers("riscv64-unknown-linux-gnu") == signed_wchar);
    const std::vector unsigned_wchar{ type_id_t::INT64, type_id_t::UINT64, type_id_t::UINT32 };
    CHECK(integers("aarch64-unknown-linux-gnu") == unsigned_wchar);
    CHECK(integers("aarch64-linux-android") == unsigned_wchar);
    const std::vector cygwin{ type_id_t::INT64, type_id_t::UINT64, type_id_t::UINT16 };
    CHECK(integers("x86_64-pc-cygwin") == cygwin);
    CHECK(integers("x86_64-pc-windows-cygnus") == cygwin);

    // A Struct of them is laid out as C does on each.
    fb::type_table_t types;
    const type_id_t pair = types.add_struct("Pair");
    const std::vector<fb::field_t> fields{
        { .name = "a", .type = type_id_t::CWCHAR },
        { .name = "b", .type = type_id_t::CLONG },
    };
    types.set_fields(pair, fields);
    CHECK(fb::c_layout(types, pair, fb::c_integers_of("x86_64-w64-windows-gnu")).size == 8);
    CHECK(fb::c_layout(types, pair, fb::c_integers_of("x86_64-unknown-linux-gnu")).size == 16);
    CHECK(
        fb::c_layout(types, type_id_t::CULONG, fb::c_integers_of("aarch64-pc-windows-msvc")).size ==
        4);
}

TEST_CASE("Win64 passes a Struct of 1, 2, 4, or 8 bytes as an integer, and others through a "
          "pointer",
          "[c_abi]") {
    const c_layout_t two = layout({ { 1, integer }, { 1, integer } });
    CHECK(argument(two, win64) == direct({ { .part = c_part_t::INTEGER, .size = 2 } }));
    CHECK(result(two, win64) == direct({ { .part = c_part_t::INTEGER, .size = 2 } }));
    // Floats too.
    const c_layout_t floats = layout({ { 4, single }, { 4, single } });
    CHECK(argument(floats, win64) == direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    // Any other size goes through a pointer to a copy, or for a result, memory the caller gives.
    const std::vector<c_layout_t> others{
        layout({ { 1, integer }, { 1, integer }, { 1, integer } }),
        layout({ { 4, single }, { 4, single }, { 4, single } }),
        layout({ { 8, integer }, { 8, wide } }),
    };
    for (const c_layout_t& other : others) {
        CHECK(argument(other, win64) == c_passing_t{ .pass = c_pass_t::INDIRECT });
        CHECK(result(other, win64) == c_passing_t{ .pass = c_pass_t::MEMORY });
    }
}

TEST_CASE("SysV passes each eightbyte of a Struct in a register of its class", "[c_abi]") {
    CHECK(argument(layout({ { 4, integer }, { 4, integer } }), sysv) ==
          direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    CHECK(argument(layout({ { 4, single }, { 4, single }, { 4, single } }), sysv) ==
          direct({ { .part = c_part_t::TWO_FLOATS }, { .part = c_part_t::FLOAT } }));
    CHECK(argument(layout({ { 8, integer }, { 8, wide } }), sysv) ==
          direct({ { .part = c_part_t::INTEGER, .size = 8 }, { .part = c_part_t::DOUBLE } }));
    CHECK(argument(layout({ { 8, wide }, { 1, integer } }), sysv) ==
          direct({ { .part = c_part_t::DOUBLE }, { .part = c_part_t::INTEGER, .size = 1 } }));
    // An eightbyte with an integer in it is INTEGER, and one covers its fields' bytes.
    CHECK(argument(layout({ { 4, integer }, { 4, single } }), sysv) ==
          direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    CHECK(argument(layout({ { 1, integer }, { 1, integer }, { 1, integer } }), sysv) ==
          direct({ { .part = c_part_t::INTEGER, .size = 3 } }));

    // Over 16 bytes goes in memory.
    const c_layout_t big = layout({ { 8, integer }, { 8, integer }, { 4, integer } });
    CHECK(argument(big, sysv) == c_passing_t{ .pass = c_pass_t::MEMORY });
    CHECK(result(big, sysv) == c_passing_t{ .pass = c_pass_t::MEMORY });

    // An argument whose pieces don't all fit in the registers left goes in memory, and takes
    // none of them.
    fb::c_registers_t registers{ .integers = 1, .sse = 8 };
    const c_layout_t two = layout({ { 8, integer }, { 8, integer } });
    CHECK(classify(two, sysv, false, registers) == c_passing_t{ .pass = c_pass_t::MEMORY });
    CHECK(registers.integers == 1);
    CHECK(classify(layout({ { 8, integer } }), sysv, false, registers) ==
          direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    CHECK(registers.integers == 0);
}

TEST_CASE("AAPCS64 passes up to four floats or doubles in their own registers, and any other "
          "Struct of 16 bytes or less in integer ones",
          "[c_abi]") {
    // A homogeneous floating-point aggregate goes as an array of its floats or doubles, both ways.
    const c_layout_t floats = layout({ { 4, single }, { 4, single }, { 4, single } });
    const c_passing_t three = together({
        { .part = c_part_t::FLOAT },
        { .part = c_part_t::FLOAT },
        { .part = c_part_t::FLOAT },
    });
    CHECK(argument(floats, aapcs64) == three);
    CHECK(result(floats, aapcs64) == three);
    const c_layout_t doubles = layout({ { 8, wide }, { 8, wide }, { 8, wide }, { 8, wide } });
    CHECK(argument(doubles, aapcs64) ==
          together(std::vector<fb::c_piece_t>(4, {
                                                     .part = c_part_t::DOUBLE,
                                                 })));

    // Five floats are no such aggregate, and a float beside a double isn't either.
    const c_layout_t five =
        layout({ { 4, single }, { 4, single }, { 4, single }, { 4, single }, { 4, single } });
    CHECK(argument(five, aapcs64) == c_passing_t{ .pass = c_pass_t::INDIRECT });
    const c_passing_t two = together({
        { .part = c_part_t::INTEGER, .size = 8 },
        { .part = c_part_t::INTEGER, .size = 8 },
    });
    CHECK(argument(layout({ { 4, single }, { 8, wide } }), aapcs64) == two);

    // Any other of 8 bytes or less is an i64, and as a result, an integer of its size; one of 16
    // bytes or less is two i64s together.
    const c_layout_t bytes = layout({ { 1, integer }, { 1, integer }, { 1, integer } });
    CHECK(argument(bytes, aapcs64) == direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    CHECK(result(bytes, aapcs64) == direct({ { .part = c_part_t::INTEGER, .size = 3 } }));
    const c_layout_t twelve = layout({ { 4, integer }, { 4, integer }, { 4, integer } });
    CHECK(argument(twelve, aapcs64) == two);
    CHECK(result(twelve, aapcs64) == two);

    // A bigger one goes through a pointer to a copy, or for a result, memory the caller gives.
    const c_layout_t big = layout({ { 8, integer }, { 8, integer }, { 4, integer } });
    CHECK(argument(big, aapcs64) == c_passing_t{ .pass = c_pass_t::INDIRECT });
    CHECK(result(big, aapcs64) == c_passing_t{ .pass = c_pass_t::MEMORY });

    // A fixed array's floats count one by one, four doubles too.
    fb::type_table_t types;
    const fb::type_id_t held = types.add_struct("Held");
    types.set_fields(held,
                     {
                         { .name = "xy", .type = types.fixed_array(fb::type_id_t::FLOAT32, { 2 }) },
                         { .name = "z", .type = fb::type_id_t::FLOAT32 },
                     });
    CHECK(argument(fb::c_layout(types, held, fb::c_integers_t()), aapcs64) == three);
    const fb::type_id_t quad = types.add_struct("Quad");
    types.set_fields(quad,
                     {
                         { .name = "v", .type = types.fixed_array(fb::type_id_t::FLOAT64, { 4 }) },
                     });
    CHECK(result(fb::c_layout(types, quad, fb::c_integers_t()), aapcs64) ==
          together(std::vector<fb::c_piece_t>(4, { .part = c_part_t::DOUBLE })));
}

TEST_CASE("a Callback in a Struct takes 8 bytes, as C's function pointer does", "[c_abi]") {
    fb::type_table_t types;
    const fb::type_id_t callback = types.callback({ fb::type_id_t::INT32 }, fb::type_id_t::INT32);
    const fb::type_id_t holder = types.add_struct("Holder");
    types.set_fields(holder, {
                                 { .name = "f", .type = callback },
                                 { .name = "value", .type = fb::type_id_t::INT32 },
                             });
    const c_layout_t found = fb::c_layout(types, holder, fb::c_integers_t());
    CHECK(found.size == 16);
    CHECK(argument(found, win64).pass == c_pass_t::INDIRECT);
    CHECK(argument(found, sysv) == direct({
                                       { .part = c_part_t::INTEGER, .size = 8 },
                                       { .part = c_part_t::INTEGER, .size = 4 },
                                   }));
}

namespace {

using fb::type_id_t;

/// A @CLayout Struct of `fields`, packed to `pack` when it isn't 0, or a union.
type_id_t c_struct(fb::type_table_t& types, const std::string& name,
                   const std::vector<type_id_t>& fields, const std::uint8_t pack = 0,
                   const bool united = false) {
    const type_id_t made = types.add_struct(name);
    std::vector<fb::field_t> list;
    list.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        list.push_back({ .name = std::format("f{}", i), .type = fields[i] });
    }

    types.set_fields(made, std::move(list));
    types.set_c_layout(made);
    if (united) {
        types.set_c_union(made);
    }

    if (pack != 0) {
        types.set_c_pack(made, pack);
    }

    return made;
}

/// A layout's size, alignment, and Fields' offsets.
struct shape_t {
    std::uint64_t size = 0;
    std::uint64_t align = 0;
    std::vector<std::uint64_t> offsets;

    bool operator==(const shape_t&) const = default;
};

shape_t shape_of(const fb::type_table_t& types, const type_id_t type) {
    const c_layout_t found = fb::c_layout(types, type, fb::c_integers_t());
    return { .size = found.size, .align = found.align, .offsets = found.offsets };
}

} // namespace

// Each layout is clang's, for x86-64 Linux and Windows, MSVC's too, and AArch64 Linux and Apple.
TEST_CASE("a packed Struct is laid out as #pragma pack has it", "[c_abi]") {
    fb::type_table_t types;
    constexpr type_id_t u8 = type_id_t::UINT8;
    constexpr type_id_t i16 = type_id_t::INT16;
    constexpr type_id_t i32 = type_id_t::INT32;
    constexpr type_id_t i64 = type_id_t::INT64;
    const std::vector<type_id_t> mixed{ u8, i16, i32, i64 };

    // A Field's alignment is the smaller of its own and the packing, and so is the Struct's.
    CHECK(shape_of(types, c_struct(types, "P1", mixed, 1)) ==
          shape_t{ .size = 15, .align = 1, .offsets = { 0, 1, 3, 7 } });
    CHECK(shape_of(types, c_struct(types, "P2", mixed, 2)) ==
          shape_t{ .size = 16, .align = 2, .offsets = { 0, 2, 4, 8 } });
    CHECK(shape_of(types, c_struct(types, "P2b", { u8, i64, u8 }, 2)) ==
          shape_t{ .size = 12, .align = 2, .offsets = { 0, 2, 10 } });
    CHECK(shape_of(types, c_struct(types, "P4", mixed, 4)) ==
          shape_t{ .size = 16, .align = 4, .offsets = { 0, 2, 4, 8 } });
    const type_id_t late = c_struct(types, "P4b", { i32, i64 }, 4);
    CHECK(shape_of(types, late) == shape_t{ .size = 12, .align = 4, .offsets = { 0, 4 } });
    CHECK(shape_of(types, c_struct(types, "P4c", { u8, i16 }, 4)) ==
          shape_t{ .size = 4, .align = 2, .offsets = { 0, 2 } });
    // Packed to its largest alignment or more, it's as it would be anyway.
    CHECK(shape_of(types, c_struct(types, "P8", { i32, i64 }, 8)) ==
          shape_t{ .size = 16, .align = 8, .offsets = { 0, 8 } });
    CHECK(shape_of(types, c_struct(types, "P16", { i32, i64 }, 16)) ==
          shape_t{ .size = 16, .align = 8, .offsets = { 0, 8 } });

    // A plain Struct in a packed one is at the packing, its inside as it is; a packed one in a
    // plain one keeps its own alignment, as each of an array of them does.
    const type_id_t plain = c_struct(types, "Plain", { i32, i64 });
    CHECK(shape_of(types, c_struct(types, "HoldsPlain", { u8, plain, u8 }, 2)) ==
          shape_t{ .size = 20, .align = 2, .offsets = { 0, 2, 18 } });
    CHECK(shape_of(types, c_struct(types, "HoldsPacked", { u8, late })) ==
          shape_t{ .size = 16, .align = 4, .offsets = { 0, 4 } });
    // A packed one in a packed one is at the smaller of its own packed alignment and the outer's
    // packing, as #pragma pack(2) around a pack(4) one puts it at 2.
    CHECK(shape_of(types, c_struct(types, "PackedInPacked", { u8, late }, 2)) ==
          shape_t{ .size = 14, .align = 2, .offsets = { 0, 2 } });
    CHECK(shape_of(types,
                   c_struct(types, "HoldsArray", { u8, types.fixed_array(late, { 2 }), u8 })) ==
          shape_t{ .size = 32, .align = 4, .offsets = { 0, 4, 28 } });

    // A union's Fields are each at 0, and its size its largest's, rounded to its alignment, which
    // a packing takes down too.
    const type_id_t nine = types.fixed_array(u8, { 9 });
    CHECK(shape_of(types, c_struct(types, "U1", { u8, i32, i64 }, 0, true)) ==
          shape_t{ .size = 8, .align = 8, .offsets = { 0, 0, 0 } });
    const type_id_t bytes = c_struct(types, "U2", { nine, i32 }, 0, true);
    CHECK(shape_of(types, bytes) == shape_t{ .size = 12, .align = 4, .offsets = { 0, 0 } });
    CHECK(shape_of(types, c_struct(types, "U3", { nine, i32 }, 1, true)) ==
          shape_t{ .size = 9, .align = 1, .offsets = { 0, 0 } });
    CHECK(shape_of(types, c_struct(types, "HoldsUnion", { type_id_t::INT8, bytes })) ==
          shape_t{ .size = 16, .align = 4, .offsets = { 0, 4 } });
}

TEST_CASE("a packed Struct or union is passed as clang passes it", "[c_abi]") {
    fb::type_table_t types;
    constexpr type_id_t i32 = type_id_t::INT32;
    constexpr type_id_t i64 = type_id_t::INT64;
    constexpr type_id_t f32 = type_id_t::FLOAT32;
    constexpr type_id_t f64 = type_id_t::FLOAT64;
    const auto of = [&](const type_id_t type) {
        return fb::c_layout(types, type, fb::c_integers_t());
    };

    // SysV: one with a scalar off its own alignment goes in memory, and one without by its
    // eightbytes, wherever its Fields are.
    const c_layout_t late = of(c_struct(types, "Late", { i32, i64 }, 4));
    CHECK(argument(late, sysv).pass == c_pass_t::MEMORY);
    CHECK(result(late, sysv).pass == c_pass_t::MEMORY);
    const c_layout_t early = of(c_struct(types, "Early", { i64, i32 }, 4));
    CHECK(argument(early, sysv) == direct({
                                       { .part = c_part_t::INTEGER, .size = 8 },
                                       { .part = c_part_t::INTEGER, .size = 4 },
                                   }));
    const c_layout_t floats = of(c_struct(types, "FloatsDouble", { f32, f32, f64 }, 4));
    CHECK(argument(floats, sysv) == direct({
                                        { .part = c_part_t::TWO_FLOATS },
                                        { .part = c_part_t::DOUBLE },
                                    }));
    // Win64 by size alone, and AArch64 by size: 12 bytes as two i64s, and 5 as one, or as an
    // integer its size for a result.
    CHECK(argument(late, win64).pass == c_pass_t::INDIRECT);
    CHECK(argument(late, aapcs64) == together({
                                         { .part = c_part_t::INTEGER, .size = 8 },
                                         { .part = c_part_t::INTEGER, .size = 8 },
                                     }));
    const c_layout_t five = of(c_struct(types, "Five", { type_id_t::UINT8, i32 }, 1));
    CHECK(argument(five, sysv).pass == c_pass_t::MEMORY);
    CHECK(argument(five, aapcs64) == direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    CHECK(result(five, aapcs64) == direct({ { .part = c_part_t::INTEGER, .size = 5 } }));
    const c_layout_t eight =
        of(c_struct(types, "EightBytes", { type_id_t::UINT8, i32, type_id_t::UINT16 }, 2));
    CHECK(argument(eight, win64) == direct({ { .part = c_part_t::INTEGER, .size = 8 } }));

    // A union's eightbyte is a double when it has one, two floats for a pair, an integer when
    // anything is, and as wide as its widest Field.
    CHECK(argument(of(c_struct(types, "FD", { f32, f64 }, 0, true)), sysv) ==
          direct({ { .part = c_part_t::DOUBLE } }));
    CHECK(argument(of(c_struct(types, "FFI", { types.fixed_array(f32, { 2 }), i32 }, 0, true)),
                   sysv) == direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
    const c_layout_t same = of(c_struct(types, "FF", { f32, f32 }, 0, true));
    CHECK(argument(same, sysv) == direct({ { .part = c_part_t::FLOAT } }));
    const type_id_t pair = c_struct(types, "Pair", { f32, f32 });
    const c_layout_t paired = of(c_struct(types, "Paired", { f32, pair }, 0, true));
    CHECK(argument(paired, sysv) == direct({ { .part = c_part_t::TWO_FLOATS } }));
    CHECK(argument(of(c_struct(types, "PD", { pair, f64 }, 0, true)), sysv) ==
          direct({ { .part = c_part_t::DOUBLE } }));
    // AArch64 counts a union's floats at one place once: two floats is one float, and a pair
    // with a float is two.
    CHECK(argument(same, aapcs64) == together({ { .part = c_part_t::FLOAT } }));
    CHECK(argument(paired, aapcs64) ==
          together({ { .part = c_part_t::FLOAT }, { .part = c_part_t::FLOAT } }));
    CHECK(argument(of(c_struct(types, "FD2", { f32, f64 }, 0, true)), aapcs64) ==
          direct({ { .part = c_part_t::INTEGER, .size = 8 } }));
}

TEST_CASE("an Extern Function's packed Structs and unions go as clang declares them", "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation = fb::compile(
        "main.fb",
        "@CLayout(Pack: 4) Struct Late\n    Field a: Int32\n    Field b: Int64\nEnd Struct\n"
        "@CLayout(Pack: 4) Struct Early\n    Field a: Int64\n    Field b: Int32\nEnd Struct\n"
        "@CLayout(Pack: 1) Struct Five\n    Field a: UInt8\n    Field b: Int32\nEnd Struct\n"
        "@CLayout(Pack: 2) Struct Mix\n    Field a: UInt8\n    Field b: Int32\n"
        "    Field c: UInt16\nEnd Struct\n"
        "@CUnion Struct FD\n    Field f: Float32\n    Field d: Float64\nEnd Struct\n"
        "@CUnion Struct FF\n    Field a: Float32\n    Field b: Float32\nEnd Struct\n"
        "@CUnion Struct F3D\n    Field f: Float32[3]\n    Field d: Float64\nEnd Struct\n"
        "@CUnion Struct B3S\n    Field b: UInt8[3]\n    Field s: UInt16\nEnd Struct\n"
        "Extern \"C\"\n"
        "    Function late(v: Late): Late\n"
        "    Function early(v: Early): Early\n"
        "    Function five(v: Five): Five\n"
        "    Function mix(v: Mix): Mix\n"
        "    Function fd(v: FD): FD\n"
        "    Function ff(v: FF): FF\n"
        "    Function f3d(v: F3D): F3D\n"
        "    Function b3s(v: B3S): B3S\n"
        "End Extern\n"
        "Local a = late(Late(1, 2))\nLocal b = early(Early(1, 2))\nLocal c = five(Five(1, 2))\n"
        "Local d = mix(Mix(1, 2, 3))\nLocal e = fd(FD())\nLocal f = ff(FF())\n"
        "Local g = f3d(F3D())\nLocal h = b3s(B3S())\n",
        files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const auto ir = [&](const std::string_view triple) {
        return fb::emit_ir(module, compilation.sources, compilation.file,
                           { .target_triple = std::string(triple) })
            .value_or("");
    };

    // Each in the registers and memory clang passes its C in, and as clang declares it but for
    // the two below.
    const std::string linux = ir("x86_64-unknown-linux-gnu");
    CHECK(linux.contains("%fb.Late = type { [0 x i32], <{ i32, i64 }> }"));
    CHECK(linux.contains("%fb.FD = type { [0 x i64], <{ [8 x i8] }> }"));
    CHECK(linux.contains("declare void @late(ptr sret(%fb.Late) align 4, ptr byval(%fb.Late) "
                         "align 8)"));
    CHECK(linux.contains("declare { i64, i32 } @early(i64, i32)"));
    CHECK(linux.contains("declare void @five(ptr sret(%fb.Five) align 1, ptr byval(%fb.Five) "
                         "align 8)"));
    CHECK(linux.contains("declare void @mix(ptr sret(%fb.Mix) align 2, ptr byval(%fb.Mix) "
                         "align 8)"));
    CHECK(linux.contains("declare double @fd(double)"));
    CHECK(linux.contains("declare float @ff(float)"));
    // clang has { double, double } and i32: the same two SSE registers, and the same register,
    // since an integer piece here covers the Fields' bytes without the padding after them, as
    // for a plain Struct.
    CHECK(linux.contains("declare { double, float } @f3d(double, float)"));
    CHECK(linux.contains("declare i24 @b3s(i24)"));

    const std::string windows = ir("x86_64-w64-windows-gnu");
    CHECK(windows.contains("declare void @late(ptr sret(%fb.Late) align 4, ptr)"));
    CHECK(windows.contains("declare void @five(ptr sret(%fb.Five) align 1, ptr)"));
    CHECK(windows.contains("declare i64 @mix(i64)"));
    CHECK(windows.contains("declare i64 @fd(i64)"));
    CHECK(windows.contains("declare i32 @ff(i32)"));

    const std::string arm = ir("aarch64-unknown-linux-gnu");
    CHECK(arm.contains("declare [2 x i64] @late([2 x i64])"));
    CHECK(arm.contains("declare [2 x i64] @early([2 x i64])"));
    CHECK(arm.contains("declare i40 @five(i64)"));
    CHECK(arm.contains("declare i64 @mix(i64)"));
    CHECK(arm.contains("declare i64 @fd(i64)"));
    CHECK(arm.contains("declare [1 x float] @ff([1 x float])"));
    CHECK(ir("arm64-apple-macosx14.0.0").contains("declare i40 @five(i64)"));
}

TEST_CASE("a Callback's thunk with a closed Enum beside its Structs is whole IR on each target",
          "[c_abi]") {
    // The Enum's check branches, and the Structs' copies go in the thunk's alloca block, not
    // after it; the thunks are made while the top-level code is.
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Enum Color\n    Red\n    Green\nEnd Enum\n"
                    "@CLayout Struct Text\n    Field bytes: Ptr<UInt8>\n    Field size: UInt64\n"
                    "End Struct\n"
                    "@CLayout Struct Point\n    Field x: Int32\n    Field y: Int32\nEnd Struct\n"
                    "@CLayout Struct Tint\n    Field color: Color\n    Field edges: Color[2]\n"
                    "End Struct\n"
                    "Function ColorText(c: Color, t: Text): Text => t\n"
                    "Function ColorPoint(c: Color): Point => Point(1, 2)\n"
                    "Function Tinted(t: Tint, c: Color): Tint => t\n"
                    "Local a: Callback(Color, Text): Text = ColorText\n"
                    "Local b: Callback(Color): Point = ColorPoint\n"
                    "Local c: Callback(Tint, Color): Tint = Tinted\n",
                    files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    for (const std::string_view triple : {
             "x86_64-unknown-linux-gnu",
             "x86_64-w64-windows-gnu",
             "aarch64-unknown-linux-gnu",
             "arm64-apple-macosx14.0.0",
             "x86_64-apple-macosx14.0.0",
         }) {
        CAPTURE(triple);
        const auto made = fb::emit_ir(module, compilation.sources, compilation.file,
                                      { .target_triple = std::string(triple) });
        INFO((made ? std::string() : made.error()));
        REQUIRE(made);
        CHECK(made->contains("@\"fb.callback.ColorText(Color, Text)\""));
        CHECK(made->contains("@\"fb.callback.ColorPoint(Color)\""));
        CHECK(made->contains("C passed 'c' a value that isn't one of Color's"));
        CHECK(made->contains("C passed Field edges of 't' a value that isn't one of Color's"));
    }
}

TEST_CASE("a Callback's Structs go as the target's C ABI has them, both ways", "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation = fb::compile(
        "main.fb",
        "@CLayout Struct Text\n"
        "    Field bytes: Ptr<UInt8>\n"
        "    Field size: UInt64\n"
        "End Struct\n"
        "@CLayout Struct Point\n"
        "    Field x: Int32\n"
        "    Field y: Int32\n"
        "End Struct\n"
        "@CLayout Struct Vec3\n"
        "    Field x: Float32\n"
        "    Field y: Float32\n"
        "    Field z: Float32\n"
        "End Struct\n"
        "@CLayout Struct Box\n"
        "    Field min: Point\n"
        "    Field max: Point\n"
        "    Field filled: Bool\n"
        "End Struct\n"
        "@CLayout(Pack: 4) Struct Late\n"
        "    Field a: Int32\n"
        "    Field b: Int64\n"
        "End Struct\n"
        "@CUnion Struct Either\n"
        "    Field n: Int32\n"
        "    Field f: Float32\n"
        "End Struct\n"
        "Function TextCb(v: Text, data: Ptr<Void>): Text => v\n"
        "Function PointCb(a: Point, b: Point): Point => a\n"
        "Function Vec3Cb(v: Vec3, by: Float32): Vec3 => v\n"
        "Function BoxCb(x: Int32, b: Box): Box => b\n"
        "Function LateCb(v: Late): Late => v\n"
        "Function EitherCb(v: Either): Either => v\n"
        "Function StackCb(a: Int64, b: Int64, c: Int64, d: Int64, e: Int64, t: Text, f: Int64): "
        "Int64 => a + f\n"
        "Local text: Callback(Text, Ptr<Void>): Text = TextCb\n"
        "Local point: Callback(Point, Point): Point = PointCb\n"
        "Local vec3: Callback(Vec3, Float32): Vec3 = Vec3Cb\n"
        "Local box: Callback(Int32, Box): Box = BoxCb\n"
        "Local late: Callback(Late): Late = LateCb\n"
        "Local either: Callback(Either): Either = EitherCb\n"
        "Local stack: Callback(Int64, Int64, Int64, Int64, Int64, Text, Int64): Int64 = StackCb\n"
        "Unsafe\n"
        "    Local a = text(Text(), Null)\n"
        "    Local b = box(1, Box())\n"
        "    Local c = late(Late())\n"
        "End Unsafe\n",
        files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const auto ir = [&](const std::string_view triple) {
        return fb::emit_ir(module, compilation.sources, compilation.file,
                           { .target_triple = std::string(triple) })
            .value_or("");
    };

    // The IR with each value's own name as %v, so a call reads as its types.
    const auto named = [&](const std::string_view triple) {
        return std::regex_replace(ir(triple), std::regex("%(c_struct[0-9]*|[0-9]+)"), "%v");
    };
    const auto thunk = [](const std::string_view name, const std::string_view types) {
        return std::format("@\"fb.callback.{}\"({})", name, types);
    };

    // C's Function gets each as clang's C function does, and a call through a Callback passes
    // each as clang's call through a pointer does, on each target. SysV's Text is two i64s where
    // clang has a ptr and an i64, and AArch64's Vec3 is [3 x float] where clang gives a Struct
    // of three floats and takes an alignstack(8) array: the same registers. Each string was
    // checked against clang's own, from `clang --target=<triple> -O1 -S -emit-llvm` of the same
    // shapes written as C, each Function as a C function and each call through a pointer, with
    // clang's attributes that change no register (noundef, captures, dead_on_return) left out.
    for (const std::string_view triple :
         { "x86_64-unknown-linux-gnu", "x86_64-apple-macosx14.0.0" }) {
        CAPTURE(triple);
        const std::string sysv = named(triple);
        CHECK(sysv.contains("{ i64, i64 } " +
                            thunk("TextCb(Text, Ptr<Void>)", "i64 %v, i64 %v, ptr %v")));
        CHECK(sysv.contains("i64 " + thunk("PointCb(Point, Point)", "i64 %v, i64 %v")));
        CHECK(sysv.contains("{ <2 x float>, float } " +
                            thunk("Vec3Cb(Vec3, Float32)", "<2 x float> %v, float %v, float %v")));
        CHECK(sysv.contains("void " + thunk("BoxCb(Int32, Box)",
                                            "ptr sret(%fb.Box) align 4 %v, i32 %v, ptr "
                                            "byval(%fb.Box) align 8 %v")));
        CHECK(sysv.contains("void " + thunk("LateCb(Late)", "ptr sret(%fb.Late) align 4 %v, ptr "
                                                            "byval(%fb.Late) align 8 %v")));
        CHECK(sysv.contains("i32 " + thunk("EitherCb(Either)", "i32 %v")));
        CHECK(
            sysv.contains("i64 " + thunk("StackCb(Int64, Int64, Int64, Int64, Int64, Text, Int64)",
                                         "i64 %v, i64 %v, i64 %v, i64 %v, i64 %v, ptr "
                                         "byval(%fb.Text) align 8 %v, i64 %v")));
        CHECK(sysv.contains("%v = call { i64, i64 } %v(i64 %v, i64 %v, ptr null)"));
        CHECK(sysv.contains("call void %v(ptr sret(%fb.Box) align 4 %v, i32 1, ptr byval(%fb.Box) "
                            "align 8 %v)"));
        CHECK(sysv.contains("call void %v(ptr sret(%fb.Late) align 4 %v, ptr byval(%fb.Late) "
                            "align 8 %v)"));
    }

    const std::string windows = named("x86_64-w64-windows-gnu");
    CHECK(windows.contains("void " + thunk("TextCb(Text, Ptr<Void>)",
                                           "ptr sret(%fb.Text) align 8 %v, ptr %v, ptr %v")));
    CHECK(windows.contains("i64 " + thunk("PointCb(Point, Point)", "i64 %v, i64 %v")));
    CHECK(windows.contains("void " + thunk("Vec3Cb(Vec3, Float32)",
                                           "ptr sret(%fb.Vec3) align 4 %v, ptr %v, float %v")));
    CHECK(windows.contains(
        "void " + thunk("BoxCb(Int32, Box)", "ptr sret(%fb.Box) align 4 %v, i32 %v, ptr %v")));
    CHECK(
        windows.contains("void " + thunk("LateCb(Late)", "ptr sret(%fb.Late) align 4 %v, ptr %v")));
    CHECK(windows.contains("i32 " + thunk("EitherCb(Either)", "i32 %v")));
    CHECK(
        windows.contains("i64 " + thunk("StackCb(Int64, Int64, Int64, Int64, Int64, Text, Int64)",
                                        "i64 %v, i64 %v, i64 %v, i64 %v, i64 %v, ptr %v, i64 %v")));
    CHECK(windows.contains("call void %v(ptr sret(%fb.Text) align 8 %v, ptr %v, ptr null)"));
    CHECK(windows.contains("call void %v(ptr sret(%fb.Box) align 4 %v, i32 1, ptr %v)"));
    CHECK(windows.contains("call void %v(ptr sret(%fb.Late) align 4 %v, ptr %v)"));

    for (const std::string_view triple :
         { "aarch64-unknown-linux-gnu", "arm64-apple-macosx14.0.0" }) {
        CAPTURE(triple);
        const std::string arm = named(triple);
        CHECK(
            arm.contains("[2 x i64] " + thunk("TextCb(Text, Ptr<Void>)", "[2 x i64] %v, ptr %v")));
        CHECK(arm.contains("i64 " + thunk("PointCb(Point, Point)", "i64 %v, i64 %v")));
        CHECK(arm.contains("[3 x float] " +
                           thunk("Vec3Cb(Vec3, Float32)", "[3 x float] %v, float %v")));
        CHECK(arm.contains(
            "void " + thunk("BoxCb(Int32, Box)", "ptr sret(%fb.Box) align 4 %v, i32 %v, ptr %v")));
        CHECK(arm.contains("[2 x i64] " + thunk("LateCb(Late)", "[2 x i64] %v")));
        CHECK(arm.contains("i32 " + thunk("EitherCb(Either)", "i64 %v")));
        CHECK(arm.contains("i64 " + thunk("StackCb(Int64, Int64, Int64, Int64, Int64, Text, Int64)",
                                          "i64 %v, i64 %v, i64 %v, i64 %v, i64 %v, [2 x i64] %v, "
                                          "i64 %v")));
        CHECK(arm.contains("%v = call [2 x i64] %v([2 x i64] %v, ptr null)"));
        CHECK(arm.contains("call void %v(ptr sret(%fb.Box) align 4 %v, i32 1, ptr %v)"));
        CHECK(arm.contains("%v = call [2 x i64] %v([2 x i64] %v)"));
    }
}

TEST_CASE("an Extern Function's Structs go as the target's C ABI has them", "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "@CLayout Struct Point\n    Field x: Int32\n    Field y: Int32\nEnd Struct\n"
                    "@CLayout Struct Vec3\n    Field x: Float\n    Field y: Float\n"
                    "    Field z: Float\nEnd Struct\n"
                    "@CLayout Struct Span\n    Field start: Int64\n    Field length: Double\n"
                    "End Struct\n"
                    "@CLayout Struct Wide\n    Field a: Int64\n    Field b: Int64\nEnd Struct\n"
                    "@CLayout Struct Box\n    Field min: Point\n    Field max: Point\n"
                    "    Field filled: Bool\nEnd Struct\n"
                    "Extern \"C\"\n"
                    "    Function add(a: Point, b: Point): Point\n"
                    "    Function scale(v: Vec3, by: Float): Vec3\n"
                    "    Function grow(span: Span, by: Int64): Span\n"
                    "    Function make(x: Int32, filled: Bool): Box\n"
                    "    Function area(box: Box): Int32\n"
                    "    Function late(a: Int64, b: Int64, c: Int64, d: Int64, e: Int64, w: Wide,"
                    " f: Int64): Int64\n"
                    "End Extern\n"
                    "Local p = add(Point(1, 2), Point(3, 4))\n"
                    "Local v = scale(Vec3(1.0, 2.0, 3.0), 2.0)\n"
                    "Local s = grow(Span(1, 2.0), 3)\n"
                    "Local b = make(1, True)\n"
                    "Local n = area(b) + late(1, 2, 3, 4, 5, Wide(6, 7), 8)\n",
                    files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const auto ir = [&](const std::string_view triple) {
        return fb::emit_ir(module, compilation.sources, compilation.file,
                           { .target_triple = std::string(triple) })
            .value_or("");
    };

    // As clang declares them, the Structs in registers become their pieces.
    const std::string linux = ir("x86_64-unknown-linux-gnu");
    CHECK(linux.contains("declare i64 @add(i64, i64)"));
    CHECK(linux.contains("declare { <2 x float>, float } @scale(<2 x float>, float, float)"));
    CHECK(linux.contains("declare { i64, double } @grow(i64, double, i64)"));
    CHECK(linux.contains("declare void @make(ptr sret(%fb.Box) align 4, i32, i1 zeroext)"));
    CHECK(linux.contains("declare i32 @area(ptr byval(%fb.Box) align 8)"));
    // Five integers leave one register, and Wide needs two, so it goes in memory, and f takes
    // the last register.
    CHECK(linux.contains(
        "declare i64 @late(i64, i64, i64, i64, i64, ptr byval(%fb.Wide) align 8, i64)"));

    const std::string windows = ir("x86_64-w64-windows-gnu");
    CHECK(windows.contains("declare i64 @add(i64, i64)"));
    CHECK(windows.contains("declare void @scale(ptr sret(%fb.Vec3) align 4, ptr, float)"));
    CHECK(windows.contains("declare void @grow(ptr sret(%fb.Span) align 8, ptr, i64)"));
    CHECK(windows.contains("declare void @make(ptr sret(%fb.Box) align 4, i32, i1 zeroext)"));
    CHECK(windows.contains("declare i32 @area(ptr)"));
    CHECK(windows.contains("declare i64 @late(i64, i64, i64, i64, i64, ptr, i64)"));

    // AArch64 keeps an array's pieces in consecutive registers, Vec3's floats and Span's i64s.
    const std::string arm = ir("aarch64-unknown-linux-gnu");
    CHECK(arm.contains("declare i64 @add(i64, i64)"));
    CHECK(arm.contains("declare [3 x float] @scale([3 x float], float)"));
    CHECK(arm.contains("declare [2 x i64] @grow([2 x i64], i64)"));
    CHECK(arm.contains("declare void @make(ptr sret(%fb.Box) align 4, i32, i1)"));
    CHECK(arm.contains("declare i32 @area(ptr)"));
    CHECK(arm.contains("declare i64 @late(i64, i64, i64, i64, i64, [2 x i64], i64)"));
    // Apple's extends a Bool.
    const std::string apple = ir("arm64-apple-macosx14.0.0");
    CHECK(apple.contains("declare [3 x float] @scale([3 x float], float)"));
    CHECK(apple.contains("declare void @make(ptr sret(%fb.Box) align 4, i32, i1 zeroext)"));
}

TEST_CASE("C's Bools and small integers are extended where the target's C ABI has them",
          "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Extern \"C\"\n"
                    "    Function take(b: Bool, c: Int8, d: UInt16): Bool\n"
                    "    Function each(f: Callback(Bool, Int8): Bool)\n"
                    "End Extern\n"
                    "Function Flip(b: Bool, c: Int8): Bool => !b\n"
                    "Local b = take(True, 1, 2)\n"
                    "each(Flip)\n",
                    files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    // The line of the IR for `triple` that declares or defines `name`.
    const auto line = [&](const std::string_view triple, const std::string_view name) {
        const std::string ir = fb::emit_ir(module, compilation.sources, compilation.file,
                                           { .target_triple = std::string(triple) })
                                   .value_or("");
        const std::size_t at = ir.find(std::string(name) + "(");
        if (at == std::string::npos) {
            return std::string();
        }

        const std::size_t start = ir.rfind('\n', at) + 1;
        return ir.substr(start, ir.find('\n', at) - start);
    };

    // As clang declares it: a Bool everywhere but AArch64 outside Apple's targets, and the
    // integers outside Windows on x86-64, and on Apple's AArch64.
    CHECK(line("x86_64-unknown-linux-gnu", "@take")
              .starts_with("declare zeroext i1 @take(i1 zeroext, i8 signext, i16 zeroext)"));
    CHECK(line("x86_64-w64-windows-gnu", "@take")
              .starts_with("declare zeroext i1 @take(i1 zeroext, i8, i16)"));
    CHECK(line("arm64-apple-macosx14.0.0", "@take")
              .starts_with("declare zeroext i1 @take(i1 zeroext, i8 signext, i16 zeroext)"));
    CHECK(line("aarch64-unknown-linux-gnu", "@take").starts_with("declare i1 @take(i1, i8, i16)"));
    CHECK(line("aarch64-pc-windows-msvc", "@take").starts_with("declare i1 @take(i1, i8, i16)"));

    // A Callback's thunk takes what C passes it as C extends it, or doesn't.
    const std::string_view thunk = "@\"fb.callback.Flip(Bool, Int8)\"";
    CHECK(line("x86_64-unknown-linux-gnu", thunk)
              .starts_with("define internal zeroext i1 @\"fb.callback.Flip(Bool, Int8)\"(i1 "
                           "zeroext %0, i8 signext %1)"));
    CHECK(line("aarch64-unknown-linux-gnu", thunk)
              .starts_with("define internal i1 @\"fb.callback.Flip(Bool, Int8)\"(i1 %0, i8 %1)"));
}

TEST_CASE("a binding using CLong, CULong, and CWChar is right on every target", "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "@CLayout Struct Pair\n    Field a: CLong\n    Field b: CWChar\nEnd Struct\n"
                    "Extern \"C\"\n"
                    "    Function labs(n: CLong): CLong\n"
                    "    Function widen(c: CWChar, u: CULong): CWChar\n"
                    "    Function swapped(p: Pair): Pair\n"
                    "    Function each(f: Callback(CLong): CULong)\n"
                    "End Extern\n"
                    "Function Hash(n: CLong): CULong => n As CULong\n"
                    "Local n: Int64 = labs(-5)\n"
                    "Local c = widen(65, 3)\n"
                    "Local p = swapped(Pair(1, 2))\n"
                    "each(Hash)\n",
                    files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const auto ir = [&](const std::string_view triple) {
        return fb::emit_ir(module, compilation.sources, compilation.file,
                           { .target_triple = std::string(triple) })
            .value_or("");
    };

    // Windows' are 32, 32, and 16 bits, and a Pair of 8 bytes goes as an integer.
    const std::string windows = ir("x86_64-w64-windows-gnu");
    CHECK(windows.contains("declare i32 @labs(i32)"));
    CHECK(windows.contains("declare i16 @widen(i16, i32)"));
    CHECK(windows.contains("declare i64 @swapped(i64)"));
    CHECK(windows.contains("%fb.Pair = type { i32, i16 }"));
    CHECK(windows.contains("define internal i32 @\"fb.callback.Hash(CLong)\"(i32 %0)"));
    // Elsewhere they're 64, 64, and 32, and a Pair of 16 bytes goes in two registers.
    const std::string linux = ir("x86_64-unknown-linux-gnu");
    CHECK(linux.contains("declare i64 @labs(i64)"));
    CHECK(linux.contains("declare i32 @widen(i32, i64)"));
    CHECK(linux.contains("declare { i64, i32 } @swapped(i64, i32)"));
    CHECK(linux.contains("%fb.Pair = type { i64, i32 }"));
    CHECK(linux.contains("define internal i64 @\"fb.callback.Hash(CLong)\"(i64 %0)"));
    const std::string arm = ir("aarch64-unknown-linux-gnu");
    CHECK(arm.contains("declare i64 @labs(i64)"));
    CHECK(arm.contains("declare i32 @widen(i32, i64)"));
    CHECK(arm.contains("declare [2 x i64] @swapped([2 x i64])"));
    // Windows' 16-bit wchar_t is extended on x86-64 only outside Windows, so nowhere.
    CHECK(ir("arm64-apple-macosx14.0.0").contains("declare i32 @widen(i32, i64)"));
}

TEST_CASE("the runtime's Extern Functions extend small integers as C does", "[c_abi]") {
    // The runtime is C code built for the target, like std.ffi's Mem.Fill's fb_mem_fill.
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation = fb::compile("main.fb",
                                                      "Import std.ffi\n"
                                                      "Unsafe\n"
                                                      "    Local memory = Mem.Alloc(16)\n"
                                                      "    Mem.Fill(memory, 200, 16)\n"
                                                      "    Mem.Free(memory)\n"
                                                      "End Unsafe\n",
                                                      files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const auto ir = [&](const std::string_view triple) {
        return fb::emit_ir(module, compilation.sources, compilation.file,
                           { .target_triple = std::string(triple) })
            .value_or("");
    };

    const std::string_view extended = "declare void @fb_mem_fill(ptr, i8 zeroext, i64)";
    CHECK(ir("x86_64-unknown-linux-gnu").contains(extended));
    CHECK(ir("arm64-apple-macosx14.0.0").contains(extended));
    CHECK(ir("x86_64-w64-windows-gnu").contains("declare void @fb_mem_fill(ptr, i8, i64)"));
}

TEST_CASE("an Extern Function keeps its symbol, whatever codegen calls its own constants",
          "[c_abi]") {
    // A String's literal, a panic's message and place, and the Classes' list are "fb." names,
    // which an Extern's symbol can't be.
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Extern \"C\"\n"
                    "    Function string(): Int32\n"
                    "    Function text(): Int32\n"
                    "    Function location(): Int32\n"
                    "    Function classes(): Int32\n"
                    "End Extern\n"
                    "Class Box\n"
                    "End Class\n"
                    "Global greeting = \"hello\"\n"
                    "Local box = Box()\n"
                    "Print(greeting)\n"
                    "Print(\"{string() + text() + location() + classes()}\")\n",
                    files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::string ir =
        fb::emit_ir(module, compilation.sources, compilation.file, {}).value_or("");
    REQUIRE_FALSE(ir.empty());
    for (const std::string_view name : { "string", "text", "location", "classes" }) {
        CHECK(ir.contains(std::format("declare i32 @{}()", name)));
        CHECK_FALSE(ir.contains(std::format("@{}.1", name)));
    }
}

// clang marks each argument that's a homogeneous float aggregate alignstack(8) on AArch64 Linux,
// which places nothing differently: clang's own IR with and without it compiles to the same
// stack stores, once the vector registers are used up and with small integers beside it on the
// stack too, and Apple's has none. fb's is as it is.
TEST_CASE("an AArch64 call of float aggregates past the registers is as clang's places it",
          "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation = fb::compile(
        "main.fb",
        "@CLayout Struct V2\n    Field x: Float32\n    Field y: Float32\nEnd Struct\n"
        "@CLayout Struct V1\n    Field x: Float32\nEnd Struct\n"
        "Extern \"C\"\n"
        "    Function take(a: V2, b: V2, c: V2, d: V2, e: V2, f: V2, g: V2, h: V2, i: V2, j: V1,"
        " k: Int32)\n"
        "End Extern\n"
        "Local v = V2(1.0, 2.0)\n"
        "take(v, v, v, v, v, v, v, v, v, V1(3.0), 4)\n",
        files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    for (const std::string_view triple :
         { "aarch64-unknown-linux-gnu", "arm64-apple-macosx14.0.0" }) {
        const std::string ir = fb::emit_ir(module, compilation.sources, compilation.file,
                                           { .target_triple = std::string(triple) })
                                   .value_or("");
        INFO(triple);
        CHECK(ir.contains("declare void @take([2 x float], [2 x float], [2 x float], [2 x float], "
                          "[2 x float], [2 x float], [2 x float], [2 x float], [2 x float], "
                          "[1 x float], i32)"));
    }
}

namespace {

/// The body of `function`, by its name in the IR, like "fb.Read(ByRef Tight)".
std::string function_body(const std::string& ir, const std::string_view function) {
    const std::size_t start = ir.find(std::format("@\"{}\"(", function));
    REQUIRE(start != std::string::npos);
    return ir.substr(start, ir.find("\n}\n", start) - start);
}

} // namespace

TEST_CASE("a packed Struct's Fields are read and written at the alignment they have", "[c_abi]") {
    const std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    const fb::compilation_t compilation = fb::compile(
        "main.fb",
        "Import std.core\n"
        "Enum Level\n    LOW = 1\nEnd Enum\n"
        "@CLayout(Pack: 1) Struct Tight\n    Field a: UInt8\n    Field b: Int64\nEnd Struct\n"
        "@CLayout(Pack: 4) Struct Late\n    Field a: Int32\n    Field b: Int64\nEnd Struct\n"
        "@CLayout Struct Plain\n    Field a: Int32\n    Field b: Int64\nEnd Struct\n"
        "@CLayout(Pack: 1) Struct Holds\n"
        "    Field a: UInt8\n"
        "    Field plain: Plain\n"
        "    Field many: Late[2]\n"
        "    Field ids: Int32[2]\n"
        "End Struct\n"
        "@CLayout(Pack: 1) Struct Zeroed\n"
        "    Field a: UInt8\n"
        "    Field level: Level\n"
        "    Field ids: Int32[2]\n"
        "End Struct\n"
        "Function Tight1(ByRef t: Tight)\n    t.b = t.b + 1\nEnd Function\n"
        "Function Late1(ByRef l: Late)\n    l.b = l.b + 1\nEnd Function\n"
        "Function Plain1(ByRef p: Plain)\n    p.b = p.b + 1\nEnd Function\n"
        "Function Nested(ByRef h: Holds)\n    h.plain.b = h.plain.b + 1\nEnd Function\n"
        "Function Element(ByRef h: Holds)\n    h.many[1].b = h.many[1].b + 1\nEnd Function\n"
        "Function Copied(ByRef h: Holds, ByRef g: Holds)\n    h.ids = g.ids\nEnd Function\n"
        "Function Made(): Zeroed Throws Error\n    Throw Error(\"no\")\nEnd Function\n"
        "Local t = Tight(1, 2)\nTight1(ByRef t)\n"
        "Local l = Late(1, 2)\nLate1(ByRef l)\n"
        "Local p = Plain(1, 2)\nPlain1(ByRef p)\n"
        "Local h = Holds()\nLocal g = Holds()\nNested(ByRef h)\nElement(ByRef h)\n"
        "Copied(ByRef h, ByRef g)\n"
        "Local z = Try? Made()\n",
        files);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::string ir = fb::emit_ir(module, compilation.sources, compilation.file,
                                       { .target_triple = "x86_64-unknown-linux-gnu" })
                               .value_or("");
    const auto loads_and_stores = [&](const std::string_view function, const int align) {
        const std::string body = function_body(ir, function);
        INFO(body);
        CHECK(std::regex_search(
            body, std::regex(std::format(R"(= load i64, ptr %[0-9a-z.]+, align {}\b)", align))));
        CHECK(std::regex_search(
            body, std::regex(std::format(R"(store i64 %[0-9a-z.]+, ptr %[0-9a-z.]+, align {}\b)",
                                         align))));
    };
    // A Field at offset 1 and one at 4, a plain Struct's inside a packed one, and an element's
    // inside one, at their real alignment; a plain Struct's as it was.
    loads_and_stores("fb.Tight1(ByRef Tight)", 1);
    loads_and_stores("fb.Late1(ByRef Late)", 4);
    loads_and_stores("fb.Plain1(ByRef Plain)", 8);
    loads_and_stores("fb.Nested(ByRef Holds)", 1);
    loads_and_stores("fb.Element(ByRef Holds)", 1);
    // A fixed array's copy, and the zero of one in a packed Struct whose zero isn't all zero
    // bytes, are at the Field's.
    CHECK(function_body(ir, "fb.Copied(ByRef Holds, ByRef Holds)")
              .contains("@llvm.memmove.p0.p0.i64(ptr align 1 %"));
    CHECK(std::regex_search(function_body(ir, "fb.Made()"),
                            std::regex(R"(@llvm.memset.p0.i64\(ptr align 1 %[0-9a-z.]+, i8 0, )"
                                       R"(i64 ptrtoint \(ptr getelementptr \(\[2 x i32\])")));
}
