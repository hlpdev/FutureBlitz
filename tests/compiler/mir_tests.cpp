#include "fb/driver.hpp"
#include "fb/lower.hpp"
#include "fb/mir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// The MIR dump of a program that analyzes without errors, with the standard library, which
/// only adds what the program uses.
std::string lower_text(const std::string_view text) {
    const fb::compilation_t compilation = fb::compile("test.fb", std::string(text));
    INFO(text);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    return fb::dump(fb::lower(compilation.ast, compilation.analysis));
}

/// The main of a program with no top-level code. Every program has a main.
constexpr std::string_view empty_main = "function main\n"
                                        "bb0:\n"
                                        "    return\n";

/// `text`, then what a program that prints gets from std.core.print after its main: Print's
/// code, and the runtime's function it calls.
std::string with_print(const std::string_view text) {
    return std::string(text) +
           "\n"
           "function std.core.print.Print(String[])\n"
           "    _0 values: String[]\n"
           "bb0:\n"
           "    call std.core.print.fb_write(String[], Int32, Int32)(_0, 0, 1)\n"
           "    return\n"
           "\n"
           "extern runtime function std.core.print.fb_write(String[], Int32, Int32) = "
           "\"fb_write\"\n"
           "    _0 texts: String[]\n"
           "    _1 to_error: Int32\n"
           "    _2 line: Int32\n";
}

} // namespace

TEST_CASE("a widening conversion is a cast", "[mir]") {
    CHECK(lower_text("Function Add(a: Int, b: Long): Long => a + b") ==
          std::string("function Add(Int32, Int64) -> Int64\n"
                      "    _0 a: Int32\n"
                      "    _1 b: Int64\n"
                      "    _2: Int64\n"
                      "    _3: Int64\n"
                      "bb0:\n"
                      "    _2 = _0 As Int64\n"
                      "    _3 = _2 + _1\n"
                      "    return _3\n"
                      "\n") +
              std::string(empty_main));
}

TEST_CASE("a String temporary moves into the variable that takes it", "[mir]") {
    // The call's result _0 and the concat's _2 hand their references to s without a retain. s's
    // old value is released before it's replaced, and s itself when main ends.
    CHECK(lower_text("Import std.core\n"
                     "Function Shout(text: String): String => text + \"!\"\n"
                     "Local s = Shout(\"hi\")\n"
                     "s = s + s\n"
                     "Print(s)") == with_print("function Shout(String) -> String\n"
                                               "    _0 text: String\n"
                                               "    _1: String\n"
                                               "bb0:\n"
                                               "    _1 = concat(_0, \"!\")\n"
                                               "    return _1\n"
                                               "\n"
                                               "function main\n"
                                               "    _0: String\n"
                                               "    _1 s: String\n"
                                               "    _2: String\n"
                                               "    _3: String[]\n"
                                               "bb0:\n"
                                               "    _0 = call Shout(String)(\"hi\")\n"
                                               "    _1 = _0\n"
                                               "    _2 = concat(_1, _1)\n"
                                               "    release _1\n"
                                               "    _1 = _2\n"
                                               "    retain _1\n"
                                               "    _3 = String[] [_1]\n"
                                               "    call std.core.print.Print(String[])(_3)\n"
                                               "    release _3\n"
                                               "    release _1\n"
                                               "    return\n"));
}

TEST_CASE("Return releases the Strings in scope", "[mir]") {
    // bb1 is the Then's Return, and bb2 leads past the If to bb3. The parameter s belongs to the
    // caller, so only t is released.
    CHECK(lower_text("Function F(s: String): Int\n"
                     "    Local t = s + s\n"
                     "    If t == s Then Return 1\n"
                     "    Return 2\n"
                     "End Function") == std::string("function F(String) -> Int32\n"
                                                    "    _0 s: String\n"
                                                    "    _1: String\n"
                                                    "    _2 t: String\n"
                                                    "    _3: Bool\n"
                                                    "bb0:\n"
                                                    "    _1 = concat(_0, _0)\n"
                                                    "    _2 = _1\n"
                                                    "    _3 = equal(_2, _0)\n"
                                                    "    branch _3, bb1, bb2\n"
                                                    "bb1:\n"
                                                    "    release _2\n"
                                                    "    return 1\n"
                                                    "bb2:\n"
                                                    "    goto bb3\n"
                                                    "bb3:\n"
                                                    "    release _2\n"
                                                    "    return 2\n"
                                                    "\n") +
                                            std::string(empty_main));
}

TEST_CASE("Break jumps past the loop", "[mir]") {
    // bb1 tests the condition and bb2 is the body. bb3 is the Break, bb4 and bb5 lead past the If
    // and back to bb1, and bb6 follows the loop.
    CHECK(lower_text("Local n = 0\n"
                     "While n < 10\n"
                     "    n++\n"
                     "    If n == 5 Then Break\n"
                     "End While") == "function main\n"
                                     "    _0 n: Int32\n"
                                     "    _1: Bool\n"
                                     "    _2: Bool\n"
                                     "bb0:\n"
                                     "    _0 = 0\n"
                                     "    goto bb1\n"
                                     "bb1:\n"
                                     "    _1 = _0 < 10\n"
                                     "    branch _1, bb2, bb6\n"
                                     "bb2:\n"
                                     "    _0 = _0 + 1\n"
                                     "    _2 = _0 == 5\n"
                                     "    branch _2, bb3, bb4\n"
                                     "bb3:\n"
                                     "    goto bb6\n"
                                     "bb4:\n"
                                     "    goto bb5\n"
                                     "bb5:\n"
                                     "    goto bb1\n"
                                     "bb6:\n"
                                     "    return\n");
}

TEST_CASE("a For loop checks the distance left before stepping", "[mir]") {
    // bb1 skips the loop when the start is already past the end, and bb2 is the body. bb3 exits
    // when the distance left to 3 is less than one step, so i never steps past the end or
    // overflows. bb4 steps and goes around again.
    CHECK(lower_text("For i = 1 To 3\n"
                     "End For") == "function main\n"
                                   "    _0 i: Int32\n"
                                   "    _1: Bool\n"
                                   "    _2: Int32\n"
                                   "    _3: UInt32\n"
                                   "    _4: UInt32\n"
                                   "    _5: Bool\n"
                                   "    _6: Int32\n"
                                   "bb0:\n"
                                   "    _0 = 1\n"
                                   "    goto bb1\n"
                                   "bb1:\n"
                                   "    _1 = _0 <= 3\n"
                                   "    branch _1, bb2, bb5\n"
                                   "bb2:\n"
                                   "    goto bb3\n"
                                   "bb3:\n"
                                   "    _2 = wrapping(3 - _0)\n"
                                   "    _3 = _2 As UInt32\n"
                                   "    _4 = 1 As UInt32\n"
                                   "    _5 = _3 < _4\n"
                                   "    _6 = wrapping(_0 + 1)\n"
                                   "    branch _5, bb5, bb4\n"
                                   "bb4:\n"
                                   "    _0 = _6\n"
                                   "    goto bb2\n"
                                   "bb5:\n"
                                   "    return\n");
}

TEST_CASE("&& only evaluates its right side when it decides the result", "[mir]") {
    // _1 holds the result. bb1 sets it to False when the left side is False, bb2 evaluates the
    // right side, and bb3 joins them.
    CHECK(lower_text("Local a = 1\n"
                     "Local ok = a > 0 && a < 10") == "function main\n"
                                                      "    _0 a: Int32\n"
                                                      "    _1: Bool\n"
                                                      "    _2: Bool\n"
                                                      "    _3: Bool\n"
                                                      "    _4 ok: Bool\n"
                                                      "bb0:\n"
                                                      "    _0 = 1\n"
                                                      "    _2 = _0 > 0\n"
                                                      "    branch _2, bb2, bb1\n"
                                                      "bb1:\n"
                                                      "    _1 = False\n"
                                                      "    goto bb3\n"
                                                      "bb2:\n"
                                                      "    _3 = _0 < 10\n"
                                                      "    _1 = _3\n"
                                                      "    goto bb3\n"
                                                      "bb3:\n"
                                                      "    _4 = _1\n"
                                                      "    return\n");
}

TEST_CASE("ByRef passes the variable, and defaults are filled in", "[mir]") {
    CHECK(lower_text("Function Bump(ByRef n: Int, by: Int = 1)\n"
                     "    n += by\n"
                     "End Function\n"
                     "Local x = 1\n"
                     "Bump(ByRef x)") == "function Bump(ByRef Int32, Int32)\n"
                                         "    _0 ByRef n: Int32\n"
                                         "    _1 by: Int32\n"
                                         "bb0:\n"
                                         "    _0 = _0 + _1\n"
                                         "    return\n"
                                         "\n"
                                         "function main\n"
                                         "    _0 x: Int32\n"
                                         "bb0:\n"
                                         "    _0 = 1\n"
                                         "    call Bump(ByRef Int32, Int32)(ByRef _0, 1)\n"
                                         "    return\n");
}

TEST_CASE("Globals known at compile time are static; the rest are set first in main", "[mir]") {
    // h depends on a Global, so it starts at 0, and main sets it before the Print even though
    // the Print comes first in the source, then marks it ready.
    CHECK(lower_text("Import std.core\n"
                     "Print(name)\n"
                     "Global g = 1\n"
                     "Global h = g * 2\n"
                     "Global name = \"fb\"") ==
          with_print("global g: Int32 = 1\n"
                     "global h: Int32 = 0\n"
                     "global name: String = \"fb\"\n"
                     "\n"
                     "function main\n"
                     "    _0: Int32\n"
                     "    _1: String[]\n"
                     "bb0:\n"
                     "    _0 = @g * 2\n"
                     "    @h = _0\n"
                     "    ready @h\n"
                     "    retain @name\n"
                     "    _1 = String[] [@name]\n"
                     "    call std.core.print.Print(String[])(_1)\n"
                     "    release _1\n"
                     "    return\n"));

    // A function checks that main has set what it uses, since an earlier Global's initializer
    // could call it first; one known at compile time is always set.
    CHECK(lower_text("Global g = 1\n"
                     "Global h = g * 2\n"
                     "Function Sum(): Int => g + h\n"
                     "Local s = Sum()") == "global g: Int32 = 1\n"
                                           "global h: Int32 = 0\n"
                                           "\n"
                                           "function Sum() -> Int32\n"
                                           "    _0: Int32\n"
                                           "bb0:\n"
                                           "    check ready @h\n"
                                           "    _0 = @g + @h\n"
                                           "    return _0\n"
                                           "\n"
                                           "function main\n"
                                           "    _0: Int32\n"
                                           "    _1: Int32\n"
                                           "    _2 s: Int32\n"
                                           "bb0:\n"
                                           "    _0 = @g * 2\n"
                                           "    @h = _0\n"
                                           "    ready @h\n"
                                           "    _1 = call Sum()()\n"
                                           "    _2 = _1\n"
                                           "    return\n");
}

TEST_CASE("a ThreadLocal Global's initializer is a function of its own, which its uses check for",
          "[mir]") {
    // hits has nothing to make or release, so its uses check nothing; name holds a reference,
    // which each thread releases, and start's initializer runs on each thread's first use.
    CHECK(lower_text("ThreadLocal Global hits = 0\n"
                     "ThreadLocal Global name = \"fb\"\n"
                     "ThreadLocal Global start = Make()\n"
                     "Function Make(): Int => 3\n"
                     "Function Bump(): Int\n    hits += start\n    Return hits\nEnd Function\n"
                     "Local n = Bump()\n"
                     "Local s = name") == "ThreadLocal global hits: Int32 = 0\n"
                                          "ThreadLocal global name: String = \"fb\"\n"
                                          "ThreadLocal global start: Int32 = 0, initializer: "
                                          "start.Initializer\n"
                                          "\n"
                                          "function Make() -> Int32\n"
                                          "bb0:\n"
                                          "    return 3\n"
                                          "\n"
                                          "function Bump() -> Int32\n"
                                          "bb0:\n"
                                          "    check ready @start\n"
                                          "    @hits = @hits + @start\n"
                                          "    return @hits\n"
                                          "\n"
                                          "function main\n"
                                          "    _0: Int32\n"
                                          "    _1 n: Int32\n"
                                          "    _2 s: String\n"
                                          "bb0:\n"
                                          "    _0 = call Bump()()\n"
                                          "    _1 = _0\n"
                                          "    check ready @name\n"
                                          "    retain @name\n"
                                          "    _2 = @name\n"
                                          "    release _2\n"
                                          "    return\n"
                                          "\n"
                                          "function start.Initializer\n"
                                          "    _0: Int32\n"
                                          "bb0:\n"
                                          "    _0 = call Make()()\n"
                                          "    @start = _0\n"
                                          "    return\n");
}

TEST_CASE("a Const known at compile time is no global, used or not", "[mir]") {
    // Each use reads its value, so a Const in a module every program loads adds nothing to them.
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    files.push_back({
        .path = "lib/limits.fb",
        .text = "Const MOST = 1 << 20\n"
                "Public Const NAME = \"limits\"\n"
                "Public Function Most(): Int => MOST\n",
    });
    const fb::compilation_t compilation = fb::compile("test.fb",
                                                      "Import \"./lib/limits\"\n"
                                                      "Const LIMIT = 10\n"
                                                      "Const UNUSED = \"never read\"\n"
                                                      "Global g = LIMIT + 1\n"
                                                      "Local n = NAME\n",
                                                      files);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    CHECK(fb::dump(fb::lower(compilation.ast, compilation.analysis)) == "global g: Int32 = 11\n"
                                                                        "\n"
                                                                        "function main\n"
                                                                        "    _0 n: String\n"
                                                                        "bb0:\n"
                                                                        "    _0 = \"limits\"\n"
                                                                        "    release _0\n"
                                                                        "    return\n");
}

TEST_CASE("a Map literal runs Map's New, then its Add for each entry, in order", "[mir]") {
    const std::string dumped = lower_text("Import std.collections\n"
                                          "Local m = { 1: \"one\", 2: \"two\" }\n");
    const std::size_t made =
        dumped.find("    call std.collections.map.Map<Int32, String>.New()(_0)\n");
    const std::size_t first = dumped.find("    call virtual std.collections.map.Map<Int32, "
                                          "String>.Add(Int32, String)(_0, 1, \"one\")\n");
    const std::size_t second = dumped.find("    call virtual std.collections.map.Map<Int32, "
                                           "String>.Add(Int32, String)(_0, 2, \"two\")\n");
    CHECK(made < first);
    CHECK(first < second);
    CHECK(second != std::string::npos);
}

TEST_CASE("constants print as source code writes them, and a coroutine says it's async", "[mir]") {
    // A T? of an Enum prints its member, and a String escapes what source code would, braces
    // too, which start interpolation.
    const std::string dumped = lower_text("Enum State\n"
                                          "    Idle\n"
                                          "    Walk\n"
                                          "End Enum\n"
                                          "Global g: State? = State.Walk\n"
                                          "Global s = \"tab\\there\\r\\n\\0 \\x01 {{x}}\"\n"
                                          "Async Function Later()\n"
                                          "End Function\n");
    CHECK(dumped.contains("global g: State? = State.Walk\n"));
    CHECK(dumped.contains("global s: String = \"tab\\there\\r\\n\\0 \\x01 {{x}}\"\n"));
    CHECK(dumped.contains("async function Later()\n"));
}

TEST_CASE("an embedded file is a String constant, whose bytes that aren't UTF-8 print by value",
          "[mir]") {
    std::vector<fb::library_file_t> library =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    library.push_back({ .path = "logo.bin", .text = "A\xFF\xC3\xA9" });
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Local logo = Embed(\"logo.bin\")\n", library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    const std::string mir = fb::dump(fb::lower(compilation.ast, compilation.analysis));
    INFO(mir);
    // A new UInt8[] each time, of the bytes, where é is UTF-8 and 0xFF isn't.
    CHECK(mir.contains("    _0 = UInt8[] []\n"
                       "    string_bytes(\"A\\xFF\xC3\xA9\", _0)\n"));
}

TEST_CASE("a Struct is built from its fields, and copies count its Strings", "[mir]") {
    // _0 holds the new Tag and hands it to t. Copying t into u retains its Strings, and changing
    // u.name releases the old one first.
    CHECK(lower_text("Struct Tag\n"
                     "    Field name: String\n"
                     "    Field id: Int = 7\n"
                     "End Struct\n"
                     "Local t = Tag(\"a\")\n"
                     "Local u = t\n"
                     "u.name = \"b\"") == "function main\n"
                                          "    _0: Tag\n"
                                          "    _1 t: Tag\n"
                                          "    _2 u: Tag\n"
                                          "bb0:\n"
                                          "    _0 = Tag { name: \"a\", id: 7 }\n"
                                          "    _1 = _0\n"
                                          "    retain _1\n"
                                          "    _2 = _1\n"
                                          "    release _2.name\n"
                                          "    _2.name = \"b\"\n"
                                          "    release _2\n"
                                          "    release _1\n"
                                          "    return\n");
}

TEST_CASE("a ByRef Method gets its Struct's address, and == compares fields", "[mir]") {
    CHECK(lower_text("Struct C\n"
                     "    Field n: Int\n"
                     "    ByRef Method Bump()\n"
                     "        n += 1\n"
                     "    End Method\n"
                     "End Struct\n"
                     "Local c = C()\n"
                     "c.Bump()\n"
                     "Local same = c == C(1)") == "function C.Bump()\n"
                                                  "    _0 ByRef Self: C\n"
                                                  "bb0:\n"
                                                  "    _0.n = _0.n + 1\n"
                                                  "    return\n"
                                                  "\n"
                                                  "function main\n"
                                                  "    _0: C\n"
                                                  "    _1 c: C\n"
                                                  "    _2: C\n"
                                                  "    _3: Bool\n"
                                                  "    _4 same: Bool\n"
                                                  "bb0:\n"
                                                  "    _0 = C { n: 0 }\n"
                                                  "    _1 = _0\n"
                                                  "    call C.Bump()(ByRef _1)\n"
                                                  "    _2 = C { n: 1 }\n"
                                                  "    _3 = _1.n == _2.n\n"
                                                  "    _4 = _3\n"
                                                  "    return\n");
}

TEST_CASE("a new object gets its field defaults, and a Class lists its Destroy", "[mir]") {
    // The runtime zeroes a new object, so only the String (whose zero is "") and the field with
    // a default are set. Changing t.name releases the old one through the reference.
    CHECK(lower_text("Import std.core\n"
                     "Class Tag\n"
                     "    Field name: String\n"
                     "    Field id: Int = 7\n"
                     "    Field count: Int\n"
                     "    Method Destroy()\n"
                     "        Print(name)\n"
                     "    End Method\n"
                     "End Class\n"
                     "Local t = Tag()\n"
                     "t.name = \"a\"") == with_print("class Tag, Destroy: Tag.Destroy()\n"
                                                     "\n"
                                                     "function Tag.Destroy()\n"
                                                     "    _0 Self: Tag\n"
                                                     "    _1: String[]\n"
                                                     "bb0:\n"
                                                     "    retain _0.name\n"
                                                     "    _1 = String[] [_0.name]\n"
                                                     "    call std.core.print.Print(String[])(_1)\n"
                                                     "    release _1\n"
                                                     "    return\n"
                                                     "\n"
                                                     "function main\n"
                                                     "    _0: Tag\n"
                                                     "    _1 t: Tag\n"
                                                     "bb0:\n"
                                                     "    _0 = new Tag\n"
                                                     "    _0.name = \"\"\n"
                                                     "    _0.id = 7\n"
                                                     "    _1 = _0\n"
                                                     "    release _1.name\n"
                                                     "    _1.name = \"a\"\n"
                                                     "    release _1\n"
                                                     "    return\n"));
}

TEST_CASE("a null check reaches the value inside, and ?? picks a side", "[mir]") {
    CHECK(lower_text("Function F(x: Int?): Int\n"
                     "    If x != Null Then Return x\n"
                     "    Return x ?? 0\n"
                     "End Function") == std::string("function F(Int32?) -> Int32\n"
                                                    "    _0 x: Int32?\n"
                                                    "    _1: Bool\n"
                                                    "    _2: Int32\n"
                                                    "    _3: Bool\n"
                                                    "bb0:\n"
                                                    "    _1 = _0 != Null\n"
                                                    "    branch _1, bb1, bb2\n"
                                                    "bb1:\n"
                                                    "    return _0.value\n"
                                                    "bb2:\n"
                                                    "    goto bb3\n"
                                                    "bb3:\n"
                                                    "    _3 = _0 != Null\n"
                                                    "    branch _3, bb4, bb5\n"
                                                    "bb4:\n"
                                                    "    _2 = _0.value\n"
                                                    "    goto bb6\n"
                                                    "bb5:\n"
                                                    "    _2 = 0\n"
                                                    "    goto bb6\n"
                                                    "bb6:\n"
                                                    "    return _2\n"
                                                    "\n") +
                                            std::string(empty_main));
}

TEST_CASE("?. only reads the field when there's an object, and owns its result", "[mir]") {
    CHECK(lower_text("Class N\n"
                     "    Field name: String\n"
                     "End Class\n"
                     "Function Name(n: N?): String => n?.name ?? \"none\"") ==
          std::string("class N\n"
                      "\n"
                      "function Name(N?) -> String\n"
                      "    _0 n: N?\n"
                      "    _1: String?\n"
                      "    _2: Bool\n"
                      "    _3: String?\n"
                      "    _4: String\n"
                      "    _5: Bool\n"
                      "bb0:\n"
                      "    _2 = _0 != Null\n"
                      "    branch _2, bb1, bb2\n"
                      "bb1:\n"
                      "    _3 = _0.value.name As String?\n"
                      "    retain _3\n"
                      "    _1 = _3\n"
                      "    goto bb3\n"
                      "bb2:\n"
                      "    _1 = Null\n"
                      "    goto bb3\n"
                      "bb3:\n"
                      "    _5 = _1 != Null\n"
                      "    branch _5, bb4, bb5\n"
                      "bb4:\n"
                      "    retain _1.value\n"
                      "    _4 = _1.value\n"
                      "    goto bb6\n"
                      "bb5:\n"
                      "    _4 = \"none\"\n"
                      "    goto bb6\n"
                      "bb6:\n"
                      "    release _1\n"
                      "    return _4\n"
                      "\n") +
              std::string(empty_main));
}

TEST_CASE("two T? values are equal when both are Null or hold equal values", "[mir]") {
    CHECK(lower_text("Function F(a: Bool?, b: Bool?): Bool => a == b") ==
          std::string("function F(Bool?, Bool?) -> Bool\n"
                      "    _0 a: Bool?\n"
                      "    _1 b: Bool?\n"
                      "    _2: Bool\n"
                      "    _3: Bool\n"
                      "    _4: Bool\n"
                      "    _5: Bool\n"
                      "    _6: Bool\n"
                      "bb0:\n"
                      "    _3 = _0 != Null\n"
                      "    _4 = _1 != Null\n"
                      "    _5 = _3 && _4\n"
                      "    branch _5, bb1, bb2\n"
                      "bb1:\n"
                      "    _6 = _0.value == _1.value\n"
                      "    _2 = _6\n"
                      "    goto bb3\n"
                      "bb2:\n"
                      "    _2 = _3 == _4\n"
                      "    goto bb3\n"
                      "bb3:\n"
                      "    return _2\n"
                      "\n") +
              std::string(empty_main));
}

TEST_CASE("a Class's Methods go in its Method table, and calls go through it", "[mir]") {
    // B's table has its override in A's slot. Super calls A's own F.
    CHECK(lower_text("Class A\n"
                     "    Method F(): Int => 1\n"
                     "    Method G()\n"
                     "    End Method\n"
                     "End Class\n"
                     "Class B Extends A\n"
                     "    Override Method F(): Int => Super.F() + 1\n"
                     "End Class\n"
                     "Function H(a: A): Int => a.F()") ==
          std::string("class A\n"
                      "    0: A.F()\n"
                      "    1: A.G()\n"
                      "class B Extends A\n"
                      "    0: B.F()\n"
                      "    1: A.G()\n"
                      "\n"
                      "function A.F() -> Int32\n"
                      "    _0 Self: A\n"
                      "bb0:\n"
                      "    return 1\n"
                      "\n"
                      "function A.G()\n"
                      "    _0 Self: A\n"
                      "bb0:\n"
                      "    return\n"
                      "\n"
                      "function B.F() -> Int32\n"
                      "    _0 Self: B\n"
                      "    _1: A\n"
                      "    _2: Int32\n"
                      "    _3: Int32\n"
                      "bb0:\n"
                      "    _1 = _0 As A\n"
                      "    _2 = call A.F()(_1)\n"
                      "    _3 = _2 + 1\n"
                      "    return _3\n"
                      "\n"
                      "function H(A) -> Int32\n"
                      "    _0 a: A\n"
                      "    _1: Int32\n"
                      "bb0:\n"
                      "    _1 = call virtual A.F()(_0)\n"
                      "    return _1\n"
                      "\n") +
              std::string(empty_main));
}

TEST_CASE("a Class's New sets up an object its caller made", "[mir]") {
    // C gets a New that runs B's, which starts with A's.
    CHECK(lower_text("Class A\n"
                     "    Field n: Int\n"
                     "    New(n: Int)\n"
                     "        Self.n = n\n"
                     "    End New\n"
                     "End Class\n"
                     "Class B Extends A\n"
                     "    Field m: Int = 1\n"
                     "    New()\n"
                     "        Super.New(2)\n"
                     "    End New\n"
                     "End Class\n"
                     "Class C Extends B\n"
                     "End Class\n"
                     "Local c = C()") == "class A\n"
                                         "class B Extends A\n"
                                         "class C Extends B\n"
                                         "\n"
                                         "function A.New(Int32)\n"
                                         "    _0 Self: A\n"
                                         "    _1 n: Int32\n"
                                         "bb0:\n"
                                         "    _0.n = _1\n"
                                         "    return\n"
                                         "\n"
                                         "function B.New()\n"
                                         "    _0 Self: B\n"
                                         "bb0:\n"
                                         "    call A.New(Int32)(_0, 2)\n"
                                         "    return\n"
                                         "\n"
                                         "function main\n"
                                         "    _0: C\n"
                                         "    _1 c: C\n"
                                         "bb0:\n"
                                         "    _0 = new C\n"
                                         "    _0.m = 1\n"
                                         "    call B.New()(_0)\n"
                                         "    _1 = _0\n"
                                         "    release _1\n"
                                         "    return\n");
}

TEST_CASE("Is, As, and As? check an object's Class", "[mir]") {
    CHECK(lower_text("Class A\n"
                     "End Class\n"
                     "Class B Extends A\n"
                     "End Class\n"
                     "Function F(a: A?): B? => a Is B ? a As B : a As? B") ==
          std::string("class A\n"
                      "class B Extends A\n"
                      "\n"
                      "function F(A?) -> B?\n"
                      "    _0 a: A?\n"
                      "    _1: B?\n"
                      "    _2: Bool\n"
                      "    _3: B\n"
                      "    _4: B?\n"
                      "    _5: B?\n"
                      "bb0:\n"
                      "    _2 = _0 Is B\n"
                      "    branch _2, bb1, bb2\n"
                      "bb1:\n"
                      "    _3 = downcast _0 As B\n"
                      "    _4 = _3 As B?\n"
                      "    retain _4\n"
                      "    _1 = _4\n"
                      "    goto bb3\n"
                      "bb2:\n"
                      "    _5 = _0 As? B?\n"
                      "    retain _5\n"
                      "    _1 = _5\n"
                      "    goto bb3\n"
                      "bb3:\n"
                      "    return _1\n"
                      "\n") +
              std::string(empty_main));
}

TEST_CASE("an element is a step into a place, with its index in a local", "[mir]") {
    CHECK(lower_text("Function F(a: Int[], i: Int): Int\n"
                     "    a[i] += 1\n"
                     "    Return a[i]\n"
                     "End Function") == std::string("function F(Int32[], Int32) -> Int32\n"
                                                    "    _0 a: Int32[]\n"
                                                    "    _1 i: Int32\n"
                                                    "    _2: Int32\n"
                                                    "    _3: Int32\n"
                                                    "bb0:\n"
                                                    "    _2 = _1\n"
                                                    "    _0[_2] = _0[_2] + 1\n"
                                                    "    _3 = _1\n"
                                                    "    return _0[_3]\n"
                                                    "\n") +
                                            std::string(empty_main));
}

TEST_CASE("a Return copies what it reads from a temporary before releasing it", "[mir]") {
    // The array Make() gives is released when the statement ends, so the element, which the
    // caller gets a reference to, is read into _2 first.
    CHECK(lower_text("Function Make(): String[] => [\"a\", \"b\"]\n"
                     "Function Last(): String => Make()[1]") ==
          std::string("function Make() -> String[]\n"
                      "    _0: String[]\n"
                      "bb0:\n"
                      "    _0 = String[] [\"a\", \"b\"]\n"
                      "    return _0\n"
                      "\n"
                      "function Last() -> String\n"
                      "    _0: String[]\n"
                      "    _1: Int32\n"
                      "    _2: String\n"
                      "bb0:\n"
                      "    _0 = call Make()()\n"
                      "    _1 = 1\n"
                      "    retain _0[_1]\n"
                      "    _2 = _0[_1]\n"
                      "    release _0\n"
                      "    return _2\n"
                      "\n") +
              std::string(empty_main));
}

TEST_CASE("For ... In holds the array, and checks its Count before each pass", "[mir]") {
    CHECK(lower_text("Function Sum(values: Int[]): Int\n"
                     "    Local total = 0\n"
                     "    For value In values\n"
                     "        total += value\n"
                     "    End For\n"
                     "\n"
                     "    Return total\n"
                     "End Function\n"
                     "Local names = [\"a\"]\n"
                     "names.Add(\"b\")") == "function Sum(Int32[]) -> Int32\n"
                                            "    _0 values: Int32[]\n"
                                            "    _1 total: Int32\n"
                                            "    _2: Int32[]\n"
                                            "    _3: Int32\n"
                                            "    _4: Int32\n"
                                            "    _5: Bool\n"
                                            "    _6 value: Int32\n"
                                            "bb0:\n"
                                            "    _1 = 0\n"
                                            "    retain _0\n"
                                            "    _2 = _0\n"
                                            "    _3 = 0\n"
                                            "    goto bb1\n"
                                            "bb1:\n"
                                            "    _4 = count(_2)\n"
                                            "    _5 = _3 < _4\n"
                                            "    branch _5, bb2, bb4\n"
                                            "bb2:\n"
                                            "    _6 = _2[_3]\n"
                                            "    _1 = _1 + _6\n"
                                            "    goto bb3\n"
                                            "bb3:\n"
                                            "    _3 = wrapping(_3 + 1)\n"
                                            "    goto bb1\n"
                                            "bb4:\n"
                                            "    release _2\n"
                                            "    return _1\n"
                                            "\n"
                                            "function main\n"
                                            "    _0: String[]\n"
                                            "    _1 names: String[]\n"
                                            "bb0:\n"
                                            "    _0 = String[] [\"a\"]\n"
                                            "    _1 = _0\n"
                                            "    add(_1, \"b\")\n"
                                            "    release _1\n"
                                            "    return\n");
}

TEST_CASE("Has, As to an Enum, and an Enum's text", "[mir]") {
    // The values run from 0 to 1, so As checks a range. A string's State needs State.Text.
    CHECK(lower_text("Import std.core\n"
                     "Enum State\n"
                     "    Idle, Walk\n"
                     "End Enum\n"
                     "@Flags Enum Layer\n"
                     "    World, Player\n"
                     "End Enum\n"
                     "Function F(n: Int, l: Layer): State\n"
                     "    If l.Has(Layer.Player) Then Return n As State\n"
                     "    Return State.Walk\n"
                     "End Function\n"
                     "Print(\"{F(0, Layer.World)}\")") ==
          with_print("function F(Int32, Layer) -> State\n"
                     "    _0 n: Int32\n"
                     "    _1 l: Layer\n"
                     "    _2: Layer\n"
                     "    _3: Bool\n"
                     "    _4: Bool\n"
                     "    _5: Bool\n"
                     "    _6: Bool\n"
                     "    _7: State\n"
                     "bb0:\n"
                     "    _2 = _1 & Layer.Player\n"
                     "    _3 = _2 == Layer.Player\n"
                     "    branch _3, bb1, bb4\n"
                     "bb1:\n"
                     "    _4 = _0 >= 0\n"
                     "    _5 = _0 <= 1\n"
                     "    _6 = _4 && _5\n"
                     "    branch _6, bb3, bb2\n"
                     "bb2:\n"
                     "    panic \"the value isn't one of State's\"\n"
                     "bb3:\n"
                     "    _7 = _0 As State\n"
                     "    return _7\n"
                     "bb4:\n"
                     "    goto bb5\n"
                     "bb5:\n"
                     "    return State.Walk\n"
                     "\n"
                     "function main\n"
                     "    _0: State\n"
                     "    _1: String\n"
                     "    _2: String\n"
                     "    _3: String[]\n"
                     "bb0:\n"
                     "    _0 = call F(Int32, Layer)(0, Layer.World)\n"
                     "    _1 = call State.Text(State)(_0)\n"
                     "    _2 = join(_1)\n"
                     "    _3 = String[] [_2]\n"
                     "    call std.core.print.Print(String[])(_3)\n"
                     "    release _3\n"
                     "    release _1\n"
                     "    return\n"
                     "\n"
                     "function State.Text(State) -> String\n"
                     "    _0 value: State\n"
                     "    _1: Bool\n"
                     "    _2: Bool\n"
                     "    _3: Int32\n"
                     "    _4: String\n"
                     "bb0:\n"
                     "    _1 = _0 == State.Idle\n"
                     "    branch _1, bb1, bb2\n"
                     "bb1:\n"
                     "    return \"Idle\"\n"
                     "bb2:\n"
                     "    _2 = _0 == State.Walk\n"
                     "    branch _2, bb3, bb4\n"
                     "bb3:\n"
                     "    return \"Walk\"\n"
                     "bb4:\n"
                     "    _3 = _0 As Int32\n"
                     "    _4 = to_text(_3)\n"
                     "    return _4\n"));
}

TEST_CASE("a Defer's statement is lowered at each way out of its block", "[mir]") {
    // Continue runs the loop body's; Return runs the loop body's, then the function's. The value
    // Return names is copied first.
    CHECK(lower_text("Import std.core\n"
                     "Function F(n: Int): Int\n"
                     "    Defer Print(\"out\")\n"
                     "    For i = 1 To n\n"
                     "        Defer Print(\"pass\")\n"
                     "        If i == 2 Then Continue\n"
                     "        If i == 3 Then Return i\n"
                     "    End For\n"
                     "    Return 0\n"
                     "End Function") ==
          with_print(std::string("function F(Int32) -> Int32\n"
                                 "    _0 n: Int32\n"
                                 "    _1 i: Int32\n"
                                 "    _2: Int32\n"
                                 "    _3: Bool\n"
                                 "    _4: Bool\n"
                                 "    _5: String[]\n"
                                 "    _6: Bool\n"
                                 "    _7: Int32\n"
                                 "    _8: String[]\n"
                                 "    _9: String[]\n"
                                 "    _10: String[]\n"
                                 "    _11: Int32\n"
                                 "    _12: UInt32\n"
                                 "    _13: UInt32\n"
                                 "    _14: Bool\n"
                                 "    _15: Int32\n"
                                 "    _16: String[]\n"
                                 "bb0:\n"
                                 "    _1 = 1\n"
                                 "    _2 = _0\n"
                                 "    goto bb1\n"
                                 "bb1:\n"
                                 "    _3 = _1 <= _2\n"
                                 "    branch _3, bb2, bb11\n"
                                 "bb2:\n"
                                 "    _4 = _1 == 2\n"
                                 "    branch _4, bb3, bb4\n"
                                 "bb3:\n"
                                 "    _5 = String[] [\"pass\"]\n"
                                 "    call std.core.print.Print(String[])(_5)\n"
                                 "    release _5\n"
                                 "    goto bb9\n"
                                 "bb4:\n"
                                 "    goto bb5\n"
                                 "bb5:\n"
                                 "    _6 = _1 == 3\n"
                                 "    branch _6, bb6, bb7\n"
                                 "bb6:\n"
                                 "    _7 = _1\n"
                                 "    _8 = String[] [\"pass\"]\n"
                                 "    call std.core.print.Print(String[])(_8)\n"
                                 "    release _8\n"
                                 "    _9 = String[] [\"out\"]\n"
                                 "    call std.core.print.Print(String[])(_9)\n"
                                 "    release _9\n"
                                 "    return _7\n"
                                 "bb7:\n"
                                 "    goto bb8\n"
                                 "bb8:\n"
                                 "    _10 = String[] [\"pass\"]\n"
                                 "    call std.core.print.Print(String[])(_10)\n"
                                 "    release _10\n"
                                 "    goto bb9\n"
                                 "bb9:\n"
                                 "    _11 = wrapping(_2 - _1)\n"
                                 "    _12 = _11 As UInt32\n"
                                 "    _13 = 1 As UInt32\n"
                                 "    _14 = _12 < _13\n"
                                 "    _15 = wrapping(_1 + 1)\n"
                                 "    branch _14, bb11, bb10\n"
                                 "bb10:\n"
                                 "    _1 = _15\n"
                                 "    goto bb2\n"
                                 "bb11:\n"
                                 "    _16 = String[] [\"out\"]\n"
                                 "    call std.core.print.Print(String[])(_16)\n"
                                 "    release _16\n"
                                 "    return 0\n"
                                 "\n") +
                     std::string(empty_main)));

    // The copy keeps what Return named from the Defer's change.
    CHECK(lower_text("Function G(): Int\n"
                     "    Local n = 1\n"
                     "    Defer n += 1\n"
                     "    Return n\n"
                     "End Function") == std::string("function G() -> Int32\n"
                                                    "    _0 n: Int32\n"
                                                    "    _1: Int32\n"
                                                    "bb0:\n"
                                                    "    _0 = 1\n"
                                                    "    _1 = _0\n"
                                                    "    _0 = _0 + 1\n"
                                                    "    return _1\n"
                                                    "\n") +
                                            std::string(empty_main));
}

TEST_CASE("a function that throws puts its error in its caller's variable", "[mir]") {
    // F's last parameter is the caller's variable. When it throws, it returns a value that G
    // never reads. G's Try? drops the error and gives Null. Error.New comes from the standard
    // library, which only adds what the program uses, after main.
    CHECK(lower_text("Import std.core\n"
                     "Class Oops Extends Error\n"
                     "End Class\n"
                     "Function F(n: Int): Int Throws Oops\n"
                     "    If n < 0 Then Throw Oops(\"negative\")\n"
                     "    Return n\n"
                     "End Function\n"
                     "Function G(): Int => Try? F(1) ?? 0") ==
          "class std.core.error.Error\n"
          "class Oops Extends std.core.error.Error\n"
          "\n"
          "function F(Int32) -> Int32\n"
          "    _0 n: Int32\n"
          "    _1 ByRef error: std.core.error.Error?\n"
          "    _2: Bool\n"
          "    _3: Oops\n"
          "    _4: std.core.error.Place\n"
          "    _5: std.core.error.Error?\n"
          "bb0:\n"
          "    _2 = _0 < 0\n"
          "    branch _2, bb1, bb2\n"
          "bb1:\n"
          "    _3 = new Oops\n"
          "    _3.Message = \"\"\n"
          "    _4 = zero std.core.error.Place\n"
          "    _3.Place = _4\n"
          "    call std.core.error.Error.New(String, std.core.error.Error?)(_3, \"negative\", "
          "Null)\n"
          "    thrown(_3)\n"
          "    _5 = _3 As std.core.error.Error?\n"
          "    _1 = _5\n"
          "    return 0\n"
          "bb2:\n"
          "    goto bb3\n"
          "bb3:\n"
          "    return _0\n"
          "\n"
          "function G() -> Int32\n"
          "    _0: Int32?\n"
          "    _1: std.core.error.Error?\n"
          "    _2: Int32\n"
          "    _3: Bool\n"
          "    _4: Int32?\n"
          "    _5: Int32\n"
          "    _6: Bool\n"
          "bb0:\n"
          "    _1 = Null\n"
          "    _2 = call F(Int32)(1, ByRef _1)\n"
          "    _3 = _1 != Null\n"
          "    branch _3, bb1, bb2\n"
          "bb1:\n"
          "    release _1\n"
          "    _0 = Null\n"
          "    goto bb3\n"
          "bb2:\n"
          "    _4 = _2 As Int32?\n"
          "    _0 = _4\n"
          "    goto bb3\n"
          "bb3:\n"
          "    _6 = _0 != Null\n"
          "    branch _6, bb4, bb5\n"
          "bb4:\n"
          "    _5 = _0.value\n"
          "    goto bb6\n"
          "bb5:\n"
          "    _5 = 0\n"
          "    goto bb6\n"
          "bb6:\n"
          "    return _5\n"
          "\n"
          "function main\n"
          "bb0:\n"
          "    return\n"
          "\n"
          "function std.core.error.Error.New(String, std.core.error.Error?)\n"
          "    _0 Self: std.core.error.Error\n"
          "    _1 message: String\n"
          "    _2 cause: std.core.error.Error?\n"
          "bb0:\n"
          "    retain _1\n"
          "    release _0.Message\n"
          "    _0.Message = _1\n"
          "    retain _2\n"
          "    release _0.Cause\n"
          "    _0.Cause = _2\n"
          "    return\n");
}

TEST_CASE("a Try's Catches test the error's Class, and its Finally runs on every way out",
          "[mir]") {
    // Where no Catch matches, top-level code lets the error out, after the Finally.
    CHECK(lower_text("Import std.core\n"
                     "Class Oops Extends Error\n"
                     "End Class\n"
                     "Function F() Throws Oops\n"
                     "End Function\n"
                     "Try\n"
                     "    F()\n"
                     "Catch e As Oops\n"
                     "    Print(e.Message)\n"
                     "Finally\n"
                     "    Print(\"done\")\n"
                     "End Try") == with_print("class std.core.error.Error\n"
                                              "class Oops Extends std.core.error.Error\n"
                                              "\n"
                                              "function F()\n"
                                              "    _0 ByRef error: std.core.error.Error?\n"
                                              "bb0:\n"
                                              "    return\n"
                                              "\n"
                                              "function main\n"
                                              "    _0: std.core.error.Error?\n"
                                              "    _1: std.core.error.Error?\n"
                                              "    _2: Bool\n"
                                              "    _3: Bool\n"
                                              "    _4 e: Oops\n"
                                              "    _5: String[]\n"
                                              "    _6: String[]\n"
                                              "    _7: String[]\n"
                                              "bb0:\n"
                                              "    _1 = Null\n"
                                              "    call F()(ByRef _1)\n"
                                              "    _2 = _1 != Null\n"
                                              "    branch _2, bb1, bb2\n"
                                              "bb1:\n"
                                              "    _0 = _1\n"
                                              "    goto bb3\n"
                                              "bb2:\n"
                                              "    goto bb6\n"
                                              "bb3:\n"
                                              "    _3 = _0 Is Oops\n"
                                              "    branch _3, bb4, bb5\n"
                                              "bb4:\n"
                                              "    _4 = downcast _0 As Oops\n"
                                              "    retain _4.Message\n"
                                              "    _5 = String[] [_4.Message]\n"
                                              "    call std.core.print.Print(String[])(_5)\n"
                                              "    release _5\n"
                                              "    release _4\n"
                                              "    goto bb6\n"
                                              "bb5:\n"
                                              "    _6 = String[] [\"done\"]\n"
                                              "    call std.core.print.Print(String[])(_6)\n"
                                              "    release _6\n"
                                              "    uncaught(_0)\n"
                                              "    unreachable\n"
                                              "bb6:\n"
                                              "    _7 = String[] [\"done\"]\n"
                                              "    call std.core.print.Print(String[])(_7)\n"
                                              "    release _7\n"
                                              "    return\n"));
}
TEST_CASE("a Weak<T> counts a weak reference, and reading it makes a strong one", "[mir]") {
    // Storing retains and releases weak references. Reading gives a new reference, which
    // Return hands to the caller.
    CHECK(lower_text("Class Node\n"
                     "    Field parent: Weak<Node>\n"
                     "End Class\n"
                     "Function Link(child: Node, parent: Node)\n"
                     "    child.parent = parent\n"
                     "End Function\n"
                     "Function Parent(child: Node): Node? => child.parent") ==
          "class Node\n"
          "\n"
          "function Link(Node, Node)\n"
          "    _0 child: Node\n"
          "    _1 parent: Node\n"
          "    _2: Weak<Node>\n"
          "bb0:\n"
          "    _2 = _1 As Weak<Node>\n"
          "    retain _2\n"
          "    release _0.parent\n"
          "    _0.parent = _2\n"
          "    return\n"
          "\n"
          "function Parent(Node) -> Node?\n"
          "    _0 child: Node\n"
          "    _1: Node?\n"
          "bb0:\n"
          "    _1 = load_weak(_0.parent)\n"
          "    return _1\n"
          "\n"
          "function main\n"
          "bb0:\n"
          "    return\n");
}

TEST_CASE("an object whose New throws is discarded instead of released", "[mir]") {
    // Discarding releases the fields without running Destroy, which could read ones that New
    // didn't set.
    CHECK(lower_text("Import std.core\n"
                     "Class Bad Extends Error\n"
                     "End Class\n"
                     "Class A\n"
                     "    New() Throws Bad\n"
                     "    End New\n"
                     "End Class\n"
                     "Function Make(): A? => Try? A()") ==
          "class std.core.error.Error\n"
          "class Bad Extends std.core.error.Error\n"
          "class A\n"
          "\n"
          "function A.New()\n"
          "    _0 Self: A\n"
          "    _1 ByRef error: std.core.error.Error?\n"
          "bb0:\n"
          "    return\n"
          "\n"
          "function Make() -> A?\n"
          "    _0: A?\n"
          "    _1: A\n"
          "    _2: std.core.error.Error?\n"
          "    _3: Bool\n"
          "    _4: A?\n"
          "bb0:\n"
          "    _1 = new A\n"
          "    _2 = Null\n"
          "    call A.New()(_1, ByRef _2)\n"
          "    _3 = _2 != Null\n"
          "    branch _3, bb1, bb2\n"
          "bb1:\n"
          "    discard(_1)\n"
          "    release _2\n"
          "    _0 = Null\n"
          "    goto bb3\n"
          "bb2:\n"
          "    _4 = _1 As A?\n"
          "    _0 = _4\n"
          "    goto bb3\n"
          "bb3:\n"
          "    return _0\n"
          "\n"
          "function main\n"
          "bb0:\n"
          "    return\n");
}

TEST_CASE("Static Fields are globals, and Properties call their Get and Set", "[mir]") {
    // A Static Method has no Self. A Struct's Set takes its variable ByRef, and += calls
    // the Get first.
    CHECK(lower_text("Struct Box\n"
                     "    Static Field made: Int = 0\n"
                     "    Field w: Int\n"
                     "    Property Size: Int\n"
                     "        Get => w\n"
                     "        Set(value)\n"
                     "            w = value\n"
                     "        End Set\n"
                     "    End Property\n"
                     "    Static Method Make(): Box\n"
                     "        made++\n"
                     "        Return Box(1)\n"
                     "    End Method\n"
                     "End Struct\n"
                     "Local b = Box.Make()\n"
                     "b.Size += 2") == "global Box.made: Int32 = 0\n"
                                       "\n"
                                       "function Box.Size() -> Int32\n"
                                       "    _0 Self: Box\n"
                                       "bb0:\n"
                                       "    return _0.w\n"
                                       "\n"
                                       "function Box.Size(Int32)\n"
                                       "    _0 ByRef Self: Box\n"
                                       "    _1 value: Int32\n"
                                       "bb0:\n"
                                       "    _0.w = _1\n"
                                       "    return\n"
                                       "\n"
                                       "function Box.Make() -> Box\n"
                                       "    _0: Box\n"
                                       "bb0:\n"
                                       "    @Box.made = @Box.made + 1\n"
                                       "    _0 = Box { w: 1 }\n"
                                       "    return _0\n"
                                       "\n"
                                       "function main\n"
                                       "    _0: Box\n"
                                       "    _1 b: Box\n"
                                       "    _2: Int32\n"
                                       "    _3: Int32\n"
                                       "bb0:\n"
                                       "    _0 = call Box.Make()()\n"
                                       "    _1 = _0\n"
                                       "    _2 = call Box.Size()(_1)\n"
                                       "    _3 = _2 + 2\n"
                                       "    call Box.Size(Int32)(ByRef _1, _3)\n"
                                       "    return\n");
}

TEST_CASE("a program gets only the library code it uses", "[mir]") {
    // Twice calls Helper, and nothing calls Unused or makes a Spare. A library Class comes with
    // all its Methods, which its Method table holds.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Function Twice(x: Int): Int => Helper(x) * 2\n"
                    "Public Function Unused()\n"
                    "End Function\n"
                    "Public Function Helper(x: Int): Int => x\n"
                    "Public Class Counter\n"
                    "    Public Field n: Int = 0\n"
                    "    Public Method Bump()\n"
                    "        n += 1\n"
                    "    End Method\n"
                    "    Public Method Drop()\n"
                    "        n -= 1\n"
                    "    End Method\n"
                    "End Class\n"
                    "Public Class Spare\n"
                    "End Class\n",
        },
    };
    const fb::compilation_t compilation = fb::compile(
        "test.fb", "Import \"./lib/a\"\nLocal c = Counter()\nLocal t = Twice(c.n)", library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    CHECK(fb::dump(fb::lower(compilation.ast, compilation.analysis)) ==
          "class lib.a.Counter\n"
          "    0: lib.a.Counter.Bump()\n"
          "    1: lib.a.Counter.Drop()\n"
          "\n"
          "function main\n"
          "    _0: lib.a.Counter\n"
          "    _1 c: lib.a.Counter\n"
          "    _2: Int32\n"
          "    _3 t: Int32\n"
          "bb0:\n"
          "    _0 = new lib.a.Counter\n"
          "    _0.n = 0\n"
          "    _1 = _0\n"
          "    _2 = call lib.a.Twice(Int32)(_1.n)\n"
          "    _3 = _2\n"
          "    release _1\n"
          "    return\n"
          "\n"
          "function lib.a.Twice(Int32) -> Int32\n"
          "    _0 x: Int32\n"
          "    _1: Int32\n"
          "    _2: Int32\n"
          "bb0:\n"
          "    _1 = call lib.a.Helper(Int32)(_0)\n"
          "    _2 = _1 * 2\n"
          "    return _2\n"
          "\n"
          "function lib.a.Counter.Bump()\n"
          "    _0 Self: lib.a.Counter\n"
          "bb0:\n"
          "    _0.n = _0.n + 1\n"
          "    return\n"
          "\n"
          "function lib.a.Counter.Drop()\n"
          "    _0 Self: lib.a.Counter\n"
          "bb0:\n"
          "    _0.n = _0.n - 1\n"
          "    return\n"
          "\n"
          "function lib.a.Helper(Int32) -> Int32\n"
          "    _0 x: Int32\n"
          "bb0:\n"
          "    return _0\n");
}

TEST_CASE("a tuple is a value of its elements, and taking one apart copies them", "[mir]") {
    // == compares each element the way its type does.
    CHECK(lower_text("Function F(): (Int, String) => (1, \"a\")\n"
                     "Local n, s = F()\n"
                     "Local same = F() == (n, s)") == "function F() -> (Int32, String)\n"
                                                      "    _0: (Int32, String)\n"
                                                      "bb0:\n"
                                                      "    _0 = (1, \"a\")\n"
                                                      "    return _0\n"
                                                      "\n"
                                                      "function main\n"
                                                      "    _0: (Int32, String)\n"
                                                      "    _1 n: Int32\n"
                                                      "    _2 s: String\n"
                                                      "    _3: (Int32, String)\n"
                                                      "    _4: (Int32, String)\n"
                                                      "    _5: Bool\n"
                                                      "    _6: Bool\n"
                                                      "    _7: Bool\n"
                                                      "    _8 same: Bool\n"
                                                      "bb0:\n"
                                                      "    _0 = call F()()\n"
                                                      "    _1 = _0.0\n"
                                                      "    retain _0.1\n"
                                                      "    _2 = _0.1\n"
                                                      "    release _0\n"
                                                      "    _3 = call F()()\n"
                                                      "    retain _2\n"
                                                      "    _4 = (_1, _2)\n"
                                                      "    _5 = _3.0 == _4.0\n"
                                                      "    _6 = equal(_3.1, _4.1)\n"
                                                      "    _7 = _5 && _6\n"
                                                      "    _8 = _7\n"
                                                      "    release _4\n"
                                                      "    release _3\n"
                                                      "    release _2\n"
                                                      "    return\n");
}

TEST_CASE("assigning a tuple's elements makes the whole tuple first", "[mir]") {
    // So a swap reads both before it changes either.
    CHECK(lower_text("Local a = \"x\"\nLocal b = \"y\"\na, b = (b, a)") ==
          "function main\n"
          "    _0 a: String\n"
          "    _1 b: String\n"
          "    _2: (String, String)\n"
          "    _3: String\n"
          "    _4: String\n"
          "bb0:\n"
          "    _0 = \"x\"\n"
          "    _1 = \"y\"\n"
          "    retain _1\n"
          "    retain _0\n"
          "    _2 = (_1, _0)\n"
          "    _3 = _2.0\n"
          "    retain _3\n"
          "    release _0\n"
          "    _0 = _3\n"
          "    _4 = _2.1\n"
          "    retain _4\n"
          "    release _1\n"
          "    _1 = _4\n"
          "    release _2\n"
          "    release _1\n"
          "    release _0\n"
          "    return\n");
}

TEST_CASE("a lambda's closure holds the cell of each Local it captures", "[mir]") {
    // The cell is shared: MakeCounter's count and the closure's are the same variable.
    CHECK(lower_text("Function MakeCounter(): Func(): Int\n"
                     "    Local count = 0\n"
                     "    Return Function()\n"
                     "        count++\n"
                     "        Return count\n"
                     "    End Function\n"
                     "End Function") == "class Function at test.fb:3:12\n"
                                        "    0: Function at test.fb:3:12\n"
                                        "class Cell<Int32>\n"
                                        "\n"
                                        "function MakeCounter() -> Func(): Int32\n"
                                        "    _0 count: Cell<Int32>\n"
                                        "    _1: Function at test.fb:3:12\n"
                                        "    _2: Func(): Int32\n"
                                        "bb0:\n"
                                        "    _0 = new Cell<Int32>\n"
                                        "    _0.value = 0\n"
                                        "    _1 = new Function at test.fb:3:12\n"
                                        "    retain _0\n"
                                        "    _1.count = _0\n"
                                        "    _2 = _1 As Func(): Int32\n"
                                        "    release _0\n"
                                        "    return _2\n"
                                        "\n"
                                        "function main\n"
                                        "bb0:\n"
                                        "    return\n"
                                        "\n"
                                        "function Function at test.fb:3:12 -> Int32\n"
                                        "    _0 closure: Function at test.fb:3:12\n"
                                        "    _1 count: Cell<Int32>\n"
                                        "bb0:\n"
                                        "    _1 = _0.count\n"
                                        "    _1.value = _1.value + 1\n"
                                        "    return _1.value\n");
}

TEST_CASE("a Function used as a value gets code that calls it", "[mir]") {
    CHECK(lower_text("Function Twice(x: Int): Int => x * 2\n"
                     "Local f: Func(Int): Int = Twice\n"
                     "Local n = f(3)") == "class Twice(Int32) as a value\n"
                                          "    0: Twice(Int32) as a value\n"
                                          "\n"
                                          "function Twice(Int32) -> Int32\n"
                                          "    _0 x: Int32\n"
                                          "    _1: Int32\n"
                                          "bb0:\n"
                                          "    _1 = _0 * 2\n"
                                          "    return _1\n"
                                          "\n"
                                          "function main\n"
                                          "    _0: Twice(Int32) as a value\n"
                                          "    _1: Func(Int32): Int32\n"
                                          "    _2 f: Func(Int32): Int32\n"
                                          "    _3: Int32\n"
                                          "    _4 n: Int32\n"
                                          "bb0:\n"
                                          "    _0 = new Twice(Int32) as a value\n"
                                          "    _1 = _0 As Func(Int32): Int32\n"
                                          "    _2 = _1\n"
                                          "    _3 = invoke _2(3)\n"
                                          "    _4 = _3\n"
                                          "    release _2\n"
                                          "    return\n"
                                          "\n"
                                          "function Twice(Int32) as a value -> Int32\n"
                                          "    _0 closure: Twice(Int32) as a value\n"
                                          "    _1 x: Int32\n"
                                          "    _2: Int32\n"
                                          "bb0:\n"
                                          "    _2 = call Twice(Int32)(_1)\n"
                                          "    return _2\n");
}

TEST_CASE("a generic Function has one instance for each set of type arguments", "[mir]") {
    CHECK(lower_text("Function First<T>(xs: T[]): T => xs[0]\n"
                     "Local a = First([1])\n"
                     "Local b = First([\"x\"])\n"
                     "Local c = First([2])") == "function main\n"
                                                "    _0: Int32[]\n"
                                                "    _1: Int32\n"
                                                "    _2 a: Int32\n"
                                                "    _3: String[]\n"
                                                "    _4: String\n"
                                                "    _5 b: String\n"
                                                "    _6: Int32[]\n"
                                                "    _7: Int32\n"
                                                "    _8 c: Int32\n"
                                                "bb0:\n"
                                                "    _0 = Int32[] [1]\n"
                                                "    _1 = call First<Int32>(Int32[])(_0)\n"
                                                "    _2 = _1\n"
                                                "    release _0\n"
                                                "    _3 = String[] [\"x\"]\n"
                                                "    _4 = call First<String>(String[])(_3)\n"
                                                "    _5 = _4\n"
                                                "    release _3\n"
                                                "    _6 = Int32[] [2]\n"
                                                "    _7 = call First<Int32>(Int32[])(_6)\n"
                                                "    _8 = _7\n"
                                                "    release _6\n"
                                                "    release _5\n"
                                                "    return\n"
                                                "\n"
                                                "function First<Int32>(Int32[]) -> Int32\n"
                                                "    _0 xs: Int32[]\n"
                                                "    _1: Int32\n"
                                                "bb0:\n"
                                                "    _1 = 0\n"
                                                "    return _0[_1]\n"
                                                "\n"
                                                "function First<String>(String[]) -> String\n"
                                                "    _0 xs: String[]\n"
                                                "    _1: Int32\n"
                                                "bb0:\n"
                                                "    _1 = 0\n"
                                                "    retain _0[_1]\n"
                                                "    return _0[_1]\n");
}

TEST_CASE("an instance of a generic Class has its own Class and Methods", "[mir]") {
    CHECK(lower_text("Class Box<T>\n"
                     "    Field Value: T\n"
                     "    New(value: T)\n"
                     "        Value = value\n"
                     "    End New\n"
                     "    Method Get(): T => Value\n"
                     "End Class\n"
                     "Local b = Box(\"x\")\n"
                     "Local v = b.Get()") == "class Box<String>\n"
                                             "    0: Box<String>.Get()\n"
                                             "\n"
                                             "function main\n"
                                             "    _0: Box<String>\n"
                                             "    _1 b: Box<String>\n"
                                             "    _2: String\n"
                                             "    _3 v: String\n"
                                             "bb0:\n"
                                             "    _0 = new Box<String>\n"
                                             "    _0.Value = \"\"\n"
                                             "    call Box<String>.New(String)(_0, \"x\")\n"
                                             "    _1 = _0\n"
                                             "    _2 = call virtual Box<String>.Get()(_1)\n"
                                             "    _3 = _2\n"
                                             "    release _3\n"
                                             "    release _1\n"
                                             "    return\n"
                                             "\n"
                                             "function Box<String>.New(String)\n"
                                             "    _0 Self: Box<String>\n"
                                             "    _1 value: String\n"
                                             "bb0:\n"
                                             "    retain _1\n"
                                             "    release _0.Value\n"
                                             "    _0.Value = _1\n"
                                             "    return\n"
                                             "\n"
                                             "function Box<String>.Get() -> String\n"
                                             "    _0 Self: Box<String>\n"
                                             "bb0:\n"
                                             "    retain _0.Value\n"
                                             "    return _0.Value\n");
}

TEST_CASE("a Class extending an instance uses the instance's Statics by name", "[mir]") {
    // Ints sees Box<Int32>'s last and Twice, and last's Null starts as an Int32?.
    CHECK(lower_text("Class Box<T>\n"
                     "    Static Field last: T? = Null\n"
                     "    Static Property Twice: Int\n"
                     "        Get => 2\n"
                     "    End Property\n"
                     "End Class\n"
                     "Class Ints Extends Box<Int>\n"
                     "    Method Fill(): Int\n"
                     "        last = 4\n"
                     "        Return Twice\n"
                     "    End Method\n"
                     "End Class\n"
                     "Local n = Ints().Fill()") == "global Box<Int32>.last: Int32? = Null\n"
                                                   "class Ints Extends Box<Int32>\n"
                                                   "    0: Box<Int32>.Twice()\n"
                                                   "    1: Ints.Fill()\n"
                                                   "class Box<Int32>\n"
                                                   "    0: Box<Int32>.Twice()\n"
                                                   "\n"
                                                   "function Ints.Fill() -> Int32\n"
                                                   "    _0 Self: Ints\n"
                                                   "    _1: Int32\n"
                                                   "bb0:\n"
                                                   "    @Box<Int32>.last = 4\n"
                                                   "    _1 = call Box<Int32>.Twice()()\n"
                                                   "    return _1\n"
                                                   "\n"
                                                   "function main\n"
                                                   "    _0: Ints\n"
                                                   "    _1: Int32\n"
                                                   "    _2 n: Int32\n"
                                                   "bb0:\n"
                                                   "    _0 = new Ints\n"
                                                   "    _1 = call virtual Ints.Fill()(_0)\n"
                                                   "    _2 = _1\n"
                                                   "    release _0\n"
                                                   "    return\n"
                                                   "\n"
                                                   "function Box<Int32>.Twice() -> Int32\n"
                                                   "bb0:\n"
                                                   "    return 2\n");
}

TEST_CASE("names outside the main module start with their module", "[mir]") {
    // Each library module has a Node, a Make, and a count of its own, which stay apart. A Method's
    // name starts with its type's, which has the module already.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Struct Node\n"
                    "    Public Field n: Int\n"
                    "    Public Method Twice(): Int => n * 2\n"
                    "End Struct\n"
                    "Public Global count = 1\n"
                    "Public Function Make(): Node => Node(count)\n"
                    "Public Function MakeA(): Int => Make().Twice()\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Public Struct Node\n"
                    "    Public Field n: Int\n"
                    "End Struct\n"
                    "Public Global count = 2\n"
                    "Public Function Make(): Node => Node(count)\n"
                    "Public Function MakeB(): Int => Make().n\n",
        },
    };
    const fb::compilation_t compilation = fb::compile(
        "test.fb", "Import \"./lib/a\"\nImport \"./lib/b\"\nLocal sum = MakeA() + MakeB()",
        library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    CHECK(fb::dump(fb::lower(compilation.ast, compilation.analysis)) ==
          "global lib.a.count: Int32 = 1\n"
          "global lib.b.count: Int32 = 2\n"
          "\n"
          "function main\n"
          "    _0: Int32\n"
          "    _1: Int32\n"
          "    _2: Int32\n"
          "    _3 sum: Int32\n"
          "bb0:\n"
          "    _0 = call lib.a.MakeA()()\n"
          "    _1 = call lib.b.MakeB()()\n"
          "    _2 = _0 + _1\n"
          "    _3 = _2\n"
          "    return\n"
          "\n"
          "function lib.a.MakeA() -> Int32\n"
          "    _0: lib.a.Node\n"
          "    _1: Int32\n"
          "bb0:\n"
          "    _0 = call lib.a.Make()()\n"
          "    _1 = call lib.a.Node.Twice()(_0)\n"
          "    return _1\n"
          "\n"
          "function lib.b.MakeB() -> Int32\n"
          "    _0: lib.b.Node\n"
          "bb0:\n"
          "    _0 = call lib.b.Make()()\n"
          "    return _0.n\n"
          "\n"
          "function lib.a.Make() -> lib.a.Node\n"
          "    _0: lib.a.Node\n"
          "bb0:\n"
          "    _0 = lib.a.Node { n: @lib.a.count }\n"
          "    return _0\n"
          "\n"
          "function lib.a.Node.Twice() -> Int32\n"
          "    _0 Self: lib.a.Node\n"
          "    _1: Int32\n"
          "bb0:\n"
          "    _1 = _0.n * 2\n"
          "    return _1\n"
          "\n"
          "function lib.b.Make() -> lib.b.Node\n"
          "    _0: lib.b.Node\n"
          "bb0:\n"
          "    _0 = lib.b.Node { n: @lib.b.count }\n"
          "    return _0\n");
}

TEST_CASE("a library's Statics, Enums, and Extends start with their module", "[mir]") {
    // Static Fields, a generic one's instances, Enum constants, and the Enum's Text function
    // start with the type's module. What an Extend adds starts with the Extend's own module,
    // then the type it extends: lib.b's Hit on lib.a's Enemy, and lib.a's Twice on Int32.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Enum State\n"
                    "    IDLE\n"
                    "    RUN\n"
                    "End Enum\n"
                    "Public Class Enemy\n"
                    "    Public Static Field count: Int = 0\n"
                    "End Class\n"
                    "Public Class Box<T>\n"
                    "    Public Static Field made: Int = 0\n"
                    "End Class\n"
                    "Extend Int\n"
                    "    Public Method Twice(): Int => Self * 2\n"
                    "End Extend\n"
                    "Public Function Bump(): Int\n"
                    "    Enemy.count += 1\n"
                    "    Box<Int>.made += 1\n"
                    "    Return (2).Twice()\n"
                    "End Function\n"
                    "Public Function Pick(): String => \"{State.RUN}\"\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./a\"\n"
                    "Extend Enemy\n"
                    "    Public Method Hit(): Int => 3\n"
                    "End Extend\n",
        },
    };
    const fb::compilation_t compilation = fb::compile("test.fb",
                                                      "Import \"./lib/a\"\n"
                                                      "Import \"./lib/b\"\n"
                                                      "Local n = Bump() + Enemy().Hit()\n"
                                                      "Local s = Pick()",
                                                      library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    CHECK(fb::dump(fb::lower(compilation.ast, compilation.analysis)) ==
          "global lib.a.Enemy.count: Int32 = 0\n"
          "global lib.a.Box<Int32>.made: Int32 = 0\n"
          "class lib.a.Enemy\n"
          "\n"
          "function main\n"
          "    _0: Int32\n"
          "    _1: lib.a.Enemy\n"
          "    _2: Int32\n"
          "    _3: Int32\n"
          "    _4 n: Int32\n"
          "    _5: String\n"
          "    _6 s: String\n"
          "bb0:\n"
          "    _0 = call lib.a.Bump()()\n"
          "    _1 = new lib.a.Enemy\n"
          "    _2 = call lib.b.lib.a.Enemy.Hit()(_1)\n"
          "    _3 = _0 + _2\n"
          "    _4 = _3\n"
          "    release _1\n"
          "    _5 = call lib.a.Pick()()\n"
          "    _6 = _5\n"
          "    release _6\n"
          "    return\n"
          "\n"
          "function lib.a.Bump() -> Int32\n"
          "    _0: Int32\n"
          "bb0:\n"
          "    @lib.a.Enemy.count = @lib.a.Enemy.count + 1\n"
          "    @lib.a.Box<Int32>.made = @lib.a.Box<Int32>.made + 1\n"
          "    _0 = call lib.a.Int32.Twice()(2)\n"
          "    return _0\n"
          "\n"
          "function lib.b.lib.a.Enemy.Hit() -> Int32\n"
          "    _0 Self: lib.a.Enemy\n"
          "bb0:\n"
          "    return 3\n"
          "\n"
          "function lib.a.Pick() -> String\n"
          "    _0: String\n"
          "    _1: String\n"
          "bb0:\n"
          "    _0 = call lib.a.State.Text(lib.a.State)(lib.a.State.RUN)\n"
          "    _1 = join(_0)\n"
          "    release _0\n"
          "    return _1\n"
          "\n"
          "function lib.a.Int32.Twice() -> Int32\n"
          "    _0 Self: Int32\n"
          "    _1: Int32\n"
          "bb0:\n"
          "    _1 = _0 * 2\n"
          "    return _1\n"
          "\n"
          "function lib.a.State.Text(lib.a.State) -> String\n"
          "    _0 value: lib.a.State\n"
          "    _1: Bool\n"
          "    _2: Bool\n"
          "    _3: Int32\n"
          "    _4: String\n"
          "bb0:\n"
          "    _1 = _0 == lib.a.State.IDLE\n"
          "    branch _1, bb1, bb2\n"
          "bb1:\n"
          "    return \"IDLE\"\n"
          "bb2:\n"
          "    _2 = _0 == lib.a.State.RUN\n"
          "    branch _2, bb3, bb4\n"
          "bb3:\n"
          "    return \"RUN\"\n"
          "bb4:\n"
          "    _3 = _0 As Int32\n"
          "    _4 = to_text(_3)\n"
          "    return _4\n");
}

TEST_CASE("two modules' Functions used as values make two Classes", "[mir]") {
    // The Class is in its Function's module, and names its parameter types with theirs.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Struct Node\n"
                    "    Public Field n: Int\n"
                    "End Struct\n"
                    "Public Function Make(n: Node): Int => n.n\n"
                    "Public Function MakerA(): Func(Node): Int => Make\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Public Struct Node\n"
                    "    Public Field n: Int\n"
                    "End Struct\n"
                    "Public Function Make(n: Node): Int => n.n * 2\n"
                    "Public Function MakerB(): Func(Node): Int => Make\n",
        },
    };
    const fb::compilation_t compilation = fb::compile(
        "test.fb", "Import \"./lib/a\"\nImport \"./lib/b\"\nLocal a = MakerA()\nLocal b = MakerB()",
        library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    CHECK(fb::dump(fb::lower(compilation.ast, compilation.analysis)) ==
          "class lib.a.Make(lib.a.Node) as a value\n"
          "    0: lib.a.Make(lib.a.Node) as a value\n"
          "class lib.b.Make(lib.b.Node) as a value\n"
          "    0: lib.b.Make(lib.b.Node) as a value\n"
          "\n"
          "function main\n"
          "    _0: Func(lib.a.Node): Int32\n"
          "    _1 a: Func(lib.a.Node): Int32\n"
          "    _2: Func(lib.b.Node): Int32\n"
          "    _3 b: Func(lib.b.Node): Int32\n"
          "bb0:\n"
          "    _0 = call lib.a.MakerA()()\n"
          "    _1 = _0\n"
          "    _2 = call lib.b.MakerB()()\n"
          "    _3 = _2\n"
          "    release _3\n"
          "    release _1\n"
          "    return\n"
          "\n"
          "function lib.a.MakerA() -> Func(lib.a.Node): Int32\n"
          "    _0: lib.a.Make(lib.a.Node) as a value\n"
          "    _1: Func(lib.a.Node): Int32\n"
          "bb0:\n"
          "    _0 = new lib.a.Make(lib.a.Node) as a value\n"
          "    _1 = _0 As Func(lib.a.Node): Int32\n"
          "    return _1\n"
          "\n"
          "function lib.b.MakerB() -> Func(lib.b.Node): Int32\n"
          "    _0: lib.b.Make(lib.b.Node) as a value\n"
          "    _1: Func(lib.b.Node): Int32\n"
          "bb0:\n"
          "    _0 = new lib.b.Make(lib.b.Node) as a value\n"
          "    _1 = _0 As Func(lib.b.Node): Int32\n"
          "    return _1\n"
          "\n"
          "function lib.a.Make(lib.a.Node) as a value -> Int32\n"
          "    _0 closure: lib.a.Make(lib.a.Node) as a value\n"
          "    _1 n: lib.a.Node\n"
          "    _2: Int32\n"
          "bb0:\n"
          "    _2 = call lib.a.Make(lib.a.Node)(_1)\n"
          "    return _2\n"
          "\n"
          "function lib.a.Make(lib.a.Node) -> Int32\n"
          "    _0 n: lib.a.Node\n"
          "bb0:\n"
          "    return _0.n\n"
          "\n"
          "function lib.b.Make(lib.b.Node) as a value -> Int32\n"
          "    _0 closure: lib.b.Make(lib.b.Node) as a value\n"
          "    _1 n: lib.b.Node\n"
          "    _2: Int32\n"
          "bb0:\n"
          "    _2 = call lib.b.Make(lib.b.Node)(_1)\n"
          "    return _2\n"
          "\n"
          "function lib.b.Make(lib.b.Node) -> Int32\n"
          "    _0 n: lib.b.Node\n"
          "    _1: Int32\n"
          "bb0:\n"
          "    _1 = _0.n * 2\n"
          "    return _1\n");
}

TEST_CASE("a library's generic Function is lowered for the arguments it's used with", "[mir]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text =
                "Public Function Id<T>(x: T): T => x\nPublic Function Unused<T>()\nEnd Function\n",
        },
    };
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Import \"./lib/a\"\nLocal n = Id(1)", library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    CHECK(fb::dump(fb::lower(compilation.ast, compilation.analysis)) ==
          "function main\n"
          "    _0: Int32\n"
          "    _1 n: Int32\n"
          "bb0:\n"
          "    _0 = call lib.a.Id<Int32>(Int32)(1)\n"
          "    _1 = _0\n"
          "    return\n"
          "\n"
          "function lib.a.Id<Int32>(Int32) -> Int32\n"
          "    _0 x: Int32\n"
          "bb0:\n"
          "    return _0\n");
}

TEST_CASE("an operator a Struct defines calls its Operator", "[mir]") {
    // "!=" calls "==" and flips it, and "+=" assigns what "+" gives.
    CHECK(lower_text("Struct Money\n"
                     "    Field Cents: Long\n"
                     "    Operator +(a: Money, b: Money): Money => Money(a.Cents + b.Cents)\n"
                     "    Operator ==(a: Money, b: Money): Bool => a.Cents == b.Cents\n"
                     "End Struct\n"
                     "Local a = Money(1)\n"
                     "Local b = a + a\n"
                     "Local same = a != b\n"
                     "b += a") == "function Money.Operator +(Money, Money) -> Money\n"
                                  "    _0 a: Money\n"
                                  "    _1 b: Money\n"
                                  "    _2: Int64\n"
                                  "    _3: Money\n"
                                  "bb0:\n"
                                  "    _2 = _0.Cents + _1.Cents\n"
                                  "    _3 = Money { Cents: _2 }\n"
                                  "    return _3\n"
                                  "\n"
                                  "function Money.Operator ==(Money, Money) -> Bool\n"
                                  "    _0 a: Money\n"
                                  "    _1 b: Money\n"
                                  "    _2: Bool\n"
                                  "bb0:\n"
                                  "    _2 = _0.Cents == _1.Cents\n"
                                  "    return _2\n"
                                  "\n"
                                  "function main\n"
                                  "    _0: Money\n"
                                  "    _1 a: Money\n"
                                  "    _2: Money\n"
                                  "    _3 b: Money\n"
                                  "    _4: Bool\n"
                                  "    _5: Bool\n"
                                  "    _6 same: Bool\n"
                                  "    _7: Money\n"
                                  "bb0:\n"
                                  "    _0 = Money { Cents: 1 }\n"
                                  "    _1 = _0\n"
                                  "    _2 = call Money.Operator +(Money, Money)(_1, _1)\n"
                                  "    _3 = _2\n"
                                  "    _4 = call Money.Operator ==(Money, Money)(_1, _3)\n"
                                  "    _5 = !_4\n"
                                  "    _6 = _5\n"
                                  "    _7 = call Money.Operator +(Money, Money)(_3, _1)\n"
                                  "    _3 = _7\n"
                                  "    return\n");
}

TEST_CASE("a call through an Interface goes through its Class's table", "[mir]") {
    // Square gets Twice from the default, which calls Area through the Method table, and
    // Shape's own Twice calls it through Shape's table.
    CHECK(lower_text("Interface Shape\n"
                     "    Method Area(): Int\n"
                     "    Method Twice(): Int => Area() * 2\n"
                     "End Interface\n"
                     "Class Square Implements Shape\n"
                     "    Field Side: Int\n"
                     "    Method Area(): Int => Side * Side\n"
                     "End Class\n"
                     "Local s: Shape = Square()\n"
                     "Local a = s.Twice()") == "class Square\n"
                                               "    0: Square.Area()\n"
                                               "    1: Square.Twice()\n"
                                               "    Shape 0: Square.Area()\n"
                                               "    Shape 1: Square.Twice()\n"
                                               "\n"
                                               "function Square.Area() -> Int32\n"
                                               "    _0 Self: Square\n"
                                               "    _1: Int32\n"
                                               "bb0:\n"
                                               "    _1 = _0.Side * _0.Side\n"
                                               "    return _1\n"
                                               "\n"
                                               "function main\n"
                                               "    _0: Square\n"
                                               "    _1: Shape\n"
                                               "    _2 s: Shape\n"
                                               "    _3: Int32\n"
                                               "    _4 a: Int32\n"
                                               "bb0:\n"
                                               "    _0 = new Square\n"
                                               "    _1 = _0 As Shape\n"
                                               "    _2 = _1\n"
                                               "    _3 = call interface Shape.Twice()(_2)\n"
                                               "    _4 = _3\n"
                                               "    release _2\n"
                                               "    return\n"
                                               "\n"
                                               "function Shape.Twice() -> Int32\n"
                                               "    _0 Self: Shape\n"
                                               "    _1: Int32\n"
                                               "    _2: Int32\n"
                                               "bb0:\n"
                                               "    _1 = call interface Shape.Area()(_0)\n"
                                               "    _2 = _1 * 2\n"
                                               "    return _2\n"
                                               "\n"
                                               "function Square.Twice() -> Int32\n"
                                               "    _0 Self: Square\n"
                                               "    _1: Int32\n"
                                               "    _2: Int32\n"
                                               "bb0:\n"
                                               "    _1 = call virtual Square.Area()(_0)\n"
                                               "    _2 = _1 * 2\n"
                                               "    return _2\n"
                                               "\n"
                                               "function Shape.Area() -> Int32\n"
                                               "    _0 Self: Shape\n"
                                               "bb0:\n"
                                               "    panic \"an Abstract Method was called\"\n");
}

TEST_CASE("in generic code, an Interface's member is the type argument's", "[mir]") {
    // T.Zero() and + are Money's own.
    CHECK(lower_text("Interface Summable\n"
                     "    Static Method Zero(): Self\n"
                     "    Operator +(a: Self, b: Self): Self\n"
                     "End Interface\n"
                     "Struct Money Implements Summable\n"
                     "    Field Cents: Long\n"
                     "    Static Method Zero(): Money => Money(0)\n"
                     "    Operator +(a: Money, b: Money): Money => a\n"
                     "End Struct\n"
                     "Function Double<T: Summable>(x: T): T => T.Zero() + x + x\n"
                     "Local m = Double(Money(1))") ==
          "function Money.Zero() -> Money\n"
          "    _0: Money\n"
          "bb0:\n"
          "    _0 = Money { Cents: 0 }\n"
          "    return _0\n"
          "\n"
          "function Money.Operator +(Money, Money) -> Money\n"
          "    _0 a: Money\n"
          "    _1 b: Money\n"
          "bb0:\n"
          "    return _0\n"
          "\n"
          "function main\n"
          "    _0: Money\n"
          "    _1: Money\n"
          "    _2 m: Money\n"
          "bb0:\n"
          "    _0 = Money { Cents: 1 }\n"
          "    _1 = call Double<Money>(Money)(_0)\n"
          "    _2 = _1\n"
          "    return\n"
          "\n"
          "function Double<Money>(Money) -> Money\n"
          "    _0 x: Money\n"
          "    _1: Money\n"
          "    _2: Money\n"
          "    _3: Money\n"
          "bb0:\n"
          "    _1 = call Money.Zero()()\n"
          "    _2 = call Money.Operator +(Money, Money)(_1, _0)\n"
          "    _3 = call Money.Operator +(Money, Money)(_2, _0)\n"
          "    return _3\n");
}

TEST_CASE("a generic default runs with the type for Self and its own type arguments", "[mir]") {
    CHECK(lower_text("Interface Picker\n"
                     "    Method Pick<U>(a: U, b: U): U => b\n"
                     "End Interface\n"
                     "Struct Pile Implements Picker\n"
                     "End Struct\n"
                     "Local n = Pile().Pick(1, 2)") ==
          "function main\n"
          "    _0: Pile\n"
          "    _1: Int32\n"
          "    _2 n: Int32\n"
          "bb0:\n"
          "    _0 = Pile {  }\n"
          "    _1 = call Pile.Pick<Int32>(Int32, Int32)(_0, 1, 2)\n"
          "    _2 = _1\n"
          "    return\n"
          "\n"
          "function Pile.Pick<Int32>(Int32, Int32) -> Int32\n"
          "    _0 Self: Pile\n"
          "    _1 a: Int32\n"
          "    _2 b: Int32\n"
          "bb0:\n"
          "    return _2\n");
}

TEST_CASE("arrays compare their elements in a function made for their type", "[mir]") {
    CHECK(lower_text("Local same = [1] == [2]") ==
          "function main\n"
          "    _0: Int32[]\n"
          "    _1: Int32[]\n"
          "    _2: Bool\n"
          "    _3 same: Bool\n"
          "bb0:\n"
          "    _0 = Int32[] [1]\n"
          "    _1 = Int32[] [2]\n"
          "    _2 = call Int32[].Operator ==(Int32[], Int32[])(_0, _1)\n"
          "    _3 = _2\n"
          "    release _1\n"
          "    release _0\n"
          "    return\n"
          "\n"
          "function Int32[].Operator ==(Int32[], Int32[]) -> Bool\n"
          "    _0 a: Int32[]\n"
          "    _1 b: Int32[]\n"
          "    _2: Int32\n"
          "    _3: Int32\n"
          "    _4: Bool\n"
          "    _5: Int32\n"
          "    _6: Int32\n"
          "    _7: Bool\n"
          "    _8: Bool\n"
          "bb0:\n"
          "    _2 = count(_0)\n"
          "    _3 = count(_1)\n"
          "    _4 = _2 == _3\n"
          "    branch _4, bb1, bb2\n"
          "bb1:\n"
          "    _5 = 0\n"
          "    goto bb3\n"
          "bb2:\n"
          "    return False\n"
          "bb3:\n"
          "    _6 = count(_0)\n"
          "    _7 = _5 < _6\n"
          "    branch _7, bb4, bb5\n"
          "bb4:\n"
          "    _8 = _0[_5] == _1[_5]\n"
          "    branch _8, bb6, bb7\n"
          "bb5:\n"
          "    return True\n"
          "bb6:\n"
          "    _5 = wrapping(_5 + 1)\n"
          "    goto bb3\n"
          "bb7:\n"
          "    return False\n");
}

TEST_CASE("what an Extend adds is a function of its type, and T[]'s has an instance for each",
          "[mir]") {
    CHECK(lower_text("Extend Int\n"
                     "    Method Doubled(): Int => Self * 2\n"
                     "End Extend\n"
                     "Extend T[]\n"
                     "    Method Last(): T => Self[Self.Count - 1]\n"
                     "End Extend\n"
                     "Local a = 5.Doubled()\n"
                     "Local b = [1].Last()") == "function Int32.Doubled() -> Int32\n"
                                                "    _0 Self: Int32\n"
                                                "    _1: Int32\n"
                                                "bb0:\n"
                                                "    _1 = _0 * 2\n"
                                                "    return _1\n"
                                                "\n"
                                                "function main\n"
                                                "    _0: Int32\n"
                                                "    _1 a: Int32\n"
                                                "    _2: Int32[]\n"
                                                "    _3: Int32\n"
                                                "    _4 b: Int32\n"
                                                "bb0:\n"
                                                "    _0 = call Int32.Doubled()(5)\n"
                                                "    _1 = _0\n"
                                                "    _2 = Int32[] [1]\n"
                                                "    _3 = call Int32[].Last()(_2)\n"
                                                "    _4 = _3\n"
                                                "    release _2\n"
                                                "    return\n"
                                                "\n"
                                                "function Int32[].Last() -> Int32\n"
                                                "    _0 Self: Int32[]\n"
                                                "    _1: Int32\n"
                                                "    _2: Int32\n"
                                                "    _3: Int32\n"
                                                "bb0:\n"
                                                "    _1 = count(_0)\n"
                                                "    _2 = _1 - 1\n"
                                                "    _3 = _2\n"
                                                "    return _0[_3]\n");
}

TEST_CASE("For ... In over an Iterator calls its Next until it gives Null", "[mir]") {
    // The loop goes over a copy, which Next changes, and x takes what Next gave.
    CHECK(lower_text("Import std.core\n"
                     "Struct Countdown Implements Iterator<Int>\n"
                     "    Field N: Int\n"
                     "    ByRef Method Next(): Int?\n"
                     "        N -= 1\n"
                     "        Return N\n"
                     "    End Method\n"
                     "End Struct\n"
                     "For x In Countdown(2)\n"
                     "    Print(\"{x}\")\n"
                     "End For") == with_print("function Countdown.Next() -> Int32?\n"
                                              "    _0 ByRef Self: Countdown\n"
                                              "    _1: Int32?\n"
                                              "bb0:\n"
                                              "    _0.N = _0.N - 1\n"
                                              "    _1 = _0.N As Int32?\n"
                                              "    return _1\n"
                                              "\n"
                                              "function main\n"
                                              "    _0: Countdown\n"
                                              "    _1: Countdown\n"
                                              "    _2: Int32?\n"
                                              "    _3: Bool\n"
                                              "    _4 x: Int32\n"
                                              "    _5: String\n"
                                              "    _6: String\n"
                                              "    _7: String[]\n"
                                              "bb0:\n"
                                              "    _0 = Countdown { N: 2 }\n"
                                              "    _1 = _0\n"
                                              "    goto bb1\n"
                                              "bb1:\n"
                                              "    _2 = call Countdown.Next()(ByRef _1)\n"
                                              "    _3 = _2 != Null\n"
                                              "    branch _3, bb2, bb4\n"
                                              "bb2:\n"
                                              "    _4 = _2.value\n"
                                              "    _5 = to_text(_4)\n"
                                              "    _6 = join(_5)\n"
                                              "    _7 = String[] [_6]\n"
                                              "    call std.core.print.Print(String[])(_7)\n"
                                              "    release _7\n"
                                              "    release _5\n"
                                              "    goto bb3\n"
                                              "bb3:\n"
                                              "    goto bb1\n"
                                              "bb4:\n"
                                              "    return\n"));
}
TEST_CASE("a slice checks its bounds, then copies the elements into a new array", "[mir]") {
    CHECK(lower_text("Local xs = [1, 2, 3]\nLocal s = xs[1..]") ==
          "function main\n"
          "    _0: Int32[]\n"
          "    _1 xs: Int32[]\n"
          "    _2: Int32\n"
          "    _3: Int64\n"
          "    _4: Bool\n"
          "    _5: Bool\n"
          "    _6: Bool\n"
          "    _7: Bool\n"
          "    _8: Bool\n"
          "    _9: String\n"
          "    _10: String\n"
          "    _11: String\n"
          "    _12: String\n"
          "    _13: Int32[]\n"
          "    _14: Int64\n"
          "    _15: Bool\n"
          "    _16 s: Int32[]\n"
          "bb0:\n"
          "    _0 = Int32[] [1, 2, 3]\n"
          "    _1 = _0\n"
          "    _2 = count(_1)\n"
          "    _3 = _2 As Int64\n"
          "    _4 = 1 >= 0\n"
          "    _5 = 1 <= _3\n"
          "    _6 = _3 <= _3\n"
          "    _7 = _4 && _5\n"
          "    _8 = _7 && _6\n"
          "    branch _8, bb2, bb1\n"
          "bb1:\n"
          "    _9 = to_text(1)\n"
          "    _10 = to_text(_3)\n"
          "    _11 = to_text(_3)\n"
          "    _12 = join(\"slice \", _9, \"..\", _10, \" is out of range for \", _11, \" "
          "elements\")\n"
          "    panic _12\n"
          "bb2:\n"
          "    _13 = Int32[] []\n"
          "    _14 = 1\n"
          "    goto bb3\n"
          "bb3:\n"
          "    _15 = _14 < _3\n"
          "    branch _15, bb4, bb5\n"
          "bb4:\n"
          "    add(_13, _1[_14])\n"
          "    _14 = wrapping(_14 + 1)\n"
          "    goto bb3\n"
          "bb5:\n"
          "    _16 = _13\n"
          "    release _16\n"
          "    release _1\n"
          "    return\n");
}

TEST_CASE("a variadic parameter's arguments are packed into a new array", "[mir]") {
    CHECK(lower_text("Function Count(names: String...): Int => names.Count\n"
                     "Local n = Count(\"a\", \"b\")\n"
                     "Local c = \"hi\".Chars") == "function Count(String[]) -> Int32\n"
                                                  "    _0 names: String[]\n"
                                                  "    _1: Int32\n"
                                                  "bb0:\n"
                                                  "    _1 = count(_0)\n"
                                                  "    return _1\n"
                                                  "\n"
                                                  "function main\n"
                                                  "    _0: String[]\n"
                                                  "    _1: Int32\n"
                                                  "    _2 n: Int32\n"
                                                  "    _3: Int32[]\n"
                                                  "    _4 c: Int32[]\n"
                                                  "bb0:\n"
                                                  "    _0 = String[] [\"a\", \"b\"]\n"
                                                  "    _1 = call Count(String[])(_0)\n"
                                                  "    _2 = _1\n"
                                                  "    release _0\n"
                                                  "    _3 = Int32[] []\n"
                                                  "    string_chars(\"hi\", _3)\n"
                                                  "    _4 = _3\n"
                                                  "    release _4\n"
                                                  "    return\n");
}

TEST_CASE("@Inline marks the function, and a Const Function's call with known arguments is its "
          "value",
          "[mir]") {
    CHECK(lower_text("@Inline Function Twice(x: Int): Int => x * 2\n"
                     "Const Function Three(): Int => 3\n"
                     "Local y = Twice(Three())") == "@Inline function Twice(Int32) -> Int32\n"
                                                    "    _0 x: Int32\n"
                                                    "    _1: Int32\n"
                                                    "bb0:\n"
                                                    "    _1 = _0 * 2\n"
                                                    "    return _1\n"
                                                    "\n"
                                                    "function Three() -> Int32\n"
                                                    "bb0:\n"
                                                    "    return 3\n"
                                                    "\n"
                                                    "function main\n"
                                                    "    _0: Int32\n"
                                                    "    _1 y: Int32\n"
                                                    "bb0:\n"
                                                    "    _0 = call Twice(Int32)(3)\n"
                                                    "    _1 = _0\n"
                                                    "    return\n");
    // So does @PanicsAtCaller, which codegen gives each call's place.
    CHECK(lower_text("@PanicsAtCaller Function Half(x: Int): Int => x / 2\nLocal y = Half(4)")
              .starts_with("@PanicsAtCaller function Half(Int32) -> Int32\n"));
}

TEST_CASE("a Const type parameter is its instance's value", "[mir]") {
    CHECK(lower_text("Function Size<Const N: Int>(v: Int[N]): Int => N\n"
                     "Local a: Int[3]\n"
                     "Local n = Size(a)") == "function main\n"
                                             "    _0: Int32[3]\n"
                                             "    _1 a: Int32[3]\n"
                                             "    _2: Int32\n"
                                             "    _3 n: Int32\n"
                                             "bb0:\n"
                                             "    _0 = zero Int32[3]\n"
                                             "    _1 = _0\n"
                                             "    _2 = call Size<3>(Int32[3])(_1)\n"
                                             "    _3 = _2\n"
                                             "    return\n"
                                             "\n"
                                             "function Size<3>(Int32[3]) -> Int32\n"
                                             "    _0 v: Int32[3]\n"
                                             "bb0:\n"
                                             "    return 3\n");
}

TEST_CASE("For Field makes a copy of its body for each field, where what it has is constants",
          "[mir]") {
    CHECK(lower_text("Attribute Key(Name: String)\n"
                     "Struct P\n"
                     "    @Key(\"px\") Field X: Int\n"
                     "    Field Y: Bool\n"
                     "End Struct\n"
                     "For Field f In P\n"
                     "    If f.Index == 1 Then Break\n"
                     "    Local key = f.Attribute(Key)\n"
                     "End For") == "function main\n"
                                   "    _0: Bool\n"
                                   "    _1: Key\n"
                                   "    _2: Key?\n"
                                   "    _3 key: Key?\n"
                                   "    _4: Bool\n"
                                   "    _5 key: Key?\n"
                                   "bb0:\n"
                                   "    _0 = 0 == 1\n"
                                   "    branch _0, bb1, bb2\n"
                                   "bb1:\n"
                                   "    goto bb9\n"
                                   "bb2:\n"
                                   "    goto bb3\n"
                                   "bb3:\n"
                                   "    _1 = Key { Name: \"px\" }\n"
                                   "    _2 = _1 As Key?\n"
                                   "    _3 = _2\n"
                                   "    release _3\n"
                                   "    goto bb4\n"
                                   "bb4:\n"
                                   "    _4 = 1 == 1\n"
                                   "    branch _4, bb5, bb6\n"
                                   "bb5:\n"
                                   "    goto bb9\n"
                                   "bb6:\n"
                                   "    goto bb7\n"
                                   "bb7:\n"
                                   "    _5 = Null\n"
                                   "    release _5\n"
                                   "    goto bb8\n"
                                   "bb8:\n"
                                   "    goto bb9\n"
                                   "bb9:\n"
                                   "    return\n");
}

TEST_CASE("a Checked Method tests for overflow, then wraps", "[mir]") {
    CHECK(lower_text("Function Add(a: Int, b: Int): Int? => a.CheckedAdd(b)\n"
                     "Function Wrap(a: Byte): Byte => a.WrappingNegate()") ==
          "function Add(Int32, Int32) -> Int32?\n"
          "    _0 a: Int32\n"
          "    _1 b: Int32\n"
          "    _2: Int32?\n"
          "    _3: Bool\n"
          "    _4: Int32\n"
          "    _5: Int32?\n"
          "bb0:\n"
          "    _3 = overflows(_0 + _1)\n"
          "    branch _3, bb1, bb2\n"
          "bb1:\n"
          "    _2 = Null\n"
          "    goto bb3\n"
          "bb2:\n"
          "    _4 = wrapping(_0 + _1)\n"
          "    _5 = _4 As Int32?\n"
          "    _2 = _5\n"
          "    goto bb3\n"
          "bb3:\n"
          "    return _2\n"
          "\n"
          "function Wrap(UInt8) -> UInt8\n"
          "    _0 a: UInt8\n"
          "    _1: UInt8\n"
          "bb0:\n"
          "    _1 = wrapping(0 - _0)\n"
          "    return _1\n"
          "\n"
          "function main\n"
          "bb0:\n"
          "    return\n");
}

TEST_CASE("an Extern Function is a declaration, and a String goes to C as a copy", "[mir]") {
    // The copy has a NUL after it, and it's freed right after the call. ByRef passes the
    // variable's address.
    CHECK(lower_text("Extern \"C\"\n"
                     "    Function strlen(text: CString): UInt64\n"
                     "    Function frexp(x: Float64, ByRef exponent: Int32): Float64\n"
                     "End Extern\n"
                     "Local e: Int32 = 0\n"
                     "Local n = strlen(\"hi\") + 1\n"
                     "Local m = frexp(2.0, ByRef e)") ==
          "extern function strlen(CString) -> UInt64 = \"strlen\"\n"
          "    _0 text: CString\n"
          "\n"
          "extern function frexp(Float64, ByRef Int32) -> Float64 = \"frexp\"\n"
          "    _0 x: Float64\n"
          "    _1 ByRef exponent: Int32\n"
          "\n"
          "function main\n"
          "    _0 e: Int32\n"
          "    _1: CString\n"
          "    _2: UInt64\n"
          "    _3: UInt64\n"
          "    _4 n: UInt64\n"
          "    _5: Float64\n"
          "    _6 m: Float64\n"
          "bb0:\n"
          "    _0 = 0\n"
          "    _1 = cstring_new(\"hi\")\n"
          "    _2 = call strlen(CString)(_1)\n"
          "    cstring_free(_1)\n"
          "    _3 = _2 + 1\n"
          "    _4 = _3\n"
          "    _5 = call frexp(Float64, ByRef Int32)(2, ByRef _0)\n"
          "    _6 = _5\n"
          "    return\n");
}

TEST_CASE("a Ptr is an address, which its Value and indexes step through", "[mir]") {
    // AddressOf gives a place's address, a Ptr steps by its T's size, and its Value and elements
    // are places.
    CHECK(lower_text("Local n: Int32 = 1\n"
                     "Unsafe\n"
                     "    Local p = AddressOf(n)\n"
                     "    Local q = p + 1\n"
                     "    Local d = q - p\n"
                     "    p.Value = 2\n"
                     "    q[-1] = 3\n"
                     "    Local b = p As Ptr<UInt8>\n"
                     "End Unsafe\n") == "function main\n"
                                        "    _0 n: Int32\n"
                                        "    _1: Ptr<Int32>\n"
                                        "    _2 p: Ptr<Int32>\n"
                                        "    _3: Ptr<Int32>\n"
                                        "    _4 q: Ptr<Int32>\n"
                                        "    _5: Int64\n"
                                        "    _6 d: Int64\n"
                                        "    _7: Int64\n"
                                        "    _8: Ptr<UInt8>\n"
                                        "    _9 b: Ptr<UInt8>\n"
                                        "bb0:\n"
                                        "    _0 = 1\n"
                                        "    _1 = AddressOf _0\n"
                                        "    _2 = _1\n"
                                        "    _3 = Ptr _2 + 1\n"
                                        "    _4 = _3\n"
                                        "    _5 = Ptr _4 - _2\n"
                                        "    _6 = _5\n"
                                        "    _2.Value = 2\n"
                                        "    _7 = -1\n"
                                        "    _4[_7] = 3\n"
                                        "    _8 = _2 As Ptr<UInt8>\n"
                                        "    _9 = _8\n"
                                        "    return\n");
}

TEST_CASE("what two modules' Extends add to one type has two names", "[mir]") {
    // src.lib's module name starts src.lib.a's, and its Extend still names what it adds itself.
    const std::vector<fb::library_file_t> library{
        {
            .path = "src/lib/a.fb",
            .text = "Public Class Node\n"
                    "    Public Field v: Int = 5\n"
                    "End Class\n"
                    "Extend Node\n"
                    "    Public Method Foo(): Int => v + 100\n"
                    "End Extend\n",
        },
        {
            .path = "src/lib.fb",
            .text = "Import \"./lib/a\"\n"
                    "Extend Node\n"
                    "    Public Method Foo(): Int => v + 1\n"
                    "End Extend\n"
                    "Public Function UseFoo(n: Node): Int => n.Foo()\n",
        },
        {
            .path = "src/uses.fb",
            .text = "Import \"./lib\"\n"
                    "Import \"./lib/a\"\n"
                    "Public Function Use(n: Node): Int => UseFoo(n)\n",
        },
    };
    const fb::compilation_t compilation =
        fb::compile("test.fb",
                    "Import \"./src/lib/a\"\nImport \"./src/uses\"\nLocal n = Node()\n"
                    "Local a = n.Foo() + Use(n)",
                    library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    const std::string mir = fb::dump(fb::lower(compilation.ast, compilation.analysis));
    CHECK(mir.contains("function src.lib.a.Node.Foo() -> Int32\n"));
    CHECK(mir.contains("function src.lib.src.lib.a.Node.Foo() -> Int32\n"));
}

TEST_CASE("functions and Globals that come out with one name are numbered", "[mir]") {
    // Module x's Class y and module x.y both name their members "x.y.".
    const std::vector<fb::library_file_t> library{
        {
            .path = "x.fb",
            .text = "Public Class y\n"
                    "    Public Static Field count: Int = 1\n"
                    "    Public Static Method Z(): String => \"method\"\n"
                    "End Class\n",
        },
        {
            .path = "x/y.fb",
            .text = "Public Global count = 2\n"
                    "Public Function Z(): String => \"function\"\n",
        },
    };
    const fb::compilation_t compilation = fb::compile("test.fb",
                                                      "Import \"./x\"\n"
                                                      "Import \"./x/y\" As yy\n"
                                                      "Local a = x.y.Z()\n"
                                                      "Local b = yy.Z()\n"
                                                      "Local c = x.y.count + yy.count",
                                                      library);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    const std::string mir = fb::dump(fb::lower(compilation.ast, compilation.analysis));
    INFO(mir);
    CHECK(mir.contains("function x.y.Z() -> String\n"));
    CHECK(mir.contains("function x.y.Z()#2 -> String\n"));
    CHECK(mir.contains("global x.y.count: Int32 = 2\n"));
    CHECK(mir.contains("global x.y.count#2: Int32 = 1\n"));
}

TEST_CASE("lowering stops where code needs a larger instance each time, which analysis missed",
          "[mir]") {
    // A Holder of the Interface's Self, which runs the default again: through a Static default,
    // and through a table default that only tests for one, which still makes its table.
    const auto stopped = [](const std::string_view text) {
        const fb::compilation_t compilation = fb::compile("test.fb", std::string(text));
        REQUIRE(compilation.diagnostics.diagnostics().empty());
        const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
        REQUIRE(module.error);
        return std::string(
                   compilation.sources.get(module.error->span.file).slice(module.error->span)) +
               ": " + module.error->message;
    };
    constexpr std::string_view holder = "Class Holder<S: Shown> Implements Shown\n"
                                        "    Field Inner: S\n"
                                        "    New(inner: S)\n"
                                        "        Inner = inner\n"
                                        "    End New\n"
                                        "    Method Show(): Int => 1\n"
                                        "End Class\n"
                                        "Class Leaf Implements Shown\n"
                                        "    Method Show(): Int => 0\n"
                                        "End Class\n";
    CHECK(stopped(std::string("Interface Shown\n"
                              "    Method Show(): Int\n"
                              "    Static Method Depth(n: Int): Int\n"
                              "        If n == 0 Then Return 0\n"
                              "        Return Holder<Self>.Depth(n - 1) + 1\n"
                              "    End Method\n"
                              "End Interface\n") +
                  std::string(holder) + "Local d = Leaf.Depth(3)") ==
          "Holder<Self>.Depth(n - 1): 'Depth' needs a larger instance each time it runs here, "
          "without end");
    CHECK(stopped(std::string("Interface Shown\n"
                              "    Method Show(): Int\n"
                              "    Method Probe(other: Shown): Bool => other Is Holder<Self>\n"
                              "End Interface\n") +
                  std::string(holder) + "Local p = Leaf().Probe(Leaf())") ==
          "other Is Holder<Self>: Holder<S> needs a larger instance each time it runs here, "
          "without end");

    // One that doubles each time stops before its names grow long, and one that grows by a
    // Const type parameter, which never nests, once it has made more instances than a program
    // needs.
    constexpr std::string_view pair = "Class Pair<A: Shown, B: Shown> Implements Shown\n"
                                      "    Method Show(): Int => 1\n"
                                      "End Class\n"
                                      "Class Leaf Implements Shown\n"
                                      "    Method Show(): Int => 0\n"
                                      "End Class\n";
    CHECK(stopped(std::string("Interface Shown\n"
                              "    Method Show(): Int\n"
                              "    Static Method Depth(n: Int): Int\n"
                              "        If n == 0 Then Return 0\n"
                              "        Return Pair<Self, Self>.Depth(n - 1) + 1\n"
                              "    End Method\n"
                              "End Interface\n") +
                  std::string(pair) + "Local d = Leaf.Depth(3)") ==
          "Pair<Self, Self>.Depth(n - 1): 'Depth' needs a larger instance each time it runs here, "
          "without end");
    CHECK(stopped(std::string("Interface Shown\n"
                              "    Method Show(): Int\n"
                              "    Method Probe(other: Shown): Bool => other Is Pair<Self, Self>\n"
                              "End Interface\n") +
                  std::string(pair) + "Local p = Leaf().Probe(Leaf())") ==
          "other Is Pair<Self, Self>: Pair<A, B> needs a larger instance each time it runs here, "
          "without end");
    CHECK(stopped("Interface Stepper<Const N: Int>\n"
                  "    Method Show(): Int\n"
                  "    Static Method Depth(n: Int): Int\n"
                  "        If n == 0 Then Return 0\n"
                  "        Return Step<N + 1>.Depth(n - 1) + 1\n"
                  "    End Method\n"
                  "End Interface\n"
                  "Class Step<Const M: Int> Implements Stepper<M>\n"
                  "    Method Show(): Int => M\n"
                  "End Class\n"
                  "Local d = Step<1>.Depth(3)") ==
          "Step<N + 1>.Depth(n - 1): 'Depth' needs a larger instance each time it runs here, "
          "without end");
    // A Class that tests for the next instance of itself, a chain of them made one from the
    // other, stops before it has lowered many.
    CHECK(stopped("Interface Stepper<Const N: Int>\n"
                  "    Method Show(): Int\n"
                  "    Method Probe(other: Stepper<N>): Bool => other Is Step<N + 1>\n"
                  "End Interface\n"
                  "Class Step<Const M: Int> Implements Stepper<M>\n"
                  "    Method Show(): Int => M\n"
                  "End Class\n"
                  "Local s = Step<1>().Show()") ==
          "other Is Step<N + 1>: Step<M> needs a larger instance each time it runs here, "
          "without end");
    // And one whose base is the next instance, past where the type table fills them.
    CHECK(stopped("Interface Maker\n"
                  "    Static Method Make(): Self\n"
                  "End Interface\n"
                  "Class Base<T: Maker>\n"
                  "    Method Spawned(): Int\n"
                  "        Local made = T.Make()\n"
                  "        Return 1\n"
                  "    End Method\n"
                  "End Class\n"
                  "Class A<Const N: Int> Extends Base<A<N + 1>> Implements Maker\n"
                  "    Static Method Make(): A<N> => A<N>()\n"
                  "End Class\n"
                  "Local a = A<1>().Spawned()")
              .ends_with("needs a larger instance each time it runs here, without end"));

    // A Const type argument an instance works out has to be a size or a count too.
    CHECK(stopped("Function Show<Const M: Int>(): Int => M\n"
                  "Function Twice<Const N: Int>(): Int => Show<N * 2>()\n"
                  "Local t = Twice<1073741824>()") ==
          "Show<N * 2>(): a Const type argument comes out as 2147483648 here, but it's a size or "
          "a count, from 1 to 2147483647");
    CHECK(stopped("Function Count<Const N: Int>(): Int\n"
                  "    Local a: Int[N * 4]\n"
                  "    Return a.Count\n"
                  "End Function\n"
                  "Local c = Count<1073741824>()") ==
          "Local a: Int[N * 4]: a Const type argument comes out as 4294967296 here, but it's a "
          "size or a count, from 1 to 2147483647");
    CHECK(stopped("Function Show<Const M: UInt8>(): Int => M\n"
                  "Function Twice<Const N: UInt8>(): Int => Show<N * 2>()\n"
                  "Local t = Twice<200>()") ==
          "Show<N * 2>(): a Const type argument comes out as 400 here, which doesn't fit in UInt8");
    CHECK(
        stopped("Function Show<Const M: Int>(): Int => M\n"
                "Function Wrap<Const N: Int>(): Int => Show<N * 65536 * 65536 * 65536 * 65536>()\n"
                "Local w = Wrap<1>()")
            .contains("a Const type argument comes out as 9223372036854775807 here"));
    // So does a size in an instance's Field, which code that isn't generic can use.
    CHECK(stopped("Struct Pair<Const N: Int>\n"
                  "    Field c: Int8[N * 2]\n"
                  "End Struct\n"
                  "Local p = Pair<2000000000>()") ==
          "Pair<2000000000>(): a Const type argument comes out as 4000000000 here, but it's a "
          "size or a count, from 1 to 2147483647");

    // A fixed array's sizes can each fit and still hold more elements than one can.
    const std::string_view grid = "Struct Grid<Const W: Int, Const H: Int>\n"
                                  "    Field c: Int8[W, H]\n"
                                  "End Struct\n";
    CHECK(stopped(std::format("{}Local g = Grid<65536, 65536>()", grid)) ==
          "Grid<65536, 65536>(): Int8[65536, 65536] comes out here with more elements than a "
          "fixed array can hold, at most 2147483647");
    CHECK(stopped(std::format("{}Class Wide<Const N: Int> Extends Base<N>\n"
                              "End Class\n"
                              "Class Base<Const N: Int>\n"
                              "    Field g: Grid<N, N>?\n"
                              "End Class\n"
                              "Local w = Wide<65536>()",
                              grid)) ==
          "Wide<65536>(): Int8[65536, 65536] comes out here with more elements than a fixed array "
          "can hold, at most 2147483647");
    CHECK(stopped("Function Cells<Const N: Int>(): Int\n"
                  "    Local c: Int8[N, N * 2]\n"
                  "    Return c.Count\n"
                  "End Function\n"
                  "Local c = Cells<40000>()") ==
          "Local c: Int8[N, N * 2]: Int8[40000, 80000] comes out here with more elements than a "
          "fixed array can hold, at most 2147483647");
    const fb::compilation_t fits = fb::compile(
        "test.fb", std::format("{}Local g = Grid<46340, 46340>()\nLocal n = g.c.Count", grid));
    REQUIRE(fits.diagnostics.diagnostics().empty());
    CHECK(!fb::lower(fits.ast, fits.analysis).error);
}

TEST_CASE("lowering makes as many instances as a program uses, one from another or not", "[mir]") {
    // Many instances of one generic function or Class, made from the same code, aren't growth.
    std::string calls = "Function Id<Const N: Int>(): Int => N\nLocal total = 0\n";
    for (int i = 1; i <= 2100; ++i) {
        calls += std::format("total += Id<{}>()\n", i);
    }

    const fb::compilation_t many = fb::compile("test.fb", calls);
    REQUIRE(many.diagnostics.diagnostics().empty());
    CHECK(!fb::lower(many.ast, many.analysis).error);

    // Nor is a deep but finite type, like a tuple made of tuples twelve times.
    std::string nested = "Function Both<T>(x: T): (T, T) => (x, x)\nLocal v = ";
    for (int i = 0; i < 12; ++i) {
        nested += "Both(";
    }

    nested += "1" + std::string(12, ')');
    const fb::compilation_t deep = fb::compile("test.fb", nested);
    REQUIRE(deep.diagnostics.diagnostics().empty());
    CHECK(!fb::lower(deep.ast, deep.analysis).error);
}

TEST_CASE("an array type has tables only once its arrays go where an Interface is expected",
          "[mir]") {
    // Iterable<T>'s Iterate is what the Extend of T[] gives, in a box, and Printable's ToString
    // is the array's own text.
    const std::string mir = lower_text("Import std.core\n"
                                       "Import std.collections\n"
                                       "Function Show(p: Printable): String => p.ToString()\n"
                                       "Local items: Iterable<Int> = [1, 2]\n"
                                       "Local text = Show([\"a\"])\n"
                                       "Local plain = [True]");
    INFO(mir);
    CHECK(mir.contains("array Int32[]\n"
                       "    std.core.interfaces.Printable 0: Int32[].ToString()\n"
                       "    std.core.iterator.Iterable<Int32> 0: std.collections.array.Int32[]."
                       "Iterate() as std.core.iterator.Iterator<Int32>\n"));
    CHECK(mir.contains("array String[]\n"
                       "    std.core.interfaces.Printable 0: String[].ToString()\n"
                       "    std.core.iterator.Iterable<String> 0: std.collections.array."
                       "String[].Iterate() as std.core.iterator.Iterator<String>\n"));
    CHECK(!mir.contains("array Bool[]"));
}

TEST_CASE("For Field's f.Set can't change a ReadOnly Field", "[mir]") {
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Import std.core\n"
                               "Class Hub\n"
                               "    ReadOnly Field Name: String\n"
                               "    Field Count: Int\n"
                               "    New(name: String)\n"
                               "        Name = name\n"
                               "    End New\n"
                               "End Class\n"
                               "Function CopyInto<T>(ByRef target: T, source: T)\n"
                               "    For Field f: F In T\n"
                               "        f.Set(ByRef target, f.Get(source))\n"
                               "    End For\n"
                               "End Function\n"
                               "Local hub = Hub(\"a\")\n"
                               "CopyInto(ByRef hub, Hub(\"b\"))");
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(module.error);
    const fb::mir_error_t error = module.error.value_or(fb::mir_error_t{});
    CHECK(std::string(compilation.sources.get(error.span.file).slice(error.span)) + ": " +
              error.message ==
          "f.Set(ByRef target, f.Get(source)): 'Name' is ReadOnly, so f.Set can't change it");
}

TEST_CASE("an assignment copies a value read through a reference before it lets go of the old one",
          "[mir]") {
    const std::string mir = lower_text("Import std.core\n"
                                       "Class Node\n"
                                       "    Field Next: Node?\n"
                                       "    Field Items: Node[] = []\n"
                                       "End Class\n"
                                       "Global first: Node? = Null\n"
                                       "Function Step(node: Node)\n"
                                       "    node.Next = node.Items[0]\n"
                                       "    node.Items = node.Items[0].Items\n"
                                       "    first = node.Next\n"
                                       "End Function");
    INFO(mir);
    CHECK(mir.contains("    _2 = _0.Items[_1] As Node?\n"
                       "    retain _2\n"
                       "    release _0.Next\n"
                       "    _0.Next = _2\n"));
    CHECK(mir.contains("    _4 = _0.Items[_3].Items\n"
                       "    retain _4\n"
                       "    release _0.Items\n"
                       "    _0.Items = _4\n"));
    CHECK(mir.contains("    _5 = _0.Next\n"
                       "    retain _5\n"
                       "    release @first\n"
                       "    @first = _5\n"));
}

TEST_CASE("SortUnstable sorts in place, making no array", "[mir]") {
    const std::string mir = lower_text("Import std.core\n"
                                       "Import std.collections\n"
                                       "Local items = [3, 1, 2]\n"
                                       "items.SortUnstable()");
    INFO(mir);
    // Each of the sort's functions, up to the blank line after it.
    for (const std::string_view name : { "SortUnstable(", "SiftDown(", "Swap(" }) {
        const std::size_t start =
            mir.find(std::format("function std.collections.array.Int32[].{}", name));
        REQUIRE(start != std::string::npos);
        const std::string_view function =
            std::string_view(mir).substr(start, mir.find("\n\n", start) - start);
        INFO(function);
        // An array's made as "Int32[] [...]", or copied.
        CHECK(!function.contains("[] ["));
        CHECK(!function.contains("Clone"));
        CHECK(!function.contains("ToArray"));
    }
}
