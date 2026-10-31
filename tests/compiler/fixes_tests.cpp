#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The quick fixes the compiler suggests, M11's step 5: each is made, and what it makes compiles.

namespace {

/// A program's files by their paths: "test.fb", its main file, and the modules it imports by
/// their paths from it, as "./shapes" does shapes.fb.
using program_t = std::map<std::string, std::string>;

fb::compilation_t compiled(const program_t& program) {
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    for (const auto& [path, text] : program) {
        if (path != "test.fb") {
            files.push_back({ .path = path, .text = text });
        }
    }

    return fb::compile("test.fb", program.at("test.fb"), files);
}

/// The messages of a program's errors and warnings.
std::vector<std::string> messages(const program_t& program) {
    std::vector<std::string> found;
    for (const fb::diagnostic_t& diagnostic : compiled(program).diagnostics.diagnostics()) {
        found.push_back(diagnostic.message);
    }

    return found;
}

/// The titles of the suggestions of the diagnostics whose messages have `about` in them.
std::vector<std::string> suggested(const program_t& program, const std::string_view about) {
    std::vector<std::string> found;
    for (const fb::diagnostic_t& diagnostic : compiled(program).diagnostics.diagnostics()) {
        if (diagnostic.message.contains(about)) {
            for (const fb::suggestion_t& suggestion : diagnostic.suggestions) {
                found.push_back(suggestion.title);
            }
        }
    }

    return found;
}

/// `program` with the suggestion titled `title` made, of a diagnostic whose message has `about`.
program_t fixed(const program_t& program, const std::string_view about,
                const std::string_view title) {
    const fb::compilation_t compilation = compiled(program);
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (!diagnostic.message.contains(about)) {
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

using messages_t = std::vector<std::string>;

} // namespace

TEST_CASE("a name another module declares offers what brings it in", "[fixes]") {
    // std's, which the program doesn't load.
    program_t program{ { "test.fb", "Import std.core\nLocal s = Stack<Int>()\nPrint(s.Count)\n" } };
    CHECK(suggested(program, "'Stack'") == messages_t{ "Import std.collections" });
    CHECK(messages(fixed(program, "'Stack'", "Import std.collections")).empty());

    // A program's own file, by its path, after the Imports at the top.
    program = {
        { "test.fb", "Import std.core\nImport \"./a\"\n\nPrint(A() + B())\n" },
        { "a.fb", "Import std.core\nImport \"./b\"\nPublic Function A(): Int => B()\n" },
        { "b.fb", "Import std.core\nPublic Function B(): Int => 2\n" },
    };
    CHECK(suggested(program, "'B'") == messages_t{ "Import \"./b\"" });
    const program_t imported = fixed(program, "'B'", "Import \"./b\"");
    CHECK(imported.at("test.fb") ==
          "Import std.core\nImport \"./a\"\nImport \"./b\"\n\nPrint(A() + B())\n");
    CHECK(messages(imported).empty());

    // What an Import As names it, and the From line that doesn't name it yet.
    program = {
        {
            "test.fb",
            "Import std.core\nImport std.collections As C\nLocal s = Stack<Int>()\n"
            "Print(s.Count)\n",
        },
    };
    CHECK(messages(fixed(program, "'Stack'", "Change to 'C.Stack'")).empty());
    program = {
        {
            "test.fb",
            "Import std.core\nFrom std.collections Import Queue\n"
            "Local s = Stack<Int>()\nPrint(s.Count)\n",
        },
    };
    const program_t named = fixed(program, "'Stack'", "Add 'Stack' to the From line");
    CHECK(named.at("test.fb").starts_with(
        "Import std.core\nFrom std.collections Import Queue, Stack\n"));
    CHECK(messages(named).empty());
}

TEST_CASE("a name spelled nearly as one code sees offers that one", "[fixes]") {
    // A Local, a built-in, and a Function.
    const program_t program{
        {
            "test.fb",
            "Import std.core\nFunction Twice(n: Int): Int => n * 2\nLocal count = 3\n"
            "Pirnt(Twcie(cuont))\n",
        },
    };
    CHECK(suggested(program, "'cuont'") == messages_t{ "Change to 'count'" });
    CHECK(suggested(program, "'Pirnt'") == messages_t{ "Change to 'Print'" });
    program_t once = fixed(program, "'cuont'", "Change to 'count'");
    once = fixed(once, "'Twcie'", "Change to 'Twice'");
    CHECK(messages(fixed(once, "'Pirnt'", "Change to 'Print'")).empty());

    // A member.
    const program_t member{
        {
            "test.fb",
            "Import std.core\nStruct Point\n    Field X: Int\n    Method Length(): Int => X\n"
            "End Struct\nPrint(Point(3).Lenght())\n",
        },
    };
    CHECK(suggested(member, "'Lenght'") == messages_t{ "Change to 'Length'" });
    CHECK(messages(fixed(member, "'Lenght'", "Change to 'Length'")).empty());

    // Nothing near it, nothing offered.
    CHECK(suggested({ { "test.fb", "Import std.core\nPrint(Zebra)\n" } }, "'Zebra'").empty());
}

TEST_CASE("a call that throws offers Try, with the Throws it needs, and Try? where it's alone",
          "[fixes]") {
    const std::string risky = "Import std.core\nFunction Risky(): Int Throws Error\n"
                              "    Throw Error(\"no\")\nEnd Function\n";
    program_t program{
        { "test.fb", risky + "Function Use(): Int\n    Risky()\n    Return 1\nEnd Function\n" },
    };
    CHECK(suggested(program, "'Risky' can throw") ==
          messages_t{ "Add Try, and Throws Error to 'Use'", "Add Try?" });
    const program_t tried =
        fixed(program, "'Risky' can throw", "Add Try, and Throws Error to 'Use'");
    CHECK(tried.at("test.fb").contains("Function Use(): Int Throws Error\n    Try Risky()\n"));
    CHECK(messages(tried).empty());
    CHECK(messages(fixed(program, "'Risky' can throw", "Add Try?")).empty());

    // A Function's Throws that has another gets it too, and top-level code needs none.
    program = {
        {
            "test.fb",
            risky + "Class Bad Extends Error\nEnd Class\nFunction Use(): Int Throws Bad\n"
                    "    Return Risky()\nEnd Function\n",
        },
    };
    CHECK(suggested(program, "'Risky' can throw") ==
          messages_t{ "Add Try, and Error to the Throws of 'Use'" });
    CHECK(messages(fixed(program, "'Risky' can throw", "Add Try, and Error to the Throws of 'Use'"))
              .empty());
    program = { { "test.fb", risky + "Print(Risky())\n" } };
    CHECK(suggested(program, "'Risky' can throw") == messages_t{ "Add Try" });
    CHECK(messages(fixed(program, "'Risky' can throw", "Add Try")).empty());

    // An error a Try lets out of a Function that doesn't say it throws it.
    program = {
        {
            "test.fb",
            risky + "Function Use(): Int\n    Return Try Risky()\nEnd Function\n",
        },
    };
    CHECK(messages(fixed(program, "isn't caught", "Add Error to the Throws of 'Use'")).empty());
}

TEST_CASE("what a type has to implement or override is written out, and a Class can be Abstract",
          "[fixes]") {
    // A generic Interface's Method and Property, with the type arguments the Class gives.
    const std::string head =
        "Import std.core\nInterface Sink<T>\n    Method Put(x: T, rest: Int...): Bool\n"
        "    ReadOnly Property Size: Int\nEnd Interface\n";
    program_t program{ { "test.fb", head + "Class Pipe Implements Sink<String>\nEnd Class\n" } };
    program_t written = fixed(program, "needs a Method 'Put'", "Implement 'Put'");
    CHECK(written.at("test.fb").contains(
        "Class Pipe Implements Sink<String>\n    Method Put(x: String, rest: Int32...): Bool\n"
        "        Panic(\"'Put' isn't written yet\")\n    End Method\nEnd Class\n"));
    written = fixed(written, "needs a Property 'Size'", "Implement 'Size'");
    CHECK(messages(written).empty());

    // A base Class's Abstract Method, overridden, or the Class made Abstract.
    program = {
        {
            "test.fb",
            "Import std.core\nAbstract Class Base\n    Abstract Method Speak(): String\n"
            "End Class\nClass Dog Extends Base\n    Field Legs: Int\nEnd Class\n",
        },
    };
    CHECK(suggested(program, "must override") ==
          messages_t{ "Override 'Speak'", "Make 'Dog' Abstract" });
    written = fixed(program, "must override", "Override 'Speak'");
    CHECK(written.at("test.fb").ends_with(
        "    Field Legs: Int\n\n    Override Method Speak(): String\n"
        "        Panic(\"'Speak' isn't written yet\")\n    End Method\nEnd Class\n"));
    CHECK(messages(written).empty());
    CHECK(messages(fixed(program, "must override", "Make 'Dog' Abstract")).empty());
}

TEST_CASE("a missing End is added after its block's code, and a wrong one changed", "[fixes]") {
    // Before the next declaration, past the blank line.
    program_t program{
        {
            "test.fb",
            "Import std.core\nFunction A(): Int\n    Return 1\n\nFunction B()\n"
            "End Function\nPrint(A())\n",
        },
    };
    const program_t ended = fixed(program, "expected 'End Function'", "Add 'End Function'");
    CHECK(ended.at("test.fb") ==
          "Import std.core\nFunction A(): Int\n    Return 1\nEnd Function\n\n"
          "Function B()\nEnd Function\nPrint(A())\n");
    CHECK(messages(ended).empty());

    // An If's, as indented as its If, and the file's last one, at its end.
    program = {
        {
            "test.fb",
            "Import std.core\nFunction A(n: Int)\n    If n > 1\n        Print(n)\n"
            "End Function\n",
        },
    };
    CHECK(messages(fixed(program, "expected 'End If'", "Add 'End If'")).empty());
    program = { { "test.fb", "Import std.core\nFunction A()\n    Print(1)" } };
    CHECK(messages(fixed(program, "expected 'End Function'", "Add 'End Function'")).empty());

    // An End that closes nothing else is the block's, written as its.
    program = { { "test.fb", "Import std.core\nWhile True\n    Print(1)\nEnd If\n" } };
    CHECK(messages(fixed(program, "expected 'End While'", "Change to 'End While'")).empty());
}

TEST_CASE("a function without a Return on every path ends with its type's zero", "[fixes]") {
    for (const auto& [type, zero] : std::vector<std::pair<std::string, std::string>>{
             { "Int", "0" },
             { "Double", "0.0" },
             { "Bool", "False" },
             { "String", "\"\"" },
             { "Int?", "Null" },
             { "Int[]", "[]" },
             { "Color", "Color.RED" },
         }) {
        INFO(type);
        const program_t program{
            {
                "test.fb",
                std::format("Import std.core\nEnum Color\n    RED, GREEN\nEnd Enum\n"
                            "Function F(n: Int): {}\n    If n > 0\n        Return {}\n    End If\n"
                            "End Function\n",
                            type, zero),
            },
        };
        const program_t returned = fixed(program, "doesn't return", "Add 'Return " + zero + "'");
        CHECK(returned.at("test.fb").ends_with("    End If\n    Return " + zero +
                                               "\nEnd Function\n"));
        CHECK(messages(returned).empty());
    }

    // A Class has no value to write.
    CHECK(suggested({ { "test.fb", "Import std.core\nClass Box\nEnd Class\nFunction F(): Box\n"
                                   "End Function\n" } },
                    "doesn't return")
              .empty());
}

TEST_CASE("a declaration a use can't see is made Package, or Public", "[fixes]") {
    program_t program{
        { "test.fb", "Import std.core\nImport \"./shapes\"\nPrint(Hidden() + Box(1).Twice())\n" },
        {
            "shapes.fb",
            "Import std.core\nFunction Hidden(): Int => 1\nPublic Struct Box\n"
            "    Public Field Size: Int\n    Method Twice(): Int => Size * 2\nEnd Struct\n",
        },
    };
    CHECK(suggested(program, "'Hidden'") ==
          messages_t{ "Make 'Hidden' Package", "Make 'Hidden' Public" });
    program_t made = fixed(program, "'Hidden'", "Make 'Hidden' Package");
    CHECK(made.at("shapes.fb").contains("\nPackage Function Hidden(): Int => 1\n"));
    made = fixed(made, "'Twice'", "Make 'Twice' Public");
    CHECK(made.at("shapes.fb").contains("\n    Public Method Twice(): Int"));
    CHECK(messages(made).empty());

    // The visibility it has written is what changes.
    program = {
        { "test.fb", "Import std.core\nImport \"./shapes\"\nPrint(Sealed())\n" },
        { "shapes.fb", "Import std.core\nPrivate Function Sealed(): Int => 1\n" },
    };
    made = fixed(program, "'Sealed'", "Make 'Sealed' Public");
    CHECK(made.at("shapes.fb") == "Import std.core\nPublic Function Sealed(): Int => 1\n");
    CHECK(messages(made).empty());

    // Not std's, which the program can't change.
    const program_t standard{ { "test.fb", "Import std.core\nPrint(MergeSort)\n" } };
    REQUIRE(messages(standard).front().contains("'MergeSort' is Package"));
    CHECK(suggested(standard, "'MergeSort'").empty());
}

TEST_CASE("a value of another type converts with As, and a T? gives a value for Null", "[fixes]") {
    // As where 'As' converts it, in parentheses for an operation of its own.
    program_t program{
        {
            "test.fb",
            "Import std.core\nFunction Half(n: Int64): Int\n    Return n / 2\n"
            "End Function\nPrint(Half(4))\n",
        },
    };
    program_t made = fixed(program, "expected Int32, found Int64", "Convert with 'As Int32'");
    CHECK(made.at("test.fb").contains("    Return (n / 2) As Int32\n"));
    CHECK(messages(made).empty());
    CHECK(
        suggested({ { "test.fb", "Import std.core\nLocal s: String = 1\n" } }, "expected").empty());

    // ?? with the type's zero, and ?. on a member of a T?.
    program = {
        {
            "test.fb",
            "Import std.core\nFunction Find(): Int?\n    Return Null\nEnd Function\n"
            "Local n: Int = Find()\nPrint(n)\n",
        },
    };
    made = fixed(program, "may be Null", "Give a value for Null with '?? 0'");
    CHECK(made.at("test.fb").contains("Local n: Int = Find() ?? 0\n"));
    CHECK(messages(made).empty());
    program = {
        {
            "test.fb",
            "Import std.core\nStruct Box\n    Field Size: Int\n"
            "    Method Grow()\n    End Method\nEnd Struct\nLocal b: Box? = Null\n"
            "b.Grow()\n",
        },
    };
    CHECK(messages(fixed(program, "may be Null", "Use '?.'")).empty());

    // A built-in type's member in a module the file doesn't import.
    program = { { "test.fb", "Import std.core\nLocal t: String = \"abc\"\nPrint(t.Length)\n" } };
    CHECK(messages(fixed(program, "'Length'", "Import std.text")).empty());
}

TEST_CASE("what a use names, and nothing declares, is made with the use's types", "[fixes]") {
    const std::string head =
        "Import std.core\nStruct Box\n    Field Size: Int\nEnd Struct\n"
        "Enum Color\n    RED, GREEN\nEnd Enum\nLocal b = Box(1)\nLocal count = 2\n";
    // A Function, from a call that's a statement, or one whose value is wanted.
    program_t program{ { "test.fb", head + "LogIt(count, \"x\")\nLocal n: Int = Compute(b)\n" } };
    program_t made = fixed(program, "'LogIt'", "Create Function 'LogIt'");
    CHECK(made.at("test.fb").ends_with("\nFunction LogIt(count: Int32, a: String)\n"
                                       "    Panic(\"'LogIt' isn't written yet\")\nEnd Function\n"));
    made = fixed(made, "'Compute'", "Create Function 'Compute'");
    CHECK(made.at("test.fb").contains("\nFunction Compute(b: Box): Int32\n"));
    CHECK(messages(made).empty());

    // A Method of the type, from a call of it.
    program = { { "test.fb", head + "b.Grow(3)\n" } };
    made = fixed(program, "'Grow'", "Create Method 'Grow' in Box");
    CHECK(made.at("test.fb").contains("    Field Size: Int\n\n    Method Grow(a: Int32)\n"));
    CHECK(messages(made).empty());

    // A Field or a Property, from an assignment to it, and a Local.
    program = { { "test.fb", head + "b.Name = \"box\"\ntotal = 5\nPrint(total)\n" } };
    CHECK(suggested(program, "'Name'") ==
          messages_t{ "Create Field 'Name' in Box", "Create Property 'Name' in Box" });
    made = fixed(program, "'Name'", "Create Field 'Name' in Box");
    CHECK(made.at("test.fb").contains("    Field Size: Int\n\n    Field Name: String\n"));
    made = fixed(made, "'total'", "Declare 'total' as a Local");
    CHECK(messages(made).empty());
    made = fixed(program, "'Name'", "Create Property 'Name' in Box");
    CHECK(messages(fixed(made, "'total'", "Declare 'total' as a Local")).empty());

    // An Enum's member, as its others are written.
    program = { { "test.fb", head + "Print(Color.BLUE)\n" } };
    made = fixed(program, "'BLUE'", "Add 'BLUE' to Color");
    CHECK(made.at("test.fb").contains("Enum Color\n    RED, GREEN, BLUE\nEnd Enum\n"));
    CHECK(messages(made).empty());
    program = {
        {
            "test.fb",
            "Import std.core\nEnum Size\n    SMALL\n    LARGE\nEnd Enum\n"
            "Print(Size.HUGE)\n",
        },
    };
    made = fixed(program, "'HUGE'", "Add 'HUGE' to Size");
    CHECK(made.at("test.fb").contains("    SMALL\n    LARGE\n    HUGE\nEnd Enum\n"));
    CHECK(messages(made).empty());
}

TEST_CASE("code that needs Unsafe is wrapped in an Unsafe block, or its function made Unsafe",
          "[fixes]") {
    // A statement, wrapped, but not a Local's, whose block would hide it.
    const std::string head = "Import std.core\nImport std.ffi\nGlobal p: Ptr<Int32> = Null\n";
    program_t program{ { "test.fb", head + "Function Set()\n    p.Value = 6\nEnd Function\n" } };
    CHECK(suggested(program, "needs Unsafe") ==
          messages_t{ "Wrap in an Unsafe block", "Make 'Set' Unsafe" });
    program_t made = fixed(program, "needs Unsafe", "Wrap in an Unsafe block");
    CHECK(made.at("test.fb").ends_with(
        "Function Set()\n    Unsafe\n        p.Value = 6\n    End Unsafe\nEnd Function\n"));
    CHECK(messages(made).empty());
    made = fixed(program, "needs Unsafe", "Make 'Set' Unsafe");
    CHECK(made.at("test.fb").contains("\nUnsafe Function Set()\n"));
    CHECK(messages(made).empty());

    program = {
        {
            "test.fb",
            head + "Function Peek(): Int\n    Local x: Int32 = 5\n"
                   "    Local q = AddressOf(x)\n    Return x\nEnd Function\n",
        },
    };
    CHECK(suggested(program, "needs Unsafe") == messages_t{ "Make 'Peek' Unsafe" });
}
