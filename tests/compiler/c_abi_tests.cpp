#include "fb/c_abi.hpp"
#include "fb/codegen.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <format>
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
    CHECK(argument(fb::c_layout(types, held), aapcs64) == three);
    const fb::type_id_t quad = types.add_struct("Quad");
    types.set_fields(quad,
                     {
                         { .name = "v", .type = types.fixed_array(fb::type_id_t::FLOAT64, { 4 }) },
                     });
    CHECK(result(fb::c_layout(types, quad), aapcs64) ==
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
