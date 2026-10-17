#include "fb/dump.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>
#include <string_view>
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
    CHECK(expression(R"("a\tb {{x}}")") == R"((string "a\tb {x}"))");
    CHECK(expression(R"("")") == "(string)");

    // The dump escapes the backslashes it prints.
    CHECK(expression(R"(r"C:\raw\{x}")") == R"((string "C:\\raw\\{x}"))");
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
    CHECK(expression("\"\"\"\n\"\"\"") == "(string)");
}

TEST_CASE("a statement continues after an operator, '=', or inside brackets", "[parser]") {
    CHECK(parse_text("x = 1 +\n    2").dump == "(= x (+ (int 1) (int 2)))");
    CHECK(parse_text("Local y =\n    3").dump == "(local y = (int 3))");
    CHECK(parse_text("Print(1,\n      2)").dump == "(call Print (int 1) (int 2))");
    CHECK(parse_text("x = (1\n     + 2)").dump == "(= x (+ (int 1) (int 2)))");
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

TEST_CASE("constructs past M1 are reported once and skipped", "[parser]") {
    const parsed_t parsed = parse_text("Interface Damageable\n"
                                       "    Method Hurt()\n"
                                       "End Interface\n"
                                       "Public Function F()\n"
                                       "End Function\n"
                                       "x = 1");

    // The whole Interface becomes one (error). 'Public' is dropped, and its Function still
    // parses.
    CHECK(parsed.dump == "(error)\n(function F (block))\n(= x (int 1))");
    CHECK(parsed.errors == std::vector<std::string>{
                               "0-9: 'Interface' isn't supported yet",
                               "53-59: 'Public' isn't supported yet",
                           });
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

    // Interfaces come later. "Implements" is at 8-18.
    CHECK(parse_text("Class A Implements B\nEnd Class").errors ==
          std::vector<std::string>{ "8-18: 'Implements' isn't supported yet" });
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
    CHECK(parse_text("Final Local x = 1").errors ==
          std::vector<std::string>{
              "6-11: Abstract, Final, and Override only go before a Class or a Method" });
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
    CHECK(parse_text("@Inline Function F()\nEnd Function").errors ==
          std::vector<std::string>{ "0-1: attributes aren't supported yet" });
}

TEST_CASE("Defer takes any statement; analysis checks which", "[parser]") {
    CHECK(parse_text("Defer Print(\"bye\")").dump == "(defer (call Print (string \"bye\")))");
    CHECK(parse_text("Defer If x Then f.Close()").dump ==
          "(defer (if-then x (block (call (. f Close)))))");
}
