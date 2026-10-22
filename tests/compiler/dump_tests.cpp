#include "fb/driver.hpp"
#include "fb/dump.hpp"
#include "fb/lower.hpp"
#include "fb/mir.hpp"
#include "fb/style.hpp"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>
#include <string_view>

namespace {

/// Color on, so the views include ANSI codes.
constexpr fb::style_t colored{ .color = true };

/// `text` wrapped in an ANSI color code, like "\x1b[35mreturn\x1b[0m".
std::string ansi(const std::string_view code, const std::string_view text) {
    return std::format("\x1b[{}m{}\x1b[0m", code, text);
}

/// The tree view of a program, which may have errors.
std::string tree(const std::string_view text, const fb::style_t& style = {}) {
    const fb::compilation_t compilation = fb::compile("test.fb", std::string(text));
    return fb::dump_tree(compilation.ast, style);
}

/// The types view of a program without errors.
std::string types(const std::string_view text) {
    const fb::compilation_t compilation = fb::compile("test.fb", std::string(text));
    INFO(text);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    return fb::dump_types(compilation.ast, compilation.analysis);
}

} // namespace

TEST_CASE("paint adds ANSI codes only when color is on", "[style]") {
    CHECK(fb::style_t{}.paint("x", fb::tone_t::ERROR) == "x");
    // ERROR is bold red. PLAIN and empty text get no codes.
    CHECK(colored.paint("x", fb::tone_t::ERROR) == "\x1b[1;31mx\x1b[0m");
    CHECK(colored.paint("x", fb::tone_t::PLAIN) == "x");
    CHECK(colored.paint("", fb::tone_t::ERROR).empty());
}

// Tokens

TEST_CASE("the token view lines up positions, kinds, and text", "[dump]") {
    const fb::compilation_t compilation = fb::compile("test.fb", "Local x = 1\n");
    // Text lines up after the longest kind that has text, "integer literal".
    CHECK(fb::dump_tokens(compilation.source(), compilation.tokens) == "1:1   'Local'\n"
                                                                       "1:7   identifier       x\n"
                                                                       "1:9   '='\n"
                                                                       "1:11  integer literal  1\n"
                                                                       "1:12  newline\n"
                                                                       "2:1   end of file\n");
}

TEST_CASE("the token view colors each kind", "[dump]") {
    const fb::compilation_t compilation = fb::compile("test.fb", "Local x = 1");
    const std::string view = fb::dump_tokens(compilation.source(), compilation.tokens, colored);
    // Gray positions and punctuation, magenta keywords, cyan identifiers, and green literals.
    // Positions are padded before they're painted.
    CHECK(view.starts_with(std::format("{}  {}\n", ansi("90", "1:1 "), ansi("35", "'Local'"))));
    CHECK(view.contains(std::format("{}       x\n", ansi("36", "identifier"))));
    CHECK(view.contains(ansi("90", "'='")));
    CHECK(view.contains(std::format("{}  1\n", ansi("32", "integer literal"))));
}

// Trees

TEST_CASE("the tree puts block contents on their own lines", "[dump]") {
    CHECK(tree("Function Twice(n: Int): Int\n"
               "    Return n * 2\n"
               "End Function") == "(function Twice (n: Int) -> Int\n"
                                  "  (return (* n 2)))\n");
    CHECK(tree("Function Main()\nEnd Function") == "(function Main)\n");
    CHECK(tree("For i = 1 To 3\n"
               "    For j = 1 To i\n"
               "        Print(\"{i}\")\n"
               "    End For\n"
               "End For") == "(for i 1 to 3\n"
                             "  (for j 1 to i\n"
                             "    (call Print (string i))))\n");
}

TEST_CASE("the tree gives Else, Case, and what follows a block their own lines", "[dump]") {
    CHECK(tree("If hp <= 0\n"
               "    Die()\n"
               "ElseIf hp < 20\n"
               "    Flash()\n"
               "Else\n"
               "    Regen()\n"
               "End If") == "(if (<= hp 0)\n"
                            "  (call Die)\n"
                            "  (elseif (< hp 20)\n"
                            "    (call Flash))\n"
                            "  (else\n"
                            "    (call Regen)))\n");
    CHECK(tree("Select state\n"
               "    Case 1, 2\n"
               "        A()\n"
               "    Case 3 To 5\n"
               "        B()\n"
               "    Default\n"
               "        C()\n"
               "End Select") == "(select state\n"
                                "  (case 1 2\n"
                                "    (call A))\n"
                                "  (case (range 3 5)\n"
                                "    (call B))\n"
                                "  (default\n"
                                "    (call C)))\n");
    CHECK(tree("Repeat\n    Tick()\nUntil done") == "(repeat\n"
                                                    "  (call Tick)\n"
                                                    "  until done)\n");
}

TEST_CASE("the tree gives each Struct member its own line", "[dump]") {
    CHECK(tree("Struct P\n"
               "    Field x: Int\n"
               "    Method M(): Int => x + Self.x\n"
               "End Struct") == "(struct P\n"
                                "  (field x: Int)\n"
                                "  (method M -> Int\n"
                                "    (return (+ x (. Self x)))))\n");
    CHECK(tree("Class C\n    Field x: Int\nEnd Class") == "(class C\n"
                                                          "  (field x: Int))\n");
}

TEST_CASE("the tree shows imports, and names after a qualifier", "[dump]") {
    CHECK(tree("Import std.collections As C\n"
               "From std.collections Import Stack, Queue\n"
               "Local s: C.Stack<Int>? = C.Stack<Int>()") ==
          "(import std.collections As C)\n"
          "(from std.collections Import Stack Queue)\n"
          "(local s: C.Stack<Int>? = (call (. C Stack<Int>)))\n");
    CHECK(tree("Import \"./hud\"", colored) ==
          std::format("({} {})\n", ansi("35", "import"), ansi("32", "\"./hud\"")));
}

TEST_CASE("the tree prints literals as values", "[dump]") {
    // 3.0 keeps its ".0" so it doesn't look like an integer, and an interpolated string becomes
    // its parts.
    CHECK(tree("Local a = 1.5\n"
               "Local b = 3.0\n"
               "Local c = \"hi\"\n"
               "Local d = \"hi {name}!\"\n"
               "Local e = 'A'\n"
               "Local f = True") == "(local a = 1.5)\n"
                                    "(local b = 3.0)\n"
                                    "(local c = \"hi\")\n"
                                    "(local d = (string \"hi \" name \"!\"))\n"
                                    "(local e = 'A')\n"
                                    "(local f = True)\n");
}

TEST_CASE("the tree colors heads, names, types, and literals", "[dump]") {
    // Magenta heads, cyan names, yellow types, and green literals.
    CHECK(tree("Local hp: Int = 100", colored) ==
          std::format("({} {} {} = {})\n", ansi("35", "local"), ansi("36", "hp:"),
                      ansi("33", "Int"), ansi("32", "100")));
}

TEST_CASE("the types view shows types, conversions, and known values", "[dump]") {
    // "Int32 → Int64" is an implicit conversion, and "= 6" a value known at compile time.
    CHECK(types("Function Add(a: Int, b: Long): Long => a + b") ==
          "(function Add (a: Int) (b: Long) -> Long\n"
          "  (return (+ a: Int32 → Int64 b: Int64): Int64))\n");
    CHECK(types("Const LIMIT = 3\n"
                "Local twice = LIMIT * 2") == "(const LIMIT = 3: Int32)\n"
                                              "(local twice = (* LIMIT: Int32 = 3 2: Int32): "
                                              "Int32 = 6)\n");
    CHECK(types("Import std.core\nPrint(\"hi\")") ==
          "(import std.core)\n(call Print \"hi\": String)\n");
}

// LLVM IR

TEST_CASE("LLVM IR is colored by kind of token", "[dump]") {
    constexpr std::string_view ir = "define i32 @f(i32 %x) {\n"
                                    "entry:\n"
                                    "  ret i32 -1 ; done\n"
                                    "}\n";
    CHECK(fb::highlight_llvm(ir, {}) == ir);
    // Magenta keywords, yellow types, cyan names, bold labels, green constants, and gray
    // comments.
    CHECK(fb::highlight_llvm(ir, colored) ==
          std::format("{} {} {}({} {}) {{\n"
                      "{}\n"
                      "  {} {} {} {}\n"
                      "}}\n",
                      ansi("35", "define"), ansi("33", "i32"), ansi("36", "@f"), ansi("33", "i32"),
                      ansi("36", "%x"), ansi("1", "entry:"), ansi("35", "ret"), ansi("33", "i32"),
                      ansi("32", "-1"), ansi("90", "; done")));
    CHECK(fb::highlight_llvm("@text = constant [3 x i8] c\"hi\\00\"\n", colored)
              .contains(ansi("32", "c\"hi\\00\"")));
}

// MIR

TEST_CASE("the MIR dump can be colored", "[dump]") {
    const fb::compilation_t compilation = fb::compile("test.fb", "Local x = 1");
    const std::string view = fb::dump(fb::lower(compilation.ast, compilation.analysis), colored);
    // "_0" is the MIR local for x.
    CHECK(view.starts_with(std::format("{} {}\n", ansi("35", "function"), ansi("1", "main"))));
    CHECK(view.contains(std::format("    {} x: {}\n", ansi("36", "_0"), ansi("33", "Int32"))));
    CHECK(view.contains(std::format("    {} = {}\n", ansi("36", "_0"), ansi("32", "1"))));
    CHECK(view.contains(std::format("    {}\n", ansi("35", "return"))));
}
