#include "fb/dump.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct parsed_t {
    std::string dump;
    std::vector<std::string> errors; // "begin-end: message", from the lexer and the parser
};

/// Lexes and parses `text` as a file named "test.fb". Error spans are byte offsets into `text`.
parsed_t parse_text(const std::string_view text) {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    const fb::ast_t ast = fb::parse(file, tokens, bag);

    parsed_t parsed{ .dump = fb::dump_ast(ast) };
    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        parsed.errors.push_back(std::format("{}-{}: {}", diagnostic.span.begin, diagnostic.span.end,
                                            diagnostic.message));
    }

    return parsed;
}

/// The dump of one expression, parsed as the value in "_ = <text>".
std::string expression(const std::string_view text) {
    const parsed_t parsed = parse_text(std::format("_ = {}", text));
    INFO(text);
    CHECK(parsed.errors.empty());
    constexpr std::string_view prefix = "(= _ ";
    if (!parsed.dump.starts_with(prefix)) {
        return parsed.dump;
    }

    return parsed.dump.substr(prefix.size(), parsed.dump.size() - prefix.size() - 1);
}

} // namespace

// Expressions

TEST_CASE("literals", "[parser]") {
    CHECK(expression("42") == "(int 42)");
    CHECK(expression("1_000") == "(int 1000)");
    CHECK(expression("0xFF") == "(int 255)");
    CHECK(expression("1.5") == "(decimal 1.5)");
    CHECK(expression("2e3") == "(decimal 2000)");
    CHECK(expression("'A'") == "(char 'A')");
    CHECK(expression(R"('\n')") == "(char U+000A)");
    CHECK(expression("True") == "(bool True)");
    CHECK(expression("False") == "(bool False)");
    CHECK(expression("Null") == "(null)");
    CHECK(expression("hp") == "hp");
}

TEST_CASE("binary operators follow the precedence table", "[parser]") {
    CHECK(expression("1 + 2 * 3") == "(+ (int 1) (* (int 2) (int 3)))");
    CHECK(expression("(1 + 2) * 3") == "(* (+ (int 1) (int 2)) (int 3))");
    CHECK(expression("a - b - c") == "(- (- a b) c)");
    CHECK(expression("a ** b ** c") == "(** a (** b c))");

    // Unlike in C, '&' binds tighter than '=='.
    CHECK(expression("x & mask == 0") == "(== (& x mask) (int 0))");
    CHECK(expression("a || b && c") == "(|| a (&& b c))");
    CHECK(expression("a ?? b ?? c") == "(?? a (?? b c))");
    CHECK(expression("a < b == c > d") == "(== (< a b) (> c d))");
    CHECK(expression("a | b ^ c & d") == "(| a (^ b (& c d)))");
    CHECK(expression("a << b + c") == "(<< a (+ b c))");
}

TEST_CASE("'>>' is a shift only when its two '>' touch", "[parser]") {
    CHECK(expression("a >> b") == "(>> a b)");
    CHECK(expression("a >> b << c") == "(<< (>> a b) c)");

    // Apart, the first '>' is a comparison, and the second (8-9) can't start its right side.
    CHECK(parse_text("_ = a > > b").errors ==
          std::vector<std::string>{ "8-9: expected an expression, found '>'" });
}

TEST_CASE("unary operators bind tighter than binary ones", "[parser]") {
    CHECK(expression("-a + b") == "(+ (- a) b)");
    CHECK(expression("!a && b") == "(&& (! a) b)");
    CHECK(expression("~a") == "(~ a)");
    CHECK(expression("-a.b") == "(- (. a b))");

    // Unlike in math, the minus applies before the power.
    CHECK(expression("-a ** b") == "(** (- a) b)");
}

TEST_CASE("the ternary is right-associative and binds loosest", "[parser]") {
    CHECK(expression("c ? a : b") == "(? c a b)");
    CHECK(expression("c ? a : d ? e : f") == "(? c a (? d e f))");
    CHECK(expression("a + b ? c : d") == "(? (+ a b) c d)");
}

TEST_CASE("parse_layout tells the formatter each line's blocks and each token's part", "[parser]") {
    const std::string text = "Select x\n    Case 1\n        y = -a > b ? c : d\nEnd Select\n"
                             "Class C<T>\n    Where T: I\n    Property P: Int\n        Get => 1\n"
                             "    End Property\nEnd Class\nz = F(1,\n      2) +\n    G<Int>()\n";
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", text);
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    fb::layout_t layout;
    const fb::ast_t ast = fb::parse_layout(file, tokens, bag, layout);
    REQUIRE_FALSE(bag.has_errors());
    CHECK(fb::dump_ast(ast) == fb::dump_ast(fb::parse(file, tokens, bag)));

    // Where each line's first word, or its token, is, and the depth the formatter gives it.
    const auto depth = [&](const std::string_view first) {
        const auto found = layout.depths.find(static_cast<std::uint32_t>(text.find(first)));
        return found != layout.depths.end() ? std::optional(found->second) : std::nullopt;
    };
    CHECK(depth("Select") == 0U);
    CHECK(depth("Case") == 1U);
    CHECK(depth("y =") == 2U);
    CHECK(depth("End Select") == 0U);
    CHECK(depth("Where") == 1U);
    CHECK(depth("Property") == 1U);
    CHECK(depth("Get") == 2U);
    CHECK(depth("End Property") == 1U);

    // The line breaks inside F's brackets and after '+' go on with the statement.
    std::size_t ends = 0;
    for (const fb::token_t& token : tokens) {
        if (token.kind == fb::token_kind_t::NEWLINE) {
            ends += layout.line_ends.contains(token.span.begin) ? 1U : 0U;
        }
    }

    CHECK(ends == 11);
    CHECK(layout.breaks.contains(static_cast<std::uint32_t>(text.find(") +") + 3)));

    const auto role = [&](const std::string_view at, const std::size_t from = 0) {
        const auto found = layout.roles.find(static_cast<std::uint32_t>(text.find(at, from)));
        return found != layout.roles.end() ? std::optional(found->second) : std::nullopt;
    };
    CHECK(role("-a") == fb::token_role_t::UNARY);
    CHECK(role("> b") == fb::token_role_t::BINARY);
    CHECK(role("? c") == fb::token_role_t::TERNARY);
    CHECK(role(": d") == fb::token_role_t::TERNARY);
    CHECK(role("<T>") == fb::token_role_t::GENERIC);
    CHECK(role("<Int>") == fb::token_role_t::GENERIC);
    CHECK(role(">()") == fb::token_role_t::GENERIC);
    CHECK(role(": I") == std::nullopt);
}

TEST_CASE("parse_layout's breaks are after a range's To, Until, and Step, and a constraint's '&'",
          "[parser]") {
    const std::string text = "r = 1 To\n    3\ns = 1 Until\n    9 Step\n    2\n"
                             "Function F<T>()\n    Where T: A &\n        B\nEnd Function\n";
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", text);
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    fb::layout_t layout;
    static_cast<void>(fb::parse_layout(file, tokens, bag, layout));
    CHECK_FALSE(bag.has_errors());
    for (const std::string_view after : { "To", "Until", "Step", "&" }) {
        INFO(after);
        CHECK(layout.breaks.contains(static_cast<std::uint32_t>(text.find(after) + after.size())));
    }
}

TEST_CASE("postfix chains", "[parser]") {
    CHECK(expression("a.b.c(1, 2)[3]") == "(index (call (. (. a b) c) (int 1) (int 2)) (int 3))");
    CHECK(expression("a?.b") == "(?. a b)");
    CHECK(expression("f()") == "(call f)");
    CHECK(expression("grid[x, y]") == "(index grid x y)");
    CHECK(expression("a[1..n]") == "(slice a (int 1) .. n)");
    CHECK(expression("a[i + 1..]") == "(slice a (+ i (int 1)) ..)");
    CHECK(expression("a[..2]") == "(slice a .. (int 2))");
    CHECK(expression("a[..]") == "(slice a ..)");
    CHECK(expression(R"(Spawn(pos, name: "boss"))") == R"((call Spawn pos name: (string "boss")))");
}

TEST_CASE("type tests and casts", "[parser]") {
    CHECK(expression("x As Float") == "(as x Float)");
    CHECK(expression("e As? Boss") == "(as? e Boss)");
    CHECK(expression("e Is Boss") == "(is e Boss)");

    // 'As' is below '+' in the table, but its right side is a type, so a '+' after it applies to
    // the cast.
    CHECK(expression("x As Int + 1") == "(+ (as x Int) (int 1))");
    CHECK(expression("a + b As Int") == "(as (+ a b) Int)");
    CHECK(expression("e Is Boss ? 1 : 2") == "(? (is e Boss) (int 1) (int 2))");
}

TEST_CASE("string values", "[parser]") {
    CHECK(expression(R"("HP: {hp}/{max}")") == R"((string "HP: " hp "/" max))");
    CHECK(expression(R"("a\tb {{x}}")") == R"((string "a\tb {{x}}"))");
    CHECK(expression(R"("")") == "(string)");

    // The dump writes a value as source would, with its backslashes escaped and its braces
    // doubled, and every control character escaped, so none reaches the terminal.
    CHECK(expression(R"(r"C:\raw\{x}")") == R"((string "C:\\raw\\{{x}}"))");
    CHECK(expression(R"("a\x1B[31mb\r\0\x7F\u{9B}")") == R"((string "a\x1B[31mb\r\0\x7F\u{9B}"))");
    CHECK(expression(R"("{name ?? "nobody"}")") == R"((string (?? name (string "nobody"))))");
    CHECK(expression(R"("\u{1F600}")") == "(string \"😀\")");
}

TEST_CASE("an interpolation's value can have a format spec", "[parser]") {
    CHECK(expression(R"("{price:.2} {n}")") == R"((string (format price ".2") " " n))");
    CHECK(expression(R"("{a ? b : c:>4}")") == R"((string (format (? a b c) ">4")))");
    CHECK(expression(R"("{x:}")") == R"((string (format x "")))");

    // A ':' with no space after it starts a spec, and one with a space can't.
    CHECK(parse_text(R"(_ = "{a ? b :c}")").errors ==
          std::vector<std::string>{ "12-14: in an interpolation, a ':' with no space after it "
                                    "starts a format spec, so a ternary's needs one" });
    CHECK(
        parse_text(R"(_ = "{n: >5}")").errors ==
        std::vector<std::string>{ "7-8: a format spec comes right after its ':', with no space" });
}

TEST_CASE("multi-line strings lose the closing quotes' indentation", "[parser]") {
    // The line breaks after the opening quotes and before the closing ones are dropped, and a
    // CRLF becomes a plain line break.
    CHECK(expression("\"\"\"\n    first\n      second\n\n    \"\"\"") ==
          R"((string "first\n  second\n"))");
    CHECK(expression("\"\"\"\r\n  a\r\n  b\r\n  \"\"\"") == R"((string "a\nb"))");
    CHECK(expression("\"\"\"\n  x = {x}\n  \"\"\"") == R"((string "x = " x))");
    // A line that starts inside an interpolation isn't string text, so its indentation is
    // neither checked nor stripped, and the text after the '}' goes on from it.
    CHECK(expression("\"\"\"\n    one {x +\n1} two\n    three\n    \"\"\"") ==
          R"((string "one " (+ x (int 1)) " two\nthree"))");
    CHECK(expression("\"\"\"\n\"\"\"") == "(string)");
}

TEST_CASE("a statement continues after an operator, '=', or inside brackets", "[parser]") {
    CHECK(parse_text("x = 1 +\n    2").dump == "(= x (+ (int 1) (int 2)))");
    CHECK(parse_text("Local y =\n    3").dump == "(local y = (int 3))");
    CHECK(parse_text("Print(1,\n      2)").dump == "(call Print (int 1) (int 2))");
    CHECK(parse_text("x = (1\n     + 2)").dump == "(= x (+ (int 1) (int 2)))");

    // Wherever a statement takes a ',', '=', or a constraint's '&', a line break can follow it.
    for (const auto& [broken, joined] :
         std::to_array<std::pair<std::string_view, std::string_view>>({
             { "Enum E\nA =\n    1\nEnd Enum", "Enum E\nA = 1\nEnd Enum" },
             { "For i =\n    0 To 5\nEnd For", "For i = 0 To 5\nEnd For" },
             { "For i,\n    x In items\nEnd For", "For i, x In items\nEnd For" },
             { "Class C Implements A,\n    B\nEnd Class", "Class C Implements A, B\nEnd Class" },
             {
                 "Interface I Extends A,\n    B\nEnd Interface",
                 "Interface I Extends A, B\nEnd Interface",
             },
             {
                 "Function F() Throws E1,\n    E2\nEnd Function",
                 "Function F() Throws E1, E2\nEnd Function",
             },
             {
                 "Function F<T>()\nWhere T: Hashable &\n    Printable\nEnd Function",
                 "Function F<T>()\nWhere T: Hashable & Printable\nEnd Function",
             },
             {
                 "Extern \"C\"\nFunction Abs(n: Int32): Int32 =\n    \"abs\"\nEnd Extern",
                 "Extern \"C\"\nFunction Abs(n: Int32): Int32 = \"abs\"\nEnd Extern",
             },
             { "Local a,\n    b = (1, 2)", "Local a, b = (1, 2)" },
             { "a,\n    b = (b, a)", "a, b = (b, a)" },
         })) {
        INFO(broken);
        const parsed_t parsed = parse_text(broken);
        CHECK(parsed.errors.empty());
        CHECK(parsed.dump == parse_text(joined).dump);
    }
}

// Statements

TEST_CASE("variable declarations", "[parser]") {
    CHECK(parse_text("Local hp: Int = 100").dump == "(local hp: Int = (int 100))");
    CHECK(parse_text(R"(Local name = "grunt")").dump == R"((local name = (string "grunt")))");
    CHECK(parse_text("Local target: Enemy?").dump == "(local target: Enemy?)");
    CHECK(parse_text("Global scores: Int[]").dump == "(global scores: Int[])");
    CHECK(parse_text("Const MAX = 10").dump == "(const MAX = (int 10))");
}

TEST_CASE("assignments and increments", "[parser]") {
    const parsed_t parsed = parse_text("x = 1\nx += 2\nx >>= 3\ngrid[i] *= 2\ni++\ni--");
    CHECK(parsed.dump == "(= x (int 1))\n"
                         "(+= x (int 2))\n"
                         "(>>= x (int 3))\n"
                         "(*= (index grid i) (int 2))\n"
                         "(++ i)\n"
                         "(-- i)");
    CHECK(parsed.errors.empty());
}

TEST_CASE("functions", "[parser]") {
    const parsed_t parsed = parse_text("Function Add(a: Int, b: Int = 1): Int\n"
                                       "    Return a + b\n"
                                       "End Function");
    CHECK(parsed.dump ==
          "(function Add (a: Int) (b: Int = (int 1)) -> Int (block (return (+ a b))))");
    CHECK(parsed.errors.empty());

    CHECK(parse_text("Function Square(x: Float): Float => x * x").dump ==
          "(function Square (x: Float) -> Float (block (return (* x x))))");
    CHECK(parse_text("Function Main()\nEnd Function").dump == "(function Main (block))");
}

TEST_CASE("a variadic parameter takes the rest of the arguments", "[parser]") {
    CHECK(parse_text("Function Sum(first: Int, rest: Int...): Int => first").dump ==
          "(function Sum (first: Int) (rest: Int ...) -> Int (block (return first)))");
}

TEST_CASE("ByRef parameters and arguments", "[parser]") {
    const parsed_t parsed = parse_text("Function Swap(ByRef a: Int, ByRef b: Int)\n"
                                       "End Function\n"
                                       "Swap(ByRef x, b: ByRef y)");
    CHECK(parsed.dump == "(function Swap (ByRef a: Int) (ByRef b: Int) (block))\n"
                         "(call Swap ByRef x b: ByRef y)");
    CHECK(parsed.errors.empty());
}

TEST_CASE("If blocks", "[parser]") {
    const parsed_t parsed = parse_text("If hp <= 0\n"
                                       "    Die()\n"
                                       "ElseIf hp < 20\n"
                                       "    Flash()\n"
                                       "Else\n"
                                       "    Regen()\n"
                                       "End If");
    CHECK(parsed.dump == "(if (<= hp (int 0)) (block (call Die)) "
                         "(elseif (< hp (int 20)) (block (call Flash))) "
                         "(else (block (call Regen))))");
    CHECK(parsed.errors.empty());
}

TEST_CASE("single-line If", "[parser]") {
    CHECK(parse_text("If hp <= 0 Then Die()").dump ==
          "(if-then (<= hp (int 0)) (block (call Die)))");
    CHECK(parse_text("If a Then x = 1 Else x = 2").dump ==
          "(if-then a (block (= x (int 1))) (else (block (= x (int 2)))))");
    CHECK(parse_text("If done Then Return Else Continue").dump ==
          "(if-then done (block (return)) (else (block (continue))))");
}

TEST_CASE("loops", "[parser]") {
    CHECK(parse_text("While running\n    Tick()\nEnd While").dump ==
          "(while running (block (call Tick)))");
    CHECK(parse_text("Repeat\n    Tick()\nUntil done").dump ==
          "(repeat (block (call Tick)) until done)");
    CHECK(parse_text("Loop\nEnd Loop").dump == "(loop (block))");
    CHECK(parse_text("For i = 0 To 9\nEnd For").dump == "(for i (int 0) to (int 9) (block))");
    CHECK(parse_text("For i = 10 Until 0 Step -1\nEnd For").dump ==
          "(for i (int 10) until (int 0) step (- (int 1)) (block))");
    CHECK(parse_text("For e In enemies\nEnd For").dump == "(for-in e enemies (block))");
    CHECK(parse_text("For i, e In enemies\nEnd For").dump == "(for-in i e enemies (block))");
}

TEST_CASE("ranges are values, outside a For loop's own bounds", "[parser]") {
    // Looser than every binary operator, so each end is a whole expression.
    CHECK(parse_text("Local r = 0 Until n - 1").dump ==
          "(local r = (until (int 0) (- n (int 1))))");
    CHECK(parse_text("Local r = 1 To count ?? 0 Step 2").dump ==
          "(local r = (to (int 1) (?? count (int 0)) step (int 2)))");
    CHECK(parse_text("Local r = 0 To 10 Step c ? 1 : 2").dump ==
          "(local r = (to (int 0) (int 10) step (? c (int 1) (int 2))))");
    CHECK(parse_text("Local r = (0 Until 10).Sum()").dump ==
          "(local r = (call (. (until (int 0) (int 10)) Sum)))");
    CHECK(parse_text("For i In 0 Until 10\nEnd For").dump ==
          "(for-in i (until (int 0) (int 10)) (block))");
    // A For loop's own To and Until are its bounds, and inside brackets they make ranges again.
    CHECK(parse_text("For i = c ? 0 : 1 To 9\nEnd For").dump ==
          "(for i (? c (int 0) (int 1)) to (int 9) (block))");
    CHECK(parse_text("For i = Sum(0 To 3) To 9\nEnd For").dump ==
          "(for i (call Sum (to (int 0) (int 3))) to (int 9) (block))");
    CHECK(parse_text("Select n\nCase 1 To 3\nEnd Select").errors.empty());
}

TEST_CASE("labels, Break, and Continue", "[parser]") {
    const parsed_t parsed = parse_text("Outer: For y = 0 Until h\n"
                                       "    For x = 0 Until w\n"
                                       "        If x == y Then Continue\n"
                                       "        Break Outer\n"
                                       "    End For\n"
                                       "End For");
    CHECK(parsed.dump == "(for Outer: y (int 0) until h (block (for x (int 0) until w (block "
                         "(if-then (== x y) (block (continue))) (break Outer)))))");
    CHECK(parsed.errors.empty());
}

TEST_CASE("Select", "[parser]") {
    const parsed_t parsed = parse_text("Select state\n"
                                       "    Case 1, 2\n"
                                       "        A()\n"
                                       "    Case 3 To 5\n"
                                       "        B()\n"
                                       "    Default\n"
                                       "        C()\n"
                                       "End Select");
    CHECK(parsed.dump == "(select state (case (int 1) (int 2) (block (call A))) "
                         "(case (range (int 3) (int 5)) (block (call B))) "
                         "(default (block (call C))))");
    CHECK(parsed.errors.empty());
}

TEST_CASE("Return", "[parser]") {
    CHECK(parse_text("Return").dump == "(return)");
    CHECK(parse_text("Return x + 1").dump == "(return (+ x (int 1)))");
}

TEST_CASE("a small program parses cleanly", "[parser]") {
    const parsed_t parsed = parse_text("Function Fib(n: Int): Int\n"
                                       "    If n < 2 Then Return n\n"
                                       "    Return Fib(n - 1) + Fib(n - 2)\n"
                                       "End Function\n"
                                       "\n"
                                       "For i = 0 To 10\n"
                                       "    Print(\"Fib({i}) = {Fib(i)}\")\n"
                                       "End For\n");
    CHECK(parsed.errors.empty());
    CHECK(parsed.dump == "(function Fib (n: Int) -> Int (block "
                         "(if-then (< n (int 2)) (block (return n))) "
                         "(return (+ (call Fib (- n (int 1))) (call Fib (- n (int 2)))))))\n"
                         "(for i (int 0) to (int 10) (block "
                         "(call Print (string \"Fib(\" i \") = \" (call Fib i)))))");
}

// Errors and recovery

TEST_CASE("a missing expression is reported once, and the next line still parses", "[parser]") {
    const parsed_t parsed = parse_text("x = ) )\ny = 2");
    CHECK(parsed.dump == "(= x (error))\n(= y (int 2))");
    CHECK(parsed.errors == std::vector<std::string>{ "4-5: expected an expression, found ')'" });

    // An operator that ends a line that already failed doesn't take the next line in.
    const parsed_t operand = parse_text("x = 1 + *\ny = 2\n*/\nz = 3");
    CHECK(operand.dump == "(= x (+ (int 1) (* (error) (error))))\n"
                          "(= y (int 2))\n"
                          "(/ (* (error) (error)) (error))\n"
                          "(= z (int 3))");
    CHECK(operand.errors == std::vector<std::string>{ "8-9: expected an expression, found '*'",
                                                      "16-17: expected an expression, found '*'" });

    // After an error the parser gets past, an operator or ',' at the line's end still does.
    const parsed_t decimal = parse_text("x = 1e999 +\n    2.0\ny = 3");
    CHECK(decimal.dump == "(= x (+ (decimal 0) (decimal 2)))\n(= y (int 3))");
    CHECK(decimal.errors == std::vector<std::string>{ "4-9: decimal literal is out of range" });
    const parsed_t listed =
        parse_text("Public @Deprecated(\"old\") Class Person Implements Named,\n"
                   "        Aged\n"
                   "End Class");
    CHECK(listed.dump == "(class @Deprecated(\"old\") Person Implements Named Aged (block))");
    CHECK(listed.errors.size() == 1);
}

TEST_CASE("a forgotten ')' is reported at the end of its line", "[parser]") {
    const parsed_t parsed = parse_text("Print(1\nx = 2");
    CHECK(parsed.dump == "(call Print (int 1))\n(= x (int 2))");

    // 7-7 is an empty span right after the "1", where the ')' belongs.
    CHECK(parsed.errors == std::vector<std::string>{ "7-7: expected ')', found newline" });
}

TEST_CASE("a missing End is reported at end of file, pointing at the opening keyword", "[parser]") {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", "If ready\n    Go()\n");
    fb::diagnostic_bag_t bag;
    const fb::ast_t ast = fb::parse(file, fb::lex(file, bag), bag);
    CHECK(fb::dump_ast(ast) == "(if ready (block (call Go)))");

    REQUIRE(bag.diagnostics().size() == 1);
    const fb::diagnostic_t& diagnostic = bag.diagnostics().front();
    CHECK(diagnostic.message == "expected 'End If', found end of file");
    REQUIRE(diagnostic.labels.size() == 1);
    CHECK(diagnostic.labels.front().span == fb::span_t{ .begin = 0, .end = 2 });
}

TEST_CASE("an End for an outer block also closes the inner one", "[parser]") {
    const parsed_t parsed = parse_text("While a\n"
                                       "    If b\n"
                                       "        c()\n"
                                       "End While");
    CHECK(parsed.dump == "(while a (block (if b (block (call c)))))");

    // Errors about an End point at its "End" keyword, here 29-32.
    CHECK(parsed.errors ==
          std::vector<std::string>{ "29-32: expected 'End If', found 'End While'" });
}

TEST_CASE("a wrong End that closes nothing else is taken as the block's End", "[parser]") {
    const parsed_t parsed = parse_text("If a\n    b()\nEnd While\nx = 1");
    CHECK(parsed.dump == "(if a (block (call b)))\n(= x (int 1))");
    CHECK(parsed.errors ==
          std::vector<std::string>{ "13-16: expected 'End If', found 'End While'" });
}

TEST_CASE("an End in brackets is shown with the keyword after it on its line only", "[parser]") {
    const parsed_t parsed = parse_text("Function F()\n    G(1, End\n    H())\nEnd Function");
    REQUIRE_FALSE(parsed.errors.empty());
    CHECK(parsed.errors.front() == "22-25: expected an expression, found 'End'");
}

TEST_CASE("a missing End inside a Get or Set is one error", "[parser]") {
    // The End Get is the Get's, so its body doesn't run on to End Property.
    const parsed_t parsed = parse_text("Class C\n"
                                       "    Property P: Int\n"
                                       "        Get\n"
                                       "            If x\n"
                                       "                Return 1\n"
                                       "        End Get\n"
                                       "    End Property\n"
                                       "End Class");
    CHECK(parsed.errors.size() == 1);
    CHECK(parsed.errors.front().ends_with("expected 'End If', found 'End Get'"));
}

TEST_CASE("a stray End is reported and skipped", "[parser]") {
    const parsed_t parsed = parse_text("End If\nx = 1");
    CHECK(parsed.dump == "(= x (int 1))");
    CHECK(parsed.errors == std::vector<std::string>{ "0-3: unexpected 'End If'" });
}

TEST_CASE("a Repeat without Until is reported", "[parser]") {
    CHECK(parse_text("Repeat\n    Tick()\n").errors ==
          std::vector<std::string>{ "18-18: expected 'Until', found end of file" });
}

TEST_CASE("only calls, assignments, and ++/-- can be statements", "[parser]") {
    CHECK(parse_text("x + 1").errors ==
          std::vector<std::string>{ "0-5: expression has no effect" });
    CHECK(parse_text(R"(Print "hi")").errors ==
          std::vector<std::string>{ "0-5: calls need parentheses, like Print(...)" });
}

TEST_CASE("Unsafe alone on a line opens a block, and before a Function or Method it's a modifier",
          "[parser]") {
    const parsed_t parsed = parse_text("Unsafe\n"
                                       "    p.Value = 1\n"
                                       "End Unsafe\n"
                                       "Unsafe Function F()\n"
                                       "End Function\n"
                                       "Public Unsafe Static Method M()\n"
                                       "End Method\n"
                                       "x = 1");
    CHECK(parsed.dump == "(unsafe (block (= (. p Value) (int 1))))\n"
                         "(function Unsafe F (block))\n"
                         "(method Public Static Unsafe M (block))\n"
                         "(= x (int 1))");
    CHECK(parsed.errors.empty());

    // Anything else it goes before is a mistake.
    CHECK(parse_text("Unsafe Local y = 2").errors ==
          std::vector<std::string>{ "0-6: Unsafe goes before a Function or Method, or alone on a "
                                    "line, where it starts an Unsafe block" });
}

TEST_CASE("invalid characters aren't reported twice", "[parser]") {
    CHECK(parse_text("x = $").errors ==
          std::vector<std::string>{ "4-5: unexpected character '$'" });
}

TEST_CASE("a label must be followed by a loop", "[parser]") {
    CHECK(parse_text("Outer: x = 1").errors ==
          std::vector<std::string>{ "0-5: a label must be followed by a loop" });
}

TEST_CASE("block statements can't go in a single-line If", "[parser]") {
    CHECK(parse_text("If a Then While b").errors ==
          std::vector<std::string>{ "10-15: a single-line If can't contain 'While'" });

    // A labeled loop, an Unsafe block, and an Extern block are blocks too.
    CHECK(parse_text("If a Then L: While b").errors ==
          std::vector<std::string>{ "13-18: a single-line If can't contain 'While'" });
    CHECK(parse_text("If a Then F() Else M: For i = 0 To 1").errors ==
          std::vector<std::string>{ "22-25: a single-line If can't contain 'For'" });
    CHECK(parse_text("If a Then Unsafe\nF()").errors ==
          std::vector<std::string>{ "10-16: a single-line If can't contain an Unsafe block" });
    CHECK(parse_text("If a Then Extern \"C\"").errors ==
          std::vector<std::string>{ "10-16: a single-line If can't contain 'Extern'" });

    // A label before anything else is reported as it is anywhere.
    CHECK(parse_text("If a Then L: x = 1").errors ==
          std::vector<std::string>{ "10-11: a label must be followed by a loop" });
}

TEST_CASE("a '>' that closes type arguments can touch the '=' after it", "[parser]") {
    for (const auto& [touching, spaced] :
         std::to_array<std::pair<std::string_view, std::string_view>>({
             { "Local t: Stack<Int>= Stack<Int>()", "Local t: Stack<Int> = Stack<Int>()" },
             { "Local s: Stack<Stack<Int>>= x", "Local s: Stack<Stack<Int>> = x" },
             { "Local w: Weak<Node>= n", "Local w: Weak<Node> = n" },
             {
                 "Function F(m: Map<K, V>= x)\nEnd Function",
                 "Function F(m: Map<K, V> = x)\nEnd Function",
             },
         })) {
        INFO(touching);
        const parsed_t parsed = parse_text(touching);
        CHECK(parsed.errors.empty());
        CHECK(parsed.dump == parse_text(spaced).dump);
    }

    // Elsewhere ">=", ">>", and ">>=" are what they always were.
    CHECK(expression("a >= b") == "(>= a b)");
    CHECK(expression("a >= b >> c") == "(>= a (>> b c))");
    CHECK(parse_text("x >>= 3").dump == "(>>= x (int 3))");
    CHECK(parse_text("Struct S\nOperator >=(other: S): Bool => True\nEnd Struct").errors.empty());
}

TEST_CASE("Blitz's New in an expression says how an object is made", "[parser]") {
    const parsed_t parsed = parse_text("Local f = New Foo()");
    CHECK(parsed.errors == std::vector<std::string>{ "10-13: an object is made by calling its "
                                                     "type, like Foo(); New only declares a "
                                                     "constructor" });
    // What comes after it is parsed, as the call it should be.
    CHECK(parsed.dump == "(local f = (call Foo))");
    // Blitz's own "New Foo", with no '(', is that call too, and a type is named as it's written.
    const parsed_t bare = parse_text("Local f = New Foo\nLocal g = New Foo().x");
    CHECK(bare.dump == "(local f = (call Foo))\n(local g = (. (call Foo) x))");
    CHECK(bare.errors == std::vector<std::string>{
                             "10-13: an object is made by calling its type, like Foo(); New only "
                             "declares a constructor",
                             "28-31: an object is made by calling its type, like Foo(); New only "
                             "declares a constructor",
                         });
    const parsed_t named = parse_text("Local c = New shapes.Circle\nLocal s = New Stack<Int>()");
    CHECK(named.dump == "(local c = (call (. shapes Circle)))\n(local s = (call Stack<Int>))");
    CHECK(named.errors == std::vector<std::string>{
                              "10-13: an object is made by calling its type, like "
                              "shapes.Circle(); New only declares a constructor",
                              "38-41: an object is made by calling its type, like Stack<Int>(); "
                              "New only declares a constructor",
                          });
    CHECK(parse_text("Local f = New").errors ==
          std::vector<std::string>{ "10-13: an object is made by calling its type, like "
                                    "Point(); New only declares a constructor" });

    // Many of them are one error, and no deeper to parse.
    std::string news = "Local f = ";
    for (std::size_t i = 0; i < 50000; ++i) {
        news += "New ";
    }

    CHECK(parse_text(news + "Foo()").errors.size() == 1);
}

TEST_CASE("broken input always finishes with at least one error", "[parser]") {
    // Each of these once could have looped forever or crashed in a careless parser.
    constexpr auto inputs = std::to_array<std::string_view>({
        ")))",
        "If",
        "End",
        "Else",
        "For",
        "For i",
        "For i = 0",
        "Function",
        "Function F(",
        "Function F(a: Int, ",
        "Select x\nCase",
        "Select x\nx()\n",
        "Repeat\nUntil",
        "Print(\n",
        "x = (((",
        "x = \"{",
        "x = \"a{b\nc",
        "\"\"\"\n{",
        "@",
        "@Attr(",
        "Class",
        "Class A\nClass B\nEnd Class",
        "If a Then",
        "If a Then x = 1 Else",
        "Outer:",
        "x = 1 +",
        "a.",
        "a?.",
        "f(a b c)",
        "x[",
        "Local",
        "Local x:",
        "x As",
        "Break 5",
        "End End End",
        "ElseIf x\nCase\nDefault\nUntil x",
    });

    for (const std::string_view input : inputs) {
        INFO(input);
        CHECK_FALSE(parse_text(input).errors.empty());
    }
}

TEST_CASE("a mistake in a name or an Operator doesn't hide the lines after it", "[parser]") {
    const parsed_t unsafe = parse_text("Unsafe Enum E\n    A\nEnd Enum\nLocal x = 1");
    CHECK(unsafe.errors == std::vector<std::string>{ "0-6: Unsafe goes before a Function or "
                                                     "Method, or alone on a line, where it starts "
                                                     "an Unsafe block" });
    CHECK(unsafe.dump.ends_with("(local x = (int 1))"));
    // Alone on its line after another modifier, it's still a block, so its End is its own.
    const parsed_t block = parse_text("Public Unsafe\n    Local y = 2\nEnd Unsafe\nLocal x = 1");
    CHECK(block.errors == std::vector<std::string>{ "7-13: Unsafe goes before a Function or "
                                                    "Method, or alone on a line, where it starts "
                                                    "an Unsafe block" });
    CHECK(block.dump == "(unsafe (block (local y = (int 2))))\n(local x = (int 1))");
    CHECK(parse_text("Struct S\n"
                     "    Operator &&(a: S, b: S): Bool => True\n"
                     "    Field X: Int\n"
                     "End Struct")
              .errors == std::vector<std::string>{
                             "22-24: '&&' always means what it does, so it can't be an Operator" });
    CHECK(parse_text("Local Async = 1").errors ==
          std::vector<std::string>{ "6-11: expected identifier, found 'Async'" });
}

TEST_CASE("Structs", "[parser]") {
    const parsed_t parsed = parse_text("Struct Point\n"
                                       "    Field x: Int\n"
                                       "    Field y = 0\n"
                                       "    New(x: Int)\n"
                                       "        Self.x = x\n"
                                       "    End New\n"
                                       "    Method Length(): Float => x\n"
                                       "    ByRef Method Move(dx: Int)\n"
                                       "        x += dx\n"
                                       "    End Method\n"
                                       "End Struct");
    CHECK(parsed.dump == "(struct Point (block (field x: Int) (field y = (int 0)) "
                         "(new (x: Int) (block (= (. Self x) x))) "
                         "(method Length -> Float (block (return x))) "
                         "(byref-method Move (dx: Int) (block (+= x dx)))))");
    CHECK(parsed.errors.empty());

    // New makes its Struct, so it has no return type. The ':' is at 5-6.
    CHECK(parse_text("New(): Int\nEnd New").errors ==
          std::vector<std::string>{ "5-6: New can't have a return type; it makes its Struct" });
}

TEST_CASE("Classes", "[parser]") {
    const parsed_t parsed = parse_text("Class Enemy\n"
                                       "    Field hp: Int\n"
                                       "    Method Destroy()\n"
                                       "    End Method\n"
                                       "End Class");
    CHECK(parsed.dump == "(class Enemy (block (field hp: Int) (method Destroy (block))))");
    CHECK(parsed.errors.empty());
    CHECK(parse_text("Class Boss Extends Enemy Implements Damageable, Shown\nEnd Class").dump ==
          "(class Boss Extends Enemy Implements Damageable Shown (block))");
    CHECK(parse_text("Struct Money Implements Numeric\nEnd Struct").dump ==
          "(struct Money Implements Numeric (block))");
}

TEST_CASE("Extends, modifiers, and Super", "[parser]") {
    // An Abstract Method is only its first line.
    const parsed_t parsed = parse_text("Abstract Class Shape Extends Thing\n"
                                       "    Abstract Method Area(): Float\n"
                                       "    Override Final Method Name(): String => \"s\"\n"
                                       "    New()\n"
                                       "        Super.New(1)\n"
                                       "    End New\n"
                                       "End Class");
    CHECK(parsed.dump == "(class Abstract Shape Extends Thing (block "
                         "(method Abstract Area -> Float (block)) "
                         "(method Final Override Name -> String (block (return (string \"s\")))) "
                         "(new (block (call (. Super New) (int 1))))))");
    CHECK(parsed.errors.empty());

    CHECK(parse_text("Final Final Class A\nEnd Class").errors ==
          std::vector<std::string>{ "6-11: 'Final' is written twice" });
    CHECK(parse_text("Final If True\nEnd If").errors ==
          std::vector<std::string>{ "6-8: 'If' doesn't take modifiers like Static or Final" });
}

TEST_CASE("a Case can match a Class", "[parser]") {
    // "Case e As? Boss" stays a value, a checked cast.
    const parsed_t parsed = parse_text("Select x\n"
                                       "    Case e As Boss\n"
                                       "        e.Enrage()\n"
                                       "    Case e As? Boss\n"
                                       "End Select");
    CHECK(parsed.dump == "(select x (case e As Boss (block (call (. e Enrage)))) "
                         "(case (as? e Boss) (block)))");
    CHECK(parsed.errors.empty());
}

TEST_CASE("array literals and fixed array types", "[parser]") {
    CHECK(parse_text("Local a = [1, 2,\n    3,\n]").dump ==
          "(local a = (array (int 1) (int 2) (int 3)))");
    CHECK(parse_text("Local e = []").dump == "(local e = (array))");
    CHECK(parse_text("Local g: Float[64, 8 * 8]").dump == "(local g: Float[64, (* 8 8)])");
    CHECK(parse_text("Local g: Int[4][]?").dump == "(local g: Int[4][]?)");
}

TEST_CASE("a Map literal is its entries in braces", "[parser]") {
    CHECK(parse_text("Local m = {\"a\": 1, \"b\": x + 1,}").dump ==
          "(local m = (map (entry (string \"a\") (int 1)) (entry (string \"b\") (+ x (int 1)))))");
    CHECK(parse_text("Local e = {}").dump == "(local e = (map))");
    CHECK(parse_text("Local n = {\n    1: {2: 3},\n}").dump ==
          "(local n = (map (entry (int 1) (map (entry (int 2) (int 3))))))");
    CHECK(parse_text("Local m = {\"a\" 1}").errors ==
          std::vector<std::string>{ "15-16: expected ':', found integer literal" });
}

TEST_CASE("Weak<T> and generic types take type arguments", "[parser]") {
    CHECK(parse_text("Field parent: Weak<Node>").dump == "(field parent: Weak<Node>)");
    CHECK(parse_text("Local w: Weak<Weak<Node>>[]").dump == "(local w: Weak<Weak<Node>>[])");
    CHECK(parse_text("Local l: Map<Int, List<String>>").dump ==
          "(local l: Map<Int, List<String>>)");

    // "Weak" is only a keyword before '<'.
    CHECK(parse_text("Local Weak = 1").dump == "(local Weak = (int 1))");
}

TEST_CASE("Static and ReadOnly go before Fields and Methods", "[parser]") {
    CHECK(parse_text("Static ReadOnly Field n: Int = 1").dump ==
          "(field Static ReadOnly n: Int = (int 1))");
    CHECK(parse_text("ReadOnly Static Field n: Int").dump == "(field Static ReadOnly n: Int)");
    CHECK(parse_text("Static Method M()\nEnd Method").dump == "(method Static M (block))");
    CHECK(parse_text("Static Static Field n: Int").errors ==
          std::vector<std::string>{ "7-13: 'Static' is written twice" });
}

TEST_CASE("ThreadLocal goes before a Global", "[parser]") {
    CHECK(parse_text("ThreadLocal Global hits = 0").dump == "(global ThreadLocal hits = (int 0))");
    CHECK(parse_text("Public ThreadLocal Global hits: Int").dump ==
          "(global Public ThreadLocal hits: Int)");
    CHECK(
        parse_text("ThreadLocal Local x = 1").errors ==
        std::vector<std::string>{
            "0-11: ThreadLocal only goes before a Global, which each thread then has its own of" });
    CHECK(
        parse_text("ThreadLocal Static Field x: Int").errors ==
        std::vector<std::string>{
            "0-11: ThreadLocal only goes before a Global, which each thread then has its own of" });
    CHECK(parse_text("ThreadLocal ThreadLocal Global x = 1").errors ==
          std::vector<std::string>{ "12-23: 'ThreadLocal' is written twice" });

    // An attribute after it, or after Shared or Async, is out of place, which is what's said.
    const std::string_view order =
        "attributes go before a declaration's modifiers, as in @Inline Static Function";
    CHECK(parse_text("ThreadLocal @Deprecated Global x = 1").errors ==
          std::vector<std::string>{ std::format("12-13: {}", order) });
    CHECK(parse_text("Shared @Deprecated Class C\nEnd Class").errors ==
          std::vector<std::string>{ std::format("7-8: {}", order) });
    CHECK(parse_text("Async @Deprecated Function F()\nEnd Function").errors ==
          std::vector<std::string>{ std::format("6-7: {}", order) });
}

TEST_CASE("a visibility goes before a declaration, among its modifiers", "[parser]") {
    CHECK(parse_text("Public Function F()\nEnd Function").dump == "(function Public F (block))");
    CHECK(parse_text("Static Public Method M()\nEnd Method").dump ==
          "(method Public Static M (block))");
    CHECK(parse_text("Package Global g = 1").dump == "(global Package g = (int 1))");
    CHECK(parse_text("Protected Field hp: Int").dump == "(field Protected hp: Int)");
    CHECK(parse_text("@Flags Public Enum Layer\n    World\nEnd Enum").dump ==
          "(enum @Flags Public Layer World)");
    CHECK(parse_text("Public Attribute Rename(Name: String)").dump ==
          "(attribute Public Rename (block (field Name: String)))");

    // Private is the default, and shows only where it's written.
    CHECK(parse_text("Private Const C = 1\nConst D = 2").dump ==
          "(const Private C = (int 1))\n(const D = (int 2))");
}

TEST_CASE("visibility mistakes are reported", "[parser]") {
    CHECK(parse_text("Public Private Function F()\nEnd Function").errors ==
          std::vector<std::string>{
              "7-14: this declaration is Public already, and has only one visibility" });
    CHECK(parse_text("Public Print(1)").errors ==
          std::vector<std::string>{
              "0-6: Public only goes before a declaration, like a Function or a Field" });
    CHECK(parse_text("Public Extend Int\nEnd Extend").errors ==
          std::vector<std::string>{
              "0-6: an Extend has no visibility of its own; put Public before its members" });

    // An Enum's members have its visibility, and the member still parses.
    const parsed_t member = parse_text("Enum E\n    Public A\nEnd Enum");
    CHECK(member.dump == "(enum E A)");
    CHECK(member.errors ==
          std::vector<std::string>{ "11-17: an Enum's members have its visibility, so they take no "
                                    "Public; put it before the Enum" });
}

TEST_CASE("a Property has a Get, and maybe a Set", "[parser]") {
    CHECK(parse_text("Property P: Int\n"
                     "    Get => 1\n"
                     "    Set(value)\n"
                     "        Print(value)\n"
                     "    End Set\n"
                     "End Property")
              .dump == "(property P: Int (get (block (return (int 1)))) (set value (block (call "
                       "Print value))))");
    CHECK(parse_text("Override Property Q: Int\n"
                     "    Get\n"
                     "        Return 2\n"
                     "    End Get\n"
                     "End Property")
              .dump == "(property Override Q: Int (get (block (return (int 2)))))");

    // Get and Set are names everywhere else.
    CHECK(parse_text("Get(Set)").dump == "(call Get Set)");
    CHECK(parse_text("Property P: Int\n    Set(v)\n    End Set\nEnd Property").errors ==
          std::vector<std::string>{ "9-10: a Property needs a Get" });
    CHECK(parse_text("Property P: Int\n    Get\n        Return 1\nEnd Property").errors ==
          std::vector<std::string>{ "41-44: expected 'End Get', found 'End Property'" });
}

TEST_CASE("Enums and @Flags", "[parser]") {
    // Members are separated by commas, which can end a line, or by lines.
    CHECK(parse_text("Enum State: UInt8\n"
                     "    Idle, Walk,\n"
                     "    Attack = 10\n"
                     "    Dead\n"
                     "End Enum")
              .dump == "(enum State: UInt8 Idle Walk (= Attack (int 10)) Dead)");
    CHECK(parse_text("@Flags Enum Layer\n    World, Player\nEnd Enum").dump ==
          "(enum @Flags Layer World Player)");
    CHECK(parse_text("@Flags Struct S\nEnd Struct").errors ==
          std::vector<std::string>{ "1-6: @Flags only goes before an Enum" });
    CHECK(parse_text("@Flags @Deprecated Enum E\n    A\nEnd Enum").dump ==
          "(enum @Flags @Deprecated E A)");
}

TEST_CASE("@Open goes before an Enum that isn't @Flags", "[parser]") {
    CHECK(parse_text("@Open Public Enum Kind: Int32\n    Known = 0\nEnd Enum").dump ==
          "(enum @Open Public Kind: Int32 (= Known (int 0)))");
    CHECK(parse_text("@Open Struct S\nEnd Struct").errors ==
          std::vector<std::string>{ "1-5: @Open only goes before an Enum" });
    CHECK(parse_text("@Open @Open Enum E\n    A\nEnd Enum").errors ==
          std::vector<std::string>{ "7-11: @Open is written twice" });
    CHECK(parse_text("@Open(1) Enum E\n    A\nEnd Enum").errors ==
          std::vector<std::string>{ "5-8: @Open takes no arguments" });
    CHECK(parse_text("@Flags @Open Enum E\n    A\nEnd Enum").errors ==
          std::vector<std::string>{ "8-12: a @Flags Enum holds any combination of its flags "
                                    "already, so it isn't @Open too" });
}

TEST_CASE("Defer takes any statement; analysis checks which", "[parser]") {
    CHECK(parse_text("Defer Print(\"bye\")").dump == "(defer (call Print (string \"bye\")))");
    CHECK(parse_text("Defer If x Then f.Close()").dump ==
          "(defer (if-then x (block (call (. f Close)))))");
}

TEST_CASE("Throws, Throw, and Try blocks", "[parser]") {
    CHECK(parse_text("Function F(): Int Throws A, B => 1").dump ==
          "(function F -> Int Throws A B (block (return (int 1))))");
    CHECK(parse_text("Throw E(\"x\")").dump == "(throw (call E (string \"x\")))");
    CHECK(parse_text("Try\n"
                     "    F()\n"
                     "Catch e As E\n"
                     "    G()\n"
                     "Catch e\n"
                     "Finally\n"
                     "    H()\n"
                     "End Try")
              .dump == "(try (block (call F)) (catch e As E (block (call G))) (catch e (block)) "
                       "(finally (block (call H))))");
    CHECK(parse_text("Try\n    F()\nEnd Try").errors ==
          std::vector<std::string>{ "0-3: a Try needs a Catch or a Finally" });
}

TEST_CASE("a Task's type takes a Throws, unless it's a function's result", "[parser]") {
    CHECK(parse_text("Local t: Task<Int> Throws A, B").dump ==
          "(local t: (Task<Int> Throws A, B))");
    CHECK(parse_text("Local t: Task Throws A").dump == "(local t: (Task Throws A))");
    CHECK(parse_text("Local t: (Task<Int> Throws A)[]").dump ==
          "(local t: (Task<Int> Throws A)[])");
    CHECK(parse_text("Local m: Map<String, Task<Int> Throws A>").dump ==
          "(local m: Map<String, (Task<Int> Throws A)>)");

    // A ',' goes on to another Class unless a parameter follows it.
    CHECK(parse_text("Function F(t: Task<Int> Throws A, B, ByRef n: Int, m: Int)\nEnd Function")
              .dump == "(function F (t: (Task<Int> Throws A, B)) (ByRef n: Int) (m: Int) (block))");

    // After a function's result, Throws is the function's, unless the Task's in parentheses.
    CHECK(parse_text("Function F(): Task<Int> Throws A\nEnd Function").dump ==
          "(function F -> Task<Int> Throws A (block))");
    CHECK(parse_text("Function F(): (Task<Int> Throws A) Throws B\nEnd Function").dump ==
          "(function F -> (Task<Int> Throws A) Throws B (block))");
    // A Func type's result is the same: a Throws after it is the Func's, so a Task there that
    // throws goes in parentheses.
    CHECK(parse_text("Local f: Func(): Task<Int> Throws A").dump ==
          "(local f: (Func(): Task<Int> Throws A))");
    CHECK(parse_text("Local f: Func(): (Task<Int> Throws A)").dump ==
          "(local f: Func(): (Task<Int> Throws A))");

    // A Func that's a function's result leaves a Throws after its own result to the function.
    CHECK(parse_text("Function F(): Func(): Task<Int> Throws A\nEnd Function").dump ==
          "(function F -> Func(): Task<Int> Throws A (block))");
    CHECK(parse_text("Function F(): Sendable Func(): Task<Int> Throws A\nEnd Function").dump ==
          "(function F -> Sendable Func(): Task<Int> Throws A (block))");
    CHECK(parse_text("Function F(): Func(): (Task<Int> Throws A)\nEnd Function").dump ==
          "(function F -> Func(): (Task<Int> Throws A) (block))");

    // In a Func type's or a lambda's parameters, a ',' goes on to the next parameter.
    CHECK(parse_text("Local f: Func(Task<Int> Throws A, Int): Int").dump ==
          "(local f: Func((Task<Int> Throws A), Int): Int)");
    CHECK(expression("Function(t: Task<Int> Throws A, n) => n") ==
          "(lambda (t: (Task<Int> Throws A)) (n) => n)");

    // A call's type arguments can be one.
    CHECK(expression("Holder<Task<Int> Throws A>()") == "(call Holder<(Task<Int> Throws A)>)");
}

TEST_CASE("Try and Try? take what comes before the next binary operator", "[parser]") {
    CHECK(parse_text("Local n = Try? F() ?? 0").dump == "(local n = (?? (try? (call F)) (int 0)))");
    CHECK(parse_text("Try a.B().C()").dump == "(try (call (. (call (. a B)) C)))");
    CHECK(parse_text("If x Then Try F()").dump == "(if-then x (block (try (call F))))");

    // "Try ?" with a space is a Try before a ternary's condition, which isn't a call.
    CHECK(parse_text("Try ? F()").errors ==
          std::vector<std::string>{ "4-5: expected an expression, found '?'" });
}

TEST_CASE("tuples, their types, and their elements", "[parser]") {
    CHECK(expression("(1, x)") == "(tuple (int 1) x)");
    CHECK(expression("((1, 2), x,)") == "(tuple (tuple (int 1) (int 2)) x)");
    CHECK(expression("(x)") == "x");
    CHECK(expression("pair.0") == "(. pair 0)");
    CHECK(expression("t.0.1") == "(. (. t 0) 1)");
    CHECK(expression("t?.1") == "(?. t 1)");
    CHECK(parse_text("Local p: (Int, String)[]?").dump == "(local p: (Int, String)[]?)");
    CHECK(parse_text("Local p: ((Int, Int), Bool)").dump == "(local p: ((Int, Int), Bool))");
    CHECK(parse_text("Local p: (Int)").dump == "(local p: Int)");
}

TEST_CASE("a Local takes a tuple apart, and an assignment can set each element", "[parser]") {
    CHECK(parse_text("Local lo, hi = MinMax(xs)").dump == "(local lo hi = (call MinMax xs))");
    CHECK(parse_text("Local _, hi = pair").dump == "(local _ hi = pair)");
    CHECK(parse_text("a, b = (b, a)").dump == "(= (tuple a b) (tuple b a))");
    CHECK(parse_text("(a, b) = (b, a)").dump == "(= (tuple a b) (tuple b a))");
    CHECK(parse_text("p.0, x[1] = F()").dump == "(= (tuple (. p 0) (index x (int 1))) (call F))");
}

TEST_CASE("tuple mistakes are reported", "[parser]") {
    CHECK(parse_text("Local p = (1,)").errors ==
          std::vector<std::string>{ "10-14: a tuple has at least two values" });
    CHECK(parse_text("Local p: (Int,)").errors ==
          std::vector<std::string>{ "9-15: a tuple has at least two types" });
    CHECK(parse_text("Global a, b = F()").errors ==
          std::vector<std::string>{ "0-6: only a Local can take a tuple apart" });
    CHECK(parse_text("Local a: Int, b = F()").errors ==
          std::vector<std::string>{
              "9-12: the names that take a tuple apart get their types from its elements" });
    CHECK(parse_text("Local a, b").errors ==
          std::vector<std::string>{ "10-10: expected '=', found end of file" });
    CHECK(parse_text("x = t.1e5").errors ==
          std::vector<std::string>{ "6-9: a tuple's elements are numbered, like .0 and .1" });
}

TEST_CASE("lambdas, written with Function", "[parser]") {
    CHECK(expression("Function(x: Int) => x * 2") == "(lambda (x: Int) => (* x (int 2)))");
    CHECK(expression("Function(x, y) => x") == "(lambda (x) (y) => x)");
    CHECK(expression("Function[Weak Self, Weak n](): Int => 1") ==
          "(lambda [Weak Self, Weak n] -> Int => (int 1))");
    CHECK(parse_text("Local f = Function()\n    Print(x)\nEnd Function").dump ==
          "(local f = (lambda (block (call Print x))))");

    // A block lambda's body ends statements at line breaks, even inside a call's brackets.
    CHECK(parse_text("Each(xs, Function(s)\n    Print(s)\n    n++\nEnd Function)\nG()").dump ==
          "(call Each xs (lambda (s) (block (call Print s) (++ n))))\n(call G)");
    CHECK(parse_text("Return Function()\n    Return 1\nEnd Function").dump ==
          "(return (lambda (block (return (int 1)))))");
}

TEST_CASE("Func types", "[parser]") {
    CHECK(parse_text("Local f: Func(Int, String): Bool").dump ==
          "(local f: Func(Int, String): Bool)");
    CHECK(parse_text("Local f: Func()").dump == "(local f: Func())");
    CHECK(parse_text("Local f: (Func(): Int)?").dump == "(local f: (Func(): Int)?)");
    CHECK(parse_text("Local f: Func()[]").dump == "(local f: Func()[])");

    // The return type takes the suffix after it.
    CHECK(parse_text("Local f: Func(): Int[]").dump == "(local f: Func(): Int[])");

    // Before Func, Sendable says other threads can call it; elsewhere it's a name.
    CHECK(parse_text("Local f: Sendable Func(Int): Int").dump ==
          "(local f: Sendable Func(Int): Int)");
    CHECK(parse_text("Local f: (Sendable Func())?").dump == "(local f: Sendable Func()?)");
    CHECK(parse_text("Local f: Sendable").dump == "(local f: Sendable)");
}

TEST_CASE("lambda mistakes are reported", "[parser]") {
    CHECK(parse_text("Local f = Function[n]() => 1").errors ==
          std::vector<std::string>{
              "19-20: a capture list only has Weak captures, like [Weak Self]" });
    CHECK(parse_text("Local f = Function(ByRef x: Int) => x").errors ==
          std::vector<std::string>{ "19-24: a lambda's parameters can't be ByRef" });
    CHECK(parse_text("Local f = Function(x = 1) => x").errors ==
          std::vector<std::string>{ "21-22: a lambda's parameters can't have defaults" });
    CHECK(parse_text("Local f = Function() 1").errors ==
          std::vector<std::string>{
              "21-22: expected '=>' or the end of the line, found integer literal",
              "22-22: expected 'End Function', found end of file",
          });
}

TEST_CASE("generic functions and types, and their type arguments", "[parser]") {
    CHECK(parse_text("Function First<T>(xs: T[]): T => xs[0]").dump ==
          "(function First<T> (xs: T[]) -> T (block (return (index xs (int 0)))))");
    CHECK(parse_text("Struct Pair<A, B>\nEnd Struct").dump == "(struct Pair<A, B> (block))");
    CHECK(parse_text("Class Stack<T>\n    Method Map<U>()\n    End Method\nEnd Class").dump ==
          "(class Stack<T> (block (method Map<U> (block))))");
    CHECK(expression("Max<Float>(1, 2.5)") == "(call Max<Float> (int 1) (decimal 2.5))");
    CHECK(expression("Box<Box<Int>>(b)") == "(call Box<Box<Int>> b)");
    CHECK(expression("s.Map<String>(f)") == "(call (. s Map<String>) f)");

    // So does a function value's, at the end of a value, as the Func it gives.
    CHECK(expression("Less<Int>") == "Less<Int>");
    CHECK(expression("[Less<Int>]") == "(array Less<Int>)");

    // Without one of those after the '>', a '<' compares.
    CHECK(expression("a < b") == "(< a b)");
    CHECK(expression("F(a < b, c > d)") == "(call F (< a b) (> c d))");
}

TEST_CASE("a Class can extend an instance of a generic one", "[parser]") {
    CHECK(parse_text("Class B Extends A<Int>\nEnd Class").dump ==
          "(class B Extends A<Int> (block))");
    CHECK(parse_text("Class D<T> Extends Pair<String, T>\nEnd Class").dump ==
          "(class D<T> Extends Pair<String, T> (block))");
}

TEST_CASE("Operators are named by their operator", "[parser]") {
    CHECK(parse_text("Operator +(a: M, b: M): M => a").dump ==
          "(operator + (a: M) (b: M) -> M (block (return a)))");
    CHECK(parse_text("Operator -(a: M): M\n    Return a\nEnd Operator").dump ==
          "(operator - (a: M) -> M (block (return a)))");
    CHECK(parse_text("Operator >>(a: M, n: Int): M => a").dump ==
          "(operator >> (a: M) (n: Int) -> M (block (return a)))");
    CHECK(parse_text("Operator Add(a: M): M => a").errors ==
          std::vector<std::string>{ "9-12: expected an operator, like + or ==, found identifier" });
    // A "<<" before type parameters, not '(', is '<' and the '<' that opens them.
    const parsed_t less = parse_text("Operator <<Const B: Int>(x: M, y: Q<B>): Bool => x.V < y.V");
    CHECK(less.errors.empty());
    CHECK(less.dump ==
          "(operator <<Const B: Int> (x: M) (y: Q<B>) -> Bool (block (return (< (. x V) (. y "
          "V)))))");
    CHECK(parse_text("Operator <<(a: M, n: Int): M => a").dump ==
          "(operator << (a: M) (n: Int) -> M (block (return a)))");

    // Indexing is "[]" and "[]=", their tokens touching.
    CHECK(parse_text("Operator [](x: Int, y: Int): Int => x").dump ==
          "(operator [] (x: Int) (y: Int) -> Int (block (return x)))");
    CHECK(parse_text("Operator []=(x: Int, value: Int)\nEnd Operator").dump ==
          "(operator []= (x: Int) (value: Int) (block))");
    CHECK(parse_text("Operator [ ](x: Int): Int => x").errors ==
          std::vector<std::string>{ "11-12: expected ']' right after '[', as in Operator [] or "
                                    "Operator []=, found ']'" });
}

TEST_CASE("Interfaces", "[parser]") {
    // A Method's first line alone is required, and one with a body is a default, even an empty
    // body. Properties are only their first line.
    const parsed_t parsed = parse_text("Interface Damageable Extends Living, Named\n"
                                       "    Method Hurt(amount: Int)\n"
                                       "    Method Kill()\n"
                                       "        Hurt(100)\n"
                                       "    End Method\n"
                                       "    Method Rest()\n"
                                       "    End Method\n"
                                       "    ReadOnly Property Health: Int\n"
                                       "    Operator +(a: Self, b: Self): Self\n"
                                       "    Static Method Zero(): Self\n"
                                       "End Interface");
    CHECK(parsed.dump == "(interface Damageable Extends Living Named (block "
                         "(method Hurt (amount: Int)) "
                         "(method Kill (block (call Hurt (int 100)))) "
                         "(method Rest (block)) "
                         "(property ReadOnly Health: Int) "
                         "(operator + (a: Self) (b: Self) -> Self) "
                         "(method Static Zero -> Self)))");
    CHECK(parsed.errors.empty());

    // A member after attributes is the next member, not a body.
    CHECK(parse_text("Interface Old\n"
                     "    Method Kept()\n"
                     "    @Deprecated Method Gone()\n"
                     "    @Deprecated(\"use Kept\") ReadOnly Property Count: Int\n"
                     "End Interface")
              .dump == "(interface Old (block (method Kept) (method @Deprecated Gone) "
                       "(property ReadOnly @Deprecated(\"use Kept\") Count: Int)))");
    // Its message is written as source writes it, so no control character reaches the terminal.
    CHECK(parse_text(R"(@Deprecated("use \x1B[2JG, {{soon}} \"now\"\n") Function F())"
                     "\n"
                     "End Function")
              .dump == R"((function @Deprecated("use \x1B[2JG, {{soon}} \"now\"\n") F (block)))");

    // So is an Async Method, and an Async Function starts a body.
    CHECK(parse_text("Interface Loader\n"
                     "    Method Size(): Int\n"
                     "    Async Method Load(): Int\n"
                     "    Method Later()\n"
                     "        Async Function Inner() => 1\n"
                     "    End Method\n"
                     "End Interface")
              .dump == "(interface Loader (block (method Size -> Int) (method Async Load -> Int) "
                       "(method Later (block (function Async Inner (block (return (int 1))))))))");

    // Modifiers go in any order, after Async too, and "Async ByRef Method" is a member for
    // analysis to refuse.
    CHECK(parse_text("Interface Maker\n"
                     "    Method Size(): Int\n"
                     "    Async Static Method MakeAsync(): Int\n"
                     "    Method Twice(): Int\n"
                     "    Async Final Method TwiceAsync(): Int => 2\n"
                     "    Method Last()\n"
                     "    Async ByRef Method Grow()\n"
                     "End Interface")
              .dump == "(interface Maker (block (method Size -> Int) "
                       "(method Static Async MakeAsync -> Int) (method Twice -> Int) "
                       "(method Final Async TwiceAsync -> Int (block (return (int 2)))) "
                       "(method Last) (byref-method Async Grow)))");

    // An Abstract Property is its first line too.
    CHECK(parse_text("Abstract Property Area: Float").dump == "(property Abstract Area: Float)");
    CHECK(parse_text("Interface I Implements J\nEnd Interface").errors ==
          std::vector<std::string>{
              "12-22: an Interface extends other Interfaces; write 'Extends'" });
}

TEST_CASE("constraints go in the type parameters, or on Where lines", "[parser]") {
    CHECK(parse_text("Function Max<T: Comparable>(a: T, b: T): T => a").dump ==
          "(function Max<T: Comparable> (a: T) (b: T) -> T (block (return a)))");
    CHECK(parse_text("Function F<T: A & B, U>()\nEnd Function").dump ==
          "(function F<T: A & B, U> (block))");
    CHECK(parse_text("Class Cache<K, V>\n"
                     "    Where K: Hashable & Equatable\n"
                     "    Where V: Sendable\n"
                     "End Class")
              .dump == "(class Cache<K, V> (where K: Hashable & Equatable) (where V: Sendable) "
                       "(block))");
    CHECK(parse_text("Method Sum(): T\n    Where T: Numeric\n    Return x\nEnd Method").dump ==
          "(method Sum -> T (where T: Numeric) (block (return x)))");

    // "Class" says a type parameter is a Class, inline or on a Where line.
    CHECK(parse_text("Function Watch<O: Class>(owner: O)\nEnd Function").dump ==
          "(function Watch<O: Class> (owner: O) (block))");
    CHECK(parse_text("Class Registry<T>\n    Where T: Class & Sendable\nEnd Class").dump ==
          "(class Registry<T> (where T: Class & Sendable) (block))");
    CHECK(parse_text("Local x: Class").errors ==
          std::vector<std::string>{ "9-14: expected a type, found 'Class'" });

    // Where lines come first. This one is at 35-40.
    CHECK(parse_text("Function F<T>()\n    Print(\"a\")\n    Where T: I\nEnd Function").errors ==
          std::vector<std::string>{ "35-40: a Where line goes at the top of a generic Function, "
                                    "Method, Struct, or Class, before the rest of its body" });
}

TEST_CASE("Extend adds to a type that's declared already", "[parser]") {
    CHECK(parse_text("Extend Box<T> Implements Printable, Shown\n"
                     "    Where T: Printable\n"
                     "    Method ToString(): String => Item\n"
                     "End Extend")
              .dump == "(extend Box<T> Implements Printable Shown (where T: Printable) "
                       "(block (method ToString -> String (block (return Item)))))");
    CHECK(parse_text("Extend T[]\nEnd Extend").dump == "(extend T[] (block))");
    CHECK(parse_text("Extend Int\n    Method M()\n    End Method\nEnd Extend").dump ==
          "(extend Int (block (method M (block))))");
}

TEST_CASE("attributes go before declarations, on their line or the one above", "[parser]") {
    CHECK(parse_text("@Inline Function Twice(x: Int): Int => x * 2").dump ==
          "(function @Inline Twice (x: Int) -> Int (block (return (* x (int 2)))))");
    CHECK(parse_text("@Deprecated(\"use G\")\nFunction F()\nEnd Function").dump ==
          "(function @Deprecated(\"use G\") F (block))");
    CHECK(parse_text("@Deprecated Struct S\nEnd Struct").dump == "(struct @Deprecated S (block))");
    CHECK(parse_text("Const Function One(): Int => 1").dump ==
          "(function Const One -> Int (block (return (int 1))))");
    // One from a module imported As comes after its qualifier, even with a built-in's name.
    CHECK(parse_text("@C.Rename(\"hp\") @C.Inline Function F()\nEnd Function").dump ==
          "(function @C.Rename(\"hp\") @C.Inline F (block))");
}

TEST_CASE("attribute mistakes are reported", "[parser]") {
    // Analysis finds the Attribute an unknown name declares, if there is one.
    CHECK(parse_text("@Foo Function F()\nEnd Function").errors.empty());
    CHECK(parse_text("@CLayout Public Struct S\nEnd Struct").dump ==
          "(struct Public @CLayout S (block))");
    CHECK(parse_text("Local c: Callback(Int, Ptr<Void>): Bool").dump ==
          "(local c: Callback(Int, Ptr<Void>): Bool)");
    CHECK(parse_text("Local c: Callback(ByRef Int)").errors ==
          std::vector<std::string>{ "18-23: a Callback's parameters can't be ByRef" });
    CHECK(parse_text("@CLayout Class C\nEnd Class").errors ==
          std::vector<std::string>{ "1-8: @CLayout only goes before a Struct, which it lays out as "
                                    "C does" });
    CHECK(parse_text("@CLayout Shared Struct S\nEnd Struct").errors.empty());
    CHECK(parse_text("@CLayout Enum E\n    A\nEnd Enum").errors ==
          std::vector<std::string>{ "1-8: @CLayout only goes before a Struct, which it lays out as "
                                    "C does" });
    CHECK(parse_text("@CLayout @CLayout Struct S\nEnd Struct").errors ==
          std::vector<std::string>{ "10-17: @CLayout is written twice" });
    CHECK(parse_text("@Inline(1) Function F()\nEnd Function").errors ==
          std::vector<std::string>{ "7-10: @Inline takes no arguments" });
    CHECK(parse_text("@Deprecated(1) Function F()\nEnd Function").errors ==
          std::vector<std::string>{
              "12-13: @Deprecated takes a message, a string without interpolation" });
    CHECK(parse_text("@Inline @Inline Function F()\nEnd Function").errors ==
          std::vector<std::string>{ "9-15: @Inline is written twice" });
    // Load and Save convert every Struct and Class, so spec 13's sketch of @Serialize is gone.
    CHECK(parse_text("@Serialize Class C\nEnd Class").errors ==
          std::vector<std::string>{ "1-10: @Serialize isn't needed: Load<T>(value) and Save(x) "
                                    "convert every Struct and Class" });
    CHECK(parse_text("@PanicsAtCaller Function F()\nEnd Function").dump ==
          "(function @PanicsAtCaller F (block))");
    CHECK(parse_text("@PanicsAtCaller(1) Function F()\nEnd Function").errors ==
          std::vector<std::string>{ "15-18: @PanicsAtCaller takes no arguments" });
    CHECK(parse_text("@MainThread(5, \"x\") Function F()\nEnd Function").errors ==
          std::vector<std::string>{ "11-19: @MainThread takes no arguments" });
    CHECK(parse_text("@PanicsAtCaller @PanicsAtCaller Function F()\nEnd Function").errors ==
          std::vector<std::string>{ "17-31: @PanicsAtCaller is written twice" });
    CHECK(parse_text("@Inline x = 1").errors ==
          std::vector<std::string>{
              "8-9: attributes go before a declaration, like a Function or a Struct" });
}

TEST_CASE("an Attribute declares an attribute, whose arguments can be named", "[parser]") {
    CHECK(parse_text("Attribute Rename(Name: String, Level: Int = 1)").dump ==
          "(attribute Rename (block (field Name: String) (field Level: Int = (int 1))))");
    CHECK(parse_text("Attribute Skip").dump == "(attribute Skip (block))");
    CHECK(parse_text("Struct S\n"
                     "    @Rename(\"hp\", Level: 2) @Skip\n"
                     "    Field Health: Int\n"
                     "End Struct")
              .dump == "(struct S (block (field @Rename(\"hp\", Level: 2) @Skip Health: Int)))");

    // "Attribute" is only a keyword before a name.
    CHECK(parse_text("Local Attribute = 1").dump == "(local Attribute = (int 1))");
}

TEST_CASE("For Field and For Method go over a type's members", "[parser]") {
    CHECK(parse_text("For Field f: F In T\n"
                     "    Where F: Printable\n"
                     "    Print(f.Name)\n"
                     "End For")
              .dump == "(for-field f: F T (where F: Printable) (block (call Print (. f Name))))");
    CHECK(parse_text("For Field f In Point\nEnd For").dump == "(for-field f Point (block))");
    CHECK(parse_text("Outer: For Method m In T\n    Continue Outer\nEnd For").dump ==
          "(for-method Outer: m T (block (continue Outer)))");
    CHECK(parse_text("For Method m: M In T\n    Where M: Func(Int): String\nEnd For").dump ==
          "(for-method m: M T (where M: Func(Int): String) (block))");
    // Its Func type can throw, a Where line on its variable picks by attributes, with "Has" and
    // "Or" words only there, and a function's result keeps its Throws.
    CHECK(parse_text("For Method m: M In T\n"
                     "    Where M: Func(Int): String Throws E, F\n"
                     "    Where m Has Get Or Web.Post\n"
                     "    Where m Has Logged\n"
                     "End For")
              .dump ==
          "(for-method m: M T (where M: (Func(Int): String Throws E, F)) (where-has Get "
          "(. Web Post)) (where-has Logged) (block))");
    CHECK(parse_text("Function Make(): Func(): Int Throws A, B\nEnd Function").dump ==
          "(function Make -> Func(): Int Throws A B (block))");
    // Without a result too; a result Func's Throws is the outer Func's, as a header's is; in a
    // Func type's parameters, a ',' goes on to the next one; and a Callback can't throw.
    CHECK(parse_text("For Method m: M In T\n    Where M: Func(Int) Throws E\nEnd For").dump ==
          "(for-method m: M T (where M: (Func(Int) Throws E)) (block))");
    CHECK(parse_text("Local f: Func(Int): Func(): Int Throws E").dump ==
          "(local f: (Func(Int): Func(): Int Throws E))");
    CHECK(parse_text("Local f: Func(Int): (Func(): Int Throws E)").dump ==
          "(local f: Func(Int): (Func(): Int Throws E))");
    CHECK(parse_text("Local f: Func(Func(): Int Throws E, Int): String").dump ==
          "(local f: Func((Func(): Int Throws E), Int): String)");
    CHECK(
        parse_text("Local f: Callback(Int): Int Throws E").errors ==
        std::vector<std::string>{ "28-34: a Callback can't throw, since C code takes no errors" });
    CHECK(parse_text("For Method m: M In T\n    Where k Has Get\nEnd For").errors ==
          std::vector<std::string>{
              "31-32: a Where line picks by attributes on the loop's variable, 'm'" });
    CHECK(parse_text("For Property p: P In T\nEnd For").dump == "(for-property p: P T (block))");
    CHECK(parse_text("Async Function F(): Int\n    Return Await G()\nEnd Function").dump ==
          "(function Async F -> Int (block (return (await (call G)))))");
    CHECK(parse_text("Async Local x = 1").errors ==
          std::vector<std::string>{
              "0-5: Async only goes before a Function or Method, whose code can Await" });
    CHECK(parse_text("Local f = Async Function(n: Int) => n").dump ==
          "(local f = (lambda Async (n: Int) => n))");
    CHECK(parse_text("Local f = Async 5").errors ==
          std::vector<std::string>{ "10-15: expected an expression, found 'Async'" });
    CHECK(parse_text("Parallel For i = 0 Until n\nEnd For").dump ==
          "(parallel-for i (int 0) until n (block))");
    CHECK(parse_text("Parallel For x In items\nEnd For").errors ==
          std::vector<std::string>{
              "0-8: a Parallel For counts, like 'Parallel For i = 0 Until n'" });
    CHECK(parse_text("Lock game.score As s\n    s += 1\nEnd Lock").dump ==
          "(lock (. game score) s (block (+= s (int 1))))");
    CHECK(parse_text("Lock score\nEnd Lock").errors ==
          std::vector<std::string>{
              "10-11: expected 'As' and a name for the Mutex's value, found newline" });
    CHECK(parse_text("For Static Field s In T\nEnd For").dump == "(for-static-field s T (block))");
}

TEST_CASE("Const type parameters take values as their type arguments", "[parser]") {
    CHECK(parse_text("Struct Grid<T, Const W: Int, Const H: Int>\nEnd Struct").dump ==
          "(struct Grid<T, Const W: Int, Const H: Int> (block))");
    CHECK(parse_text("Function Sum<Const N: Int>(v: Int[N]): Int => N").dump ==
          "(function Sum<Const N: Int> (v: Int[N]) -> Int (block (return N)))");
    CHECK(parse_text("Local g: Grid<Float, 64, -1>").dump == "(local g: Grid<Float, 64, (- 1)>)");
    CHECK(expression("Make<3>()") == "(call Make<3>)");

    // One computed with +, -, *, /, and %, and -N, which a '(' can start, unlike a tuple type.
    CHECK(parse_text("Local q: Q<A - B, -N, M / 2 % 3, (A - 1) * 2>").dump ==
          "(local q: Q<(- A B), (- N), (% (/ M 2) 3), (* (- A 1) 2)>)");
    CHECK(expression("Show<10 / N>()") == "(call Show<(/ 10 N)>)");
    CHECK(parse_text("Local t: (Int[4], Q<-1>)").dump == "(local t: (Int[4], Q<(- 1)>))");
    // That '(' can start a line the arguments go on to, and the value can break lines too.
    CHECK(parse_text("Local q: Q2<A,\n    (A - 1) * 2>").dump == "(local q: Q2<A, (* (- A 1) 2)>)");
    CHECK(parse_text("Local q: Q2<A, (A\n    - 1) * 2>").dump == "(local q: Q2<A, (* (- A 1) 2)>)");
    CHECK(parse_text("Local t: Q2<A,\n    (Int, String)>").dump ==
          "(local t: Q2<A, (Int, String)>)");
}

TEST_CASE("an Alias names a type, after its visibility", "[parser]") {
    CHECK(parse_text("Alias MetersPerSecond = Quantity<1, 0, -1>").dump ==
          "(alias MetersPerSecond Quantity<1, 0, (- 1)>)");
    CHECK(parse_text("Public Alias Row = Int[]?\nAlias Pair = (Int, String)").dump ==
          "(alias Public Row Int[]?)\n(alias Pair (Int, String))");
    CHECK(parse_text("Alias X Int").errors ==
          std::vector<std::string>{ "8-11: expected '=', found identifier" });
    CHECK(parse_text("Static Alias X = Int").errors ==
          std::vector<std::string>{ "7-12: 'Alias' doesn't take modifiers like Static or Final" });
}

TEST_CASE("Import and From name another module", "[parser]") {
    CHECK(parse_text("Import std.collections").dump == "(import std.collections)");
    CHECK(parse_text("Import \"./ui/hud\"").dump == "(import \"./ui/hud\")");
    CHECK(parse_text("Import std.collections As C").dump == "(import std.collections As C)");
    CHECK(parse_text("From std.math Import Vec3, Lerp").dump == "(from std.math Import Vec3 Lerp)");
    CHECK(parse_text("From \"./hud\" Import Draw,\n    Clear").dump ==
          "(from \"./hud\" Import Draw Clear)");

    // "From" is a keyword only before a module at the start of a line.
    CHECK(parse_text("From = 1\nFrom(2)").dump == "(= From (int 1))\n(call From (int 2))");
}

TEST_CASE("an Extern block declares Functions that C code defines", "[parser]") {
    CHECK(parse_text("Extern \"C\"\n"
                     "    Function abs(n: Int32): Int32\n"
                     "    Function Root(x: Float64): Float64 = \"sqrt\"\n"
                     "End Extern")
              .dump == "(extern \"C\" (function abs (n: Int32) -> Int32) (function Root (x: "
                       "Float64) -> Float64 = \"sqrt\"))");

    // The convention is "C" without one, and a visibility goes before the block or a Function.
    CHECK(parse_text("Public Extern Lib \"vecmath\"\n"
                     "    Private Function Add(ByRef a: Int32, b: Int32)\n"
                     "End Extern")
              .dump ==
          "(extern Public \"C\" Lib \"vecmath\" (function Private Add (ByRef a: Int32) "
          "(b: Int32)))");
    CHECK(parse_text("Extern \"runtime\"\n    Function fb_print(text: String)\nEnd Extern").dump ==
          "(extern \"runtime\" (function fb_print (text: String)))");
}

TEST_CASE("Extern block mistakes are reported", "[parser]") {
    CHECK(parse_text("Extern \"pascal\"\nEnd Extern").errors ==
          std::vector<std::string>{ "7-15: an Extern block's calling convention is \"C\" or "
                                    "\"runtime\", not \"pascal\"" });
    // 64-bit targets have one C convention, which stdcall is there.
    CHECK(parse_text("Extern \"stdcall\"\nEnd Extern").errors ==
          std::vector<std::string>{ "7-16: FutureBlitz's targets are 64-bit, where stdcall is the "
                                    "C convention, so the block is \"C\"" });
    CHECK(parse_text("Extern\n    Method M()\n    End Method\nEnd Extern").errors ==
          std::vector<std::string>{ "11-36: an Extern block holds only Functions" });
    CHECK(parse_text("Extern\n"
                     "    Function F(): Int\n"
                     "        Return 1\n"
                     "    End Function\n"
                     "    Async Function G()\n"
                     "End Extern")
              .errors == std::vector<std::string>{
                             "20-21: an Extern block's Function has no body; C code defines it" });
    CHECK(parse_text("Extern\n"
                     "    Function F(n: Int): Int\n"
                     "        If n > 0\n"
                     "            Return n\n"
                     "        End If\n"
                     "        Return 0\n"
                     "    End Function\n"
                     "End Extern")
              .errors == std::vector<std::string>{
                             "20-21: an Extern block's Function has no body; C code defines it" });
    CHECK(parse_text("Extern\n    Function F() => 1\nEnd Extern").errors ==
          std::vector<std::string>{
              "24-26: an Extern block's Function has no body; C code defines it" });
    CHECK(parse_text("Extern\n    Function F() = 1\nEnd Extern").errors ==
          std::vector<std::string>{ "26-27: expected the C symbol's name, like "
                                    "\"JPH_CreateBody\", found integer literal" });
    CHECK(parse_text("Static Extern\nEnd Extern").errors ==
          std::vector<std::string>{
              "7-13: an Extern block takes only a visibility, like 'Public Extern'" });
    CHECK(parse_text("If True\n    Extern\n    End Extern\nEnd If").errors ==
          std::vector<std::string>{ "12-18: an Extern block goes at the top level of a file" });
    CHECK(parse_text("Extern Lib\nEnd Extern").errors ==
          std::vector<std::string>{
              "10-11: expected a library's name, like \"joltc\", found newline" });
}

TEST_CASE("names after a module's qualifier, in values and types", "[parser]") {
    CHECK(parse_text("Local m: C.Map<String, C.Set<Int>>[]").dump ==
          "(local m: C.Map<String, C.Set<Int>>[])");
    CHECK(expression("C.Stack<C.Map<K, V>>()") == "(call (. C Stack<C.Map<K, V>>))");
    CHECK(parse_text("Class Circle Extends shapes.Shape<Int>\nEnd Class").dump ==
          "(class Circle Extends shapes.Shape<Int> (block))");
}

TEST_CASE("import mistakes are reported", "[parser]") {
    CHECK(parse_text("Import").errors ==
          std::vector<std::string>{ "6-6: expected a module, like std.collections, or a quoted "
                                    "path, like \"./hud\", found end of file" });
    CHECK(parse_text("Import std.").errors ==
          std::vector<std::string>{ "11-11: expected identifier, found end of file" });
    CHECK(parse_text("Import \"./{x}\"").errors ==
          std::vector<std::string>{ "7-11: an Import's path is plain text, without {...}" });
    CHECK(parse_text("From std.math Vec3").errors ==
          std::vector<std::string>{ "14-18: expected 'Import', found identifier" });
    CHECK(parse_text("Import std.math As").errors ==
          std::vector<std::string>{ "18-18: expected identifier, found end of file" });
    CHECK(parse_text("Import std.math, std.io").errors ==
          std::vector<std::string>{ "15-16: expected end of line, found ','" });

    // The next line still parses.
    CHECK(parse_text("Import\nLocal x = 1").dump == "(error)\n(local x = (int 1))");
}

TEST_CASE("code nested deeper than the compiler goes is one error, not a crash", "[parser]") {
    const auto repeated = [](const std::string_view part, const std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += part;
        }

        return text;
    };
    // The message after its span, when it's the only error.
    const auto only_error = [](const std::string& text) {
        const parsed_t parsed = parse_text(text);
        REQUIRE(parsed.errors.size() == 1);
        return parsed.errors.front().substr(parsed.errors.front().find(": ") + 2);
    };
    const std::string message =
        "this is nested more than 1000 deep, deeper than the compiler goes; "
        "split it up with Locals or Functions";

    // Parentheses, a chain of operators, calls, prefix operators, and blocks.
    CHECK(only_error("_ = " + repeated("(", 5000) + "1" + repeated(")", 5000)) == message);
    CHECK(only_error("_ = 1" + repeated(" + 1", 3000)) == message);
    CHECK(only_error("_ = f" + repeated("()", 3000)) == message);
    CHECK(only_error("_ = " + repeated("- ", 3000) + "1") == message);
    CHECK(only_error(repeated("If x\n", 3000) + repeated("End If\n", 3000)) == message);

    // Up to the limit is fine: 1000 levels, and not one more.
    CHECK(parse_text("_ = " + repeated("(", 1000) + "1" + repeated(")", 1000)).errors.empty());
    CHECK(only_error("_ = " + repeated("(", 1001) + "1" + repeated(")", 1001)) == message);
    CHECK(parse_text("_ = 1" + repeated(" + 1", 1000)).errors.empty());
    CHECK(only_error("_ = 1" + repeated(" + 1", 1001)) == message);
    CHECK(parse_text("_ = f" + repeated("()", 1000)).errors.empty());
    CHECK(only_error("_ = f" + repeated("()", 1001)) == message);
    CHECK(parse_text("_ = " + repeated("- ", 1000) + "1").errors.empty());
    CHECK(only_error("_ = " + repeated("- ", 1001) + "1") == message);
    CHECK(parse_text(repeated("If x\n", 1000) + repeated("End If\n", 1000)).errors.empty());
    CHECK(only_error(repeated("If x\n", 1001) + repeated("End If\n", 1001)) == message);

    // Types nest like expressions: each pair of parentheses, type argument list, and suffix.
    CHECK(
        parse_text("Local x: " + repeated("(", 1000) + "Int" + repeated(")", 1000)).errors.empty());
    CHECK(only_error("Local x: " + repeated("(", 1001) + "Int" + repeated(")", 1001)) == message);
    CHECK(parse_text("Local x: Int" + repeated("[]", 1000)).errors.empty());
    CHECK(only_error("Local x: Int" + repeated("[]", 1001)) == message);
    CHECK(only_error("Local x: " + repeated("(", 50000) + "Int" + repeated(")", 50000)) == message);
    CHECK(only_error("Local x: Stack" + repeated("<Stack", 50000) + repeated(">", 50000)) ==
          message);
    CHECK(only_error("Local x: " + repeated("Weak<", 50000) + "Int" + repeated(">", 50000)) ==
          message);
    CHECK(only_error("Local x: " + repeated("Func(", 50000) + repeated(")", 50000)) == message);
    CHECK(only_error("Local x: Int" + repeated("[]", 50000)) == message);
    CHECK(only_error("Local x: Int" + repeated("[1]", 50000)) == message);
    CHECK(only_error("Local x: Int" + repeated("?", 50000)) == message);
    CHECK(only_error("Local x: Stack<" + repeated("N + ", 50000) + "1>") == message);

    // So do statements after Defer, a single-line If's, and an Extern block's: the Function's
    // block, 998 Defers, and the call make 1000.
    CHECK(parse_text("Function G()\n" + repeated("Defer ", 998) + "F()\nEnd Function")
              .errors.empty());
    CHECK(only_error("Function G()\n" + repeated("Defer ", 999) + "F()\nEnd Function") == message);
    CHECK(only_error("Function G()\n" + repeated("Defer ", 50000) + "F()\nEnd Function") ==
          message);
    CHECK(only_error("If x Then " + repeated("Defer ", 50000) + "F()") == message);
    // Each Extern inside another is an error of its own, until the one 1001 deep.
    const parsed_t externs = parse_text(repeated("Extern \"C\"\n", 50000));
    CHECK(externs.errors.size() == 1001);
    CHECK(externs.errors.back().ends_with(message));

    // A mistake that leads to another statement on its line is reported, and the statement
    // 1000 deep is cut off, so later passes don't go deeper.
    for (const std::string& chain : {
             repeated("a: ", 50000) + "F()",
             repeated("Parallel ", 50000) + "F()",
             repeated("Static @A ", 50000) + "Function F()\nEnd Function",
             repeated("@A Public ", 50000) + "Function F()\nEnd Function",
         }) {
        const parsed_t parsed = parse_text(chain);
        INFO(chain.substr(0, 20));
        CHECK(parsed.errors.size() == 1);
        CHECK(parsed.dump.starts_with("(error)"));
    }
}

TEST_CASE("an interface file's declarations can leave their code in the library's binary",
          "[parser]") {
    const std::string text = "@Inline Public Function Area(r: Float): Float => ...\n"
                             "Public Global Count: Int = ...\n"
                             "Public Class Body Extends Shape\n"
                             "    Public Property Mass: Float\n"
                             "        Get => ...\n"
                             "        Set(value) => ...\n"
                             "    End Property\n"
                             "End Class\n";
    const fb::source_file_t file(fb::file_id_t{}, "physics.fbi", text);
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    fb::ast_t ast;
    fb::parse_into(file, tokens, bag, ast, true);
    CHECK(bag.diagnostics().empty());
    REQUIRE(ast.items().size() == 3);

    const auto& area = std::get<fb::function_stmt_t>(ast.get(ast.items()[0]).node);
    CHECK(area.prebuilt);
    CHECK(area.body.empty());
    CHECK(area.modifiers.start == 0);
    CHECK(file.slice({ .begin = 0, .end = area.header_end }) ==
          "@Inline Public Function Area(r: Float): Float");

    const auto& count = std::get<fb::variable_stmt_t>(ast.get(ast.items()[1]).node);
    CHECK(count.prebuilt);
    CHECK_FALSE(count.value);

    const auto& body = std::get<fb::type_stmt_t>(ast.get(ast.items()[2]).node);
    const std::uint32_t start = body.modifiers.start.value_or(1);
    CHECK(file.slice({ .begin = start, .end = body.header_end }) ==
          "Public Class Body Extends Shape");
    const auto& mass = std::get<fb::property_stmt_t>(ast.get(body.members.at(0)).node);
    REQUIRE(mass.getter);
    REQUIRE(mass.setter);
    CHECK(std::get<fb::function_stmt_t>(ast.get(mass.getter.value_or(fb::stmt_id_t{})).node)
              .prebuilt);
    CHECK(std::get<fb::function_stmt_t>(ast.get(mass.setter.value_or(fb::stmt_id_t{})).node)
              .prebuilt);

    // Elsewhere, "=> ..." is a mistake.
    CHECK_FALSE(parse_text("Function Area(r: Float): Float => ...\n").errors.empty());
    CHECK_FALSE(parse_text("Global Count: Int = ...\n").errors.empty());
}

namespace {

/// Parses `text` as an interface file, giving its AST, its file, and its errors' messages.
struct interface_parse_t {
    fb::source_file_t file;
    fb::ast_t ast;
    std::vector<std::string> errors;
};

interface_parse_t parse_interface(const std::string_view text) {
    interface_parse_t parsed{
        .file = fb::source_file_t(fb::file_id_t{}, "x.fbi", std::string(text)),
    };
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(parsed.file, bag);
    fb::parse_into(parsed.file, tokens, bag, parsed.ast, true);
    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        parsed.errors.push_back(diagnostic.message);
    }

    return parsed;
}

} // namespace

TEST_CASE("an interface file says what of its prebuilt code only the main thread can run",
          "[parser]") {
    const interface_parse_t parsed = parse_interface(
        "@MainThreadOnly(\"uses the Global 'score'\") Public Function Bump() => ...\n"
        "Public Struct Stats\n"
        "    @MainThreadOnly(Get: \"reads\", Set: \"writes\") Public Property S: Int\n"
        "        Get => ...\n"
        "        Set(value) => ...\n"
        "    End Property\n"
        "End Struct\n");
    CHECK(parsed.errors.empty());
    REQUIRE(parsed.ast.items().size() == 2);
    const auto& bump = std::get<fb::function_stmt_t>(parsed.ast.get(parsed.ast.items()[0]).node);
    CHECK(bump.modifiers.main_thread_only == "uses the Global 'score'");
    const auto& stats = std::get<fb::type_stmt_t>(parsed.ast.get(parsed.ast.items()[1]).node);
    const auto& score = std::get<fb::property_stmt_t>(parsed.ast.get(stats.members.at(0)).node);
    const auto part = [&](const std::optional<fb::stmt_id_t> id) {
        return std::get<fb::function_stmt_t>(parsed.ast.get(id.value_or(fb::stmt_id_t{})).node)
            .modifiers.main_thread_only.value_or("");
    };
    CHECK(part(score.getter) == "reads");
    CHECK(part(score.setter) == "writes");

    // It takes plain text.
    CHECK(parse_interface("@MainThreadOnly(1) Function F() => ...\n").errors ==
          std::vector<std::string>{ "@MainThreadOnly takes why, like \"uses the Global 'score'\", "
                                    "or a Property's Get: and Set:" });
}

TEST_CASE("an interface file's '= ...' and '=> ...' go only where code is in the binary",
          "[parser]") {
    const std::string misplaced =
        "\"= ...\" only follows the type of a Global or a Static Field, whose value is in the "
        "library's binary";
    CHECK(parse_interface("Public Global G = ...\n").errors ==
          std::vector<std::string>{ misplaced });
    CHECK(parse_interface("Public Const C: Int = ...\n").errors ==
          std::vector<std::string>{ misplaced });
    CHECK(parse_interface("Function F<T>()\n    Local y: Int = ...\nEnd Function\n").errors ==
          std::vector<std::string>{ misplaced });
    CHECK(parse_interface("Public Const Function K(): Int => ...\n").errors ==
          std::vector<std::string>{
              "a Const Function's code is in an interface file, since programs run it" });
    CHECK(parse_interface("Public Global G: Int = ...\n"
                          "Public Class C\n"
                          "    Public Static Field S: Int = ...\n"
                          "End Class\n")
              .errors.empty());
    // A Field's default is its value, which the code that makes its Class sets.
    CHECK(parse_interface("Public Class C\n    Field f: Int = ...\nEnd Class\n").errors ==
          std::vector<std::string>{ misplaced });

    // Each first line ends where its type, Implements, or Lib does.
    const interface_parse_t parsed = parse_interface("Extend Int32 Implements Shown\n"
                                                     "    Method Show(): String => ...\n"
                                                     "End Extend\n"
                                                     "Public Extern \"C\" Lib \"m\"\n"
                                                     "    Function sqrt(x: Float64): Float64\n"
                                                     "End Extern\n"
                                                     "Struct P\n"
                                                     "    Property X: Int\n"
                                                     "        Get => ...\n"
                                                     "    End Property\n"
                                                     "End Struct\n");
    CHECK(parsed.errors.empty());
    REQUIRE(parsed.ast.items().size() == 3);
    const auto first_line = [&](const std::uint32_t begin, const std::uint32_t end) {
        return std::string(parsed.file.slice({ .begin = begin, .end = end }));
    };
    const fb::stmt_t& extend = parsed.ast.get(parsed.ast.items()[0]);
    CHECK(first_line(extend.span.begin, std::get<fb::extend_stmt_t>(extend.node).header_end) ==
          "Extend Int32 Implements Shown");
    const auto& block = std::get<fb::extern_stmt_t>(parsed.ast.get(parsed.ast.items()[1]).node);
    CHECK(first_line(block.modifiers.start.value_or(1), block.header_end) ==
          "Public Extern \"C\" Lib \"m\"");
    const auto& point = std::get<fb::type_stmt_t>(parsed.ast.get(parsed.ast.items()[2]).node);
    const fb::stmt_t& property = parsed.ast.get(point.members.at(0));
    CHECK(first_line(property.span.begin,
                     std::get<fb::property_stmt_t>(property.node).header_end) == "Property X: Int");
}

TEST_CASE("a syntax error after a miscased keyword names the keyword, with a fix-it", "[parser]") {
    // The fix-it is the one change: the name's span, spelled as the keyword.
    const auto fixed = [](const std::string_view text) {
        const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
        fb::diagnostic_bag_t bag;
        (void)fb::parse(file, fb::lex(file, bag), bag);
        REQUIRE_FALSE(bag.diagnostics().empty());
        const fb::diagnostic_t& error = bag.diagnostics().front();
        if (error.fixes.empty()) {
            return std::string("no fix");
        }

        const fb::fix_t& fix = error.fixes.front();
        std::string out(text);
        out.replace(fix.span.begin, fix.span.end - fix.span.begin, fix.replacement);
        return std::format("{} | {}", out, error.notes.empty() ? "" : error.notes.front());
    };

    CHECK(fixed("if x Then Print(1)\n") ==
          "If x Then Print(1)\n | 'if' is a name here; keywords are written as they are, as 'If'");
    CHECK(fixed("Function F()\n    Return 1\nend Function\n") ==
          "Function F()\n    Return 1\nEnd Function\n | 'end' is a name here; keywords are written "
          "as they are, as 'End'");
    CHECK(fixed("For i = 1 TO 3\nNext\n").starts_with("For i = 1 To 3\n"));
    // A miscased word on another line, or none, isn't suggested.
    CHECK(fixed("Local then = 1\nLocal x = = 2\n") == "no fix");
    CHECK(fixed("Local x = = 2\n") == "no fix");
}
