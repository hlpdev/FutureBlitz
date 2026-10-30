#include "fb/diagnostic.hpp"
#include "fb/dump.hpp"
#include "fb/format.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// `text` formatted to `width`, which must succeed, and which formatting again keeps.
std::string formatted(const std::string_view text, const std::uint32_t width = 100) {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    const std::expected<std::string, std::vector<fb::diagnostic_t>> result =
        fb::format_source(file, width);
    REQUIRE(result.has_value());
    const fb::source_file_t again(fb::file_id_t{}, "test.fb", *result);
    const std::expected<std::string, std::vector<fb::diagnostic_t>> twice =
        fb::format_source(again, width);
    REQUIRE(twice.has_value());
    CHECK(*twice == *result);
    return *result;
}

/// Checks that `text` is formatted already.
void unchanged(const std::string_view text, const std::uint32_t width = 100) {
    CHECK(formatted(text, width) == text);
}

/// The tree a build sees, with `defined` the symbols #If finds, and the tokens but for line
/// breaks.
std::string tree_of(const fb::source_file_t& file,
                    const std::span<const std::string_view> defined) {
    fb::diagnostic_bag_t diagnostics;
    const std::vector<fb::token_t> tokens = fb::lex(file, diagnostics, defined);
    const fb::ast_t ast = fb::parse(file, tokens, diagnostics);
    std::string out;
    for (const fb::token_t& token : tokens) {
        if (token.kind != fb::token_kind_t::NEWLINE) {
            out += std::string(fb::describe(token.kind)) + " " +
                   std::string(file.slice(token.span)) + "\n";
        }
    }

    return out + fb::dump_ast(ast);
}

/// The repository's folder, which tests/programs is in.
std::filesystem::path repository() {
    return std::filesystem::path(FB_TEST_PROGRAMS).parent_path().parent_path();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_CASE("formatting puts one space around binary operators and after commas", "[format]") {
    CHECK(formatted("x=a+b*c\n") == "x = a + b * c\n");
    CHECK(formatted("Print(a,b ,  c)\n") == "Print(a, b, c)\n");
    CHECK(formatted("If a&&b||!c Then x+=1\n") == "If a && b || !c Then x += 1\n");
    CHECK(formatted("x = a ?? b\ny = c**2\n") == "x = a ?? b\ny = c ** 2\n");
    CHECK(formatted("x = a<<2 | b&3 ^ c\n") == "x = a << 2 | b & 3 ^ c\n");
}

TEST_CASE("formatting writes prefix operators against what they apply to", "[format]") {
    CHECK(formatted("x = - a\ny = ! b\nz = ~ c\n") == "x = -a\ny = !b\nz = ~c\n");
    CHECK(formatted("x = a - -b\n") == "x = a - -b\n");
    CHECK(formatted("For i = 10 To 0 Step - 1\nEnd For\n") == "For i = 10 To 0 Step -1\nEnd For\n");
    CHECK(formatted("Return(-x)\n") == "Return (-x)\n");
}

TEST_CASE("formatting puts no space inside brackets of any kind", "[format]") {
    CHECK(formatted("x = ( a + b ) * f( c )[ 0 ]\n") == "x = (a + b) * f(c)[0]\n");
    CHECK(formatted("Local m = { \"a\": 1,\"b\": 2 }\n") == "Local m = {\"a\": 1, \"b\": 2}\n");
    CHECK(formatted("Local m: Map<String, Int> = {  }\n") == "Local m: Map<String, Int> = {}\n");
    // A map's brace stays apart from an interpolation's, which "{{" would escape.
    unchanged("Print(\"{ {1: 2}.Count} { {3: 4} }\")\n");
    CHECK(formatted("Local a = [ 1, 2 ]\nReturn [3]\n") == "Local a = [1, 2]\nReturn [3]\n");
}

TEST_CASE("formatting writes a colon after a name with a space after it", "[format]") {
    CHECK(formatted("Function F(a : Int, b:String) : Bool\n    Return True\nEnd Function\n") ==
          "Function F(a: Int, b: String): Bool\n    Return True\nEnd Function\n");
    CHECK(formatted("Print(Mismatch(x, path:\"\"))\n") == "Print(Mismatch(x, path: \"\"))\n");
}

TEST_CASE("formatting puts spaces around a ternary's '?' and ':'", "[format]") {
    CHECK(formatted("x = a?b:c\n") == "x = a ? b : c\n");
    CHECK(formatted("x = a ? b : c ? d : e\n") == "x = a ? b : c ? d : e\n");
}

TEST_CASE("formatting keeps a nullable's '?', and As? and Try?, against what's before them",
          "[format]") {
    unchanged("Local x: Int? = Null\nLocal y: Int?[] = []\nLocal z: Map<String, Int?> = {}\n");
    unchanged("Local n = x As? Int\nLocal m = Try? Parse(\"1\") ?? 0\n");
    unchanged("Local t: Int?? = Null\n");
    unchanged(
        "Struct Q<A>\n    Field V: Int\n\n    Operator <<Const B: Int>(x: Q<A>, y: Q<B>): Bool => "
        "x.V < y.V\nEnd Struct\n");
    unchanged("Function F(): Int? Throws IOError\n    Return Null\nEnd Function\n");
}

TEST_CASE("formatting writes type arguments and parameters without spaces", "[format]") {
    CHECK(formatted("Local d = Deque < Int > ()\n") == "Local d = Deque<Int>()\n");
    CHECK(formatted("Local d: List<List<Int> > = []\n") == "Local d: List<List<Int>> = []\n");
    unchanged("Field Owner: Weak<LinkedList<T>>\n");
    unchanged(
        "Function Max<T: Comparable>(a: T, b: T): T\n    Return a < b ? b : a\nEnd Function\n");
    unchanged("Print(1 < 2, 3 > 2, 1 << 3 >> 1, a >= b)\n");
}

TEST_CASE("formatting writes member access, ranges, and calls without spaces", "[format]") {
    CHECK(formatted("x = a . b ?. c\n") == "x = a.b?.c\n");
    CHECK(formatted("x = a[1 .. 3]\n") == "x = a[1..3]\n");
    CHECK(formatted("x = F (1)\n") == "x = F(1)\n");
    CHECK(formatted("If(a)Then Print(1)\n") == "If (a) Then Print(1)\n");
    unchanged("Function Sum(values: Int...): Int\n    Return 0\nEnd Function\n");
    unchanged("i++\nj--\n");
}

TEST_CASE("formatting writes an Operator's name against its parameters", "[format]") {
    unchanged(
        "Struct V\n    Field X: Int\n\n    Operator [](i: Int): Int => X\n    Operator []=(i: "
        "Int, v: Int)\n    End Operator\n    Operator ==(a: V, b: V): Bool => a.X == "
        "b.X\n    Operator -(a: V): V => V()\n    Operator >>(a: V, n: Int): V => a\nEnd "
        "Struct\n");
    CHECK(formatted("Struct V\n    Operator *  (a: V, b: V): V => a\nEnd Struct\n") ==
          "Struct V\n    Operator *(a: V, b: V): V => a\nEnd Struct\n");
}

TEST_CASE("formatting leaves a string's insides as they are, and spaces an interpolation",
          "[format]") {
    unchanged("Print(\"a  +  b {x}\")\n");
    CHECK(formatted("Print(\"{a+b} and {c:.2}\")\n") == "Print(\"{a + b} and {c:.2}\")\n");
    unchanged("Local s = \"\"\"\n      kept   as is\n    \"\"\"\n");
}

TEST_CASE("formatting writes attributes and lambdas as std does", "[format]") {
    CHECK(formatted("@ PanicsAtCaller\nFunction F()\nEnd Function\n") ==
          "@PanicsAtCaller\nFunction F()\nEnd Function\n");
    CHECK(formatted("Local f = Function (x: Int) => x*2\n") ==
          "Local f = Function(x: Int) => x * 2\n");
    unchanged("Local f: Func(Int): Int = Function(x) => x\n");
}

TEST_CASE("formatting indents each block 4 spaces", "[format]") {
    CHECK(formatted("Function F(x: Int): Int\nIf x > 0\nReturn 1\nElseIf x < 0\nReturn -1\n"
                    "Else\nReturn 0\nEnd If\nEnd Function\n") ==
          "Function F(x: Int): Int\n    If x > 0\n        Return 1\n    ElseIf x < 0\n        "
          "Return -1\n    Else\n        Return 0\n    End If\nEnd Function\n");
    CHECK(formatted("Class C\n  Field A: Int\n      Method M()\n  End Method\nEnd Class\n") ==
          "Class C\n    Field A: Int\n    Method M()\n    End Method\nEnd Class\n");
    CHECK(formatted("\tPrint(1)\n") == "Print(1)\n");
}

TEST_CASE("formatting indents a Select's Cases, and their bodies under them", "[format]") {
    CHECK(formatted("Select x\nCase 1, 2\nPrint(1)\nDefault\nPrint(2)\nEnd Select\n") ==
          "Select x\n    Case 1, 2\n        Print(1)\n    Default\n        Print(2)\nEnd Select\n");
}

TEST_CASE("formatting indents Get, Set, Where lines, Enums, Extern blocks, and Try", "[format]") {
    unchanged("Class C\n    Property P: Int\n        Get => 1\n        Set(value)\n            "
              "Print(value)\n"
              "        End Set\n    End Property\nEnd Class\n");
    unchanged("Class Box<T>\n    Where T: Equatable & Hashable\n    Field V: T\nEnd Class\n");
    unchanged("Enum E\n    A, B,\n    C\nEnd Enum\n");
    unchanged("Extern \"C\"\n    Function puts(text: CString): Int32\nEnd Extern\n");
    unchanged("Try\n    Print(1)\nCatch e As IOError\n    Print(2)\nFinally\n    Print(3)\nEnd "
              "Try\n");
    unchanged("Repeat\n    i += 1\nUntil i > 3\n");
}

TEST_CASE("formatting indents a lambda's block under the statement it's in", "[format]") {
    unchanged("order.Sort(Function(a: Int, b: Int): Bool\n    Return a < b\nEnd Function)\n");
    CHECK(formatted("Function F()\n    order.Sort(Function(a: Int, b: Int): Bool\n  If a\n"
                    "Return True\nEnd If\n        Return a < b\n      End Function)\nEnd "
                    "Function\n") ==
          "Function F()\n    order.Sort(Function(a: Int, b: Int): Bool\n        If a\n            "
          "Return True\n        End If\n        Return a < b\n    End Function)\nEnd Function\n");
}

TEST_CASE("formatting keeps at most one blank line, and none at a block's start or end",
          "[format]") {
    CHECK(formatted("\n\nPrint(1)\n\n\n\nPrint(2)\n\n\n") == "Print(1)\n\nPrint(2)\n");
    CHECK(formatted("Function F()\n\n    Print(1)\n\n    Print(2)\n\nEnd Function\n") ==
          "Function F()\n    Print(1)\n\n    Print(2)\nEnd Function\n");
    CHECK(formatted("If a\n    Print(1)\n\nElse\n\n    Print(2)\nEnd If\n") ==
          "If a\n    Print(1)\nElse\n    Print(2)\nEnd If\n");
    CHECK(formatted("Print(1)") == "Print(1)\n");
    CHECK(formatted("").empty());
}

TEST_CASE("formatting keeps comments where they are, at their line's indentation", "[format]") {
    CHECK(formatted("// A file.\nFunction F()\n// Says hi.\n        Print(1)   // hi   \nEnd "
                    "Function\n") ==
          "// A file.\nFunction F()\n    // Says hi.\n    Print(1) // hi\nEnd Function\n");
    // One written inside a block before its end stays inside it.
    CHECK(formatted("If a\n    Print(1)\n        // later\nEnd If\n") ==
          "If a\n    Print(1)\n    // later\nEnd If\n");
    CHECK(formatted("If a\n    Print(1)\n// about Else\nElse\n    Print(2)\nEnd If\n") ==
          "If a\n    Print(1)\n// about Else\nElse\n    Print(2)\nEnd If\n");
    CHECK(formatted("Print( /* one */ 1, 2)/* two */\n") == "Print(/* one */ 1, 2) /* two */\n");
    unchanged("/* a\n   block */\nPrint(1)\n// last\n");
}

TEST_CASE("formatting keeps the spaces at a doc comment's line's end", "[format]") {
    // Markdown's line break, and an example's string, which its test compares.
    const std::string_view text = "//! First line  \n"
                                  "//! second.\n"
                                  "\n"
                                  "/// Pads.\n"
                                  "///\n"
                                  "/// ```\n"
                                  "/// Local s = \"\"\"\n"
                                  "///     ab  \n"
                                  "///     \"\"\"\n"
                                  "/// Expect(Pad(s) == 4)\n"
                                  "/// ```\n"
                                  "Public Function Pad(s: String): Int => s.Length\n";
    unchanged(text);
    // A comment that isn't a doc comment still loses them.
    CHECK(formatted("//// banner   \nPrint(1)\n") == "//// banner\nPrint(1)\n");

    // A change to a doc comment's text changes what the file means, which fb fmt never makes.
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    CHECK_FALSE(fb::same_meaning(file, "//! First line\n"
                                       "//! second.\n"
                                       "\n"
                                       "/// Pads.\n"
                                       "Public Function Pad(s: String): Int => s.Length\n"));
}

TEST_CASE("formatting lines a list that goes on up inside its bracket", "[format]") {
    CHECK(formatted("Print(1, 2,\n  3)\nLocal a = [1,\n          2]\n") ==
          "Print(1, 2,\n      3)\nLocal a = [1,\n           2]\n");
    CHECK(formatted("Local m = {\"a\": 1,\n\"b\": 2}\n") ==
          "Local m = {\"a\": 1,\n           \"b\": 2}\n");
    unchanged("Local terms = Plus(Plus(Minus(a, b),\n                        Minus(c, d)),\n"
              "                   Minus(e, f))\n");
}

TEST_CASE("formatting lines a statement that goes on after an operator up with its expression",
          "[format]") {
    CHECK(formatted("Local x = a +\nb\nReturn a &&\n  b\nIf a ||\nb\nEnd If\n") ==
          "Local x = a +\n          b\nReturn a &&\n       b\nIf a ||\n   b\nEnd If\n");
    CHECK(formatted("Local x =\na + b\n") == "Local x =\n    a + b\n");
    CHECK(formatted("Function F(): Int =>\n1\n") == "Function F(): Int =>\n    1\n");
    CHECK(formatted("Class C Implements A,\nB\nEnd Class\n") ==
          "Class C Implements A,\n                   B\nEnd Class\n");
    // A Throws on a later line of the statement leads its own expression.
    CHECK(formatted("Function F(a: Int,\nb: Int): Int Throws AError,\nBError\n    Return 1\nEnd "
                    "Function\n") ==
          "Function F(a: Int,\n           b: Int): Int Throws AError,\n"
          "                               BError\n    Return 1\nEnd Function\n");
}

TEST_CASE("formatting puts what's inside a bracket that ends its line a step deeper", "[format]") {
    CHECK(formatted("Return [\n1, 2,\n3\n]\n") == "Return [\n    1, 2,\n    3\n]\n");
    CHECK(formatted("Local x = F(\na, b)\n") == "Local x = F(\n    a, b)\n");
    // A declaration's parameters go two steps deeper, past its body.
    CHECK(formatted("Function F(\na: Int, b: Int)\n    Print(a)\nEnd Function\n") ==
          "Function F(\n        a: Int, b: Int)\n    Print(a)\nEnd Function\n");
}

TEST_CASE("formatting lines a ternary's ':' that starts a line up with its '?'", "[format]") {
    CHECK(formatted("Print(a ? b\n: c)\n") == "Print(a ? b\n        : c)\n");
}

TEST_CASE("formatting breaks a line that's too wide after a comma or an operator", "[format]") {
    CHECK(formatted("Print(aaaa, bbbb, cccc, dddd)\n", 20) ==
          "Print(aaaa, bbbb,\n      cccc, dddd)\n");
    CHECK(formatted("Local x = aaaa + bbbb + cccc\n", 20) ==
          "Local x = aaaa +\n          bbbb +\n          cccc\n");
    // The loosest operator, at the outermost level, breaks first.
    CHECK(formatted("Return aa * bb + cc * dd && ee\n", 28) ==
          "Return aa * bb + cc * dd &&\n       ee\n");
    CHECK(formatted("x = F(aa, G(bb, cc), dd)\n", 20) == "x = F(aa, G(bb, cc),\n      dd)\n");
    // A first element too wide for the rest of the line goes on the next, a step deeper.
    CHECK(formatted("x = Something(aaaaaaaaaa)\n", 20) == "x = Something(\n    aaaaaaaaaa)\n");
    CHECK(formatted("Local x = aaaaaaaaaaaaaaaa\n", 20) == "Local x =\n    aaaaaaaaaaaaaaaa\n");
    // After '=' or "=>" comes before an opening bracket when the rest fits on the next line.
    CHECK(formatted("Local value = Something.Long(argumentsss)\n", 40) ==
          "Local value =\n    Something.Long(argumentsss)\n");
}

TEST_CASE("formatting breaks a ternary in brackets before its ':'", "[format]") {
    CHECK(formatted("Print(flag ? aaaa : bbbb)\n", 20) ==
          "Print(flag ? aaaa\n           : bbbb)\n");
}

TEST_CASE("formatting leaves a statement that can't fit as written", "[format]") {
    unchanged("Print(\"a string too long for the line\")\n", 20);
    unchanged("Print(\"a string too long for the line\",\n      1)\n", 20);
    // An operator's break would split a ternary's operands, outside brackets, where its ':'
    // can't start a line.
    unchanged("x = flag ? aaaa + bbbb : cccc\n", 20);
    unchanged("Local keep = flag ? value == ((i + 1) As Float64) : value.Truth()\n", 40);
    // A single-line If breaks only at a ',' in its statement's brackets.
    unchanged("If aaaa && bbbb Then Print(cccc)\n", 20);
    CHECK(formatted("If aa Then Print(bbbb, cccc)\n", 24) ==
          "If aa Then Print(bbbb,\n                 cccc)\n");
}

TEST_CASE("formatting keeps the line breaks written in a statement", "[format]") {
    unchanged("Print(1,\n      2)\nLocal x = a +\n          b\n");
}

TEST_CASE("formatting leaves an #If's lines as they are", "[format]") {
    unchanged("Print(1)\n#If DEBUG\n  Print( 2 )\n#Else\n anything @@@ \"\n#End If\nPrint(3)\n");
    CHECK(formatted("Function F()\n#If PLATFORM_WINDOWS\n Print(1)\n#End If\n  Print( 2 )\nEnd "
                    "Function\n") ==
          "Function F()\n#If PLATFORM_WINDOWS\n Print(1)\n#End If\n    Print(2)\nEnd Function\n");
}

TEST_CASE("formatting keeps \\r\\n line breaks", "[format]") {
    CHECK(formatted("If a\r\nPrint(1)\r\nEnd If\r\n") == "If a\r\n    Print(1)\r\nEnd If\r\n");
}

TEST_CASE("formatting leaves a file that doesn't parse, and says why", "[format]") {
    for (const std::string_view text : { "Print(1\n", "If a\n", "x = \"open\n", "Local = 1\n" }) {
        INFO(text);
        const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
        const std::expected<std::string, std::vector<fb::diagnostic_t>> result =
            fb::format_source(file);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
        for (const fb::diagnostic_t& diagnostic : result.error()) {
            CHECK(diagnostic.severity == fb::severity_t::ERROR);
        }
    }
}

TEST_CASE("formatting writes type arguments that a '>=' ends apart from the '='", "[format]") {
    CHECK(formatted("Local x: List<Int>=[]\n") == "Local x: List<Int> = []\n");
    CHECK(formatted("Local t: List<Int?>=[]\n") == "Local t: List<Int?> = []\n");
    CHECK(formatted("Local s: Map<Int, Map<Int, Int>>= Map<Int, Map<Int, Int>>()\n") ==
          "Local s: Map<Int, Map<Int, Int>> = Map<Int, Map<Int, Int>>()\n");
    unchanged("Print(a >= b, a >> 2 >= 1)\nx >>= 1\n");
}

TEST_CASE("formatting keeps prefix minuses apart, which together would be '--'", "[format]") {
    unchanged("Local b = - -a\n");
    unchanged("Local b = a - - -a\n");
    CHECK(formatted("Local b = !  !a\n") == "Local b = !!a\n");
}

TEST_CASE("formatting tries the next break when what follows the best one can't fit", "[format]") {
    CHECK(formatted("Local value = Something(aaaa, bbbbbbbbbbbbbbbbbbbb)\n", 40) ==
          "Local value = Something(\n    aaaa, bbbbbbbbbbbbbbbbbbbb)\n");
    CHECK(formatted("Function F()\n    Local message = Format.Describe(code, \"the request could "
                    "not be completed because the server went away\")\nEnd Function\n") ==
          "Function F()\n    Local message =\n        Format.Describe(code, \"the request could "
          "not be completed because the server went away\")\nEnd Function\n");
}

TEST_CASE("formatting doesn't break a statement where its next line would be too wide too",
          "[format]") {
    unchanged("Local message = \"a string that is much too long for a line of forty\"\n", 40);
    unchanged("Local x = aaaa + \"a string that is much too long for a line of forty\"\n", 40);
}

TEST_CASE("formatting keeps comments on one line of their own together", "[format]") {
    unchanged("Function F()\n    /* a */ /* b */\n    Print(1)\nEnd Function\n", 40);
    unchanged("/* a */ // b\nPrint(1)\n", 40);
    unchanged("Print(1,\n      /* a */ /* b */\n      2)\n", 40);
}

TEST_CASE("formatting keeps a comment at the end of a Select's last Case in its body", "[format]") {
    unchanged("Select x\n    Case 1\n        Print(1)\n        // still in the case\nEnd Select\n");
    CHECK(
        formatted("Select x\n    Case 1\n        Print(1)\n            // deeper\nEnd Select\n") ==
        "Select x\n    Case 1\n        Print(1)\n        // deeper\nEnd Select\n");
    unchanged("If a\n    Print(1)\n    // still inside\nEnd If\n");
}

TEST_CASE("formatting drops a blank line at the start of a block of comments or #If lines",
          "[format]") {
    CHECK(formatted("Function F()\n\n#If DEBUG\n    Print(1)\n#End If\n\nEnd Function\n") ==
          "Function F()\n#If DEBUG\n    Print(1)\n#End If\nEnd Function\n");
    CHECK(formatted("Function F()\n\n    // c\n\nEnd Function\n") ==
          "Function F()\n    // c\nEnd Function\n");
    unchanged("Function F()\n    Print(1)\n\n    // c\nEnd Function\n");
}

TEST_CASE("formatting puts a declaration's parameters 8 deep with its attribute on its line",
          "[format]") {
    CHECK(
        formatted("@Inline Function Lerp(\na: Float, b: Float): Float\n    Return a\nEnd "
                  "Function\n") ==
        "@Inline Function Lerp(\n        a: Float, b: Float): Float\n    Return a\nEnd Function\n");
    CHECK(formatted(
              "@Renamed(\"x\") Function Lerp(\na: Float): Float\n    Return a\nEnd Function\n") ==
          "@Renamed(\"x\") Function Lerp(\n        a: Float): Float\n    Return a\nEnd Function\n");
}

TEST_CASE("formatting counts a wide character's columns", "[format]") {
    CHECK(formatted("Print(aaaa, \"中文中文中文中文\", bbbbbbbb, ccc)\n", 40) ==
          "Print(aaaa, \"中文中文中文中文\",\n      bbbbbbbb, ccc)\n");
    unchanged("Print(aaaa, \"éééééééééééééé\", bbbbbbbb)\n", 40);
}

TEST_CASE("formatting's check tells what changes meaning from what doesn't", "[format]") {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb",
                                 "x = a+b\nPrint(1)\nLocal y: List<Int>=[]\n");
    CHECK(fb::same_meaning(file, "x = a + b\nPrint(1)\nLocal y: List<Int> = []\n"));
    CHECK_FALSE(fb::same_meaning(file, "x = a + c\nPrint(1)\nLocal y: List<Int> = []\n"));
    CHECK_FALSE(fb::same_meaning(file, "x = a + b Print(1)\nLocal y: List<Int> = []\n"));
    CHECK_FALSE(fb::same_meaning(file, "x = a\n+ b\nPrint(1)\nLocal y: List<Int> = []\n"));
}

TEST_CASE("formatting says an #If that holds part of a statement keeps it from formatting",
          "[format]") {
    for (const std::string_view text : {
             "#If DEBUG\nFunction F(): Int\n#Else\nFunction F(): Int\n#End If\n    Return 1\nEnd "
             "Function\n",
             "Local a = True\n#If DEBUG\nIf a\n#Else\nIf !a\n#End If\n    Print(\"x\")\nEnd If\n",
         }) {
        INFO(text);
        const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
        const std::expected<std::string, std::vector<fb::diagnostic_t>> result =
            fb::format_source(file);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().size() == 1);
        CHECK(result.error().front().message ==
              "fb fmt leaves an #If's lines as they are, so it formats a file only when each #If "
              "holds whole statements, and this one holds part of one");
        CHECK(file.slice(result.error().front().span) == "#If DEBUG");
    }
}

TEST_CASE("formatting keeps a block comment that ends on an #If's line once", "[format]") {
    unchanged("Local a = 1\n/* x\ny */ #If X\nLocal b = 2\n#End If\nPrint(a)\n");
}

TEST_CASE("std, tests/programs, and tests/packages are formatted", "[format][repository]") {
    for (const char* folder : { "std", "tests/programs", "tests/packages" }) {
        std::error_code error;
        auto entry = std::filesystem::recursive_directory_iterator(repository() / folder, error);
        for (; !error && entry != std::filesystem::recursive_directory_iterator();
             entry.increment(error)) {
            if (entry->path().extension() != ".fb") {
                continue;
            }

            const std::string text = read_file(entry->path());
            const fb::source_file_t file(fb::file_id_t{}, entry->path().string(), text);
            const std::expected<std::string, std::vector<fb::diagnostic_t>> result =
                fb::format_source(file);
            INFO(entry->path().string() + ": fb fmt formats it");
            if (result) {
                CHECK(*result == text);
            } else {
                // Only a test program that doesn't parse is left as it is, not one fb fmt refuses.
                CHECK_FALSE(fb::format_unchecked(file).has_value());
            }
        }

        CHECK_FALSE(error);
    }
}

TEST_CASE("formatting every .fb in the repository keeps what it means", "[format][repository]") {
    // The symbols each platform's builds define, so each #If branch is in some tree.
    const std::array<std::vector<std::string_view>, 3> symbol_sets = {
        std::vector<std::string_view>{ "DEBUG", "PLATFORM_WINDOWS" },
        std::vector<std::string_view>{ "RELEASE", "PLATFORM_LINUX" },
        std::vector<std::string_view>{ "DEBUG", "PLATFORM_MACOS" },
    };
    std::size_t files = 0;
    std::error_code error;
    auto entry = std::filesystem::recursive_directory_iterator(repository(), error);
    for (; !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        // A build folder, by any name, as CLion's cmake-build-debug in the repository, holds
        // the tests' own files, which other tests write and remove as this one runs; npm's
        // packages are no FutureBlitz.
        const std::filesystem::path& path = entry->path();
        if (entry->is_directory() &&
            (path.filename().string().starts_with('.') || path.filename() == "build" ||
             path.filename() == "node_modules" ||
             std::filesystem::exists(path / "CMakeCache.txt"))) {
            entry.disable_recursion_pending();
            continue;
        }

        if (path.extension() != ".fb") {
            continue;
        }

        const fb::source_file_t file(fb::file_id_t{}, path.string(), read_file(path));
        const std::expected<std::string, std::vector<fb::diagnostic_t>> result =
            fb::format_unchecked(file);
        // A test program that doesn't parse is left as it is.
        if (!result) {
            continue;
        }

        ++files;
        INFO(path.string());
        const fb::source_file_t after(fb::file_id_t{}, path.string(), *result);
        for (const std::vector<std::string_view>& defined : symbol_sets) {
            CHECK(tree_of(after, defined) == tree_of(file, defined));
        }

        const std::expected<std::string, std::vector<fb::diagnostic_t>> again =
            fb::format_unchecked(after);
        REQUIRE(again.has_value());
        CHECK(*again == *result);
    }

    CHECK_FALSE(error);
    CHECK(files > 1000);
}
