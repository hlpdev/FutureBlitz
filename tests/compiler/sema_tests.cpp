#include "fb/const_value.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"
#include "fb/sema.hpp"
#include "printers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using errors_t = std::vector<std::string>;

struct analyzed_t {
    errors_t errors;  // "<source text>: message", from every stage, in source order
    std::string last; // see probe()
};

/// Lexes, parses, and analyzes `text`. `last` is only filled in when the last line declares a
/// variable with a value.
analyzed_t analyze_text(const std::string_view text) {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    const fb::ast_t ast = fb::parse(file, tokens, bag);
    const fb::analysis_t analysis = fb::analyze(file, ast, bag);
    bag.sort();

    analyzed_t analyzed;
    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        analyzed.errors.push_back(
            std::format("{}: {}", file.slice(diagnostic.span), diagnostic.message));
    }

    if (ast.items().empty()) {
        return analyzed;
    }

    const auto* variable = std::get_if<fb::variable_stmt_t>(&ast.get(ast.items().back()).node);
    if (variable == nullptr || !variable->value) {
        return analyzed;
    }

    const fb::expr_info_t& info = analysis.expr(*variable->value);
    analyzed.last = analysis.types.name(info.converted);
    if (info.value) {
        analyzed.last += " " + fb::to_text(*info.value);
    }

    if (info.type != info.converted) {
        analyzed.last += std::format(" (from {})", analysis.types.name(info.type));
    }

    return analyzed;
}

/// The initializer on the program's last line, as "<type> <value>": "Int32 3", or just "Int32"
/// when it isn't known at compile time. A value that was widened ends in "(from <type>)".
std::string probe(const std::string_view program) {
    const analyzed_t analyzed = analyze_text(program);
    INFO(program);
    CHECK(analyzed.errors.empty());
    return analyzed.last;
}

errors_t errors_of(const std::string_view program) {
    return analyze_text(program).errors;
}

} // namespace

// Literals and conversions

TEST_CASE("literals have default types", "[sema]") {
    CHECK(probe("Local x = 42") == "Int32 42");
    CHECK(probe("Local x = 1.5") == "Float32 1.5");

    // A character literal is its code point.
    CHECK(probe("Local x = 'A'") == "Int32 65");
    CHECK(probe("Local x = True") == "Bool True");
    CHECK(probe("Local x = \"hi\"") == "String hi");

    // The minus sign is part of the literal, so the most negative Int32 fits.
    CHECK(probe("Local x = -2147483648") == "Int32 -2147483648");
}

TEST_CASE("a literal takes the expected type when its value fits", "[sema]") {
    CHECK(probe("Local x: Int8 = 100") == "Int8 100");
    CHECK(probe("Local x: Int8 = -128") == "Int8 -128");
    CHECK(probe("Local x: Double = 1") == "Float64 1");
    CHECK(probe("Local x: Double = 0.1") == "Float64 0.1");
    CHECK(probe("Local x: Double = 1 + 2") == "Float64 3");
    CHECK(probe("Local x: Long = 5_000_000_000") == "Int64 5000000000");
    CHECK(probe("Local x: Byte = 'A'") == "UInt8 65");
    CHECK(probe("Local x: Int8 = True ? 1 : 2") == "Int8 1");
}

TEST_CASE("a literal that doesn't fit is an error", "[sema]") {
    CHECK(errors_of("Local x: Int8 = 300") == errors_t{ "300: 300 doesn't fit in Int8" });
    CHECK(errors_of("Local x: UInt8 = -1") == errors_t{ "-1: -1 doesn't fit in UInt8" });
    CHECK(errors_of("Local x: Int = 1.5") == errors_t{ "1.5: 1.5 doesn't fit in Int32" });
    CHECK(errors_of("Local x = 5_000_000_000") ==
          errors_t{ "5_000_000_000: 5_000_000_000 doesn't fit in Int32" });

    // A decimal literal is a Float32 by default, and Float32 tops out near 3.4e38.
    CHECK(errors_of("Local x = 1.0e40") == errors_t{ "1.0e40: 1.0e40 doesn't fit in Float32" });
}

TEST_CASE("mixed numeric operands widen losslessly", "[sema]") {
    CHECK(probe("Local i = 1\nLocal l: Long = 2\nLocal x = i + l") == "Int64");
    CHECK(probe("Local i = 1\nLocal d: Double = 2\nLocal x = i * d") == "Float64");
    CHECK(probe("Local b: Byte = 1\nLocal s: Int16 = 2\nLocal x = b + s") == "Int16");
    CHECK(probe("Local i = 1\nLocal x: Long = i") == "Int64 (from Int32)");
    CHECK(errors_of("Local i = 1\nLocal f = 2.0\nLocal x = i + f") ==
          errors_t{ "i + f: '+' can't combine Int32 and Float32; convert one side with 'As'" });
    CHECK(errors_of("Local u: UInt32 = 1\nLocal i = 2\nLocal x = u + i") ==
          errors_t{ "u + i: '+' can't combine UInt32 and Int32; convert one side with 'As'" });
    CHECK(errors_of("Local l: Long = 0\nLocal i: Int = l") ==
          errors_t{ "l: expected Int32, found Int64" });
}

TEST_CASE("a literal operand takes the other side's type", "[sema]") {
    CHECK(probe("Local l: Long = 2\nLocal x = l + 1") == "Int64");
    CHECK(probe("Local f = 2.0\nLocal x = f * 2") == "Float32");
    CHECK(probe("Local d: Double = 1\nLocal x = d * 0.1") == "Float64");
    CHECK(probe("Local b: Byte = 1\nLocal x = b | 0x80") == "UInt8");
    CHECK(probe("Local b: Byte = 1\nLocal x = 1 + b") == "UInt8");
    CHECK(probe("Local x = 1 + 0.5") == "Float32 1.5");

    // 300 doesn't fit in a Byte, so it stays an Int, and the Byte widens to it.
    CHECK(probe("Local b: Byte = 1\nLocal x = b + 300") == "Int32");
}

// Compile-time values

TEST_CASE("constant expressions are computed in their own type", "[sema]") {
    // As in C, integer '/' rounds toward zero, '%' takes the dividend's sign, and '>>' on a signed
    // integer keeps the sign.
    CHECK(probe("Local x = 24 * 60 * 60") == "Int32 86400");
    CHECK(probe("Local x = 7 / 2") == "Int32 3");
    CHECK(probe("Local x = -7 % 3") == "Int32 -1");
    CHECK(probe("Local x = 2 ** 10") == "Int32 1024");
    CHECK(probe("Local x = 1 << 4 | 1") == "Int32 17");
    CHECK(probe("Local x = -16 >> 2") == "Int32 -4");
    CHECK(probe("Local x: UInt8 = ~0x0F") == "UInt8 240");
    CHECK(probe("Local x = 7.0 / 2") == "Float32 3.5");
    CHECK(probe("Local x = 1 < 2 && !False") == "Bool True");
    CHECK(probe("Local x = \"a\" + \"b\"") == "String ab");
    CHECK(probe("Local x = \"a\" == \"a\"") == "Bool True");
    CHECK(probe("Local x = True ? 1 : 2") == "Int32 1");
}

TEST_CASE("overflow and division by zero in constants are errors", "[sema]") {
    CHECK(errors_of("Local x = 2147483647 + 1") ==
          errors_t{ "2147483647 + 1: the result of '+' overflows Int32" });
    CHECK(errors_of("Local x: Int8 = 100 + 100") ==
          errors_t{ "100 + 100: the result of '+' overflows Int8" });
    CHECK(errors_of("Local x = -(-2147483648)") ==
          errors_t{ "-(-2147483648): the result of '-' overflows Int32" });
    CHECK(errors_of("Local x = 1 / 0") == errors_t{ "1 / 0: division by zero" });
    CHECK(errors_of("Local x = 1.0 / 0") == errors_t{ "1.0 / 0: division by zero" });
    CHECK(errors_of("Local x = 1 << 32") ==
          errors_t{ "1 << 32: the shift amount must be from 0 to 31" });
    CHECK(errors_of("Local x = 2 ** -1") ==
          errors_t{ "2 ** -1: an integer power needs an exponent of 0 or more" });
}

TEST_CASE("As converts between numbers", "[sema]") {
    // Integers keep their low bits (300 - 256 = 44), and decimals drop the fraction.
    CHECK(probe("Local x = 300 As UInt8") == "UInt8 44");
    CHECK(probe("Local x = -1 As UInt8") == "UInt8 255");
    CHECK(probe("Local x = 5_000_000_000 As Long") == "Int64 5000000000");
    CHECK(probe("Local x = 2.9 As Int") == "Int32 2");
    CHECK(probe("Local x = -2.9 As Int") == "Int32 -2");
    CHECK(probe("Local x = 7 As Float") == "Float32 7");
    CHECK(probe("Local i = 1\nLocal x = i As Float") == "Float32");
    CHECK(errors_of("Local x = 1.0e10 As Int") ==
          errors_t{ "1.0e10 As Int: 1e+10 doesn't fit in Int32" });
    CHECK(errors_of("Local x = \"1\" As Int") ==
          errors_t{ "\"1\" As Int: 'As' can't convert String to Int32" });
}

TEST_CASE("As turns a Bool into 0 or 1", "[sema]") {
    CHECK(probe("Local x = True As Int") == "Int32 1");
    CHECK(probe("Local x = False As UInt8") == "UInt8 0");
    CHECK(probe("Local b = True\nLocal x = b As Long") == "Int64");
    CHECK(errors_of("Local x = True As Float") ==
          errors_t{ "True As Float: 'As' can't convert Bool to Float32" });
    CHECK(errors_of("Local x = 1 As Bool") ==
          errors_t{ "1 As Bool: 'As' can't convert Int32 to Bool" });
}

TEST_CASE("strings", "[sema]") {
    CHECK(probe("Local hp = 5\nLocal x = \"HP: {hp}\"") == "String");
    CHECK(probe("Const MAX = 100\nLocal x = \"{MAX} {0.1} {True} {'A'}\"") ==
          "String 100 0.1 True 65");
    CHECK(errors_of("Local x = \"a\" < \"b\"") ==
          errors_t{ "\"a\" < \"b\": Strings can only be compared with == and != for now" });
    CHECK(errors_of("Local x = \"a\" + 1") ==
          errors_t{ "\"a\" + 1: '+' can't combine String and Int32" });
    CHECK(errors_of("Local x = \"a\" - \"b\"") ==
          errors_t{ "\"a\" - \"b\": '-' can't be used on String" });
}

TEST_CASE("operators check their operand types", "[sema]") {
    CHECK(errors_of("Local x = !1") == errors_t{ "!1: '!' can't be used on Int32" });
    CHECK(errors_of("Local u: UInt32 = 1\nLocal x = -u") ==
          errors_t{ "-u: '-' can't be used on UInt32" });
    CHECK(errors_of("Local f = 1.5\nLocal x = f & 1") ==
          errors_t{ "f & 1: '&' can't be used on Float32" });
    CHECK(errors_of("Local x = True | False") ==
          errors_t{ "True | False: '|' can't be used on Bool" });
    CHECK(errors_of("Local f = 1.5\nLocal x = f << 1") ==
          errors_t{ "f << 1: shifts only work on integers, not Float32" });
    CHECK(errors_of("Local x = 1 && True") == errors_t{ "1: expected Bool, found Int32" });
    CHECK(errors_of("Local x = True < False") ==
          errors_t{ "True < False: '<' can't be used on Bool" });
    CHECK(errors_of("Local x = True + 1") ==
          errors_t{ "True + 1: '+' can't combine Bool and Int32" });
    CHECK(
        errors_of("Local x = True ? 1 : \"a\"") ==
        errors_t{ "True ? 1 : \"a\": the results of '?' have different types, Int32 and String" });
}

// Names and scopes

TEST_CASE("names must be declared", "[sema]") {
    CHECK(errors_of("Local x = y") == errors_t{ "y: 'y' isn't declared" });
    CHECK(errors_of("Local x = Int") == errors_t{ "Int: 'Int' is a type, not a value" });
    CHECK(errors_of("Function F()\nEnd Function\nLocal x = F") ==
          errors_t{ "F: 'F' is a function, not a value" });
    CHECK(errors_of("Local x: Foo") == errors_t{ "Foo: 'Foo' isn't a type" });

    // Vec3 is a built-in type from the spec that comes after M1.
    CHECK(errors_of("Local v: Vec3") == errors_t{ "Vec3: 'Vec3' isn't supported yet" });
}

TEST_CASE("functions and Globals are visible anywhere in the file", "[sema]") {
    CHECK(errors_of("Print(Greeting())\n"
                    "Function Greeting(): String\n"
                    "    Return \"hi {count}\"\n"
                    "End Function\n"
                    "Global count = 3")
              .empty());
}

TEST_CASE("functions can't see top-level Locals", "[sema]") {
    CHECK(errors_of("Local hp = 10\n"
                    "Function F(): Int\n"
                    "    Return hp\n"
                    "End Function") == errors_t{ "hp: 'hp' isn't declared" });
}

TEST_CASE("a name can't be declared twice", "[sema]") {
    CHECK(errors_of("Local x = 1\nLocal x = 2") ==
          errors_t{ "x: 'x' is already declared in this scope" });
    CHECK(errors_of("Local x = 1\n"
                    "If True\n"
                    "    Local x = 2\n"
                    "End If") == errors_t{ "x: 'x' is already declared in an enclosing block" });
    CHECK(errors_of("Global g = 1\nGlobal g = 2") == errors_t{ "g: 'g' is already declared" });
    CHECK(errors_of("Global g = 1\nFunction g()\nEnd Function") ==
          errors_t{ "g: 'g' is already declared" });
    CHECK(errors_of("Function F(a: Int, a: Int)\nEnd Function") ==
          errors_t{ "a: 'a' is already declared in this scope" });
    CHECK(errors_of("Function F(a: Int)\n"
                    "    Local a = 1\n"
                    "End Function") == errors_t{ "a: 'a' is already declared in this scope" });
}

TEST_CASE("sibling blocks can reuse a name, and a Local can hide a Global", "[sema]") {
    CHECK(errors_of("If True\n"
                    "    Local x = 1\n"
                    "End If\n"
                    "If True\n"
                    "    Local x = 2\n"
                    "End If")
              .empty());
    CHECK(probe("Global x = 1.5\nLocal x = 2") == "Int32 2");
}

TEST_CASE("Globals and Consts initialize in source order", "[sema]") {
    CHECK(
        errors_of("Global a = b\nGlobal b = 1") ==
        errors_t{ "b: 'b' isn't initialized yet; Globals and Consts initialize in source order" });
    CHECK(errors_of("Const A = 2\nGlobal b = A * 3").empty());

    // An initializer can only use the Consts and Globals above it, not top-level Locals.
    CHECK(errors_of("Local x = 1\nGlobal g = x") == errors_t{ "x: 'x' isn't declared" });
}

TEST_CASE("Consts", "[sema]") {
    CHECK(probe("Const DAY = 24 * 60 * 60\nLocal x = DAY * 7") == "Int32 604800");
    CHECK(probe("Const NAME = \"FB\"\nLocal x = \"{NAME}!\"") == "String FB!");

    // A Const whose value isn't known at compile time is computed once at run time.
    CHECK(probe("Global g = 1\nConst C = g") == "Int32");
    CHECK(errors_of("Const X: Int") == errors_t{ "X: 'X' is a Const, so it needs a value" });
    CHECK(errors_of("Const X = 1\nX = 2") == errors_t{ "X: 'X' is a Const and can't be changed" });
}

TEST_CASE("a variable needs a type or a value", "[sema]") {
    CHECK(errors_of("Local x") == errors_t{ "x: 'x' needs a type or a value" });
    CHECK(errors_of("Global g") == errors_t{ "g: 'g' needs a type or a value" });
    CHECK(errors_of("Global g: Int\nLocal x = g").empty());
}

TEST_CASE("Globals and functions are declared only at the top level", "[sema]") {
    CHECK(errors_of("If True\n"
                    "    Global g = 1\n"
                    "End If") ==
          errors_t{ "Global g = 1: Globals can only be declared at the top level" });
    CHECK(errors_of("If True\n"
                    "    Function F()\n"
                    "    End Function\n"
                    "End If") == errors_t{ "F: Functions can only be declared at the top level" });
}

// Definite assignment

TEST_CASE("Locals must be assigned before they're read", "[sema]") {
    CHECK(errors_of("Local hp: Int\nPrint(\"{hp}\")") ==
          errors_t{ "hp: 'hp' is read before it's assigned" });
    CHECK(errors_of("Local hp: Int\nhp = 5\nPrint(\"{hp}\")").empty());
    CHECK(errors_of("Local hp: Int\nhp += 1") ==
          errors_t{ "hp: 'hp' is read before it's assigned" });

    // Reported once, not at every read.
    CHECK(errors_of("Local hp: Int\nPrint(\"{hp} {hp}\")") ==
          errors_t{ "hp: 'hp' is read before it's assigned" });
}

TEST_CASE("an ElseIf condition only sees what was assigned before the If", "[sema]") {
    // The condition only runs when the branches before it didn't, so their assignments don't
    // count, and a Return in one of them doesn't hide the read.
    CHECK(errors_of("Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    hp = 1\n"
                    "ElseIf hp > 0\n"
                    "    Print(\"positive\")\n"
                    "End If") == errors_t{ "hp: 'hp' is read before it's assigned" });
    CHECK(errors_of("Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    Return\n"
                    "ElseIf hp > 0\n"
                    "    Print(\"positive\")\n"
                    "End If") == errors_t{ "hp: 'hp' is read before it's assigned" });
}

TEST_CASE("assignment is tracked through branches and loops", "[sema]") {
    CHECK(errors_of("Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    hp = 1\n"
                    "Else\n"
                    "    hp = 2\n"
                    "End If\n"
                    "Print(\"{hp}\")")
              .empty());
    CHECK(errors_of("Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    hp = 1\n"
                    "End If\n"
                    "Print(\"{hp}\")") == errors_t{ "hp: 'hp' is read before it's assigned" });

    // A While body may run zero times. A Repeat body runs at least once, and the Loop only ends
    // at its Break.
    CHECK(errors_of("Local c = True\n"
                    "Local hp: Int\n"
                    "While c\n"
                    "    hp = 1\n"
                    "End While\n"
                    "Print(\"{hp}\")") == errors_t{ "hp: 'hp' is read before it's assigned" });
    CHECK(errors_of("Local hp: Int\n"
                    "Repeat\n"
                    "    hp = 1\n"
                    "Until True\n"
                    "Print(\"{hp}\")")
              .empty());
    CHECK(errors_of("Local hp: Int\n"
                    "Loop\n"
                    "    hp = 1\n"
                    "    Break\n"
                    "End Loop\n"
                    "Print(\"{hp}\")")
              .empty());

    // Default returns, so only the Case 1 path reaches the Print.
    CHECK(errors_of("Local n = 1\n"
                    "Local hp: Int\n"
                    "Select n\n"
                    "    Case 1\n"
                    "        hp = 1\n"
                    "    Default\n"
                    "        Return\n"
                    "End Select\n"
                    "Print(\"{hp}\")")
              .empty());
}

// Functions

TEST_CASE("a function with a return type returns on every path", "[sema]") {
    CHECK(errors_of("Function Sign(x: Int): Int\n"
                    "    If x < 0\n"
                    "        Return -1\n"
                    "    ElseIf x > 0\n"
                    "        Return 1\n"
                    "    End If\n"
                    "End Function") ==
          errors_t{ "Sign: 'Sign' doesn't return a value on every path" });
    CHECK(errors_of("Function Sign(x: Int): Int\n"
                    "    If x < 0\n"
                    "        Return -1\n"
                    "    End If\n"
                    "    Return 1\n"
                    "End Function")
              .empty());

    // A While True without a Break never ends, so the end of the function can't be reached.
    CHECK(errors_of("Function Forever(): Int\n"
                    "    While True\n"
                    "    End While\n"
                    "End Function")
              .empty());
}

TEST_CASE("Return matches the function's return type", "[sema]") {
    CHECK(errors_of("Function F(): Int\n    Return\nEnd Function") ==
          errors_t{ "Return: 'F' returns Int32, so Return needs a value" });
    CHECK(errors_of("Function F()\n    Return 1\nEnd Function") ==
          errors_t{ "1: 'F' has no return type, so it can't return a value" });
    CHECK(errors_of("Function F(): Int => \"no\"") ==
          errors_t{ "\"no\": expected Int32, found String" });
    CHECK(errors_of("Function F(): Long => 1").empty());
}

TEST_CASE("top-level code can end early with a bare Return", "[sema]") {
    CHECK(errors_of("Print(\"a\")\nReturn\nPrint(\"b\")").empty());
    CHECK(errors_of("Return 1") == errors_t{ "1: top-level code can't return a value" });
}

TEST_CASE("parameters and loop variables can't be assigned", "[sema]") {
    CHECK(errors_of("Function F(hp: Int)\n    hp = 1\nEnd Function") ==
          errors_t{ "hp: 'hp' is a parameter and can't be changed; copy it into a Local, or "
                    "make it ByRef" });
    CHECK(errors_of("For i = 0 To 9\n    i = 5\nEnd For") ==
          errors_t{ "i: 'i' is a loop variable and can't be changed" });
    CHECK(errors_of("For i = 0 To 9\n    i++\nEnd For") ==
          errors_t{ "i: 'i' is a loop variable and can't be changed" });
}

TEST_CASE("calls match arguments to parameters", "[sema]") {
    constexpr std::string_view spawn = "Function Spawn(x: Float, hp: Int = 100, name: String = "
                                       "\"grunt\"): Int\n"
                                       "    Return hp\n"
                                       "End Function\n";
    const auto call = [&](const std::string_view text) {
        return errors_of(std::format("{}Local a = {}", spawn, text));
    };

    CHECK(probe(std::format("{}Local a = Spawn(1.5)", spawn)) == "Int32");
    CHECK(call("Spawn(0, name: \"boss\")").empty());
    CHECK(call("Spawn()") == errors_t{ "Spawn(): missing an argument for parameter 'x'" });
    CHECK(call("Spawn(1, 2, \"a\", 4)") == errors_t{ "4: too many arguments to 'Spawn'" });
    CHECK(call("Spawn(1, size: 2)") == errors_t{ "size: 'Spawn' has no parameter named 'size'" });
    CHECK(call("Spawn(name: \"a\", 1)") ==
          errors_t{ "1: positional arguments must come before named ones" });
    CHECK(call("Spawn(1, hp: 2, hp: 3)") ==
          errors_t{ "3: parameter 'hp' already has an argument" });
    CHECK(call("Spawn(\"x\")") == errors_t{ "\"x\": expected Float32, found String" });
}

TEST_CASE("only functions can be called, and only values used", "[sema]") {
    CHECK(errors_of("Function Log(text: String)\nEnd Function\nLocal x = Log(\"a\")") ==
          errors_t{ "Log(\"a\"): 'Log' doesn't return a value" });
    CHECK(errors_of("Local x = 1\nx(2)") == errors_t{ "x: 'x' isn't a function" });
    CHECK(errors_of("Local x = Int(2)") ==
          errors_t{ "Int: 'Int' is a type; convert to it with 'As'" });

    // The built-in Print takes a String.
    CHECK(errors_of("Print(1)") == errors_t{ "1: expected String, found Int32" });
}

TEST_CASE("default values are known at compile time", "[sema]") {
    CHECK(errors_of("Const D = 4\nFunction F(a: Int = D * 2)\nEnd Function").empty());
    CHECK(errors_of("Global g = 1\nFunction F(a: Int = g)\nEnd Function") ==
          errors_t{ "g: a default value must be known at compile time" });
    CHECK(errors_of("Function F(a: Int8 = 300)\nEnd Function") ==
          errors_t{ "300: 300 doesn't fit in Int8" });
}

TEST_CASE("overloads: an exact match beats a conversion", "[sema]") {
    // Each overload returns a different type, so the result shows which one was picked.
    constexpr std::string_view show = "Function Show(v: Int): Bool => True\n"
                                      "Function Show(v: Double): String => \"d\"\n"
                                      "Function Show(v: Long): Float => 1.0\n";
    const auto call = [&](const std::string_view text) {
        return std::format("{}Local b: Byte = 1\nLocal x = {}", show, text);
    };

    CHECK(probe(call("Show(1)")) == "Bool");
    CHECK(probe(call("Show(1.5)")) == "String");
    CHECK(probe(call("Show(b As Long)")) == "Float32");

    // Both fit without being exact: a Byte widens to all three, and the literal is Long or Double.
    CHECK(errors_of(call("Show(b)")) == errors_t{ "Show: the call to 'Show' is ambiguous" });
    CHECK(errors_of(call("Show(5_000_000_000)")) ==
          errors_t{ "Show: the call to 'Show' is ambiguous" });
    CHECK(errors_of(call("Show(True)")) == errors_t{ "Show: no overload of 'Show' takes (Bool)" });
    CHECK(errors_of("Function F(a: Int)\nEnd Function\nFunction F(b: Int)\nEnd Function") ==
          errors_t{ "F: 'F' is already declared with the same parameter types" });
}

TEST_CASE("the built-in Print can be overloaded", "[sema]") {
    CHECK(errors_of("Function Print(value: Int)\n"
                    "End Function\n"
                    "Print(1)\n"
                    "Print(\"a\")")
              .empty());
    CHECK(errors_of("Function Print(text: String)\nEnd Function") ==
          errors_t{ "Print: 'Print' is already declared with the same parameter types" });
}

TEST_CASE("ByRef arguments", "[sema]") {
    constexpr std::string_view swap = "Function Swap(ByRef a: Int, ByRef b: Int)\n"
                                      "    Local t = a\n"
                                      "    a = b\n"
                                      "    b = t\n"
                                      "End Function\n"
                                      "Local x = 1\n"
                                      "Local y = 2\n";
    const auto call = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", swap, text));
    };

    CHECK(call("Swap(ByRef x, ByRef y)").empty());
    CHECK(call("Swap(x, ByRef y)") == errors_t{ "x: parameter 'a' is ByRef; write 'ByRef x'" });
    CHECK(call("Swap(ByRef x, ByRef x)") == errors_t{ "x: 'x' is passed ByRef twice in one call" });
    CHECK(call("Swap(ByRef 1, ByRef y)") ==
          errors_t{ "1: only a variable or one of its fields can be passed ByRef" });
    CHECK(call("Const C = 1\nSwap(ByRef C, ByRef y)") ==
          errors_t{ "C: 'C' is a Const and can't be passed ByRef" });
    CHECK(call("Local l: Long = 1\nSwap(ByRef l, ByRef y)") ==
          errors_t{ "l: a ByRef argument must be exactly Int32, but this is Int64" });
    CHECK(call("Local u: Int\nSwap(ByRef u, ByRef y)") ==
          errors_t{ "u: 'u' is read before it's assigned" });
}

TEST_CASE("ByRef parameters", "[sema]") {
    CHECK(errors_of("Function F(a: Int)\nEnd Function\nLocal x = 1\nF(ByRef x)") ==
          errors_t{ "x: parameter 'a' isn't ByRef; remove 'ByRef'" });
    CHECK(errors_of("Function F(ByRef a: Int = 1)\nEnd Function") ==
          errors_t{ "1: a ByRef parameter can't have a default value" });
    CHECK(errors_of("Function Twice(ByRef a: Int)\n"
                    "    a *= 2\n"
                    "End Function\n"
                    "Function Four(ByRef a: Int)\n"
                    "    Twice(ByRef a)\n"
                    "    Twice(ByRef a)\n"
                    "End Function")
              .empty());
    CHECK(errors_of("Function F(a: Int)\n"
                    "    Twice(ByRef a)\n"
                    "End Function\n"
                    "Function Twice(ByRef a: Int)\n"
                    "End Function") ==
          errors_t{ "a: 'a' is a parameter and can't be passed ByRef; copy it into a Local, or "
                    "make it ByRef" });
}

// Statements

TEST_CASE("assignments convert losslessly", "[sema]") {
    CHECK(errors_of("Local l: Long = 0\nLocal i = 5\nl = i").empty());
    CHECK(errors_of("Local l: Long = 0\nLocal i = 5\ni = l") ==
          errors_t{ "l: expected Int32, found Int64" });
    CHECK(errors_of("Local b: Byte = 0\nb += 1").empty());
    CHECK(errors_of("Local b: Byte = 0\nLocal i = 1\nb += i") ==
          errors_t{ "i: expected UInt8, found Int32" });
    CHECK(errors_of("Local b: Byte = 0\nb += 300") == errors_t{ "300: 300 doesn't fit in UInt8" });
    CHECK(errors_of("Local b: Byte = 0\nb <<= 2").empty());
    CHECK(errors_of("Local s = \"a\"\ns += \"b\"").empty());
    CHECK(errors_of("Local s = \"a\"\ns++") == errors_t{ "s++: '++' can't be used on String" });
    CHECK(errors_of("F() = 1\nFunction F(): Int => 1") ==
          errors_t{ "F(): only a variable can be assigned to" });
}

TEST_CASE("conditions must be Bool", "[sema]") {
    CHECK(errors_of("If 1\nEnd If") == errors_t{ "1: expected Bool, found Int32" });
    CHECK(errors_of("Local n = 1\nWhile n\nEnd While") ==
          errors_t{ "n: expected Bool, found Int32" });
    CHECK(errors_of("Repeat\nUntil \"no\"") == errors_t{ "\"no\": expected Bool, found String" });
}

TEST_CASE("For loops", "[sema]") {
    CHECK(errors_of("For i = 0 To 9\n    Print(\"{i}\")\nEnd For").empty());
    CHECK(errors_of("For i = 0.0 To 1.0 Step 0.25\nEnd For").empty());
    CHECK(errors_of("For i = 10 To 0 Step -1\nEnd For").empty());

    // A literal start takes the end's type.
    CHECK(errors_of("Local n: Long = 10\n"
                    "For i = 0 Until n\n"
                    "    Local x: Int = i\n"
                    "End For") == errors_t{ "i: expected Int32, found Int64" });

    CHECK(errors_of("For i = 0 To 9\nEnd For\nPrint(\"{i}\")") ==
          errors_t{ "i: 'i' isn't declared" });
    CHECK(errors_of("For i = 0 To 9\n    Local i = 1\nEnd For") ==
          errors_t{ "i: 'i' is already declared in an enclosing block" });
    CHECK(errors_of("For i = 1 To 9 Step 0\nEnd For") == errors_t{ "0: Step can't be 0" });
    CHECK(errors_of("For s = \"a\" To \"b\"\nEnd For") ==
          errors_t{ "\"a\": a For loop counts with numbers, not String" });
}

TEST_CASE("Break and Continue", "[sema]") {
    CHECK(errors_of("Break") == errors_t{ "Break: Break must be inside a loop" });
    CHECK(errors_of("Outer: Loop\n"
                    "    For i = 0 To 9\n"
                    "        Break Outer\n"
                    "    End For\n"
                    "End Loop")
              .empty());
    CHECK(errors_of("Loop\n    Continue Outer\nEnd Loop") ==
          errors_t{ "Outer: no enclosing loop is labeled 'Outer'" });
    CHECK(errors_of("A: Loop\n"
                    "    A: Loop\n"
                    "        Break A\n"
                    "    End Loop\n"
                    "End Loop") == errors_t{ "A: 'A' already labels an enclosing loop" });
}

TEST_CASE("Select", "[sema]") {
    const auto select = [](const std::string_view subject, const std::string_view cases) {
        return errors_of(std::format("{}\nSelect s\n{}\nEnd Select", subject, cases));
    };

    CHECK(select("Local s = 3", "Case 1, 2\nCase 3 To 5\nDefault").empty());
    CHECK(select("Local s = \"a\"", "Case \"a\", \"b\"\nCase \"c\"").empty());
    CHECK(select("Local s = 3", "Case 1 To 5\nCase 4") ==
          errors_t{ "4: this Case value is already covered by an earlier Case" });
    CHECK(select("Local s = \"a\"", "Case \"a\"\nCase \"a\"") ==
          errors_t{ "\"a\": this Case value is already covered by an earlier Case" });
    CHECK(select("Local s = 3\nLocal m = 1", "Case m") ==
          errors_t{ "m: a Case value must be known at compile time" });
    CHECK(select("Local s = \"a\"", "Case \"a\" To \"c\"") ==
          errors_t{ "\"a\" To \"c\": 'To' ranges only work on integers" });
    CHECK(select("Local s = 3", "Case 5 To 1") == errors_t{ "5 To 1: this range is empty" });
    CHECK(select("Local s: Byte = 1", "Case 300") == errors_t{ "300: 300 doesn't fit in UInt8" });
    CHECK(select("Local s = 1.5", "Case 1") ==
          errors_t{ "s: Select works on integers, Bool, and String, not Float32" });
}

TEST_CASE("built-in types after M2 are reported as not supported yet", "[sema]") {
    CHECK(errors_of("Local x: Vec3") == errors_t{ "Vec3: 'Vec3' isn't supported yet" });
}

// Structs

TEST_CASE("Structs get a constructor that takes their fields", "[sema]") {
    constexpr std::string_view point = "Struct Point\n"
                                       "    Field x: Int\n"
                                       "    Field y: Int = 9\n"
                                       "End Struct\n";
    const auto with_point = [&](const std::string_view text) {
        return std::format("{}{}", point, text);
    };

    CHECK(probe(with_point("Local p = Point(1, 2)")) == "Point");
    CHECK(probe(with_point("Local p = Point(y: 2)\nLocal x = p.x")) == "Int32");
    CHECK(errors_of(with_point("Local p = Point(z: 3)")) ==
          errors_t{ "z: 'Point' has no parameter named 'z'" });
    CHECK(errors_of(with_point("Local p = Point(1, 2, 3)")) ==
          errors_t{ "3: too many arguments to 'Point'" });

    // A New replaces it.
    CHECK(errors_of("Struct P\n"
                    "    Field x: Int\n"
                    "    New()\n"
                    "    End New\n"
                    "End Struct\n"
                    "Local p = P(x: 1)") == errors_t{ "x: 'P' has no parameter named 'x'" });
}

TEST_CASE("fields and their defaults", "[sema]") {
    CHECK(errors_of("Struct P\n    Field x\nEnd Struct") ==
          errors_t{ "x: 'x' needs a type or a value" });
    CHECK(errors_of("Struct P\n    Field x: Int\n    Field x: Float\nEnd Struct") ==
          errors_t{ "x: 'x' is already declared" });
    CHECK(errors_of("Global g = 1\nStruct P\n    Field x: Int = g\nEnd Struct") ==
          errors_t{ "g: a field's default must be known at compile time" });

    // A field without a type takes its default's, which can use Consts declared above.
    CHECK(probe("Const BASE = 5\n"
                "Struct P\n"
                "    Field x = BASE * 2\n"
                "End Struct\n"
                "Local x = P().x") == "Int32");

    // Above its Struct, such a field's type isn't known yet.
    CHECK(errors_of("Global q = P().x\n"
                    "Struct P\n"
                    "    Field x = 3\n"
                    "End Struct") ==
          errors_t{ "P: 'x' takes its type from its default, so it can't be used above its "
                    "Struct" });
    CHECK(errors_of("Global q = P().x\n"
                    "Struct P\n"
                    "    Field x: Int = 3\n"
                    "End Struct")
              .empty());
}

TEST_CASE("member access", "[sema]") {
    constexpr std::string_view point = "Struct P\n"
                                       "    Field x: Int\n"
                                       "    Method M(): Int => x\n"
                                       "End Struct\n"
                                       "Local p = P()\n";
    const auto with_point = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", point, text));
    };

    CHECK(with_point("Local z = p.z") == errors_t{ "z: P has no member named 'z'" });
    CHECK(with_point("Local m = p.M") == errors_t{ "M: 'M' is a method; call it with ()" });
    CHECK(with_point("p.x()") == errors_t{ "x: 'x' is a field, not a method" });
    CHECK(errors_of("Local a = 1\nLocal b = a.x") == errors_t{ "x: Int32 has no members" });
    CHECK(errors_of("Struct P\nEnd Struct\nLocal p = P") ==
          errors_t{ "P: 'P' is a type, not a value" });
    CHECK(errors_of("Local s = Self") ==
          errors_t{ "Self: 'Self' can only be used in a Method or New" });

    // Changing a field reads the rest of its variable, so that has to be assigned.
    CHECK(errors_of("Struct P\n    Field x: Int\nEnd Struct\nLocal p: P\np.x = 1") ==
          errors_t{ "p: 'p' is read before it's assigned" });
}

TEST_CASE("only a ByRef Method can change Self", "[sema]") {
    const auto in_struct = [](const std::string_view members) {
        return errors_of(std::format("Struct P\n"
                                     "    Field x: Int\n"
                                     "    ByRef Method Bump()\n"
                                     "        x += 1\n"
                                     "    End Method\n"
                                     "{}"
                                     "End Struct\n",
                                     members));
    };

    CHECK(in_struct("").empty());
    CHECK(in_struct("    Method M()\n        x = 1\n    End Method\n") ==
          errors_t{ "x: 'x' is a field, and a Method can't change Self; make it a ByRef Method" });
    CHECK(in_struct("    Method M()\n        Self.x = 1\n    End Method\n") ==
          errors_t{ "Self.x: a Method can't change Self; make it a ByRef Method" });
    CHECK(in_struct("    Method M()\n        Bump()\n    End Method\n") ==
          errors_t{ "Bump: 'Bump' is a ByRef Method, and a Method can't change Self; make this "
                    "a ByRef Method too" });
    CHECK(in_struct("    ByRef Method M()\n        Bump()\n        Self = P()\n    End Method\n")
              .empty());
}

TEST_CASE("a ByRef Method needs something it can change", "[sema]") {
    constexpr std::string_view point = "Struct P\n"
                                       "    Field x: Int\n"
                                       "    ByRef Method Bump()\n"
                                       "        x += 1\n"
                                       "    End Method\n"
                                       "End Struct\n";
    const auto with_point = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", point, text));
    };

    CHECK(with_point("Local p = P()\np.Bump()").empty());
    CHECK(with_point("Function F(p: P)\n    p.Bump()\nEnd Function") ==
          errors_t{ "p: 'p' is a parameter and can't be changed by 'Bump'; copy it into a Local, "
                    "or make it ByRef" });
    CHECK(with_point("Const C = P()\nC.Bump()") ==
          errors_t{ "C: 'C' is a Const and can't be changed by 'Bump'" });
    CHECK(with_point("P().Bump()") ==
          errors_t{ "P(): 'Bump' is a ByRef Method, so it needs a variable to change, not a "
                    "value" });
}

TEST_CASE("fields can be passed ByRef", "[sema]") {
    constexpr std::string_view program = "Struct P\n"
                                         "    Field x: Int\n"
                                         "    Field y: Int\n"
                                         "End Struct\n"
                                         "Function Swap(ByRef a: Int, ByRef b: Int)\n"
                                         "End Function\n"
                                         "Function Take(ByRef p: P, ByRef b: Int)\n"
                                         "End Function\n"
                                         "Local p = P()\n";
    const auto call = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", program, text));
    };

    CHECK(call("Swap(ByRef p.x, ByRef p.y)").empty());
    CHECK(call("Swap(ByRef p.x, ByRef p.x)") ==
          errors_t{ "p.x: 'p.x' is passed ByRef twice in one call" });
    CHECK(call("Take(ByRef p, ByRef p.x)") ==
          errors_t{ "p.x: 'p' and 'p.x' overlap, so only one can be passed ByRef" });
}

TEST_CASE("a Struct can't contain itself", "[sema]") {
    CHECK(errors_of("Struct N\n    Field next: N\nEnd Struct") ==
          errors_t{ "next: 'N' can't contain itself; a Struct is stored inline" });
    CHECK(errors_of("Struct A\n    Field b: B\nEnd Struct\nStruct B\n    Field a: A\nEnd Struct") ==
          errors_t{ "b: 'A' can't contain itself; a Struct is stored inline" });
}

TEST_CASE("Structs compare with == and != only", "[sema]") {
    constexpr std::string_view point =
        "Struct P\n    Field x: Int\nEnd Struct\nStruct Q\nEnd Struct\n";
    const auto with_point = [&](const std::string_view text) {
        return std::format("{}{}", point, text);
    };

    CHECK(probe(with_point("Local same = P() == P(1)")) == "Bool");
    CHECK(errors_of(with_point("Local b = P() < P()")) ==
          errors_t{ "P() < P(): '<' can't be used on P" });
    CHECK(errors_of(with_point("Local b = P() == Q()")) ==
          errors_t{ "P() == Q(): '==' can't combine P and Q" });
    CHECK(errors_of(with_point("Local p = P()\nPrint(\"{p}\")")) ==
          errors_t{ "p: P can't be shown in a string" });
}

TEST_CASE("New", "[sema]") {
    CHECK(errors_of("Struct P\n    New()\n        Return 1\n    End New\nEnd Struct") ==
          errors_t{ "1: New can't return a value; it returns Self" });
    CHECK(errors_of("Struct P\n"
                    "    Field x: Int\n"
                    "    New(x: Int)\n"
                    "        Self.x = x\n"
                    "        If x < 0 Then Return\n"
                    "    End New\n"
                    "End Struct\n"
                    "Local p = P(1)")
              .empty());

    // Its overloads are picked like a function's.
    CHECK(errors_of("Struct P\n"
                    "    New(a: Int)\n"
                    "    End New\n"
                    "    New(b: Int)\n"
                    "    End New\n"
                    "End Struct") ==
          errors_t{ "New: 'New' is already declared with the same parameter types" });
}

TEST_CASE("Struct members only go in Structs", "[sema]") {
    CHECK(errors_of("Method M()\nEnd Method") ==
          errors_t{ "M: Methods can only be declared in a Struct or Class" });
    CHECK(errors_of("Field x: Int") == errors_t{ "x: Fields can only be declared in a Struct" });
    CHECK(errors_of("New()\nEnd New") ==
          errors_t{ "New: New can only be declared in a Struct or Class" });
    CHECK(errors_of("Function F()\n    Struct P\n    End Struct\nEnd Function") ==
          errors_t{ "P: Structs can only be declared at the top level" });
    CHECK(errors_of("Struct P\n    Print(\"x\")\nEnd Struct") ==
          errors_t{ "Print(\"x\"): a Struct can only hold Fields, Methods, and News" });
    CHECK(errors_of("Struct Int\nEnd Struct") == errors_t{ "Int: 'Int' is a built-in type" });
}

TEST_CASE("names in a Method", "[sema]") {
    // A Local can hide a field; Self still reaches it. Methods call each other by name.
    CHECK(errors_of("Struct P\n"
                    "    Field x: Int\n"
                    "    Method A(): Int\n"
                    "        Local x = 5\n"
                    "        Return x + Self.x + B()\n"
                    "    End Method\n"
                    "    Method B(): Int => x\n"
                    "End Struct")
              .empty());
}

// Classes

TEST_CASE("a Class without a New gets one without parameters", "[sema]") {
    CHECK(probe("Class A\n    Field x: Int = 3\nEnd Class\nLocal a = A()") == "A");
    CHECK(errors_of("Class A\n    Field x: Int\nEnd Class\nLocal a = A(1)") ==
          errors_t{ "1: too many arguments to 'A'" });
}

TEST_CASE("a Class's fields change through any reference", "[sema]") {
    constexpr std::string_view point = "Class P\n    Field x: Int\nEnd Class\n";
    const auto with_point = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", point, text));
    };

    // A read-only parameter or Const can't be replaced, but its object can change.
    CHECK(with_point("Function F(p: P)\n    p.x = 1\nEnd Function").empty());
    CHECK(with_point("Const C = P()\nC.x = 2").empty());
    CHECK(with_point("Function F(p: P)\n    p = P()\nEnd Function") ==
          errors_t{ "p: 'p' is a parameter and can't be changed; copy it into a Local, or make it "
                    "ByRef" });

    // Its Methods can change its fields, but Self is the object and can't be replaced.
    CHECK(errors_of("Class C\n"
                    "    Field n: Int\n"
                    "    Method Bump()\n"
                    "        n += 1\n"
                    "        Self.n += 1\n"
                    "    End Method\n"
                    "End Class")
              .empty());
    CHECK(errors_of("Class C\n    Method M()\n        Self = C()\n    End Method\nEnd Class") ==
          errors_t{ "Self: Self is the object itself and can't be replaced" });
    CHECK(errors_of("Class C\n    ByRef Method M()\n    End Method\nEnd Class") ==
          errors_t{ "M: only a Struct's Methods can be ByRef; a Class's Methods can always change "
                    "its fields" });
}

TEST_CASE("Destroy", "[sema]") {
    CHECK(errors_of("Class C\n    Method Destroy()\n    End Method\nEnd Class").empty());
    CHECK(errors_of("Class C\n    Method Destroy(a: Int)\n    End Method\nEnd Class") ==
          errors_t{ "Destroy: Destroy takes no parameters and returns nothing" });
    CHECK(errors_of("Struct S\n    Method Destroy()\n    End Method\nEnd Struct") ==
          errors_t{ "Destroy: a Struct can't have a Destroy; only a Class's runs when its last "
                    "reference goes away" });
    CHECK(errors_of("Class C\n"
                    "    Method Destroy()\n"
                    "    End Method\n"
                    "End Class\n"
                    "Local c = C()\n"
                    "c.Destroy()") ==
          errors_t{ "c.Destroy: Destroy runs by itself when an object's last reference goes away; "
                    "it can't be called" });
}

TEST_CASE("Class references", "[sema]") {
    constexpr std::string_view a = "Class A\nEnd Class\n";
    const auto with_a = [&](const std::string_view text) { return std::format("{}{}", a, text); };

    CHECK(probe(with_a("Local x = A()\nLocal same = x == x")) == "Bool");
    CHECK(probe(with_a("Local x = A()\nLocal same = RefEquals(x, A())")) == "Bool");
    CHECK(errors_of("Local same = RefEquals(1, 2)") ==
          errors_t{ "1: RefEquals compares Class references, not Int32",
                    "2: RefEquals compares Class references, not Int32" });
    CHECK(errors_of(with_a("Local b = A() < A()")) ==
          errors_t{ "A() < A(): '<' can't be used on A" });
    CHECK(errors_of(with_a("Print(\"{A()}\")")) == errors_t{ "A(): A can't be shown in a string" });

    // RefEquals takes an A? too, but checking for Null takes ==.
    CHECK(probe(with_a("Local x: A? = Null\nLocal same = RefEquals(x, A())")) == "Bool");
    CHECK(errors_of(with_a("Local x: A? = Null\nLocal same = RefEquals(x, Null)")) ==
          errors_t{ "Null: RefEquals compares two references; check for Null with '== Null'" });

    // A Local can wait for its value; a Global can't, since a Class has no zero. An A? starts
    // at Null.
    CHECK(errors_of(with_a("Local x: A\nx = A()")).empty());
    CHECK(errors_of(with_a("Global g: A")) ==
          errors_t{ "g: 'g' needs a value, since A has no zero value" });
    CHECK(errors_of(with_a("Global g: A?")).empty());
}

// Nullability

TEST_CASE("a T? holds a T or Null", "[sema]") {
    CHECK(probe("Local x: Int? = 1") == "Int32? 1 (from Int32)");
    CHECK(probe("Local x: Long? = 1") == "Int64? 1 (from Int64)");
    CHECK(probe("Local x: Int? = Null") == "Int32? Null");
    CHECK(probe("Local x = 5 As Long?") == "Int64? 5");
    CHECK(probe("Local a: Int = 1\nLocal x: Long? = a") == "Int64? (from Int32)");
    CHECK(probe("Function F(): String? => Null\nLocal x = F()") == "String?");

    // Null takes the T? it's used as.
    CHECK(errors_of("Local x = Null") ==
          errors_t{ "Null: Null needs a type from where it's used, like 'Local e: Enemy? = "
                    "Null'" });
    CHECK(errors_of("Local x: Int = Null") == errors_t{ "Null: Int32 can't be Null; Int32? can" });
    CHECK(errors_of("Print(Null)") == errors_t{ "Null: String can't be Null; String? can" });
}

TEST_CASE("a T? has to be checked before it's used as a T", "[sema]") {
    // The messages are split around "??" so the compiler doesn't see a trigraph in them.
    CHECK(errors_of("Function F(s: String?)\n    Print(s)\nEnd Function") ==
          errors_t{ "s: expected String, found String?, which may be Null; check it first, or "
                    "give a value for Null with '?"
                    "?'" });
    CHECK(errors_of("Function F(x: Int?)\n    Print(\"{x}\")\nEnd Function") ==
          errors_t{ "x: Int32? can't be shown in a string, since it may be Null; give a value "
                    "for Null with '?"
                    "?'" });
    CHECK(errors_of("Function F(x: Int?): Int => x + 1") ==
          errors_t{ "x + 1: '+' can't be used on Int32?, which may be Null" });
}

TEST_CASE("a null check narrows a T? to its T", "[sema]") {
    constexpr std::string_view enemy = "Class E\n    Field hp: Int\nEnd Class\n";
    const auto with_enemy = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", enemy, text));
    };

    CHECK(with_enemy("Function F(e: E?): Int => e.hp") ==
          errors_t{ "e: 'e' may be Null; check that it isn't, or use '?.'" });

    // If, ElseIf, an early Return, !, &&, ||, ?:, and While all narrow.
    CHECK(with_enemy("Function F(e: E?): Int\n"
                     "    If e != Null Then Return e.hp\n"
                     "    Return 0\n"
                     "End Function")
              .empty());
    CHECK(with_enemy("Function F(e: E?): Int\n"
                     "    If e == Null\n"
                     "        Return 0\n"
                     "    ElseIf e.hp > 0\n"
                     "        Return 1\n"
                     "    End If\n"
                     "    Return e.hp\n"
                     "End Function")
              .empty());
    CHECK(with_enemy("Function F(e: E?): Bool => !(e == Null) && e.hp > 0").empty());
    CHECK(with_enemy("Function F(e: E?): Bool => e == Null || e.hp > 0").empty());
    CHECK(with_enemy("Function F(e: E?): Int => e != Null ? e.hp : 0").empty());
    CHECK(with_enemy("Function F(e: E?): Int\n"
                     "    Local x = e\n"
                     "    While x == Null\n"
                     "        x = E()\n"
                     "    End While\n"
                     "    Return x.hp\n"
                     "End Function")
              .empty());

    // Assigning something that can't be Null narrows too.
    CHECK(with_enemy("Local e: E? = Null\ne = E()\nLocal hp = e.hp").empty());

    // The other side of a check doesn't.
    CHECK(with_enemy("Function F(e: E?): Bool => e != Null || e.hp > 0") ==
          errors_t{ "e: 'e' may be Null; check that it isn't, or use '?.'" });
}

TEST_CASE("narrowing lasts while the variable can't change", "[sema]") {
    constexpr std::string_view enemy = "Class E\n    Field hp: Int\nEnd Class\n";
    const auto with_enemy = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", enemy, text));
    };

    // A Global or a field can change during any call, so they need a Local copy.
    CHECK(with_enemy("Global g: E? = Null\nIf g != Null Then Print(\"{g.hp}\")") ==
          errors_t{ "g: 'g' may be Null; check a Local copy of it, or use '?.'" });
    CHECK(with_enemy("Class H\n"
                     "    Field e: E?\n"
                     "End Class\n"
                     "Local h = H()\n"
                     "If h.e != Null Then Print(\"{h.e.hp}\")") ==
          errors_t{ "h.e: 'h.e' may be Null; check a Local copy of it, or use '?.'" });

    // Assigning a T?, or passing the variable ByRef, ends it.
    CHECK(with_enemy("Function F(e: E?, other: E?): Int\n"
                     "    Local x = e\n"
                     "    If x == Null Then Return 0\n"
                     "    x = other\n"
                     "    Return x.hp\n"
                     "End Function") ==
          errors_t{ "x: 'x' may be Null; check that it isn't, or use '?.'" });
    CHECK(with_enemy("Function Clear(ByRef e: E?)\n"
                     "    e = Null\n"
                     "End Function\n"
                     "Local x: E? = E()\n"
                     "Clear(ByRef x)\n"
                     "Local hp = x.hp") ==
          errors_t{ "x: 'x' may be Null; check that it isn't, or use '?.'" });

    // A loop that changes the variable can't count on a check from before it.
    CHECK(with_enemy("Local x: E? = E()\n"
                     "For i = 1 To 2\n"
                     "    x.hp += 1\n"
                     "    x = Null\n"
                     "End For") ==
          errors_t{ "x: 'x' may be Null; check that it isn't, or use '?.'" });
    CHECK(with_enemy("Local x: E? = E()\n"
                     "For i = 1 To 2\n"
                     "    x.hp += 1\n"
                     "End For")
              .empty());
}

TEST_CASE("?. and ?? give Null, or a value for it", "[sema]") {
    constexpr std::string_view enemy = "Class E\n"
                                       "    Field hp: Int\n"
                                       "    Method Hp(): Int => hp\n"
                                       "End Class\n"
                                       "Local e: E? = Null\n";
    const auto with_enemy = [&](const std::string_view text) {
        return std::format("{}{}", enemy, text);
    };

    CHECK(probe(with_enemy("Local x = e?.hp")) == "Int32?");
    CHECK(probe(with_enemy("Local x = e?.Hp()")) == "Int32?");
    CHECK(probe(with_enemy("Local x = e?.hp ?? 0")) == "Int32");
    CHECK(probe("Local a: Int8? = Null\nLocal x = a ?? 1000") == "Int32");
    CHECK(probe("Const C: Int? = Null\nLocal x = C ?? 7") == "Int32 7");
    CHECK(probe("Const C: Int? = 3\nLocal x = C ?? 7") == "Int32 3");

    // The messages are split around "??" so the compiler doesn't see a trigraph in them.
    CHECK(errors_of("Local a: Int? = Null\nLocal x = a ?? \"no\"") ==
          errors_t{ "a ?? \"no\": the sides of '?"
                    "?' have different types, Int32 and String" });
    CHECK(errors_of(with_enemy("e?.hp = 1")) ==
          errors_t{ "e?.hp: a '?.' can't be assigned to; check that it isn't Null first" });

    // Something that can't be Null doesn't need them.
    CHECK(errors_of("Local a = 1\nLocal x = a ?? 2") == errors_t{ "a: 'a' can't be Null here, so '?"
                                                                  "?' always gives it" });
    CHECK(errors_of(with_enemy("Local f = E()\nLocal x = f?.hp")) ==
          errors_t{ "f: 'f' can't be Null here, so '?.' isn't needed" });
}

TEST_CASE("comparing with Null", "[sema]") {
    CHECK(probe("Local a: Int? = Null\nLocal x = a == Null") == "Bool");
    CHECK(probe("Local a: Int? = Null\nLocal x = Null != a") == "Bool");
    CHECK(probe("Local a: Int? = Null\nLocal x = a == 5") == "Bool");
    CHECK(probe("Const C: Int? = Null\nLocal x = C == Null") == "Bool True");
    CHECK(errors_of("Local a = 1\nLocal x = a == Null") ==
          errors_t{ "a == Null: 'a' can't be Null here, so this is always False" });
}

TEST_CASE("overloads take Null as any T?", "[sema]") {
    CHECK(probe("Function F(a: Int?): Int => 1\n"
                "Function F(a: String): String => \"s\"\n"
                "Local x = F(Null)") == "Int32");
    CHECK(errors_of("Function F(a: Int?)\n"
                    "End Function\n"
                    "Function F(a: String?)\n"
                    "End Function\n"
                    "F(Null)") == errors_t{ "F: the call to 'F' is ambiguous" });
}

TEST_CASE("fields that refer to Classes are assigned by every New", "[sema]") {
    constexpr std::string_view enemy = "Class E\nEnd Class\n";
    const auto with_enemy = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", enemy, text));
    };

    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    New(e: E)\n"
                     "        Self.e = e\n"
                     "    End New\n"
                     "End Class")
              .empty());
    CHECK(with_enemy("Class H\n    Field e: E?\nEnd Class").empty());
    CHECK(with_enemy("Class H\n    Field e: E\nEnd Class") ==
          errors_t{ "e: H needs a New that assigns 'e', since E has no zero value" });

    // On every path, and before any Return.
    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    New(make: Bool)\n"
                     "        If make Then e = E()\n"
                     "    End New\n"
                     "End Class") ==
          errors_t{ "New: New must assign 'e' on every path, since E has no zero value" });
    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    New(make: Bool)\n"
                     "        If !make Then Return\n"
                     "        e = E()\n"
                     "    End New\n"
                     "End Class") ==
          errors_t{ "Return: New must assign 'e' before this Return, since E has no zero value" });

    // Until then, nothing can read it, or use Self, which a Method could read it through.
    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    New()\n"
                     "        Local x = Self.e\n"
                     "        e = E()\n"
                     "    End New\n"
                     "End Class") == errors_t{ "Self.e: 'e' is read before New assigns it" });
    CHECK(with_enemy("Function Use(h: H)\n"
                     "End Function\n"
                     "Class H\n"
                     "    Field e: E\n"
                     "    New()\n"
                     "        Use(Self)\n"
                     "        e = E()\n"
                     "    End New\n"
                     "End Class") == errors_t{ "Self: Self can't be used before New assigns 'e'" });
    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    New()\n"
                     "        Check()\n"
                     "        e = E()\n"
                     "    End New\n"
                     "    Method Check()\n"
                     "    End Method\n"
                     "End Class") ==
          errors_t{ "Check: Self can't be used before New assigns 'e'" });
}

TEST_CASE("a Struct with a field that has no zero has none either", "[sema]") {
    constexpr std::string_view pair = "Class E\n"
                                      "End Class\n"
                                      "Struct S\n"
                                      "    Field e: E\n"
                                      "    Field n: Int\n"
                                      "End Struct\n";
    const auto with_pair = [&](const std::string_view text) {
        return std::format("{}{}", pair, text);
    };

    CHECK(probe(with_pair("Local s = S(E())")) == "S");
    CHECK(errors_of(with_pair("Local s = S(n: 1)")) ==
          errors_t{ "S(n: 1): missing an argument for parameter 'e'" });
    CHECK(errors_of(with_pair("Global g: S")) ==
          errors_t{ "g: 'g' needs a value, since S has no zero value" });

    // A T? holds its T inline, but a Class's fields are in its object.
    CHECK(errors_of("Struct S\n    Field next: S?\nEnd Struct") ==
          errors_t{ "next: 'S' can't contain itself; a Struct is stored inline" });
    CHECK(errors_of("Class N\n    Field next: N?\nEnd Class").empty());
}

// Inheritance

TEST_CASE("a Class extends one other Class and inherits its members", "[sema]") {
    constexpr std::string_view base = "Class A\n"
                                      "    Field hp: Int = 3\n"
                                      "    Method Hit(n: Int)\n"
                                      "    End Method\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return std::format("{}{}", base, text);
    };

    CHECK(probe(with_base("Class B Extends A\nEnd Class\nLocal hp = B().hp")) == "Int32");
    CHECK(errors_of(with_base("Class B Extends A\n"
                              "    Method Show()\n"
                              "        Hit(hp)\n"
                              "        Self.Hit(Self.hp)\n"
                              "    End Method\n"
                              "End Class\n"
                              "B().Hit(1)"))
              .empty());
    CHECK(errors_of(with_base("Class B Extends A\n    Field hp: Int\nEnd Class")) ==
          errors_t{ "hp: 'hp' is already declared in A" });

    CHECK(errors_of("Struct S\nEnd Struct\nStruct T Extends S\nEnd Struct") ==
          errors_t{ "S: only a Class can extend another; a Struct is a value" });
    CHECK(errors_of("Class B Extends Nope\nEnd Class") == errors_t{ "Nope: 'Nope' isn't a Class" });
    CHECK(errors_of("Class A Extends B\nEnd Class\nClass B Extends A\nEnd Class") ==
          errors_t{ "A: B can't extend itself, directly or through other Classes" });
    CHECK(errors_of("Final Class A\nEnd Class\nClass B Extends A\nEnd Class") ==
          errors_t{ "A: A is Final, so it can't be extended" });
}

TEST_CASE("overriding a Method takes Override", "[sema]") {
    constexpr std::string_view base = "Class A\n"
                                      "    Method F(): Int => 1\n"
                                      "    Final Method G()\n"
                                      "    End Method\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", base, text));
    };

    CHECK(with_base("Class B Extends A\n    Override Method F(): Int => 2\nEnd Class").empty());
    CHECK(with_base("Class B Extends A\n    Method F(): Int => 2\nEnd Class") ==
          errors_t{ "F: 'F' overrides A's; write 'Override Method F'" });
    CHECK(with_base("Class B Extends A\n    Override Method F(x: Int): Int => 2\nEnd Class") ==
          errors_t{ "F: 'F' has Override, but A has no Method like it" });
    CHECK(with_base("Class B Extends A\n    Override Method F(): Long => 2\nEnd Class") ==
          errors_t{ "F: 'F' must return Int32, like A's" });
    CHECK(with_base("Class B Extends A\n    Override Method G()\n    End Method\nEnd Class") ==
          errors_t{ "G: 'G' is Final in A, so it can't be overridden" });

    // Other parameters make an overload. Every Class's Destroy runs, so none overrides another.
    CHECK(with_base("Class B Extends A\n    Method F(x: Int): Int => x\nEnd Class").empty());
    CHECK(with_base("Class B Extends A\n"
                    "    Override Method Destroy()\n"
                    "    End Method\n"
                    "End Class") ==
          errors_t{ "Destroy: Destroy can't be Abstract, Final, or Override; every Class's "
                    "Destroy runs" });
}

TEST_CASE("Abstract and Final", "[sema]") {
    CHECK(errors_of("Abstract Class A\nEnd Class\nLocal a = A()") ==
          errors_t{ "A(): A is Abstract, so it can't be made; make a Class that extends it" });
    CHECK(errors_of("Class A\n    Abstract Method F()\nEnd Class") ==
          errors_t{ "F: 'F' is Abstract, so A must be Abstract too" });
    CHECK(errors_of("Abstract Class A\n"
                    "    Abstract Method F(): Int\n"
                    "End Class\n"
                    "Class B Extends A\n"
                    "End Class") ==
          errors_t{ "B: B must override A's Abstract Method 'F', or be Abstract" });
    CHECK(errors_of("Abstract Final Class A\nEnd Class") ==
          errors_t{ "A: 'A' can't be both Abstract and Final" });
    CHECK(errors_of("Final Struct S\n    Override Method F()\n    End Method\nEnd Struct") ==
          errors_t{ "S: a Struct can't be Abstract or Final; only a Class can",
                    "F: a Struct's Methods can't be Abstract, Final, or Override" });
    CHECK(errors_of("Final Function F()\nEnd Function") ==
          errors_t{ "F: Abstract, Final, and Override only go before a Class's Methods" });
}

TEST_CASE("Super calls the Methods and New of the Class extended", "[sema]") {
    constexpr std::string_view base = "Abstract Class A\n"
                                      "    Field hp: Int\n"
                                      "    New(hp: Int)\n"
                                      "        Self.hp = hp\n"
                                      "    End New\n"
                                      "    Method F(): Int => hp\n"
                                      "    Abstract Method G()\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", base, text));
    };

    CHECK(with_base("Class B Extends A\n"
                    "    New()\n"
                    "        Super.New(1)\n"
                    "    End New\n"
                    "    Override Method F(): Int => Super.F() + 1\n"
                    "    Override Method G()\n"
                    "    End Method\n"
                    "End Class")
              .empty());
    CHECK(with_base("Class B Extends A\n"
                    "    New()\n"
                    "        Super.New(1)\n"
                    "    End New\n"
                    "    Override Method G()\n"
                    "        Super.G()\n"
                    "    End Method\n"
                    "End Class") ==
          errors_t{ "Super.G(): 'G' is Abstract in A, so Super can't call it" });

    // A has no New without parameters, so B's New starts with Super.New, and Self waits for it.
    CHECK(with_base("Abstract Class B Extends A\nEnd Class") ==
          errors_t{ "B: B needs a New that starts with Super.New(...), since A has no New "
                    "without parameters" });
    CHECK(with_base("Abstract Class B Extends A\n"
                    "    New()\n"
                    "        Print(\"x\")\n"
                    "        Super.New(1)\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "New: New must start with Super.New(...), since A has no New without "
                    "parameters",
                    "Super.New(1): Super.New can only start a New" });
    CHECK(with_base("Abstract Class B Extends A\n"
                    "    New()\n"
                    "        Super.New(F())\n"
                    "    End New\n"
                    "End Class") == errors_t{ "F: Self can't be used before Super.New" });

    CHECK(errors_of("Class A\n    Method F()\n        Super.F()\n    End Method\nEnd Class") ==
          errors_t{ "Super: 'Super' needs a Class that extends another, in one of its Methods" });
    CHECK(with_base("Abstract Class B Extends A\n"
                    "    New()\n"
                    "        Super.New(1)\n"
                    "    End New\n"
                    "    Method H()\n"
                    "        Local x = Super\n"
                    "    End Method\n"
                    "End Class") ==
          errors_t{ "Super: 'Super' only comes before a call, like 'Super.Hurt(5)'" });
}

TEST_CASE("an object converts to the Classes its Class extends", "[sema]") {
    constexpr std::string_view family = "Class A\nEnd Class\n"
                                        "Class B Extends A\nEnd Class\n"
                                        "Class C Extends A\nEnd Class\n"
                                        "Class D\nEnd Class\n";
    const auto with_family = [&](const std::string_view text) {
        return std::format("{}{}", family, text);
    };

    CHECK(probe(with_family("Local a: A = B()\nLocal x: A? = a")) == "A? (from A)");
    CHECK(probe(with_family("Local x = True ? B() : C()")) == "A");
    CHECK(probe(with_family("Local a: A = B()\nLocal x = a Is B")) == "Bool");
    CHECK(probe(with_family("Local a: A = B()\nLocal x = a As B")) == "B");
    CHECK(probe(with_family("Local a: A = B()\nLocal x = a As? C")) == "C?");
    CHECK(probe(with_family("Local a: A? = Null\nLocal x = a As A")) == "A");

    CHECK(errors_of(with_family("Local x = B() Is D")) ==
          errors_t{ "B() Is D: B and D aren't related, so this is always False" });
    CHECK(errors_of(with_family("Local x = B() As C")) ==
          errors_t{ "B() As C: 'As' can't convert B to C" });
    CHECK(errors_of(with_family("Local x = B() As? D")) ==
          errors_t{ "B() As? D: B and D aren't related, so this is always Null" });
    CHECK(errors_of(with_family("Local a: A = B()\nLocal x = a As? B?")) ==
          errors_t{ "B?: 'As?' gives a B? already; write 'As? B'" });
    CHECK(errors_of("Local x = 1 Is Int") ==
          errors_t{ "1: 'Is' tests objects of Classes, not Int32" });
    CHECK(errors_of(with_family("Local b = B()\nLocal x = b Is A")) ==
          errors_t{ "b Is A: this is always True: 'b' is B already" });
}

TEST_CASE("a Select can match Classes", "[sema]") {
    constexpr std::string_view family = "Class A\nEnd Class\n"
                                        "Class B Extends A\nEnd Class\n"
                                        "Class D\nEnd Class\n"
                                        "Local a: A? = B()\n";
    const auto with_family = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", family, text));
    };

    CHECK(with_family("Select a\n"
                      "    Case b As B\n"
                      "        Local x: B = b\n"
                      "    Case other As A\n"
                      "End Select")
              .empty());
    CHECK(with_family("Select a\n    Case x As A\n    Case y As B\nEnd Select") ==
          errors_t{ "B: this Case is already covered by an earlier Case" });
    CHECK(with_family("Select a\n    Case x As D\nEnd Select") ==
          errors_t{ "D: A and D aren't related, so this Case never matches" });
    CHECK(with_family("Select a\n    Case x As B\n    Case 1\nEnd Select") ==
          errors_t{ "1: a Select's Cases are all values, or all type matches like 'Case e As "
                    "Boss'" });
}

// Arrays

TEST_CASE("an array literal takes the expected type, or its elements' common one", "[sema]") {
    CHECK(probe("Local a = [1, 2]") == "Int32[]");
    CHECK(probe("Local a = [1, 2.5]") == "Float32[]");
    CHECK(probe("Local big: Long = 5\nLocal a = [big, 1]") == "Int64[]");
    CHECK(probe("Local a: Long[] = [1, 2]") == "Int64[]");
    CHECK(probe("Local a: Int[3] = [1, 2, 3]") == "Int32[3]");
    CHECK(probe("Local a = [[1], [2, 3]]") == "Int32[][]");
    CHECK(probe("Local a: String[]? = []") == "String[]? (from String[])");

    CHECK(errors_of("Local a = []") ==
          errors_t{ "[]: an empty array needs a type from where it's used, like 'Local a: Int[] = "
                    "[]'" });
    CHECK(errors_of("Local a = [1, \"x\"]") ==
          errors_t{ "1: the elements of an array have different types, String and Int32" });
    CHECK(errors_of("Local a: Int[3] = [1, 2]") ==
          errors_t{ "[1, 2]: Int32[3] needs 3 elements, not 2" });
    CHECK(errors_of("Local a: Int[2, 2] = [1, 2, 3, 4]") ==
          errors_t{ "[1, 2, 3, 4]: Int32[2, 2] can't come from an array literal; it starts with "
                    "every element at zero" });

    // A T[] holds what it's made with, not what extends it: a B[] isn't an A[].
    constexpr std::string_view family = "Class A\nEnd Class\nClass B Extends A\nEnd Class\n";
    const auto with_family = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", family, text));
    };

    CHECK(with_family("Local a: A[] = [B()]").empty());
    CHECK(with_family("Local b = [B()]\nLocal a: A[] = b") ==
          errors_t{ "b: expected A[], found B[]" });
}

TEST_CASE("a fixed array's size is known at compile time", "[sema]") {
    CHECK(probe("Local g: Int[2, 3]\nLocal n = g.Count") == "Int32 6");
    CHECK(probe("Const N = 4\nFunction F()\n    Local a: Int[N * 2]\nEnd Function\nLocal x = 1") ==
          "Int32 1");
    CHECK(errors_of("Function F(n: Int)\n    Local a: Int[n]\nEnd Function") ==
          errors_t{ "n: a fixed array's size must be an integer known at compile time, like 64 "
                    "or 8 * 8" });
    CHECK(errors_of("Local a: Int[0]") == errors_t{ "0: a fixed array's size must be at least 1" });

    // It starts at zero, unless its elements have none.
    CHECK(errors_of("Local g: Float[4]\ng[1] = 2.5").empty());
    CHECK(errors_of("Class E\nEnd Class\nLocal e: E[2]") ==
          errors_t{ "e: 'e' needs a value, since E[2] has no zero value" });
    CHECK(errors_of("Struct S\n    Field kids: S[2]\nEnd Struct") ==
          errors_t{ "kids: 'S' can't contain itself; a Struct is stored inline" });
    CHECK(errors_of("Struct S\n    Field kids: S[] = []\nEnd Struct").empty());
}

TEST_CASE("indexing", "[sema]") {
    CHECK(probe("Local a = [1.5]\nLocal x = a[0]") == "Float32");
    CHECK(probe("Local g: Int[2, 3]\nLocal i: Byte = 1\nLocal x = g[1, i]") == "Int32");

    CHECK(errors_of("Local x = 1\nLocal y = x[0]") == errors_t{ "x: Int32 can't be indexed" });
    CHECK(errors_of("Local g: Int[2, 2]\nLocal y = g[0]") ==
          errors_t{ "g[0]: Int32[2, 2] takes 2 indices" });
    CHECK(errors_of("Local a = [1]\nLocal y = a[0.5]") ==
          errors_t{ "0.5: an index must be an integer, not Float32" });
    CHECK(errors_of("Local a = [1]\nLocal y = a[-1]") ==
          errors_t{ "-1: an index can't be negative" });
    CHECK(errors_of("Local g: Int[4]\nLocal y = g[4]") ==
          errors_t{ "4: index 4 is out of range for Int32[4]" });
    CHECK(errors_of("Function F(a: Int[]?): Int => a[0]") ==
          errors_t{ "a: 'a' may be Null; check that it isn't, or use '?.'" });

    // A T[]'s elements change through any reference to it; a fixed array is a value.
    CHECK(errors_of("Function F(a: Int[])\n    a[0] = 5\nEnd Function").empty());
    CHECK(errors_of("Function F(a: Int[2])\n    a[0] = 5\nEnd Function") ==
          errors_t{ "a[0]: 'a' is a parameter and can't be changed; copy it into a Local, or make "
                    "it ByRef" });
}

TEST_CASE("a T[]'s Count and Methods", "[sema]") {
    CHECK(probe("Local a = [1]\nLocal x = a.Count") == "Int32");
    CHECK(probe("Local a = [1]\nLocal x = a.Contains(1)") == "Bool");
    CHECK(probe("Local a = [1]\nLocal x = a.Clone()") == "Int32[]");
    CHECK(errors_of("Local a = [1]\na.Add(2)\na.Insert(0, 3)\na.RemoveAt(1)\na.Clear()").empty());

    CHECK(errors_of("Local a = [1]\na.Count = 3") ==
          errors_t{ "a.Count: an array's Count can't be changed; use Add, Insert, RemoveAt, or "
                    "Clear" });
    CHECK(errors_of("Local a = [1]\nLocal n = a.Count()") ==
          errors_t{ "Count: Count isn't a Method; leave out the ()" });
    CHECK(errors_of("Local a = [1]\nLocal f = a.Add") ==
          errors_t{ "Add: 'Add' is a method; call it with ()" });
    CHECK(errors_of("Local g: Int[3]\ng.Add(1)") ==
          errors_t{ "Add: Int32[3] has no member named 'Add'" });
    CHECK(errors_of("Local a = [1]\na.Add(\"x\")") ==
          errors_t{ "\"x\": expected Int32, found String" });
    CHECK(errors_of("Local a = [[1]]\nLocal b = a.Contains([1])") ==
          errors_t{ "a.Contains: Contains compares with ==, which Int32[] doesn't have" });
}

TEST_CASE("arrays have no ==, but RefEquals tells T[]s apart", "[sema]") {
    CHECK(errors_of("Local a = [1]\nLocal b = a == a") ==
          errors_t{ "a == a: '==' can't be used on Int32[]; RefEquals tells whether two are the "
                    "same array" });
    CHECK(errors_of("Local a: Int[2]\nLocal b = a == a") ==
          errors_t{ "a == a: '==' can't be used on Int32[2]" });
    CHECK(errors_of("Struct S\n"
                    "    Field v: Int[]\n"
                    "End Struct\n"
                    "Local s = S([1])\n"
                    "Local b = s == s") == errors_t{ "s == s: '==' can't be used on S" });
    CHECK(probe("Local a = [1]\nLocal b = RefEquals(a, a)") == "Bool");
}

TEST_CASE("For ... In goes over an array", "[sema]") {
    CHECK(errors_of("Local total = 0\n"
                    "For i, x In [1, 2]\n"
                    "    total += i + x\n"
                    "End For")
              .empty());
    CHECK(errors_of("For x In 5\nEnd For") ==
          errors_t{ "5: 'For ... In' goes over arrays, not Int32" });
    CHECK(errors_of("For x In [1, 2]\n    x = 3\nEnd For") ==
          errors_t{ "x: 'x' is a loop variable and can't be changed" });
    CHECK(errors_of("Function F(a: Int[]?)\n    For x In a\n    End For\nEnd Function") ==
          errors_t{ "a: 'a' may be Null; check that it isn't, or use '?.'" });
}

TEST_CASE("an array's element can't be changed ByRef where it could move", "[sema]") {
    constexpr std::string_view point = "Struct P\n"
                                       "    Field x: Int\n"
                                       "    ByRef Method Move()\n"
                                       "        x += 1\n"
                                       "    End Method\n"
                                       "End Struct\n";
    const auto with_point = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", point, text));
    };

    CHECK(errors_of("Function S(ByRef n: Int)\nEnd Function\nLocal a = [1]\nS(ByRef a[0])") ==
          errors_t{ "a[0]: an array's element can't be passed ByRef yet; copy it into a Local" });
    CHECK(with_point("Local a = [P(1)]\na[0].Move()") ==
          errors_t{ "a[0]: 'Move' is a ByRef Method, and an element of a T[] can move when the "
                    "array grows; change a Local copy, then store it back" });
    CHECK(with_point("Local g: P[2]\ng[1].Move()").empty());
}

TEST_CASE("a T[] has no zero, but [] can be a default", "[sema]") {
    CHECK(errors_of("Global g: Int[]") ==
          errors_t{ "g: 'g' needs a value, since Int32[] has no zero value" });
    CHECK(errors_of("Class C\n    Field a: Int[]\nEnd Class") ==
          errors_t{ "a: C needs a New that assigns 'a', since Int32[] has no zero value" });
    CHECK(errors_of("Class C\n    Field a: Int[] = []\nEnd Class\nLocal c = C()").empty());
    CHECK(errors_of("Struct S\n    Field a: Int[]\nEnd Struct\nLocal s = S()") ==
          errors_t{ "S(): missing an argument for parameter 'a'" });
    CHECK(probe("Function F(a: Int[] = []): Int => a.Count\nLocal n = F()") == "Int32");
}

// What codegen reads

TEST_CASE("the analysis records calls, conversions, and declarations", "[sema]") {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb",
                                 "Function F(a: Long, b: Int = 7): Long => a\n"
                                 "Global g = 1\n"
                                 "Local r = F(g)\n"
                                 "Loop\n"
                                 "    Break\n"
                                 "End Loop");
    fb::diagnostic_bag_t bag;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag);
    const fb::ast_t ast = fb::parse(file, tokens, bag);
    const fb::analysis_t analysis = fb::analyze(file, ast, bag);
    REQUIRE(bag.diagnostics().empty());
    REQUIRE(analysis.functions.size() == 1);
    REQUIRE(analysis.globals.size() == 1);
    const fb::symbol_id_t function = analysis.functions[0];
    CHECK(analysis.stmt(ast.items()[0]).symbol == function);
    CHECK(analysis.symbol(analysis.globals[0]).name == "g");

    const auto& local = std::get<fb::variable_stmt_t>(ast.get(ast.items()[2]).node);
    REQUIRE(local.value);
    const fb::expr_id_t call = local.value.value_or(fb::expr_id_t{});
    const fb::expr_id_t argument = std::get<fb::call_expr_t>(ast.get(call).node).arguments[0].value;
    CHECK(analysis.expr(call).symbol == function);
    CHECK(analysis.expr(call).type == fb::type_id_t::INT64);

    // The argument widens to Long, and b takes its default.
    CHECK(analysis.expr(call).arguments ==
          std::vector<std::optional<fb::expr_id_t>>{ argument, std::nullopt });
    CHECK(analysis.expr(argument).type == fb::type_id_t::INT32);
    CHECK(analysis.expr(argument).converted == fb::type_id_t::INT64);
    const fb::symbol_t& b = analysis.symbol(analysis.symbol(function).parameters[1]);
    CHECK(b.value == fb::make_number(7, false, fb::type_id_t::INT32));

    // The Break records the loop it leaves.
    const auto& loop = std::get<fb::loop_stmt_t>(ast.get(ast.items()[3]).node);
    CHECK(analysis.stmt(loop.body[0]).loop == ast.items()[3]);
}
