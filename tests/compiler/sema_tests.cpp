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
    analyzed.last = fb::type_name(info.converted);
    if (info.value) {
        analyzed.last += " " + fb::to_text(*info.value);
    }

    if (info.type != info.converted) {
        analyzed.last += std::format(" (from {})", fb::type_name(info.type));
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
    CHECK(call("Swap(ByRef 1, ByRef y)") == errors_t{ "1: only a variable can be passed ByRef" });
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

TEST_CASE("features after M1 are reported as not supported yet", "[sema]") {
    CHECK(errors_of("Local x = Null") == errors_t{ "Null: 'Null' isn't supported yet" });
    CHECK(errors_of("Local x: Int? = 1") ==
          errors_t{ "Int?: nullable types aren't supported yet" });
    CHECK(errors_of("Local x: Int[]") == errors_t{ "Int[]: arrays aren't supported yet" });
    CHECK(errors_of("Local a = 1\nLocal x = a.b") ==
          errors_t{ "a.b: member access isn't supported yet" });
    CHECK(errors_of("Local a = 1\nLocal x = a[0]") ==
          errors_t{ "a[0]: indexing isn't supported yet" });

    // The message is split in two so the compiler doesn't see a trigraph in it.
    CHECK(errors_of("Local a = 1\nLocal x = a ?? 2") == errors_t{ "a ?? 2: '?"
                                                                  "?' isn't supported yet" });
    CHECK(errors_of("Local a = 1\nLocal x = a Is Int") ==
          errors_t{ "a Is Int: 'Is' isn't supported yet" });
    CHECK(errors_of("Local a = 1\nLocal x = a As? Int") ==
          errors_t{ "a As? Int: 'As?' isn't supported yet" });
    CHECK(errors_of("Local a = 1\nFor e In a\nEnd For") ==
          errors_t{ "For e In a: 'For ... In' isn't supported yet" });
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
