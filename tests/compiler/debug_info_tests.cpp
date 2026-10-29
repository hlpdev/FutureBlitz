#include "fb/codegen.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The debug info a debugger reads, in a debug build's LLVM IR: where a line's code is, which
// scope a Local is in, and what each variable and Class is described as.

namespace {

/// The LLVM IR of main.fb's `text`, with the standard library and `files`, in a debug build,
/// with `options` changed by `change`.
std::string
debug_ir(const std::string_view text, std::vector<fb::library_file_t> files = {},
         const std::function<void(const fb::compilation_t&, fb::codegen_options_t&)>& change = {}) {
    std::vector<fb::library_file_t> all =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    all.insert(all.end(), files.begin(), files.end());
    const fb::compilation_t compilation =
        fb::compile("main.fb", std::string(text), std::move(all), fb::build_mode_t::DEBUG);
    INFO(text);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    fb::codegen_options_t options{
        .mode = fb::build_mode_t::DEBUG,
        .target_triple = "x86_64-unknown-linux-gnu",
    };
    if (change) {
        change(compilation, options);
    }

    return fb::emit_ir(module, compilation.sources, compilation.file, options).value_or("");
}

/// The body of the function whose LLVM name is `name`, its blocks and their code.
std::string body_of(const std::string& ir, const std::string_view name) {
    const std::size_t start = ir.find(std::format("@\"{}\"(", name));
    REQUIRE(start != std::string::npos);
    const std::size_t end = ir.find("\n}\n", start);
    return ir.substr(start, end - start);
}

/// The line of each DILocation in `ir`, by its metadata number.
std::map<std::string, int> location_lines(const std::string& ir) {
    std::map<std::string, int> lines;
    const std::regex location(R"(\n(![0-9]+) = !DILocation\(line: ([0-9]+))");
    for (auto at = std::sregex_iterator(ir.begin(), ir.end(), location);
         at != std::sregex_iterator(); ++at) {
        lines[at->str(1)] = std::stoi(at->str(2));
    }

    return lines;
}

/// The lines of a function's blocks that are only a jump, as an If's join is.
std::vector<int> lone_jump_lines(const std::string& ir, const std::string_view function) {
    const std::map<std::string, int> lines = location_lines(ir);
    const std::string body = body_of(ir, function);
    const std::regex jump(
        R"(\n[A-Za-z0-9_.]+:[^\n]*\n  br label %[A-Za-z0-9_.]+, !dbg (![0-9]+)\n\n)");
    std::vector<int> found;
    for (auto at = std::sregex_iterator(body.begin(), body.end(), jump);
         at != std::sregex_iterator(); ++at) {
        const auto known = lines.find(at->str(1));
        found.push_back(known != lines.end() ? known->second : -1);
    }

    return found;
}

/// The scopes of the variables named `name`, by their metadata numbers.
std::set<std::string> scopes_of(const std::string& ir, const std::string_view name) {
    std::set<std::string> scopes;
    const std::regex variable(
        std::format(R"(!DILocalVariable\(name: "{}",(?: arg: [0-9]+,)? scope: (![0-9]+))", name));
    for (auto at = std::sregex_iterator(ir.begin(), ir.end(), variable);
         at != std::sregex_iterator(); ++at) {
        scopes.insert(at->str(1));
    }

    return scopes;
}

} // namespace

TEST_CASE("a line's code is in the order it's written, so a breakpoint at its start stops",
          "[codegen][debug]") {
    // A debugger breaks at a line's lowest address. A check's panic and the code after it come
    // right after the check, not after the blocks of the code further down.
    const std::string ir = debug_ir("Import std.core\n"
                                    "Function Check(n: Int)\n"
                                    "    Local total = n + 1\n"
                                    "    If total > 5 Then Print(\"big\")\n"
                                    "    Print(\"total {total}\")\n"
                                    "End Function\n"
                                    "Check(1)\n");
    const std::string body = body_of(ir, "fb.Check(Int32)");
    INFO(body);
    REQUIRE(body.contains("\nok:"));
    REQUIRE(body.contains("\nbb1:"));
    CHECK(body.find("\npanic:") < body.find("\nbb1:"));
    CHECK(body.find("\nok:") < body.find("\nbb1:"));
}

TEST_CASE("a Local is in the lexical block of the scope it's declared in", "[codegen][debug]") {
    // Two Fors' i and x, and two Catches' e, are each their own, which a debugger shows only in
    // their code.
    const std::string ir = debug_ir("Import std.core\n"
                                    "Function Fail(n: Int) Throws IOError, ParseError\n"
                                    "    If n > 2 Then Throw IOError(IOErrorKind.OTHER, \"io\")\n"
                                    "    Throw ParseError(ParseErrorKind.OTHER, \"parse\")\n"
                                    "End Function\n"
                                    "Function Sum(n: Int): Int\n"
                                    "    Local total = 0\n"
                                    "    For i = 1 To n\n"
                                    "        Local x = i * 2\n"
                                    "        total += x\n"
                                    "    End For\n"
                                    "    For i = 1 To n\n"
                                    "        Local x = i * 3\n"
                                    "        total += x\n"
                                    "    End For\n"
                                    "    Try\n"
                                    "        Fail(n)\n"
                                    "    Catch e As IOError\n"
                                    "        total += 1\n"
                                    "    Catch e As ParseError\n"
                                    "        total += e.Line\n"
                                    "    End Try\n"
                                    "    Return total\n"
                                    "End Function\n"
                                    "Print(\"{Sum(3)}\")\n");
    for (const std::string_view name : { "i", "x", "e" }) {
        INFO(name);
        const std::set<std::string> scopes = scopes_of(ir, name);
        CHECK(scopes.size() == 2);
        for (const std::string& scope : scopes) {
            CHECK(ir.contains(std::format("\n{} = distinct !DILexicalBlock(", scope)));
        }
    }

    // The function's own Local is in its subprogram.
    for (const std::string& scope : scopes_of(ir, "total")) {
        CHECK(ir.contains(std::format("\n{} = distinct !DISubprogram(name: \"Sum\"", scope)));
    }
}

TEST_CASE("code with no place at a block's start is at line 0, not the last block's line",
          "[codegen][debug]") {
    // The If's join, which the Then's code reaches, had the Else's line, so stepping showed it.
    const std::string ir = debug_ir("Import std.core\n"
                                    "Function Work(n: Int): Int\n"
                                    "    Local total = 0\n"
                                    "    For i = 1 To n\n"
                                    "        If i % 2 == 0\n"
                                    "            total += i\n"
                                    "        Else\n"
                                    "            total -= 1\n"
                                    "        End If\n"
                                    "    End For\n"
                                    "    Return total\n"
                                    "End Function\n"
                                    "Print(\"{Work(3)}\")\n");
    const std::vector<int> joins = lone_jump_lines(ir, "fb.Work(Int32)");
    REQUIRE_FALSE(joins.empty());
    for (const int line : joins) {
        CHECK(line != 8);
    }
}

TEST_CASE("a library's file is named by its path in the program, from the folder it's in",
          "[codegen][debug]") {
    // A library's build names its modules by their paths in the program, which a debugger finds
    // from the folder that path is from where the file is.
    const std::string ir = debug_ir(
        "Import std.core\nImport \"./lib/tools\"\nPrint(\"{Triple(2)}\")\n",
        { { .path = "lib/tools.fb", .text = "Public Function Triple(n: Int): Int => n * 3\n" } },
        [](const fb::compilation_t& compilation, fb::codegen_options_t& options) {
            options.folder = "/work/game";
            for (std::size_t i = 0; i < compilation.sources.size(); ++i) {
                const auto id = static_cast<fb::file_id_t>(i);
                if (compilation.sources.get(id).path() == "lib/tools.fb") {
                    options.paths[id] = "tools.fb";
                }
            }
        });
    CHECK(ir.contains("!DIFile(filename: \"tools.fb\", directory: \"/work/game/lib\")"));
    CHECK(ir.contains("!DIFile(filename: \"main.fb\", directory: \"/work/game\")"));
}

TEST_CASE("a Local a lambda captures is its value, in its Cell", "[codegen][debug]") {
    const std::string ir = debug_ir("Import std.core\n"
                                    "Local limit = 10\n"
                                    "Local scaled = Function(x: Int) => x * limit\n"
                                    "limit = 100\n"
                                    "Print(\"{scaled(2)}\")\n");
    const std::regex limit(
        R"re(!DILocalVariable\(name: "limit", scope: ![0-9]+, file: ![0-9]+, line: [0-9]+, type: (![0-9]+)\))re");
    std::smatch found;
    REQUIRE(std::regex_search(ir, found, limit));
    CHECK(ir.contains(std::format("\n{} = !DIBasicType(name: \"Int32\"", found[1].str())));
    CHECK(ir.contains("!DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16)"));
}

TEST_CASE("an Async Function's variables last through its Awaits, by its own name",
          "[codegen][debug]") {
    const std::string ir = debug_ir("Import std.core\n"
                                    "Async Function Sum(n: Int): Int\n"
                                    "    Return Await Spawn(Function() => n + 1)\n"
                                    "End Function\n"
                                    "Async Function Describe(name: String, count: Int): String\n"
                                    "    Local parts = 0\n"
                                    "    For i = 1 To count\n"
                                    "        parts = parts + Await Sum(i)\n"
                                    "    End For\n"
                                    "    Return \"{name}:{parts}\"\n"
                                    "End Function\n"
                                    "Print(Await Describe(\"d\", 2))\n");
    // The piece that resumes it has `count`, which the frame keeps.
    const std::regex resume(R"(@"fb\.Describe\(String, Int32\)\.resume"\([^\n]*!dbg (![0-9]+) \{)");
    std::smatch piece;
    REQUIRE(std::regex_search(ir, piece, resume));
    const std::regex count(std::format(
        R"(\n(![0-9]+) = !DILocalVariable\(name: "count", arg: 3, scope: {},)", piece[1].str()));
    std::smatch variable;
    REQUIRE(std::regex_search(ir, variable, count));
    CHECK(body_of(ir, "fb.Describe(String, Int32).resume")
              .contains(std::format("#dbg_declare(ptr %.debug, {},", variable[1].str())));
    // A debugger shows the pieces by the Function's name, which a linkage name would hide.
    CHECK_FALSE(ir.contains("linkageName: \"fb.Describe(String, Int32)"));
}

TEST_CASE("each Class has its struct, by its full name, though no variable has its type",
          "[codegen][debug]") {
    const std::string ir =
        debug_ir("Import std.core\n"
                 "Import \"./lib/a\" As A\n"
                 "Import \"./lib/b\" As B\n"
                 "Class Shape\n"
                 "    Field Name: String\n"
                 "End Class\n"
                 "Class Circle Extends Shape\n"
                 "    Field Radius: Float64\n"
                 "End Class\n"
                 "Local shape: Shape = Circle()\n"
                 "Local x = A.Make()\n"
                 "Local y = B.Make()\n"
                 "Print(shape.Name)\n",
                 {
                     {
                         .path = "lib/a.fb",
                         .text = "Public Class Cat\n    Public Field Lives: Int32\nEnd Class\n"
                                 "Public Function Make(): Cat => Cat()\n",
                     },
                     {
                         .path = "lib/b.fb",
                         .text = "Public Class Cat\n    Public Field Name: String\nEnd Class\n"
                                 "Public Function Make(): Cat => Cat()\n",
                     },
                 });
    // The script finds an object's own Class's struct by the full name its fb_class_t has.
    CHECK(ir.contains("DW_TAG_structure_type, name: \"fb.main.Circle\""));
    CHECK(ir.contains("DW_TAG_structure_type, name: \"fb.lib.a.Cat\""));
    CHECK(ir.contains("DW_TAG_structure_type, name: \"fb.lib.b.Cat\""));
    CHECK(ir.contains("c\"main.Circle\\00\""));
}
