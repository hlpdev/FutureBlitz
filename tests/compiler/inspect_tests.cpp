#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/lint.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The inspections, M11's step 6: when each warns and when it doesn't, what its fixes make, and
// [lints]'s levels.

namespace {

/// A program's files by their paths: "test.fb", its main file, and the modules it imports by
/// their paths from it, as "./shapes" does shapes.fb.
using program_t = std::map<std::string, std::string>;

using found_t = std::vector<std::string>;

fb::compilation_t compiled(const program_t& program, const fb::lints_t& lints = {}) {
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    for (const auto& [path, text] : program) {
        if (path != "test.fb") {
            files.push_back({ .path = path, .text = text });
        }
    }

    return fb::compile("test.fb", program.at("test.fb"), files, fb::build_mode_t::DEBUG, lints);
}

/// What a diagnostic covers and says, as "<its text>: <message>".
std::string shown(const fb::compilation_t& compilation, const fb::diagnostic_t& diagnostic) {
    const fb::source_file_t& file = compilation.sources.get(diagnostic.span.file);
    return std::format("{}: {}", file.slice(diagnostic.span), diagnostic.message);
}

/// The errors of a program.
found_t errors(const program_t& program) {
    const fb::compilation_t compilation = compiled(program);
    found_t found;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (diagnostic.severity == fb::severity_t::ERROR) {
            found.push_back(shown(compilation, diagnostic));
        }
    }

    return found;
}

/// The warnings about `lint` a program gets with the inspections on, which has no errors.
found_t found(const program_t& program, const fb::lint_t lint) {
    const fb::compilation_t compilation = compiled(program);
    found_t warnings;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        INFO(shown(compilation, diagnostic));
        CHECK(diagnostic.severity != fb::severity_t::ERROR);
        if (diagnostic.lint == lint) {
            CHECK(diagnostic.code == fb::lint_info(lint).name);
            warnings.push_back(shown(compilation, diagnostic));
        }
    }

    return warnings;
}

found_t found(const std::string_view text, const fb::lint_t lint) {
    return found(program_t{ { "test.fb", std::string(text) } }, lint);
}

/// `program` with the suggestion titled `title` made, of a warning about `lint` whose message
/// has `about` in it.
program_t fixed(const program_t& program, const fb::lint_t lint, const std::string_view about,
                const std::string_view title) {
    const fb::compilation_t compilation = compiled(program);
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (diagnostic.lint != lint || !diagnostic.message.contains(about)) {
            continue;
        }

        for (const fb::suggestion_t& suggestion : diagnostic.suggestions) {
            if (suggestion.title != title) {
                continue;
            }

            // Back to front, so each fix's span is still where it was.
            program_t out = program;
            std::vector<fb::fix_t> fixes = suggestion.fixes;
            std::ranges::sort(fixes, std::greater{},
                              [](const fb::fix_t& fix) { return fix.span.begin; });
            for (const fb::fix_t& fix : fixes) {
                std::string& text = out.at(compilation.sources.get(fix.span.file).path());
                text.replace(fix.span.begin, fix.span.length(), fix.replacement);
            }

            return out;
        }
    }

    FAIL("no suggestion '" << title << "' for '" << about << "'");
    return program;
}

/// The main file of `program` with the fix made.
std::string fixed(const std::string_view text, const fb::lint_t lint, const std::string_view about,
                  const std::string_view title) {
    return fixed(program_t{ { "test.fb", std::string(text) } }, lint, about, title).at("test.fb");
}

found_t errors(const std::string_view text) {
    return errors(program_t{ { "test.fb", std::string(text) } });
}

} // namespace

TEST_CASE("a Local that's never read is reported, with fixes that leave it out or rename it",
          "[inspect]") {
    const std::string program = "Import std.core\n"
                                "Local unused = 4\n"
                                "Local set = 1\n"
                                "set = 2\n"
                                "Local lo, hi = (1, 2)\n"
                                "Print(lo)\n";
    CHECK(found(program, fb::lint_t::UNUSED_LOCAL) == found_t{
                                                          "unused: 'unused' is never used",
                                                          "set: 'set' is set, but never read",
                                                          "hi: 'hi' is never used",
                                                      });

    std::string out = fixed(program, fb::lint_t::UNUSED_LOCAL, "'unused'", "Remove 'unused'");
    CHECK(out == "Import std.core\nLocal set = 1\nset = 2\nLocal lo, hi = (1, 2)\nPrint(lo)\n");
    out = fixed(program, fb::lint_t::UNUSED_LOCAL, "'set'", "Rename to '_set'");
    CHECK(out.contains("Local _set = 1\n_set = 2\n"));
    CHECK(errors(out).empty());
    out = fixed(program, fb::lint_t::UNUSED_LOCAL, "'hi'", "Skip it with '_'");
    CHECK(out.contains("Local lo, _ = (1, 2)\n"));
    CHECK(errors(out).empty());

    // A call's Local isn't left out, which would drop the call.
    CHECK(found("Import std.core\n"
                "Function Two(): Int => 2\n"
                "Function F()\n"
                "    Local two = Two()\n"
                "End Function\n"
                "F()\n",
                fb::lint_t::UNUSED_LOCAL) == found_t{ "two: 'two' is never used" });
    const std::string call = "Import std.core\nFunction Two(): Int => 2\nLocal two = Two()\n";
    const fb::compilation_t compilation = compiled(program_t{ { "test.fb", call } });
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (diagnostic.lint == fb::lint_t::UNUSED_LOCAL) {
            REQUIRE(diagnostic.suggestions.size() == 1);
            CHECK(diagnostic.suggestions[0].title == "Rename to '_two'");
        }
    }

    // What's read, a "_" name, a loop's variable, a Catch's, and what a lambda reads.
    CHECK(found("Import std.core\n"
                "Local _quiet = 1\n"
                "Local read = 2\n"
                "Local counted = 0\n"
                "counted += 1\n"
                "Print(read)\n"
                "For i = 1 To 2\n"
                "    Print(\"x\")\n"
                "End For\n"
                "Try\n"
                "    Throw Error(\"no\")\n"
                "Catch e\n"
                "    Print(\"caught\")\n"
                "End Try\n"
                "Local captured = 3\n"
                "Local show = Function() => Print(captured)\n"
                "show()\n",
                fb::lint_t::UNUSED_LOCAL)
              .empty());
}

TEST_CASE("a parameter that's never used is reported, unless something else sets it", "[inspect]") {
    const std::string program = "Import std.core\n"
                                "Function Scale(n: Int, by: Int): Int\n"
                                "    Return n * 2\n"
                                "End Function\n"
                                "Print(Scale(1, by: 3))\n";
    CHECK(found(program, fb::lint_t::UNUSED_PARAMETER) == found_t{ "by: 'by' is never used" });
    // A call that names it names the new one.
    const std::string out = fixed(program, fb::lint_t::UNUSED_PARAMETER, "'by'", "Rename to '_by'");
    CHECK(out.contains("Function Scale(n: Int, _by: Int): Int\n"));
    CHECK(out.contains("Print(Scale(1, _by: 3))\n"));
    CHECK(errors(out).empty());

    // A Public function's, an override's, an Interface's Method's, an Operator's, one used as a
    // value, one with no code yet, a "_" one, and a Property's Set.
    CHECK(found("Import std.core\n"
                "Public Function Api(n: Int): Int => 1\n"
                "Interface Shape\n"
                "    Method Area(scale: Float): Float\n"
                "End Interface\n"
                "Class Square Implements Shape\n"
                "    Method Area(scale: Float): Float => 1.0\n"
                "End Class\n"
                "Class Base\n"
                "    Method Hit(power: Int)\n"
                "        Print(\"hit\")\n"
                "    End Method\n"
                "End Class\n"
                "Class Boss Extends Base\n"
                "    Override Method Hit(power: Int)\n"
                "        Print(power)\n"
                "    End Method\n"
                "End Class\n"
                "Struct Money\n"
                "    Field cents: Int\n"
                "    Operator +(a: Money, b: Money): Money => a\n"
                "End Struct\n"
                "Function Compare(a: Int, b: Int): Int => a\n"
                "Function Later(n: Int): Int\n"
                "    Todo()\n"
                "End Function\n"
                "Function Quiet(_n: Int): Int => 1\n"
                "Class Gauge\n"
                "    Field level: Int\n"
                "    Property Level: Int\n"
                "        Get => level\n"
                "        Set(value)\n"
                "            Print(\"set\")\n"
                "        End Set\n"
                "    End Property\n"
                "End Class\n"
                "Local f: Func(Int, Int): Int = Compare\n"
                "Local s: Shape = Square()\n"
                "Local b: Base = Boss()\n"
                "b.Hit(1)\n"
                "Local g = Gauge()\n"
                "g.Level = 2\n"
                "Print(Api(1) + f(1, 2) + Later(1) + Quiet(1) + g.Level)\n"
                "Print(s.Area(1.0))\n"
                "Print((Money(1) + Money(2)).cents)\n",
                fb::lint_t::UNUSED_PARAMETER)
              .empty());
}

TEST_CASE("an Import or a From line's name that nothing uses is reported", "[inspect]") {
    const std::string program = "Import std.core\n"
                                "Import std.collections\n"
                                "Import std.math\n"
                                "From std.text Import Normalize, Levenshtein\n"
                                "Local s = Stack<Int>()\n"
                                "s.Push(Levenshtein(\"a\", \"b\"))\n"
                                "Print(s.Count)\n";
    CHECK(found(program, fb::lint_t::UNUSED_IMPORT) == found_t{
                                                           "std.math: nothing in this file "
                                                           "uses std.math",
                                                           "Normalize: nothing in this file uses "
                                                           "'Normalize'",
                                                       });
    std::string out = fixed(program, fb::lint_t::UNUSED_IMPORT, "std.math", "Remove the Import");
    CHECK(out.starts_with("Import std.core\nImport std.collections\nFrom std.text"));
    CHECK(errors(out).empty());
    out = fixed(program, fb::lint_t::UNUSED_IMPORT, "'Normalize'", "Remove 'Normalize'");
    CHECK(out.contains("From std.text Import Levenshtein\n"));
    CHECK(errors(out).empty());

    // Every name of a From line.
    CHECK(fixed("Import std.core\nFrom std.math Import Clamp, Lerp\nPrint(1)\n",
                fb::lint_t::UNUSED_IMPORT, "From line",
                "Remove the From line") == "Import std.core\nPrint(1)\n");

    // A file that uses nothing.
    CHECK(found("Import std.math\n", fb::lint_t::UNUSED_IMPORT) ==
          found_t{ "std.math: nothing in this file uses std.math" });

    // A program's own file, by the qualifier an As gives it.
    const program_t own{
        { "test.fb", "Import std.core\nImport \"./a\" As a\nImport \"./b\"\nPrint(a.A())\n" },
        { "a.fb", "Public Function A(): Int => 1\n" },
        { "b.fb", "Public Function B(): Int => 2\n" },
    };
    CHECK(found(own, fb::lint_t::UNUSED_IMPORT) == found_t{ "\"./b\": nothing in this file "
                                                            "uses ./b" });

    // What a module's Extend adds, a For ... In's Iterate, and std.test's built-ins.
    CHECK(found("Import std.core\n"
                "Import std.text\n"
                "Import std.collections\n"
                "Import std.test\n"
                "Local list = Set<Int>()\n"
                "For n In list\n"
                "    Print(n)\n"
                "End For\n"
                "Print(\"abc\".Length)\n"
                "@Test\n"
                "Function Works()\n"
                "    Expect(1 == 1)\n"
                "End Function\n",
                fb::lint_t::UNUSED_IMPORT)
              .empty());

    // The only Import of a module whose Extend gives a String an Interface, which the program
    // uses without naming anything of the module's, and which without it, isn't loaded.
    program_t extended{
        { "test.fb",
          "Import std.core\nImport \"./sized\"\nImport \"./extend\"\nPrint(SizeOf(\"abc\"))\n" },
        { "sized.fb", "Public Interface Sized\n"
                      "    Method Measure(): Int\n"
                      "End Interface\n"
                      "Public Function SizeOf<T: Sized>(x: T): Int => x.Measure()\n" },
        { "extend.fb", "Import \"./sized\"\n"
                       "Extend String Implements Sized\n"
                       "    Method Measure(): Int => 3\n"
                       "End Extend\n" },
    };
    CHECK(found(extended, fb::lint_t::UNUSED_IMPORT).empty());
    extended.at("test.fb") = "Import std.core\nImport \"./sized\"\nPrint(SizeOf(\"abc\"))\n";
    CHECK(!errors(extended).empty());
}

TEST_CASE("a Private declaration nothing in its file uses is reported, with a fix that leaves "
          "it out",
          "[inspect]") {
    const std::string program = "Import std.core\n"
                                "/// Says hello.\n"
                                "Function Hello()\n"
                                "    Print(\"hello\")\n"
                                "End Function\n"
                                "Function Again()\n"
                                "    Again()\n"
                                "End Function\n"
                                "Global score = 0\n"
                                "Const LIMIT = 5\n"
                                "Class Helper\n"
                                "    Method Help()\n"
                                "        Print(\"help\")\n"
                                "    End Method\n"
                                "End Class\n"
                                "Struct Point\n"
                                "    Field x: Int\n"
                                "    Method Unused(): Int => x\n"
                                "End Struct\n"
                                "Enum Color\n"
                                "    RED\n"
                                "End Enum\n"
                                "Print(Point(1).x)\n";
    CHECK(found(program, fb::lint_t::UNUSED_PRIVATE) ==
          found_t{
              "Hello: the Function 'Hello' is never used",
              "Again: the Function 'Again' is never used",
              "score: the Global 'score' is never used",
              "LIMIT: the Const 'LIMIT' is never used",
              "Helper: the Class 'Helper' is never used",
              "Unused: the Method 'Unused' is never used",
              "Color: the Enum 'Color' is never used",
          });
    const std::string out = fixed(program, fb::lint_t::UNUSED_PRIVATE, "'Hello'", "Remove 'Hello'");
    CHECK(out.substr(0, 40) == "Import std.core\nFunction Again()\n    Aga");
    CHECK(errors(out).empty());
    CHECK(
        errors(fixed(program, fb::lint_t::UNUSED_PRIVATE, "'Helper'", "Remove 'Helper'")).empty());
    CHECK(
        errors(fixed(program, fb::lint_t::UNUSED_PRIVATE, "'Unused'", "Remove 'Unused'")).empty());

    // What's Public or used, a Field and a New, what's found without its name, and what has
    // attributes.
    CHECK(found("Import std.core\n"
                "Import std.test\n"
                "Public Function Api()\n"
                "    Print(\"api\")\n"
                "End Function\n"
                "Function Used(): Int => 1\n"
                "Class Node\n"
                "    Field next: Node?\n"
                "    New()\n"
                "        Print(\"new\")\n"
                "    End New\n"
                "    Method Destroy()\n"
                "        Print(\"gone\")\n"
                "    End Method\n"
                "    Method ToString(): String => \"node\"\n"
                "End Class\n"
                "Interface Named\n"
                "    Method Name(): String\n"
                "End Interface\n"
                "Class Cat Implements Named\n"
                "    Method Name(): String => \"cat\"\n"
                "End Class\n"
                "Attribute Route(Path: String)\n"
                "Class Api2\n"
                "    @Route(\"/\")\n"
                "    Method Home()\n"
                "        Print(\"home\")\n"
                "    End Method\n"
                "End Class\n"
                "@Test\n"
                "Function Works()\n"
                "    Expect(Used() == 1)\n"
                "End Function\n"
                "Local named: Named = Cat()\n"
                "Local routes = [Api2()]\n"
                "Print(\"{Node().ToString()} {named.Name()} {routes.Count}\")\n",
                fb::lint_t::UNUSED_PRIVATE) == found_t{});
}

TEST_CASE("code nothing reaches is reported, from where it starts to its block's end",
          "[inspect]") {
    const std::string program = "Import std.core\n"
                                "Function F(n: Int): Int Throws Error\n"
                                "    If n > 0\n"
                                "        Return 1\n"
                                "        Print(\"after\")\n"
                                "    End If\n"
                                "    For i = 1 To 3\n"
                                "        Continue\n"
                                "        Print(i)\n"
                                "    End For\n"
                                "    Throw Error(\"no\")\n"
                                "    Print(\"thrown\")\n"
                                "    Print(\"too\")\n"
                                "End Function\n"
                                "Try\n"
                                "    Print(F(1))\n"
                                "Catch e\n"
                                "    Print(\"no\")\n"
                                "End Try\n";
    CHECK(found(program, fb::lint_t::UNREACHABLE) ==
          found_t{
              "Print(\"after\"): nothing reaches this code, as what comes before it always leaves",
              "Print(i): nothing reaches this code, as what comes before it always leaves",
              "Print(\"thrown\")\n    Print(\"too\"): nothing reaches this code, as what comes "
              "before it always leaves",
          });
    const std::string out =
        fixed(program, fb::lint_t::UNREACHABLE, "", "Remove the code nothing reaches");
    CHECK(out.contains("        Return 1\n    End If\n"));
    CHECK(errors(out).empty());

    // A branch that leaves, a Loop with a Break, and what's after Panic.
    CHECK(found("Import std.core\n"
                "Function G(n: Int): Int\n"
                "    If n > 0\n"
                "        Return 1\n"
                "    End If\n"
                "    Loop\n"
                "        Break\n"
                "    End Loop\n"
                "    Return 2\n"
                "End Function\n"
                "Print(G(1))\n",
                fb::lint_t::UNREACHABLE)
              .empty());
    CHECK(found("Import std.core\nFunction H()\n    Panic(\"no\")\n    Print(\"x\")\nEnd "
                "Function\nH()\n",
                fb::lint_t::UNREACHABLE)
              .size() == 1);
}

TEST_CASE("a Local or parameter named as what it hides is reported", "[inspect]") {
    CHECK(found("Import std.core\n"
                "Global count = 0\n"
                "Const LIMIT = 10\n"
                "Function Helper(): Int => 1\n"
                "Class Box\n"
                "    Field size: Int\n"
                "    Method Grow()\n"
                "        Local size = 2\n"
                "        Print(size)\n"
                "    End Method\n"
                "End Class\n"
                "Function F(LIMIT: Int): Int\n"
                "    Local count = 1\n"
                "    Local Helper = 2\n"
                "    Return count + Helper + LIMIT\n"
                "End Function\n"
                "Box().Grow()\n"
                "Print(F(1) + count + Helper() + LIMIT)\n",
                fb::lint_t::SHADOWED) ==
          found_t{
              "size: 'size' has the name of Self's Field, which it hides here, where that one "
              "is Self.size",
              "LIMIT: 'LIMIT' has the name of a Const, which it hides here",
              "count: 'count' has the name of a Global, which it hides here",
              "Helper: 'Helper' has the name of a Function, which it hides here",
          });

    // A parameter named as a Field, as New's are, and a "_" name.
    CHECK(found("Import std.core\n"
                "Global _x = 0\n"
                "Class Box\n"
                "    Field size: Int\n"
                "    New(size: Int)\n"
                "        Self.size = size\n"
                "    End New\n"
                "End Class\n"
                "Function F(): Int\n"
                "    Local _x = 1\n"
                "    Return _x\n"
                "End Function\n"
                "Print(Box(1).size + F() + _x)\n",
                fb::lint_t::SHADOWED)
              .empty());
}

TEST_CASE("an As that changes nothing is reported, but not one its value takes its type from",
          "[inspect]") {
    const std::string program = "Import std.core\n"
                                "Function F(n: Int): Int\n"
                                "    Return n As Int\n"
                                "End Function\n"
                                "Print(F(1))\n";
    CHECK(found(program, fb::lint_t::REDUNDANT_AS) ==
          found_t{ "n As Int: 'n' is Int32 already, so the As doesn't change it" });
    const std::string out = fixed(program, fb::lint_t::REDUNDANT_AS, "'n'", "Remove the As");
    CHECK(out.contains("    Return n\n"));
    CHECK(errors(out).empty());

    // A literal, Null, a lambda, an empty array, a widening, and a generic call.
    CHECK(found("Import std.core\n"
                "Function Empty<T>(): T[] => []\n"
                "Class Box\n"
                "End Class\n"
                "Local n = 1\n"
                "Local b = 5 As Byte\n"
                "Local box = Null As Box?\n"
                "Local f = (Function(x: Int) => x) As Func(Int): Int\n"
                "Local a = [] As Int[]\n"
                "Local wide = n As Int64\n"
                "Local z = Empty() As Int[]\n"
                "Print(\"{b} {box == Null} {f(1)} {a.Count} {wide} {z.Count}\")\n",
                fb::lint_t::REDUNDANT_AS)
              .empty());
}

TEST_CASE("a condition that's always True or False is reported, but not True, False, or a "
          "Const",
          "[inspect]") {
    CHECK(found("Import std.core\n"
                "Function F(u: UInt32, b: Byte, i: Int8): Int\n"
                "    If 1 > 2\n"
                "        Return 1\n"
                "    End If\n"
                "    If u < 0 || b > 255 || 127 < i\n"
                "        Return 2\n"
                "    End If\n"
                "    While u >= 0\n"
                "        Return 3\n"
                "    End While\n"
                "    Return 4\n"
                "End Function\n"
                "Print(F(1, 2, 3))\n",
                fb::lint_t::CONSTANT_CONDITION) ==
          found_t{
              "1 > 2: this is always False, as nothing in it can change",
              "u < 0: this is always False: UInt32 goes from 0 to 4294967295",
              "b > 255: this is always False: UInt8 goes from 0 to 255",
              "127 < i: this is always False: Int8 goes from -128 to 127",
              "u >= 0: this is always True: UInt32 goes from 0 to 4294967295",
          });

    CHECK(found("Import std.core\n"
                "Const DEBUGGING = False\n"
                "Function F(u: UInt32, i: Int): Int\n"
                "    If DEBUGGING || u > 0 || i < 0 || u <= 4294967294\n"
                "        Return 1\n"
                "    End If\n"
                "    While True\n"
                "        Return 2\n"
                "    End While\n"
                "End Function\n"
                "Print(F(1, 2))\n",
                fb::lint_t::CONSTANT_CONDITION)
              .empty());
}

TEST_CASE("the warnings about Null and Try have fixes, and every check gives them", "[inspect]") {
    const std::string program = "Import std.core\n"
                                "Local text = \"a\"\n"
                                "Print(text ?? \"b\")\n"
                                "Print(text?.ToString() ?? \"\")\n";
    CHECK(fixed(program, fb::lint_t::REDUNDANT_NULL, "'??'", "Remove the '??'")
              .contains("Print(text)\n"));
    const std::string out =
        fixed(program, fb::lint_t::REDUNDANT_NULL, "'?.'", "Change '?.' to '.'");
    CHECK(out.contains("Print(text.ToString() ?? \"\")\n"));
    CHECK(errors(out).empty());

    const std::string tried = "Import std.core\nFunction F(): Int => 1\nPrint(Try F())\n";
    CHECK(fixed(tried, fb::lint_t::REDUNDANT_TRY, "'F()'", "Remove the Try") ==
          "Import std.core\nFunction F(): Int => 1\nPrint(F())\n");

    // compile() without lints gives these, but not the inspections.
    const fb::compilation_t plain = fb::compile("test.fb", program + "Local unused = 1\n");
    std::vector<std::string> lints;
    for (const fb::diagnostic_t& diagnostic : plain.diagnostics.diagnostics()) {
        lints.push_back(std::format("{}: {}", diagnostic.code, diagnostic.message));
    }

    CHECK(lints == std::vector<std::string>{
                       "redundant_null: 'text' can't be Null here, so '??' always gives it",
                       "redundant_null: 'text' can't be Null here, so '?.' isn't needed",
                   });
}

TEST_CASE("[lints] turns a lint off or makes it an error", "[inspect]") {
    const program_t program{ { "test.fb", "Import std.core\nLocal unused = 1\nPrint(1 ?? 2)\n" } };
    fb::lints_t lints;
    lints.set(fb::lint_t::UNUSED_LOCAL, fb::lint_level_t::ERROR);
    lints.set(fb::lint_t::REDUNDANT_NULL, fb::lint_level_t::OFF);
    const fb::compilation_t compilation = compiled(program, lints);
    REQUIRE(compilation.diagnostics.diagnostics().size() == 1);
    const fb::diagnostic_t& error = compilation.diagnostics.diagnostics().front();
    CHECK(error.severity == fb::severity_t::ERROR);
    CHECK(error.lint == fb::lint_t::UNUSED_LOCAL);
    CHECK(compilation.diagnostics.has_errors());

    CHECK(fb::lint_named("unused_local") == fb::lint_t::UNUSED_LOCAL);
    CHECK(!fb::lint_named("unused"));
    for (const fb::lint_info_t& lint : fb::lints()) {
        CHECK(fb::lint_named(lint.name) == lint.lint);
        CHECK(&fb::lint_info(lint.lint) == &lint);
    }
}

TEST_CASE("the inspections leave code an #If may leave out, and a file with errors", "[inspect]") {
    // Each #If's branch that's off may use what the rest doesn't.
    CHECK(found("Import std.core\n"
                "Function F(n: Int)\n"
                "    Local shown = n\n"
                "#If PLATFORM_NONE\n"
                "    Print(shown)\n"
                "#End If\n"
                "End Function\n"
                "F(1)\n",
                fb::lint_t::UNUSED_LOCAL)
              .empty());
    CHECK(found("Import std.core\n"
                "Import std.math\n"
                "#If PLATFORM_NONE\n"
                "Print(Clamp(1, 0, 2))\n"
                "#End If\n",
                fb::lint_t::UNUSED_IMPORT)
              .empty());

    // An error leaves what it's in unchecked.
    const fb::compilation_t compilation =
        compiled(program_t{ { "test.fb", "Import std.core\nLocal unused = 1\nPrint(missing)\n" } });
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        CHECK(diagnostic.lint != fb::lint_t::UNUSED_LOCAL);
    }
}

TEST_CASE("std has none of the inspections' warnings", "[inspect]") {
    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_standard_docs(fb::build_mode_t::DEBUG, fb::lints_t{});
    REQUIRE(compiled.has_value());
    found_t warnings;
    for (const fb::diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
        const fb::source_file_t& file = compiled->sources.get(diagnostic.span.file);
        warnings.push_back(std::format(
            "{}:{}: {}", file.path(),
            file.location_of(diagnostic.span.begin, fb::column_unit_t::CODEPOINT).line + 1,
            diagnostic.message));
    }

    for (const std::string& warning : warnings) {
        UNSCOPED_INFO(warning);
    }

    CHECK(warnings.empty());
}
