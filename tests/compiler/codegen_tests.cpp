#include "fb/codegen.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// What codegen makes of a program's MIR: the LLVM IR, its debug info, and what a release build's
// optimizer does to it.

namespace {

/// The LLVM IR of `text`, with the standard library, in a build of `mode` for `triple`.
std::string ir_of(const std::string_view text, const fb::build_mode_t mode,
                  const std::string_view triple) {
    const fb::compilation_t compilation =
        fb::compile("main.fb", std::string(text),
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}), mode);
    INFO(text);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    return fb::emit_ir(module, compilation.sources, compilation.file,
                       { .mode = mode, .target_triple = std::string(triple) })
        .value_or("");
}

} // namespace

TEST_CASE("a build leaves out the code only tests reach, which a test build has", "[codegen]") {
    const std::string_view text = "Import std.test\n"
                                  "Function OnlyTested(): Int => 41\n"
                                  "Function Used(): Int => 1\n"
                                  "Local n = Used()\n"
                                  "@Test Function Checks()\n"
                                  "    Expect(OnlyTested() + Used() == 42)\n"
                                  "End Function\n";
    for (const fb::build_mode_t mode : { fb::build_mode_t::DEBUG, fb::build_mode_t::RELEASE }) {
        const fb::compilation_t compilation =
            fb::compile("main.fb", std::string(text),
                        fb::standard_library().value_or(std::vector<fb::library_file_t>{}), mode);
        REQUIRE_FALSE(compilation.diagnostics.has_errors());
        const std::string built =
            fb::emit_ir(fb::lower(compilation.ast, compilation.analysis), compilation.sources,
                        compilation.file, { .mode = mode })
                .value_or("");
        REQUIRE_FALSE(built.empty());
        CHECK_FALSE(built.contains("OnlyTested"));
        CHECK_FALSE(built.contains("Checks"));
        CHECK_FALSE(built.contains("fb_test_"));

        // Debug builds keep what main calls, and release builds inline it.
        const std::string tested =
            fb::emit_ir(fb::lower_tests(compilation.ast, compilation.analysis,
                                        fb::package_tests(compilation)),
                        compilation.sources, compilation.file, { .mode = mode })
                .value_or("");
        REQUIRE_FALSE(tested.empty());
        CHECK(tested.contains("fb_test_run"));
        if (mode == fb::build_mode_t::DEBUG) {
            CHECK(tested.contains("OnlyTested"));
        }
    }
}

TEST_CASE("release builds turn a Vec4's and a Mat4's Operators into SIMD instructions",
          "[codegen]") {
    // Where the results go to memory, in a loop or not.
    const std::string_view looped = "Import std.core\n"
                                    "Import std.math\n"
                                    "Function AddAll(a: Vec4[], b: Vec4[], out: Vec4[])\n"
                                    "    For i = 0 Until a.Count\n"
                                    "        out[i] = a[i] + b[i]\n"
                                    "    End For\n"
                                    "End Function\n"
                                    "Function MultiplyAll(a: Mat4[], b: Mat4[], out: Mat4[])\n"
                                    "    For i = 0 Until a.Count\n"
                                    "        out[i] = a[i] * b[i]\n"
                                    "    End For\n"
                                    "End Function\n"
                                    "Local vectors = [Vec4(1, 2, 3, 4)]\n"
                                    "AddAll(vectors, vectors, vectors)\n"
                                    "Local matrices = [Mat4.Identity]\n"
                                    "MultiplyAll(matrices, matrices, matrices)\n";
    const std::string_view once = "Import std.core\n"
                                  "Import std.math\n"
                                  "Class Body\n"
                                  "    Field Position: Vec4\n"
                                  "    Field Velocity: Vec4\n"
                                  "    Field Spin: Mat4\n"
                                  "End Class\n"
                                  "Function Step(body: Body, time: Float)\n"
                                  "    body.Position = body.Position + body.Velocity * time\n"
                                  "    body.Spin = body.Spin * body.Spin\n"
                                  "End Function\n"
                                  "Local body = Body()\n"
                                  "body.Spin = Mat4.Identity\n"
                                  "Step(body, 0.5)\n";
    for (const std::string_view program : { looped, once }) {
        for (const std::string_view triple :
             { "x86_64-unknown-linux-gnu", "aarch64-apple-darwin" }) {
            INFO(triple);
            const std::string release = ir_of(program, fb::build_mode_t::RELEASE, triple);
            REQUIRE_FALSE(release.empty());
            CHECK(release.contains("fadd <4 x float>"));
            CHECK(release.contains("fmul <4 x float>"));
            // A debug build leaves its code as written.
            CHECK_FALSE(ir_of(program, fb::build_mode_t::DEBUG, triple).contains("<4 x float>"));
        }
    }
}

TEST_CASE("debug builds describe functions, lines, variables, and types, and release builds lines",
          "[codegen]") {
    const std::string_view program =
        "Import std.core\n"
        "Struct Point\n"
        "    Field X: Int32\n"
        "    Field Y: Int32\n"
        "End Struct\n"
        "Class Node\n"
        "    Field Value: String\n"
        "    Field Next: Node?\n"
        "End Class\n"
        "Enum Color\n"
        "    RED, GREEN\n"
        "End Enum\n"
        "Function Sum(values: Int32[], start: Point, ByRef total: Int): Int\n"
        "    Local color = Color.GREEN\n"
        "    Local maybe: Int32? = 3\n"
        "    total += values[0] + start.X\n"
        "    Return total\n"
        "End Function\n"
        "Local node = Node()\n"
        "node.Value = \"a\"\n"
        "Local total = 0\n"
        "Print(\"{Sum([1, 2], Point(), ByRef total)}\")\n";
    for (const std::string_view triple :
         { "x86_64-unknown-linux-gnu", "aarch64-apple-darwin", "x86_64-w64-windows-gnu" }) {
        INFO(triple);
        const std::string debug = ir_of(program, fb::build_mode_t::DEBUG, triple);
        REQUIRE_FALSE(debug.empty());
        CHECK(debug.contains("!\"Dwarf Version\", i32 4}"));
        CHECK(debug.contains("!\"CodeView\", i32 1}") ==
              std::string_view(triple).contains("windows"));
        // A compile unit for each file a function is in, std's too.
        CHECK(debug.contains("!DIFile(filename: \"main.fb\""));
        CHECK(debug.contains("!DIFile(filename: \"std/core/print.fb\""));
        CHECK(debug.contains("emissionKind: FullDebug"));
        // Functions by their names, at their lines, and their code's lines.
        CHECK(debug.contains("!DISubprogram(name: \"Sum\", scope: "));
        CHECK(debug.contains("line: 13, type: "));
        CHECK(debug.contains("!DISubprogram(name: \"top-level code\""));
        CHECK(debug.contains("!DILocation(line: 16, column: "));
        // Parameters and Locals, and their types.
        CHECK(debug.contains("!DILocalVariable(name: \"values\", arg: 1"));
        CHECK(debug.contains("!DILocalVariable(name: \"total\", arg: 3"));
        CHECK(debug.contains("!DILocalVariable(name: \"color\""));
        CHECK(debug.contains("!DILocalVariable(name: \"node\""));
        CHECK(debug.contains("DW_TAG_structure_type, name: \"Point\""));
        CHECK(debug.contains("DW_TAG_enumeration_type, name: \"Color\""));
        CHECK(debug.contains("!DIEnumerator(name: \"GREEN\", value: 1"));
        CHECK(debug.contains("DW_TAG_typedef, name: \"Node\""));
        CHECK(debug.contains("DW_TAG_structure_type, name: \"fb.main.Node\""));
        CHECK(debug.contains("DW_TAG_structure_type, name: \"Node?\""));
        CHECK(debug.contains("DW_TAG_typedef, name: \"Int32[]\""));
        CHECK(debug.contains("DW_TAG_typedef, name: \"String\""));
        CHECK(debug.contains("DW_TAG_structure_type, name: \"Int32?\""));
        CHECK(debug.contains("!DIDerivedType(tag: DW_TAG_member, name: \"HasValue\""));
        // A ByRef parameter's variable is where its slot says the caller's is.
        CHECK(debug.contains("!DIExpression(DW_OP_deref)"));

        const std::string release = ir_of(program, fb::build_mode_t::RELEASE, triple);
        REQUIRE_FALSE(release.empty());
        CHECK(release.contains("emissionKind: LineTablesOnly"));
        CHECK(release.contains("!DISubprogram(name: \"Sum\""));
        CHECK_FALSE(release.contains("!DILocalVariable("));
    }
}

TEST_CASE("each target's object files have the stack trace table's markers", "[codegen]") {
    // The markers are inline assembly, in each object format's section: what this machine can't
    // link and run, it still assembles.
    const std::string_view program = "Import std.core\n"
                                     "Function Check(n: Int)\n"
                                     "    Assert(n > 0)\n"
                                     "End Function\n"
                                     "Check(1)\n";
    const fb::compilation_t compilation =
        fb::compile("main.fb", std::string(program),
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "trace_objects";
    std::filesystem::create_directories(folder);
    // The table is added as the binary loads, by a function in the section the C runtime runs:
    // on ELF .init_array, as lld keeps a .ctors apart, and nothing runs it.
    for (const auto& [triple, section, runs] :
         std::initializer_list<std::tuple<std::string, std::string, std::string>>{
             { "x86_64-unknown-linux-gnu", "fb_trace", ".init_array" },
             { "aarch64-unknown-linux-gnu", "fb_trace", ".init_array" },
             { "arm64-apple-macosx13.0.0", "__fb_trace", "__mod_init_func" },
             { "x86_64-apple-macosx13.0.0", "__fb_trace", "__mod_init_func" },
             { "x86_64-w64-windows-gnu", ".fbtrace$m", ".ctors" },
         }) {
        for (const fb::build_mode_t mode : { fb::build_mode_t::DEBUG, fb::build_mode_t::RELEASE }) {
            INFO(triple);
            const std::filesystem::path object = folder / std::format("{}.o", triple);
            const std::expected<void, std::string> made =
                fb::emit_object(module, compilation.sources, compilation.file,
                                { .mode = mode, .target_triple = triple }, object);
            INFO((made ? std::string() : made.error()));
            REQUIRE(made);
            std::ifstream file(object, std::ios::binary);
            const std::string bytes{ std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>() };
            CHECK(bytes.contains(section));
            CHECK(bytes.contains(runs));
            if (runs == ".init_array") {
                CHECK_FALSE(bytes.contains(".ctors"));
            }
        }
    }
}
