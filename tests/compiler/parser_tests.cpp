#include "fb/dump.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
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
    CHECK(parse_text("Extern \"stdcall\"\nEnd Extern").dump == "(extern \"stdcall\")");
}

TEST_CASE("Extern block mistakes are reported", "[parser]") {
    CHECK(parse_text("Extern \"pascal\"\nEnd Extern").errors ==
          std::vector<std::string>{ "7-15: an Extern block's calling convention is \"C\", "
                                    "\"stdcall\", or \"runtime\", not \"pascal\"" });
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
