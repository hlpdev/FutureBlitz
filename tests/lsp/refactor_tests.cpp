#include "client.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/lower.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/lsp/refactor.hpp"
#include "fb/process.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// M11's step 7, the refactorings: each made on std's own code, which a program that uses it then
// builds with, printing what it printed before.

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

/// A program in memory: its main file's text, and the modules it builds with, std's among them.
struct program_t {
    std::string main;
    std::vector<fb::library_file_t> library;
};

std::string read(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// tests/programs' `name`, with std.
program_t test_program(const std::string_view name) {
    return {
        .main = read(std::filesystem::path(FB_TEST_PACKAGES).parent_path() / "programs" / name),
        .library = fb::standard_library().value_or(std::vector<fb::library_file_t>{}),
    };
}

/// A program of its own text, with std.
program_t own_program(std::string text) {
    return {
        .main = std::move(text),
        .library = fb::standard_library().value_or(std::vector<fb::library_file_t>{}),
    };
}

fb::compilation_t compiled(const program_t& program) {
    return fb::compile("main.fb", program.main, program.library);
}

/// The file of the check at `path`: "main.fb", or a module's, like "std/text/diff.fb".
fb::file_id_t file_at(const fb::compilation_t& compilation, const std::string_view path) {
    for (const fb::module_t& module : compilation.ast.modules().modules()) {
        if (compilation.sources.get(module.file).path() == path) {
            return module.file;
        }
    }

    FAIL("no file " << path);
    return {};
}

/// Where `text` is in the file, its `nth` time from 0, or, with `inside`, where that is in it.
fb::span_t selected(const fb::compilation_t& compilation, const fb::file_id_t file,
                    const std::string_view text, const std::string_view inside = {},
                    const std::size_t nth = 0) {
    const std::string_view source = compilation.sources.get(file).text();
    std::size_t at = source.find(text);
    for (std::size_t i = 0; i < nth && at != std::string_view::npos; ++i) {
        at = source.find(text, at + 1);
    }

    REQUIRE(at != std::string_view::npos);
    std::size_t length = text.size();
    if (!inside.empty()) {
        const std::size_t offset = text.find(inside);
        REQUIRE(offset != std::string_view::npos);
        at += offset;
        length = inside.size();
    }

    return {
        .file = file,
        .begin = static_cast<std::uint32_t>(at),
        .end = static_cast<std::uint32_t>(at + length),
    };
}

/// The program with `edits` made, each in its file's text.
program_t applied(const fb::compilation_t& compilation, program_t program,
                  std::vector<fb::fix_t> edits) {
    std::ranges::sort(edits, std::greater{}, [](const fb::fix_t& fix) { return fix.span.begin; });
    for (const fb::fix_t& fix : edits) {
        const std::string path = compilation.sources.get(fix.span.file).path();
        std::string* text = &program.main;
        if (path != "main.fb") {
            const auto found = std::ranges::find(program.library, path, &fb::library_file_t::path);
            REQUIRE(found != program.library.end());
            text = &found->text;
        }

        text->replace(fix.span.begin, fix.span.length(), fix.replacement);
    }

    return program;
}

/// What the program prints, built and run as `name`, which it has to build without errors.
std::string printed(const program_t& program, const std::string_view name) {
    const fb::compilation_t compilation = compiled(program);
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        INFO(compilation.sources.get(diagnostic.span.file).path()
             << ": " << compilation.sources.get(diagnostic.span.file).slice(diagnostic.span) << ": "
             << diagnostic.message);
        CHECK(diagnostic.severity != fb::severity_t::ERROR);
    }

    REQUIRE(!compilation.diagnostics.has_errors());
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "fb-refactor-tests";
    std::filesystem::create_directories(folder);
    std::filesystem::path program_path = folder / name;
    program_path += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::expected<fb::process_result_t, std::string> ran =
        fb::build_program(module, compilation, program_path, fb::build_mode_t::DEBUG).and_then([&] {
            return fb::run_process(program_path, {});
        });
    INFO((ran ? std::string() : ran.error()));
    REQUIRE(ran);
    return ran->output;
}

/// The refactoring titled `title` that `selection` offers.
fb::lsp::refactoring_t offered(const fb::compilation_t& compilation, const fb::span_t selection,
                               const std::string_view title) {
    const fb::lsp::query_t query(compilation, selection.file);
    for (const fb::lsp::refactoring_t& refactoring : fb::lsp::refactorings(query, selection)) {
        if (refactoring.title == title) {
            return refactoring;
        }
    }

    FAIL("no refactoring '" << title << "'");
    return {};
}

/// The titles of what `selection` offers.
std::vector<std::string> titles(const fb::compilation_t& compilation, const fb::span_t selection) {
    std::vector<std::string> found;
    for (const fb::lsp::refactoring_t& refactoring :
         fb::lsp::refactorings(fb::lsp::query_t(compilation, selection.file), selection)) {
        found.push_back(refactoring.title);
    }

    return found;
}

/// The text the edits put in, together.
std::string inserted(const std::vector<fb::fix_t>& edits) {
    std::string text;
    for (const fb::fix_t& fix : edits) {
        text += fix.replacement;
    }

    return text;
}

} // namespace

TEST_CASE("extracting a Local, a Function, or a Method from std's code keeps what a program does",
          "[lsp][refactor]") {
    const program_t program = test_program("text_diff.fb");
    const std::string before = printed(program, "extract");
    const fb::compilation_t compilation = compiled(program);
    const fb::file_id_t diff = file_at(compilation, "std/text/diff.fb");

    // A value worked out once, first, in its statement.
    const fb::lsp::refactoring_t local =
        offered(compilation, selected(compilation, diff, "Local finish = at + 1", "at + 1"),
                "Extract to a Local");
    CHECK(local.kind == "refactor.extract");
    CHECK(inserted(local.edits).contains("Local value = at + 1\n"));
    CHECK(printed(applied(compilation, program, local.edits), "extract") == before);

    // Statements with what they read from around them as parameters, and the Local the code
    // after them reads as their result, from a Static Method, which can be a Method or a Function.
    const fb::span_t chars = selected(compilation, diff,
                                      "        Local from: String[] = []\n"
                                      "        For c In a.Chars\n"
                                      "            from.Add(\"{c}\")\n"
                                      "        End For\n");
    for (const std::string_view title : { "Extract to a Method", "Extract to a Function" }) {
        const fb::lsp::refactoring_t extracted = offered(compilation, chars, title);
        CHECK(inserted(extracted.edits).contains("Local from = Extracted(a)"));
        CHECK(inserted(extracted.edits).contains("(a: String): String[]"));
        CHECK(printed(applied(compilation, program, extracted.edits), "extract") == before);
    }

    // From a Function's code, with two parameters.
    const fb::span_t counted = selected(compilation, diff,
                                        "    Local inLeft: Int[] = []\n"
                                        "    inLeft.Resize(classes.Count, 0)\n"
                                        "    For line In left\n"
                                        "        inLeft[line] += 1\n"
                                        "    End For\n");
    const fb::lsp::refactoring_t function = offered(compilation, counted, "Extract to a Function");
    CHECK(inserted(function.edits).contains("Local inLeft = Extracted(classes, left)"));
    CHECK(printed(applied(compilation, program, function.edits), "extract") == before);
}

TEST_CASE("inlining a Local or a Function in std's code keeps what a program does",
          "[lsp][refactor]") {
    const program_t templates = test_program("text_template.fb");
    const std::string templated = printed(templates, "inline");
    fb::compilation_t compilation = compiled(templates);
    const fb::file_id_t template_file = file_at(compilation, "std/text/template.fb");
    const fb::lsp::refactoring_t local =
        offered(compilation, selected(compilation, template_file, "Local set = templates", "set"),
                "Inline 'set'");
    CHECK(local.kind == "refactor.inline");
    CHECK(printed(applied(compilation, templates, local.edits), "inline") == templated);

    const program_t yaml = test_program("data_yaml.fb");
    const std::string parsed = printed(yaml, "inline");
    compilation = compiled(yaml);
    const fb::file_id_t scanner = file_at(compilation, "std/data/yaml_scanner.fb");
    const fb::span_t call = selected(compilation, scanner, "!IsWhiteByte(before)", "IsWhiteByte");
    const fb::lsp::refactoring_t function =
        offered(compilation, call, "Inline this call of 'IsWhiteByte'");
    CHECK(inserted(function.edits) == "(before == 32 || before == 9)");
    CHECK(printed(applied(compilation, yaml, function.edits), "inline") == parsed);

    // Its other call's argument indexes, which may panic, and the code uses it twice.
    CHECK(!std::ranges::contains(
        titles(compilation,
               selected(compilation, scanner, "!IsWhiteByte(Bytes[at])", "IsWhiteByte")),
        "Inline this call of 'IsWhiteByte'"));
}

TEST_CASE("changing a signature in std's code changes every call, and keeps what a program does",
          "[lsp][refactor]") {
    const program_t program = test_program("text_diff.fb");
    const std::string before = printed(program, "signature");
    fb::compilation_t compilation = compiled(program);
    fb::file_id_t diff = file_at(compilation, "std/text/diff.fb");
    const auto part = [&](const fb::compilation_t& checked) {
        const fb::lsp::query_t query(checked, diff);
        const std::optional<fb::lsp::named_t> named =
            query.name_at(selected(checked, diff, "Function Part(", "Part").begin);
        REQUIRE(named);
        return std::pair(query.key(named->symbol), named->symbol);
    };

    const fb::lsp::query_t query(compilation, diff);
    CHECK(fb::lsp::signature_parameters(query, part(compilation).second) ==
          "lines: String[], start: Int, finish: Int");

    // Reordered, renamed, and one added, whose value each call passes.
    const std::expected<std::vector<fb::fix_t>, std::string> changed = fb::lsp::change_signature(
        query, part(compilation).first,
        "start: Int, finish: Int, source: String[] <- lines, extra: Int = 5");
    REQUIRE(changed);
    const program_t reordered = applied(compilation, program, *changed);
    CHECK(reordered.main == program.main);
    const auto diff_text = [](const program_t& changed_program) {
        return std::ranges::find(changed_program.library, "std/text/diff.fb",
                                 &fb::library_file_t::path)
            ->text;
    };
    CHECK(diff_text(reordered).contains(
        "Function Part(start: Int, finish: Int, source: String[], extra: Int): String[]"));
    CHECK(diff_text(reordered).contains("part.Add(source[i])"));
    CHECK(printed(reordered, "signature") == before);

    // And the one added taken out, whose arguments are its value.
    compilation = compiled(reordered);
    diff = file_at(compilation, "std/text/diff.fb");
    const std::expected<std::vector<fb::fix_t>, std::string> removed =
        fb::lsp::change_signature(fb::lsp::query_t(compilation, diff), part(compilation).first,
                                  "source: String[], start: Int, finish: Int");
    REQUIRE(removed);
    const program_t back = applied(compilation, reordered, *removed);
    CHECK(!diff_text(back).contains(", 5)"));
    CHECK(printed(back, "signature") == before);

    // An argument that does something can't go, nor move past another.
    const program_t small = own_program("Import std.core\n"
                                        "Function Next(): Int\n"
                                        "    Print(\"next\")\n"
                                        "    Return 1\n"
                                        "End Function\n"
                                        "Function Sum(a: Int, b: Int): Int => a + b\n"
                                        "Print(Sum(Next(), Next()))\n");
    const fb::compilation_t small_check = compiled(small);
    const fb::lsp::query_t small_query(small_check, small_check.file);
    const fb::span_t sum = selected(small_check, small_check.file, "Function Sum", "Sum");
    const std::optional<fb::lsp::named_t> summed = small_query.name_at(sum.begin);
    REQUIRE(summed);
    const fb::lsp::symbol_key_t key = small_query.key(summed.value_or(fb::lsp::named_t{}).symbol);
    CHECK(!fb::lsp::change_signature(small_query, key, "a: Int"));
    CHECK(!fb::lsp::change_signature(small_query, key, "b: Int, a: Int"));
    CHECK(!fb::lsp::change_signature(small_query, key, "a: Int, b: Int, c: Int"));
    CHECK(fb::lsp::change_signature(small_query, key, "a: Int, b: Int, c: Int = 0"));
}

TEST_CASE("organizing Imports sorts std's code's, and keeps what a program does",
          "[lsp][refactor]") {
    const program_t program = test_program("text_diff.fb");
    const std::string before = printed(program, "organize");
    const fb::compilation_t compilation = compiled(program);
    const fb::file_id_t diff = file_at(compilation, "std/text/diff.fb");
    const std::vector<fb::fix_t> organized =
        fb::lsp::organized_imports(fb::lsp::query_t(compilation, diff));
    REQUIRE(organized.size() == 1);
    CHECK(organized.front().replacement ==
          "Import std.collections.array\nImport std.collections.map\nImport std.core\n"
          "Import std.text.char\nImport std.text.string\n");
    CHECK(printed(applied(compilation, program, organized), "organize") == before);
}

TEST_CASE("a refactoring isn't offered where it would change what the code does",
          "[lsp][refactor]") {
    const program_t program = own_program("Import std.core\n"
                                          "Function Next(): Int\n"
                                          "    Print(\"next\")\n"
                                          "    Return 1\n"
                                          "End Function\n"
                                          "Function Twice(n: Int): Int => n + n\n"
                                          "Function F(limit: Int): Int\n"
                                          "    Local count = 0\n"
                                          "    While count < limit * 2\n"
                                          "        count += 1\n"
                                          "        If count > 5 Then Return count\n"
                                          "    End While\n"
                                          "    Local total = Next() + Twice(Next())\n"
                                          "    Local ok = limit > 0 && Twice(limit) > 2\n"
                                          "    Return total\n"
                                          "End Function\n"
                                          "Print(F(3))\n");
    const fb::compilation_t compilation = compiled(program);
    REQUIRE(!compilation.diagnostics.has_errors());
    const fb::file_id_t file = compilation.file;
    // A While's condition works out each time round; the right of && only sometimes; and the
    // second Next() runs after the first.
    CHECK(titles(compilation, selected(compilation, file, "limit * 2")).empty());
    CHECK(
        !std::ranges::contains(titles(compilation, selected(compilation, file, "Twice(limit) > 2")),
                               "Extract to a Local"));
    CHECK(!std::ranges::contains(titles(compilation, selected(compilation, file, "Twice(Next())")),
                                 "Extract to a Local"));
    // Code with a Return of the function's.
    CHECK(titles(compilation, selected(compilation, file,
                                       "        count += 1\n"
                                       "        If count > 5 Then Return count\n"))
              .empty());
    // A Local that changes, and a call whose argument acts, which the code uses twice.
    CHECK(!std::ranges::contains(
        titles(compilation, selected(compilation, file, "Local count = 0", "count")),
        "Inline 'count'"));
    CHECK(!std::ranges::contains(
        titles(compilation, selected(compilation, file, "Twice(Next())", "Twice")),
        "Inline this call of 'Twice'"));
}

namespace {

/// A Struct whose count a ByRef Method and a ByRef parameter change, which the review's cases
/// share.
constexpr std::string_view counters = "Import std.core\n"
                                      "Struct Counter\n"
                                      "    Field count: Int\n"
                                      "    ByRef Method Bump()\n"
                                      "        count += 1\n"
                                      "    End Method\n"
                                      "End Struct\n"
                                      "Struct Holder\n"
                                      "    Field inner: Counter\n"
                                      "End Struct\n"
                                      "Function Add(ByRef n: Int)\n"
                                      "    n += 1\n"
                                      "End Function\n";

/// The change of `function`'s signature in the program's main file, or why not.
std::expected<std::vector<fb::fix_t>, std::string>
changed_signature(const fb::compilation_t& compilation, const std::string_view function,
                  const std::string_view parameters) {
    const fb::lsp::query_t query(compilation, compilation.file);
    const fb::span_t at =
        selected(compilation, compilation.file,
                 std::string("Function ") + std::string(function) + "(", function);
    const std::optional<fb::lsp::named_t> named = query.name_at(at.begin);
    REQUIRE(named);
    return fb::lsp::change_signature(query, query.key(named.value_or(fb::lsp::named_t{}).symbol),
                                     parameters);
}

} // namespace

TEST_CASE("a value passed ByRef, or a ByRef Method's Struct, isn't extracted, nor a Struct copy "
          "it changes inlined",
          "[lsp][refactor]") {
    const program_t program =
        own_program(std::string(counters) + "Local c = Counter(0)\n"
                                            "Add(ByRef c.count)\n"
                                            "Local h = Holder(Counter(0))\n"
                                            "h.inner.Bump()\n"
                                            "Local a = Counter(0)\n"
                                            "Local b = a\n"
                                            "b.Bump()\n"
                                            "Local d = a\n"
                                            "Add(ByRef d.count)\n"
                                            "Local e = a\n"
                                            "a.Bump()\n"
                                            "Print(\"{c.count} {h.inner.count} {a.count} {b.count} "
                                            "{d.count} {e.count}\")\n");
    const fb::compilation_t compilation = compiled(program);
    REQUIRE(!compilation.diagnostics.has_errors());
    const fb::file_id_t file = compilation.file;
    // A copy of the place would be what changes.
    CHECK(!std::ranges::contains(
        titles(compilation, selected(compilation, file, "Add(ByRef c.count)", "c.count")),
        "Extract to a Local"));
    CHECK(!std::ranges::contains(
        titles(compilation, selected(compilation, file, "h.inner.Bump()", "h.inner")),
        "Extract to a Local"));
    // A copy that changes, or whose Struct does after it's made, isn't the Struct.
    for (const std::string_view name : { "b", "d", "e" }) {
        const std::string local = std::format("Local {} = a", name);
        CHECK(!std::ranges::contains(titles(compilation, selected(compilation, file, local, name)),
                                     std::format("Inline '{}'", name)));
    }
}

TEST_CASE("an inlined call keeps its arguments' conversions to its parameters' types",
          "[lsp][refactor]") {
    const program_t program = own_program("Import std.core\n"
                                          "Function Half(x: Float64): Float64 => x / 2\n"
                                          "Local n = 3\n"
                                          "Print(Half(n))\n"
                                          "Print(Half(3))\n");
    const std::string before = printed(program, "converted");
    const fb::compilation_t compilation = compiled(program);
    for (const std::string_view call : { "Half(n)", "Half(3)" }) {
        const fb::lsp::refactoring_t inlined =
            offered(compilation, selected(compilation, compilation.file, call, "Half"),
                    "Inline this call of 'Half'");
        CHECK(inserted(inlined.edits).contains("As Float64)"));
        CHECK(printed(applied(compilation, program, inlined.edits), "converted") == before);
    }
}

TEST_CASE("a signature's change doesn't take out a parameter its code uses, nor rename one to a "
          "name its code has",
          "[lsp][refactor]") {
    const program_t program = own_program("Import std.core\n"
                                          "Global scale = 10\n"
                                          "Function Grow(n: Int): Int => n * scale\n"
                                          "Function Sum(a: Int, b: Int): Int\n"
                                          "    Local total = a + b\n"
                                          "    Return total\n"
                                          "End Function\n"
                                          "Print(Grow(2) + Sum(1, 2))\n");
    const fb::compilation_t compilation = compiled(program);
    REQUIRE(!compilation.diagnostics.has_errors());
    CHECK(!changed_signature(compilation, "Grow", "scale: Int <- n"));
    CHECK(!changed_signature(compilation, "Sum", "total: Int <- a, b: Int"));
    CHECK(!changed_signature(compilation, "Sum", "a: Int"));
    CHECK(changed_signature(compilation, "Grow", "size: Int <- n"));
}

TEST_CASE("a signature's change keeps, moves, and renames a ByRef parameter", "[lsp][refactor]") {
    const program_t program = own_program("Import std.core\n"
                                          "Function P(ByRef a: Int, b: Int)\n"
                                          "    a += b\n"
                                          "End Function\n"
                                          "Local x = 1\n"
                                          "P(ByRef x, 2)\n"
                                          "Print(x)\n");
    const std::string before = printed(program, "by_ref");
    const fb::compilation_t compilation = compiled(program);
    const fb::lsp::query_t query(compilation, compilation.file);
    const std::optional<fb::lsp::named_t> named =
        query.name_at(selected(compilation, compilation.file, "Function P(", "P").begin);
    REQUIRE(named);
    const fb::symbol_id_t function = named.value_or(fb::lsp::named_t{}).symbol;
    REQUIRE(fb::lsp::signature_parameters(query, function) == "ByRef a: Int, b: Int");
    // The list as it was changes nothing.
    CHECK(changed_signature(compilation, "P", "ByRef a: Int, b: Int"));

    const std::expected<std::vector<fb::fix_t>, std::string> changed =
        changed_signature(compilation, "P", "b: Int, ByRef total: Int <- a");
    REQUIRE(changed);
    const program_t after = applied(compilation, program, *changed);
    CHECK(after.main.contains("Function P(b: Int, ByRef total: Int)\n    total += b\n"));
    CHECK(after.main.contains("P(2, ByRef x)\n"));
    CHECK(printed(after, "by_ref") == before);
}

TEST_CASE("a signature's change finds the parentheses by the code, not by their characters",
          "[lsp][refactor]") {
    const program_t program = own_program(
        "Import std.core\n"
        "Function Tag(open: String, n: Int, close: String = \")\"): String => open + \"{n}\" + "
        "close\n"
        "Print(Tag(\"(\", 1))\n"
        "Print(Tag(\"(\", 2, \"]\"))\n");
    const std::string before = printed(program, "parentheses");
    const fb::compilation_t compilation = compiled(program);
    const std::expected<std::vector<fb::fix_t>, std::string> changed =
        changed_signature(compilation, "Tag", "n: Int, open: String, close: String = \")\"");
    REQUIRE(changed);
    const program_t after = applied(compilation, program, *changed);
    CHECK(after.main.contains("Print(Tag(1, \"(\"))\n"));
    CHECK(after.main.contains("Print(Tag(2, \"(\", \"]\"))\n"));
    CHECK(printed(after, "parentheses") == before);
}

TEST_CASE("code that changes a variable through ByRef, or rethrows from a Catch, isn't "
          "extracted",
          "[lsp][refactor]") {
    const program_t program = own_program(std::string(counters) + "Function F()\n"
                                                                  "    Local c = Counter(0)\n"
                                                                  "    c.Bump()\n"
                                                                  "    Add(ByRef c.count)\n"
                                                                  "    Print(c.count)\n"
                                                                  "End Function\n"
                                                                  "Function G() Throws Error\n"
                                                                  "    Try\n"
                                                                  "        Throw Error(\"inner\")\n"
                                                                  "    Catch e\n"
                                                                  "        Throw Error(\"again\")\n"
                                                                  "    End Try\n"
                                                                  "End Function\n"
                                                                  "F()\n"
                                                                  "Try\n"
                                                                  "    G()\n"
                                                                  "Catch e\n"
                                                                  "    Print(e.Message)\n"
                                                                  "End Try\n");
    const fb::compilation_t compilation = compiled(program);
    REQUIRE(!compilation.diagnostics.has_errors());
    const fb::file_id_t file = compilation.file;
    CHECK(titles(compilation, selected(compilation, file, "    c.Bump()\n    Add(ByRef c.count)\n"))
              .empty());
    CHECK(titles(compilation, selected(compilation, file,
                                       "    Try\n"
                                       "        Throw Error(\"inner\")\n"
                                       "    Catch e\n"
                                       "        Throw Error(\"again\")\n"
                                       "    End Try\n"))
              .empty());
}

TEST_CASE("an extracted Function's parameter has the type a check narrowed its value to",
          "[lsp][refactor]") {
    const program_t program = own_program("Import std.core\n"
                                          "Function F(x: Int?)\n"
                                          "    If x != Null\n"
                                          "        Print(x + 1)\n"
                                          "    End If\n"
                                          "End Function\n"
                                          "F(4)\n"
                                          "F(Null)\n");
    const std::string before = printed(program, "narrowed");
    const fb::compilation_t compilation = compiled(program);
    const fb::lsp::refactoring_t extracted =
        offered(compilation, selected(compilation, compilation.file, "        Print(x + 1)\n"),
                "Extract to a Function");
    CHECK(inserted(extracted.edits).contains("(x: Int32)"));
    CHECK(printed(applied(compilation, program, extracted.edits), "narrowed") == before);
}

TEST_CASE("fb-lsp offers the refactorings, organizes Imports, and changes signatures",
          "[lsp][refactor]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n" },
        { "shapes.fb", "Public Function Area(w: Int, h: Int): Int => w * h\n" },
    });
    const std::string main = "Import std.math\n"
                             "Import std.core\n"
                             "Import \"./shapes\"\n"
                             "Print(Area(2, 3) + 1)\n";
    folder.write("main.fb", main);
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("main.fb");
    client.open(uri, main);
    client.settle();

    // A selection's refactorings, without the source actions.
    const json_t actions = client.request(
        "textDocument/codeAction",
        {
            { "textDocument", { { "uri", uri } } },
            {
                "range",
                { { "start", client_t::position(3, 6) }, { "end", client_t::position(3, 16) } },
            },
            { "context", { { "diagnostics", json_t::array() } } },
        });
    const auto local = std::ranges::find_if(
        actions, [](const json_t& action) { return action["title"] == "Extract to a Local"; });
    REQUIRE(local != actions.end());
    CHECK((*local)["kind"] == "refactor.extract");
    CHECK((*local)["edit"]["changes"][uri].size() == 2);
    CHECK(std::ranges::none_of(
        actions, [](const json_t& action) { return action["kind"] == "source.organizeImports"; }));

    // The source action, as a save asks for it: sorted, without std.math, which nothing uses.
    const json_t organized = client.request(
        "textDocument/codeAction",
        {
            { "textDocument", { { "uri", uri } } },
            {
                "range",
                { { "start", client_t::position(0, 0) }, { "end", client_t::position(0, 0) } },
            },
            {
                "context",
                {
                    { "diagnostics", json_t::array() },
                    { "only", json_t::array({ "source.organizeImports" }) },
                },
            },
        });
    REQUIRE(organized.size() == 1);
    CHECK(organized[0]["edit"]["changes"][uri][0]["newText"] ==
          "Import std.core\nImport \"./shapes\"\n");

    // A signature's parameters, and the edits of its declaration and its calls.
    const json_t at{
        { "textDocument", { { "uri", uri } } },
        { "position", client_t::position(3, 7) },
    };
    CHECK(client.request("fb/changeSignature", at)["parameters"] == "w: Int, h: Int");
    json_t change = at;
    change["parameters"] = "height: Int <- h, w: Int";
    const json_t edit = client.request("fb/changeSignature", change);
    CHECK(edit["changes"][folder.uri("shapes.fb")].size() == 2);
    REQUIRE(edit["changes"][uri].size() == 1);
    CHECK(edit["changes"][uri][0]["newText"] == "3, 2");
    change["parameters"] = "w: Int, w: Int";
    CHECK(client.send("fb/changeSignature", change).contains("error"));
}
