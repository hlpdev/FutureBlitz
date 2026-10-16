#include "fb/lexer.hpp"
#include "fb/lower.hpp"
#include "fb/mir.hpp"
#include "fb/parser.hpp"
#include "fb/sema.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// The MIR dump of a program that analyzes without errors.
std::string lower_text(const std::string_view text) {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    const fb::ast_t ast = fb::parse(file, tokens, bag);
    const fb::analysis_t analysis = fb::analyze(file, ast, bag);
    INFO(text);
    REQUIRE(bag.diagnostics().empty());
    return fb::dump(fb::lower(ast, analysis));
}

/// The main of a program with no top-level code. Every program has a main.
constexpr std::string_view empty_main = "function main\n"
                                        "bb0:\n"
                                        "    return\n";

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
    CHECK(lower_text("Function Shout(text: String): String => text + \"!\"\n"
                     "Local s = Shout(\"hi\")\n"
                     "s = s + s\n"
                     "Print(s)") == "function Shout(String) -> String\n"
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
                                    "bb0:\n"
                                    "    _0 = call Shout(String)(\"hi\")\n"
                                    "    _1 = _0\n"
                                    "    _2 = concat(_1, _1)\n"
                                    "    release _1\n"
                                    "    _1 = _2\n"
                                    "    print(_1)\n"
                                    "    release _1\n"
                                    "    return\n");
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
    // the Print comes first in the source.
    CHECK(lower_text("Print(name)\n"
                     "Global g = 1\n"
                     "Global h = g * 2\n"
                     "Global name = \"fb\"") == "global g: Int32 = 1\n"
                                                "global h: Int32 = 0\n"
                                                "global name: String = \"fb\"\n"
                                                "\n"
                                                "function main\n"
                                                "    _0: Int32\n"
                                                "bb0:\n"
                                                "    _0 = @g * 2\n"
                                                "    @h = _0\n"
                                                "    print(@name)\n"
                                                "    return\n");
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
