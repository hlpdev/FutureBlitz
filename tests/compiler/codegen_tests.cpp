#include "fb/codegen.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

// What codegen makes of a program's MIR: the LLVM IR, and what a release build's optimizer does
// to it.

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
