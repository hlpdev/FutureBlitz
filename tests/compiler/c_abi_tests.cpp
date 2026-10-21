#include "fb/c_abi.hpp"
#include "fb/codegen.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <string>
#include <string_view>
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

constexpr fb::c_abi_t win64 = fb::c_abi_t::WIN64;
constexpr fb::c_abi_t sysv = fb::c_abi_t::SYSV;

} // namespace

TEST_CASE("a target's triple says its C ABI", "[c_abi]") {
    CHECK(fb::c_abi_of("x86_64-w64-windows-gnu") == win64);
    CHECK(fb::c_abi_of("x86_64-pc-windows-msvc") == win64);
    CHECK(fb::c_abi_of("x86_64-unknown-linux-gnu") == sysv);
    CHECK(fb::c_abi_of("x86_64-apple-macosx14.0.0") == sysv);
    CHECK(fb::c_abi_of("aarch64-apple-macosx14.0.0") == fb::c_abi_t::OTHER);
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

TEST_CASE("a Callback in a Struct takes 8 bytes, as C's function pointer does", "[c_abi]") {
    fb::type_table_t types;
    const fb::type_id_t callback = types.callback({ fb::type_id_t::INT32 }, fb::type_id_t::INT32);
    const fb::type_id_t holder = types.add_struct("Holder");
    types.set_fields(holder, {
                                 { .name = "f", .type = callback },
                                 { .name = "value", .type = fb::type_id_t::INT32 },
                             });
    const c_layout_t found = fb::c_layout(types, holder);
    CHECK(found.size == 16);
    CHECK(argument(found, win64).pass == c_pass_t::INDIRECT);
    CHECK(argument(found, sysv) == direct({
                                       { .part = c_part_t::INTEGER, .size = 8 },
                                       { .part = c_part_t::INTEGER, .size = 4 },
                                   }));
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
}
