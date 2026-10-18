#include "fb/const_value.hpp"
#include "fb/driver.hpp"
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

/// Lexes, parses, and analyzes `text`, with the standard library. `last` is only filled in
/// when the last line declares a variable with a value.
analyzed_t analyze_text(const std::string_view text) {
    const fb::compilation_t compilation = fb::compile("test.fb", std::string(text));
    const fb::ast_t& ast = compilation.ast;
    const fb::analysis_t& analysis = compilation.analysis;

    analyzed_t analyzed;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        const fb::source_file_t& file = compilation.sources.get(diagnostic.span.file);
        analyzed.errors.push_back(
            std::format("{}: {}", file.slice(diagnostic.span), diagnostic.message));
    }

    // The main file's items come last.
    if (ast.items().empty() || ast.from_library(ast.items().back())) {
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
    CHECK(probe("Local x = \"a\" < \"b\"") == "Bool True");
    CHECK(probe("Local x = \"b\" <= \"a\"") == "Bool False");
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
    CHECK(errors_of("Local x = Print") ==
          errors_t{ "Print: 'Print' is built in, so it can't be a value; use a lambda that calls "
                    "it" });
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
          errors_t{ "s: Select works on integers, Bool, String, and Enums, not Float32" });
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
    CHECK(errors_of("Local a = 1\nLocal b = a.x") ==
          errors_t{ "x: Int32 has no member named 'x'" });
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
          errors_t{ "p: P can't be shown in a string; implement Printable, with a ToString" });
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
          errors_t{ "Print(\"x\"): a Struct can only hold Fields, Methods, Properties, and News" });
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

TEST_CASE("a Class without a New takes each New of the Class it extends", "[sema]") {
    constexpr std::string_view base = "Class A\n"
                                      "    New(x: Int)\n"
                                      "    End New\n"
                                      "    New(s: String, n: Int = 1)\n"
                                      "    End New\n"
                                      "End Class\n"
                                      "Class B Extends A\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", base, text));
    };

    CHECK(with_base("Local a = B(1)\nLocal b = B(\"x\")\nLocal c = B(\"x\", 2)").empty());
    CHECK(with_base("Local a = B()") == errors_t{ "B: no overload of 'B' takes ()" });
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
    CHECK(errors_of(with_a("Print(\"{A()}\")")) ==
          errors_t{ "A(): A can't be shown in a string; implement Printable, with a ToString" });

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

    // Without a New of its own, B takes A's parameters. A has no New without parameters, so a
    // New of B's starts with Super.New, and Self waits for it.
    CHECK(with_base("Abstract Class B Extends A\n"
                    "End Class\n"
                    "Class C Extends B\n"
                    "    Override Method G()\n"
                    "    End Method\n"
                    "End Class\n"
                    "Local c = C(3)")
              .empty());
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
    CHECK(errors_of("Local a = [Function() => 1]\nLocal b = a.Contains(a[0])") ==
          errors_t{ "a.Contains: Contains compares with ==, which Func(): Int32 doesn't have" });
}

TEST_CASE("arrays compare their elements with ==, and RefEquals tells T[]s apart", "[sema]") {
    CHECK(probe("Local a = [1]\nLocal b = a == a") == "Bool");
    CHECK(probe("Local a: Int[2]\nLocal b = a == a") == "Bool");
    CHECK(probe("Struct S\n"
                "    Field v: Int[]\n"
                "End Struct\n"
                "Local s = S([1])\n"
                "Local b = s == s") == "Bool");
    CHECK(errors_of("Local a = [Function() => 1]\nLocal b = a == a") ==
          errors_t{ "a == a: '==' can't be used on (Func(): Int32)[]; RefEquals tells whether two "
                    "are the same array" });
    CHECK(probe("Local a = [1]\nLocal b = RefEquals(a, a)") == "Bool");
}

TEST_CASE("For ... In goes over an array", "[sema]") {
    CHECK(errors_of("Local total = 0\n"
                    "For i, x In [1, 2]\n"
                    "    total += i + x\n"
                    "End For")
              .empty());
    CHECK(errors_of("For x In 5\nEnd For") ==
          errors_t{ "5: 'For ... In' goes over arrays, Iterators, and values whose Iterate() "
                    "gives an Iterator, not Int32" });
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

// Enums

TEST_CASE("an Enum's values count up, or take the next bit for @Flags", "[sema]") {
    constexpr std::string_view state = "Enum State: UInt8\n"
                                       "    Idle, Walk, Attack = 10, Dead\n"
                                       "End Enum\n";
    constexpr std::string_view layer =
        "@Flags Enum Layer\n"
        "    None = 0, World, Player, Trigger, Solid = World | Player\n"
        "End Enum\n";
    const auto with = [](const std::string_view declaration, const std::string_view text) {
        return std::format("{}{}", declaration, text);
    };

    CHECK(probe(with(state, "Local x = State.Walk As Int")) == "Int32 1");
    CHECK(probe(with(state, "Local x = State.Dead As Int")) == "Int32 11");
    CHECK(probe(with(state, "Local x = State.Dead As Byte")) == "UInt8 11");
    CHECK(probe(with(layer, "Local x = Layer.Trigger As Int")) == "Int32 4");
    CHECK(probe(with(layer, "Local x = Layer.Solid As Int")) == "Int32 3");
    CHECK(probe(with(layer, "Local x = (Layer.World | Layer.Trigger) As Int")) == "Int32 5");
    CHECK(probe("Enum D\n    North = -1, East\nEnd Enum\nLocal x = D.East As Int") == "Int32 0");

    CHECK(errors_of("Enum E: Float\n    A\nEnd Enum") ==
          errors_t{ "Float: an Enum's type must be an integer, not Float32" });
    CHECK(errors_of("Enum E\nEnd Enum") == errors_t{ "E: an Enum needs at least one member" });
    CHECK(errors_of("Enum E\n    A, B, A\nEnd Enum") ==
          errors_t{ "A: 'A' is already a member of E" });
    CHECK(errors_of("Enum E\n    A = 1, B = 0, C\nEnd Enum") ==
          errors_t{ "C: 'C' has the same value as 'A'" });
    CHECK(errors_of("Enum E: UInt8\n    A = 255, B\nEnd Enum") ==
          errors_t{ "B: 'B' doesn't fit in UInt8; give it a value" });
    CHECK(errors_of("@Flags Enum E: UInt8\n    A = 128, B\nEnd Enum") ==
          errors_t{ "B: 'B' doesn't fit in UInt8; give it a value" });
    CHECK(errors_of("Global g = 1\nEnum E\n    A = g\nEnd Enum") ==
          errors_t{ "g: an Enum's value must be known at compile time" });
    CHECK(errors_of("Function F()\n    Enum E\n        A\n    End Enum\nEnd Function") ==
          errors_t{ "E: Enums can only be declared at the top level" });
}

TEST_CASE("an Enum's values compare, and @Flags ones combine", "[sema]") {
    constexpr std::string_view state = "Enum State\n    Idle, Walk\nEnd Enum\n";
    constexpr std::string_view layer = "@Flags Enum Layer\n    World, Player\nEnd Enum\n";
    const auto with = [](const std::string_view declaration, const std::string_view text) {
        return std::format("{}{}", declaration, text);
    };

    CHECK(probe(with(state, "Local x = State.Idle == State.Walk")) == "Bool False");
    constexpr std::string_view both = "Const both = Layer.World | Layer.Player\n"
                                      "Local x = both == ~(~Layer.World & ~Layer.Player)";
    CHECK(probe(with(layer, both)) == "Bool True");
    CHECK(probe(with(layer, "Local l = Layer.World\nLocal x = l.Has(Layer.Player)")) == "Bool");

    CHECK(errors_of(with(state, "Local s = State.Run")) ==
          errors_t{ "Run: State has no member named 'Run'" });
    CHECK(errors_of(with(state, "Local s = State.Idle | State.Walk")) ==
          errors_t{ "State.Idle | State.Walk: '|' can't be used on State; only @Flags Enums "
                    "combine" });
    CHECK(errors_of(with(state, "Local b = State.Idle < State.Walk")) ==
          errors_t{ "State.Idle < State.Walk: '<' can't be used on State" });
    CHECK(errors_of(with(state, "Local b = State.Idle == 0")) ==
          errors_t{ "State.Idle == 0: '==' can't combine State and Int32" });
    CHECK(errors_of(with(state, "Local s: State = 1")) ==
          errors_t{ "1: expected State, found Int32" });
    CHECK(errors_of(with(layer, "Local l = Layer.World\nLocal h = l.Has")) ==
          errors_t{ "Has: 'Has' is a method; call it with ()" });
}

TEST_CASE("As turns an Enum to and from integers", "[sema]") {
    constexpr std::string_view state = "Enum State\n    Idle, Walk\nEnd Enum\n";
    constexpr std::string_view layer = "@Flags Enum Layer\n    World, Player\nEnd Enum\n";
    const auto with = [](const std::string_view declaration, const std::string_view text) {
        return std::format("{}{}", declaration, text);
    };

    CHECK(probe(with(state, "Local x = 1 As State")) == "State 1");
    CHECK(probe(with(layer, "Local x = 7 As Layer")) == "Layer 7");
    CHECK(probe(with(state, "Local n: Long = 1\nLocal x = n As State")) == "State");
    CHECK(errors_of(with(state, "Local x = 7 As State")) ==
          errors_t{ "7 As State: 7 isn't one of State's values" });
    CHECK(errors_of(with(state, "Local f = State.Idle As Float")) ==
          errors_t{ "State.Idle As Float: 'As' can't convert State to Float32" });
}

TEST_CASE("a Select on an Enum covers every value, or has a Default", "[sema]") {
    constexpr std::string_view state = "Enum State\n    Idle, Walk, Dead\nEnd Enum\n";
    const auto with_state = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", state, text));
    };

    // Covering every value is like having a Default: no path goes past the Select.
    CHECK(with_state("Function F(s: State): Int\n"
                     "    Select s\n"
                     "        Case State.Idle, State.Walk\n"
                     "            Return 1\n"
                     "        Case State.Dead\n"
                     "            Return 2\n"
                     "    End Select\n"
                     "End Function")
              .empty());
    CHECK(with_state("Local s = State.Idle\nSelect s\n    Case State.Idle\nEnd Select") ==
          errors_t{ "s: Select doesn't cover State.Walk, State.Dead; add a Case for each, or a "
                    "Default" });
    CHECK(errors_of("@Flags Enum Layer\n"
                    "    World, Player\n"
                    "End Enum\n"
                    "Local l = Layer.World\n"
                    "Select l\n"
                    "    Case Layer.World\n"
                    "End Select") ==
          errors_t{ "l: a Select on Layer, a @Flags Enum, needs a Default, since its values "
                    "combine" });
    CHECK(with_state("Local s = State.Idle\n"
                     "Select s\n"
                     "    Case State.Idle To State.Dead\n"
                     "End Select") ==
          errors_t{ "State.Idle To State.Dead: 'To' ranges only work on integers" });
}

TEST_CASE("an Enum's zero is its first value, or no flags", "[sema]") {
    CHECK(errors_of("Enum State\n"
                    "    Idle, Walk\n"
                    "End Enum\n"
                    "Struct S\n"
                    "    Field s: State\n"
                    "End Struct\n"
                    "Global g: State\n"
                    "Local s = S()")
              .empty());

    // A string shows a member's name, which isn't known until lowering looks it up.
    CHECK(probe("Enum State\n    Idle, Walk\nEnd Enum\nLocal t = \"{State.Walk}\"") == "String");
}

// Defer

TEST_CASE("Defer runs one statement that can't leave the block", "[sema]") {
    constexpr std::string_view message = "Defer runs one call, assignment, ++ or --, or one-line "
                                         "If, which can't leave the block";
    const auto rejected = [&](const std::string_view statement) {
        return errors_t{ std::format("{}: {}", statement, message) };
    };

    CHECK(errors_of("Local n = 0\n"
                    "Defer Print(\"{n}\")\n"
                    "Defer n += 1\n"
                    "Defer n++\n"
                    "Defer If n > 1 Then Print(\"big\") Else n = 0")
              .empty());
    CHECK(errors_of("Function F(): Int\n    Defer Return 1\n    Return 0\nEnd Function") ==
          rejected("Defer Return 1"));
    CHECK(errors_of("For i = 1 To 2\n    Defer If i == 2 Then Break\nEnd For") ==
          rejected("Defer If i == 2 Then Break"));

    // A Local in it would be declared where its value isn't, so it isn't checked any further.
    CHECK(errors_of("Defer Local x = 1") == rejected("Defer Local x = 1"));
    CHECK(errors_of("Defer If True\n    Print(\"a\")\nEnd If") ==
          rejected("Defer If True\n    Print(\"a\")\nEnd If"));
    CHECK(errors_of("Defer Defer Print(\"a\")") == rejected("Defer Defer Print(\"a\")"));
}

TEST_CASE("a Defer's statement runs as its block exits", "[sema]") {
    constexpr std::string_view enemy = "Class E\n    Field hp: Int\nEnd Class\n";
    const auto with_enemy = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", enemy, text));
    };

    // A check before it lasts unless the rest of the block changes the variable.
    CHECK(with_enemy("Local x: E? = E()\nDefer Print(\"{x.hp}\")").empty());
    CHECK(with_enemy("Local x: E? = E()\nDefer Print(\"{x.hp}\")\nx = Null") ==
          errors_t{ "x: 'x' may be Null; check that it isn't, or use '?.'" });
    CHECK(with_enemy("Function F(ok: Bool)\n"
                     "    Local x: E? = E()\n"
                     "    If ok\n"
                     "        Defer Print(\"{x.hp}\")\n"
                     "    End If\n"
                     "    x = Null\n"
                     "End Function")
              .empty());

    // What it assigns isn't assigned until then, and what it reads must already be.
    CHECK(errors_of("Local n: Int\nDefer n = 1\nPrint(\"{n}\")") ==
          errors_t{ "n: 'n' is read before it's assigned" });
    CHECK(errors_of("Local n: Int\nDefer Print(\"{n}\")\nn = 1") ==
          errors_t{ "n: 'n' is read before it's assigned" });
}

// Errors

/// Two errors, and a function that can throw either.
constexpr std::string_view two_errors = "Class FileError Extends Error\n"
                                        "End Class\n"
                                        "Class ParseError Extends Error\n"
                                        "End Class\n"
                                        "Function Load(): Int Throws FileError, ParseError => 1\n";

namespace {

errors_t with_errors(const std::string_view text) {
    return errors_of(std::format("{}{}", two_errors, text));
}

} // namespace

TEST_CASE("Error is a Class with a Message, from the standard library", "[sema]") {
    CHECK(probe("Local e = Error(\"x\")") == "Error");
    CHECK(probe("Local m = Error().Message") == "String");
    CHECK(probe("Class Oops Extends Error\nEnd Class\nLocal e: Error = Oops(\"x\")") ==
          "Error (from Oops)");
    CHECK(errors_of("Class Error\nEnd Class") == errors_t{ "Error: 'Error' is already declared" });
}

TEST_CASE("Throws lists the Classes a function's errors extend", "[sema]") {
    CHECK(with_errors("Class Plain\n"
                      "End Class\n"
                      "Function F() Throws Plain, Int\n"
                      "End Function") ==
          errors_t{ "Plain: Throws lists Classes that extend Error, not Plain",
                    "Int: Throws lists Classes that extend Error, not Int32" });
    CHECK(with_errors("Function F() Throws FileError, Error, FileError\nEnd Function") ==
          errors_t{ "FileError: FileError is already covered by this Throws",
                    "FileError: FileError is already covered by this Throws" });

    // Destroy can't throw, and an override throws only what the Method it overrides can.
    CHECK(
        with_errors("Class A\n    Method Destroy() Throws FileError\n    End Method\nEnd Class") ==
        errors_t{ "Destroy: Destroy can't throw, since it runs by itself" });
    CHECK(with_errors("Class A\n"
                      "    Method M() Throws FileError\n"
                      "    End Method\n"
                      "End Class\n"
                      "Class B Extends A\n"
                      "    Override Method M() Throws Error\n"
                      "    End Method\n"
                      "End Class") == errors_t{ "M: 'M' can't throw Error, since A's doesn't" });
    CHECK(with_errors("Class A\n"
                      "    Method M() Throws Error\n"
                      "    End Method\n"
                      "End Class\n"
                      "Class B Extends A\n"
                      "    Override Method M() Throws FileError\n"
                      "    End Method\n"
                      "End Class\n"
                      "Class C Extends A\n"
                      "    Override Method M()\n"
                      "    End Method\n"
                      "End Class")
              .empty());
}

TEST_CASE("a call that throws needs Try, Try?, or a Try block", "[sema]") {
    CHECK(with_errors("Function F(): Int\n    Return Load()\nEnd Function") ==
          errors_t{ "Load(): 'Load' can throw FileError, ParseError, so calling it needs Try, "
                    "Try?, or a Try block" });

    // Try lets the errors out, and the function's Throws has to cover them.
    CHECK(with_errors("Function F(): Int Throws FileError\n    Return Try Load()\nEnd Function") ==
          errors_t{ "Load(): ParseError isn't caught; catch it in a Try block, or add it to the "
                    "Throws of 'F'" });
    CHECK(with_errors("Function F(): Int Throws Error\n    Return Try Load() + 1\nEnd Function")
              .empty());

    // Try? catches them all.
    CHECK(with_errors("Function F(): Int\n    Return Try? Load() ?? 0\nEnd Function").empty());
    CHECK(probe(std::format("{}Local n = Try? Load()", two_errors)) == "Int32?");
    CHECK(errors_of("Local n = Try? 1") == errors_t{ "Try? 1: '1' doesn't throw, so it doesn't "
                                                     "need Try?" });

    // Top-level code can let any error out, which ends the program.
    CHECK(with_errors("Local n = Try Load()").empty());
}

TEST_CASE("Throw ends the path with an Error", "[sema]") {
    CHECK(with_errors("Function F(): Int Throws FileError\n"
                      "    Throw FileError(\"x\")\n"
                      "End Function")
              .empty());
    CHECK(with_errors("Function F()\n    Throw ParseError(\"x\")\nEnd Function") ==
          errors_t{ "ParseError(\"x\"): ParseError isn't caught; catch it in a Try block, or add "
                    "it to the Throws of 'F'" });
    CHECK(errors_of("Class Plain\nEnd Class\nThrow Plain()") ==
          errors_t{ "Plain(): Throw takes an object of a Class that extends Error, not Plain" });
    CHECK(errors_of("Function F(e: Error?) Throws Error\n    Throw e\nEnd Function") ==
          errors_t{ "e: 'e' may be Null; check that it isn't, or use '?.'" });
}

TEST_CASE("a Catch takes the errors of its Class, in order", "[sema]") {
    CHECK(with_errors("Function F() Throws ParseError\n"
                      "    Try\n"
                      "        Local n = Load()\n"
                      "    Catch e As FileError\n"
                      "        Print(e.Message)\n"
                      "    End Try\n"
                      "End Function")
              .empty());
    CHECK(with_errors("Try\n    Load()\nCatch e\nCatch f As FileError\nEnd Try") ==
          errors_t{ "FileError: this Catch is already covered by an earlier Catch" });
    CHECK(with_errors("Try\n    Print(\"x\")\nCatch e As FileError\nEnd Try") ==
          errors_t{ "FileError: nothing in the Try block throws FileError" });
    CHECK(with_errors("Try\n    Load()\nCatch e As String\nEnd Try") ==
          errors_t{ "String: Catch takes a Class that extends Error, not String" });

    // A Catch of a Class that extends one thrown may match.
    CHECK(errors_of("Class Oops Extends Error\n"
                    "End Class\n"
                    "Function F() Throws Error\n"
                    "End Function\n"
                    "Try\n"
                    "    F()\n"
                    "Catch e As Oops\n"
                    "End Try")
              .empty());
}

TEST_CASE("a Catch and a Finally start from what was true before the Try", "[sema]") {
    // The error can come from anywhere in the body, so what it assigns may not be assigned.
    CHECK(with_errors("Local n: Int\nTry\n    n = Load()\nCatch e\nEnd Try\nPrint(\"{n}\")") ==
          errors_t{ "n: 'n' is read before it's assigned" });
    CHECK(with_errors("Local n: Int\nTry\n    n = Load()\nCatch e\n    n = 0\nEnd Try\n"
                      "Print(\"{n}\")")
              .empty());
    CHECK(with_errors("Local n: Int\nTry\n    Load()\nCatch e\nFinally\n    n = 1\nEnd Try\n"
                      "Print(\"{n}\")")
              .empty());
    CHECK(with_errors("Local n: Int\nTry\n    n = Load()\nCatch e\nFinally\n    Print(\"{n}\")\n"
                      "End Try") == errors_t{ "n: 'n' is read before it's assigned" });

    // A check before the Try doesn't hold in a Catch when the body changes the variable.
    CHECK(with_errors("Function F(s: String?)\n"
                      "    Local t = s\n"
                      "    If t == Null Then Return\n"
                      "    Try\n"
                      "        t = Null\n"
                      "        Load()\n"
                      "    Catch e\n"
                      "        Print(t)\n"
                      "    End Try\n"
                      "End Function") ==
          errors_t{ "t: expected String, found String?, which may be Null; check it first, or "
                    "give a value for Null with '?"
                    "?'" });
}

TEST_CASE("a Defer and a Finally can't let errors out, or leave", "[sema]") {
    CHECK(with_errors("Defer Try Load()") ==
          errors_t{ "Load(): a Defer can't throw, so FileError has to be caught in it",
                    "Load(): a Defer can't throw, so ParseError has to be caught in it" });
    CHECK(with_errors("Defer Try? Load()").empty());
    CHECK(with_errors("Try\n    Print(\"x\")\nFinally\n    Load()\nEnd Try") ==
          errors_t{ "Load(): 'Load' can throw FileError, ParseError, so calling it needs Try, "
                    "Try?, or a Try block" });
    CHECK(with_errors("Try\n"
                      "    Print(\"x\")\n"
                      "Finally\n"
                      "    Try\n"
                      "        Load()\n"
                      "    Catch e\n"
                      "    End Try\n"
                      "End Try")
              .empty());

    // A Finally runs on the way out, so it can't go somewhere else.
    CHECK(errors_of("Function F(): Int\n"
                    "    Try\n"
                    "        Return 1\n"
                    "    Finally\n"
                    "        Return 2\n"
                    "    End Try\n"
                    "End Function") == errors_t{ "Return 2: Return can't leave a Finally" });
    CHECK(errors_of("For i = 1 To 2\n"
                    "    Try\n"
                    "        Print(\"x\")\n"
                    "    Finally\n"
                    "        Break\n"
                    "    End Try\n"
                    "End For") == errors_t{ "Break: Break can't leave a Finally" });
    CHECK(errors_of("Try\n"
                    "    Print(\"x\")\n"
                    "Finally\n"
                    "    For i = 1 To 2\n"
                    "        Continue\n"
                    "    End For\n"
                    "End Try")
              .empty());
}

TEST_CASE("a New can throw, and its callers handle it", "[sema]") {
    constexpr std::string_view base = "Class Bad Extends Error\n"
                                      "End Class\n"
                                      "Class A\n"
                                      "    New() Throws Bad\n"
                                      "    End New\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", base, text));
    };

    CHECK(with_base("Local a = A()") ==
          errors_t{ "A(): 'A' can throw Bad, so calling it needs Try, Try?, or a Try block" });
    CHECK(probe(std::format("{}Local a = Try? A()", base)) == "A?");

    // The Class extending it calls it with Try, or takes it, Throws and all.
    CHECK(with_base("Class B Extends A\n"
                    "    New() Throws Bad\n"
                    "        Try Super.New()\n"
                    "    End New\n"
                    "End Class\n"
                    "Class C Extends A\n"
                    "End Class\n"
                    "Local b = Try? B()\n"
                    "Local c = Try? C()")
              .empty());
    CHECK(with_base("Class B Extends A\n    New()\n        Print(\"x\")\n    End New\nEnd Class") ==
          errors_t{ "New: A's New can throw, so this New has to start with 'Try Super.New()'" });
    CHECK(with_base("Class C Extends A\nEnd Class\nLocal c = C()") ==
          errors_t{ "C(): 'C' can throw Bad, so calling it needs Try, Try?, or a Try block" });
}

// Weak references

TEST_CASE("a Weak<T> refers to an object of a Class", "[sema]") {
    constexpr std::string_view node = "Class Node\n    Field name: String\nEnd Class\n";
    const auto with_node = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", node, text));
    };

    CHECK(with_node("Local a: Weak<Int>\nLocal b: Weak<Node?>") ==
          errors_t{ "Weak<Int>: Weak<T> refers to an object of a Class, not Int32",
                    "Weak<Node?>: Weak<T> refers to an object of a Class, not Node?" });
    CHECK(with_node("Local c: Weak<Node>?") ==
          errors_t{ "Weak<Node>?: Weak<Node> already reads as Null once its object is gone" });

    // It has a zero, which refers to nothing, so a field of one needs no New.
    CHECK(
        with_node("Class Tree\n    Field parent: Weak<Node>\nEnd Class\nLocal t = Tree()").empty());
}

TEST_CASE("a Weak<T> is assigned a T, and reading it gives a T?", "[sema]") {
    constexpr std::string_view node = "Class Node\n"
                                      "    Field name: String\n"
                                      "End Class\n"
                                      "Class Leaf Extends Node\n"
                                      "End Class\n";
    const auto with_node = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", node, text));
    };

    CHECK(probe(std::format("{}Local w: Weak<Node> = Node()\nLocal n = w", node)) == "Node?");
    CHECK(with_node("Local n: Node? = Null\n"
                    "Local w: Weak<Node> = Leaf()\n"
                    "w = n\n"
                    "w = Null\n"
                    "Print(w?.name ?? \"gone\")")
              .empty());
    CHECK(with_node("Local w: Weak<Node> = \"x\"") ==
          errors_t{ "\"x\": expected Weak<Node>, found String" });

    // Its object can go at any time, so a check doesn't narrow it.
    CHECK(with_node("Local w: Weak<Node> = Node()\nIf w != Null Then Print(w.name)") ==
          errors_t{ "w: 'w' may be Null, since its object can go at any time; read it into a "
                    "Local and check that, or use '?.'" });

    // A Struct holding one compares its fields, which would compare the references.
    CHECK(with_node("Struct S\n    Field w: Weak<Node>\nEnd Struct\nLocal x = S() == S()") ==
          errors_t{ "S() == S(): '==' can't be used on S" });
}

// Class extras

/// A Class with a Static Field, a Static Method, a ReadOnly Field, and a Property.
constexpr std::string_view enemy_class = "Class Enemy\n"
                                         "    Static Field count: Int = 0\n"
                                         "    Static ReadOnly Field limit: Int = 10\n"
                                         "    ReadOnly Field id: Int\n"
                                         "    Field hp: Int\n"
                                         "    New()\n"
                                         "        count++\n"
                                         "        id = count\n"
                                         "    End New\n"
                                         "    Static Method Make(): Enemy => Enemy()\n"
                                         "    Property Alive: Bool\n"
                                         "        Get => hp > 0\n"
                                         "    End Property\n"
                                         "End Class\n";

namespace {

errors_t with_enemy_class(const std::string_view text) {
    return errors_of(std::format("{}{}", enemy_class, text));
}

} // namespace

TEST_CASE("Static members belong to their type", "[sema]") {
    CHECK(probe(std::format("{}Local n = Enemy.count", enemy_class)) == "Int32");
    CHECK(probe(std::format("{}Local e = Enemy.Make()", enemy_class)) == "Enemy");
    CHECK(with_enemy_class("Enemy.count = 3\nEnemy.count++").empty());

    // They're used through the type, and the others through a value.
    CHECK(with_enemy_class("Local e = Enemy()\nLocal n = e.count") ==
          errors_t{ "count: 'count' is Static; use it on Enemy" });
    CHECK(with_enemy_class("Local e = Enemy()\ne.Make()") ==
          errors_t{ "Make: 'Make' is Static; call it on Enemy" });
    CHECK(with_enemy_class("Local n = Enemy.hp") ==
          errors_t{ "hp: 'hp' belongs to each Enemy, so it's used on one, not on Enemy" });
    CHECK(with_enemy_class("Enemy.limit = 3") ==
          errors_t{ "Enemy.limit: 'limit' is ReadOnly and Static, so it keeps its first value" });
}

TEST_CASE("a Static Method has no Self", "[sema]") {
    const auto in_static = [](const std::string_view body) {
        return errors_of(std::format("Class A\n"
                                     "    Static Field n: Int\n"
                                     "    Field x: Int\n"
                                     "    Method M()\n"
                                     "    End Method\n"
                                     "    Static Method S()\n"
                                     "{}\n"
                                     "    End Method\n"
                                     "End Class",
                                     body));
    };

    CHECK(in_static("        n = 1").empty());
    CHECK(in_static("        Print(\"{x}\")") ==
          errors_t{ "x: 'x' belongs to each A, and a Static Method has no Self" });
    CHECK(in_static("        M()") ==
          errors_t{ "M: 'M' belongs to each A, and a Static Method has no Self" });
    CHECK(in_static("        Local a = Self") == errors_t{ "Self: a Static Method has no Self" });

    // It isn't called through an object, so it can't be virtual, and New belongs to an object.
    CHECK(errors_of("Class A\n    Static Final Method M()\n    End Method\nEnd Class") ==
          errors_t{ "M: a Static Method can't be Abstract, Final, or Override; it isn't called "
                    "through an object" });
    CHECK(errors_of("Class A\n    Static New()\n    End New\nEnd Class") ==
          errors_t{ "New: New can't be Static; a Static Method can make objects" });
}

TEST_CASE("Static and ReadOnly only go before a type's Fields and Methods", "[sema]") {
    CHECK(errors_of("ReadOnly Global g = 1") ==
          errors_t{ "g: a Global that never changes is a Const" });
    CHECK(errors_of("Static Function F()\nEnd Function") ==
          errors_t{ "F: Static only goes before a Field or Method of a Struct or Class" });
    CHECK(errors_of("Class A\n    Static ReadOnly Field n: Int\nEnd Class") ==
          errors_t{ "n: 'n' is ReadOnly and Static, so it needs a value" });
    CHECK(errors_of("Final Local x = 1") ==
          errors_t{ "x: Abstract, Final, and Override only go before a Class's Methods" });
}

TEST_CASE("a ReadOnly Field only changes in its Class's New", "[sema]") {
    CHECK(with_enemy_class("Local e = Enemy()\ne.id = 5") ==
          errors_t{ "e.id: 'id' is ReadOnly; only Enemy's New can change it" });
    CHECK(errors_of("Class A\n"
                    "    ReadOnly Field id: Int\n"
                    "    Method M()\n"
                    "        id = 3\n"
                    "    End Method\n"
                    "End Class") == errors_t{ "id: 'id' is ReadOnly; only A's New can change it" });

    // Its parts can't change either, and it can't be passed ByRef.
    CHECK(errors_of("Struct P\n"
                    "    Field x: Int\n"
                    "End Struct\n"
                    "Class A\n"
                    "    ReadOnly Field where: P\n"
                    "    New()\n"
                    "        where.x = 1\n"
                    "    End New\n"
                    "    Method M()\n"
                    "        where.x = 2\n"
                    "    End Method\n"
                    "End Class") ==
          errors_t{ "where: 'where' is ReadOnly; only A's New can change it" });
    CHECK(with_enemy_class("Function Bump(ByRef n: Int)\n"
                           "    n++\n"
                           "End Function\n"
                           "Local e = Enemy()\n"
                           "Bump(ByRef e.id)") ==
          errors_t{ "e.id: 'id' is ReadOnly; only Enemy's New can change it" });
}

TEST_CASE("a Property is read through its Get and assigned through its Set", "[sema]") {
    constexpr std::string_view counter = "Class Counter\n"
                                         "    Field n: Int\n"
                                         "    Property Count: Int\n"
                                         "        Get => n\n"
                                         "        Set(value)\n"
                                         "            n = value\n"
                                         "        End Set\n"
                                         "    End Property\n"
                                         "End Class\n";
    const auto with_counter = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", counter, text));
    };

    CHECK(probe(std::format("{}Local c = Counter()\nLocal n = c.Count", counter)) == "Int32");
    CHECK(with_counter("Local c = Counter()\nc.Count = 1\nc.Count += 2\nc.Count++").empty());
    CHECK(with_enemy_class("Local e = Enemy()\ne.Alive = False") ==
          errors_t{ "e.Alive: 'Alive' has no Set, so it can't be assigned" });
    CHECK(with_counter("Function Bump(ByRef n: Int)\n"
                       "    n++\n"
                       "End Function\n"
                       "Local c = Counter()\n"
                       "Bump(ByRef c.Count)") ==
          errors_t{ "c.Count: 'Count' is a Property, which has no variable to pass ByRef" });

    // A Struct's Property gives a copy, so changing part of it does nothing.
    CHECK(errors_of("Struct P\n"
                    "    Field x: Int\n"
                    "    Property Twice: P\n"
                    "        Get => P(x * 2)\n"
                    "    End Property\n"
                    "End Struct\n"
                    "Local p = P(1)\n"
                    "p.Twice.x = 3") ==
          errors_t{ "p.Twice: 'Twice' is a Property, which gives a copy; set the whole of it "
                    "instead" });
}

TEST_CASE("a Property overrides the one a Class inherits, with Override", "[sema]") {
    constexpr std::string_view base = "Class Base\n"
                                      "    Property P: Int\n"
                                      "        Get => 1\n"
                                      "    End Property\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", base, text));
    };

    CHECK(with_base("Class A Extends Base\n"
                    "    Override Property P: Int\n"
                    "        Get => 2\n"
                    "    End Property\n"
                    "End Class")
              .empty());
    CHECK(with_base("Class A Extends Base\n    Property P: Int\n        Get => 2\n    End "
                    "Property\nEnd Class") ==
          errors_t{ "P: 'P' overrides Base's; write 'Override Property P'" });
    CHECK(with_base("Class A Extends Base\n    Override Property P: String\n        Get => "
                    "\"x\"\n    End Property\nEnd Class") ==
          errors_t{ "P: 'P' must be Int32, like Base's" });
    CHECK(errors_of("Property P: Int\n    Get => 1\nEnd Property") ==
          errors_t{ "P: Properties can only be declared in a Struct or Class" });
}

TEST_CASE("Panic ends the path, and Assert goes on where its condition holds", "[sema]") {
    CHECK(errors_of("Function F(): Int\n    Panic(\"no\")\nEnd Function").empty());
    CHECK(errors_of("Function F(): Int\n    Assert(False, \"never\")\nEnd Function").empty());
    CHECK(errors_of("Function F(s: String?)\n    Assert(s != Null)\n    Print(s)\nEnd Function")
              .empty());
    CHECK(errors_of("Panic(1)") == errors_t{ "1: expected String, found Int32" });
}

// What codegen reads

TEST_CASE("the analysis records calls, conversions, and declarations", "[sema]") {
    // Without the standard library, the main file's items are the only ones.
    const fb::compilation_t compilation = fb::compile("test.fb",
                                                      "Function F(a: Long, b: Int = 7): Long => a\n"
                                                      "Global g = 1\n"
                                                      "Local r = F(g)\n"
                                                      "Loop\n"
                                                      "    Break\n"
                                                      "End Loop",
                                                      {});
    const fb::ast_t& ast = compilation.ast;
    const fb::analysis_t& analysis = compilation.analysis;
    REQUIRE(compilation.diagnostics.diagnostics().empty());
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

// Tuples

TEST_CASE("a tuple's type comes from its values, or from what's expected", "[sema]") {
    CHECK(probe("Local p = (1, \"a\")") == "(Int32, String)");
    CHECK(probe("Local p: (Long, String?) = (1, Null)") == "(Int64, String?)");
    CHECK(probe("Local p: (Int, Int)? = (1, 2)") == "(Int32, Int32)? (from (Int32, Int32))");
    CHECK(probe("Function F(): (Long, Long) => (1, 2)\nLocal p = F()") == "(Int64, Int64)");
    CHECK(probe("Local p = (1, 2)\nLocal q = p.1") == "Int32");
    CHECK(probe("Local p = ((1, 2.5), True)\nLocal q = p.0.1") == "Float32");
    CHECK(probe("Local p: (Int, Int)? = Null\nLocal q = p?.0") == "Int32?");
    CHECK(probe("Local p: (Long, Long) = (1, 2)\nLocal same = (1, 2) == p") == "Bool");
    CHECK(probe("Local text = \"{(1, \"a\")}\"") == "String");
}

TEST_CASE("taking a tuple apart declares Locals, and assigning one sets each target", "[sema]") {
    CHECK(probe("Local lo, hi = (1, \"a\")\nLocal x = hi") == "String");
    CHECK(probe("Local _, hi = (1, 2.5)\nLocal x = hi") == "Float32");
    CHECK(errors_of("Local a, a = (1, 2)") ==
          errors_t{ "a: 'a' is already declared in this scope" });

    // The literal's values convert to the targets' types, and each target is assigned.
    CHECK(probe("Local a: Long\nLocal b: Int\na, b = (2, 3)\nLocal x = a + b") == "Int64");
    CHECK(probe("Local s: String? = Null\nLocal n = 0\ns, n = (\"x\", 1)\nLocal t = s") ==
          "String");
    CHECK(errors_of("Local a = 1\nLocal b = \"x\"\na, b = (b, a)") ==
          errors_t{ "a: 'a' is Int32, so it can't take String",
                    "b: 'b' is String, so it can't take Int32" });
    CHECK(errors_of("Const c = 1\nLocal d = 2\nc, d = (3, 4)") ==
          errors_t{ "c: 'c' is a Const and can't be changed" });
}

TEST_CASE("tuple mistakes", "[sema]") {
    constexpr std::string_view pair = "Function Pair(): (Int, Int) => (1, 2)\n";
    const auto with_pair = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", pair, text));
    };

    CHECK(with_pair("Local a, b, c = Pair()") ==
          errors_t{ "Pair(): (Int32, Int32) has 2 elements, but 3 names take it apart" });
    CHECK(with_pair("Local x = Pair().2") ==
          errors_t{ "2: (Int32, Int32) has no element '2'; its elements are .0 to .1" });
    CHECK(with_pair("Local m: (Int, Int)? = Null\nLocal a, b = m") ==
          errors_t{ "m: (Int32, Int32)? may be Null; check it first, or give a value for Null "
                    "with '?"
                    "?'" });
    CHECK(errors_of("Local s = 5\nLocal a, b = s") ==
          errors_t{ "s: expected a tuple to take apart, found Int32" });
    CHECK(with_pair("Local n = 0\nLocal o = 0\nn, o += Pair()") ==
          errors_t{ "n, o += Pair(): '+=' can't take a tuple apart; use '='" });
    CHECK(errors_of("Class Node\nEnd Class\nLocal w: (Weak<Node>, Int)") ==
          errors_t{ "Weak<Node>: a tuple can't hold a Weak<Node>" });
    CHECK(errors_of("Local e = (Function() => 1, 2)\nLocal same = e == e") ==
          errors_t{ "e == e: '==' can't be used on (Func(): Int32, Int32)" });
    CHECK(errors_of("Class Node\nEnd Class\nLocal t = \"{(Node(), 1)}\"") ==
          errors_t{ "(Node(), 1): (Node, Int32) can't be shown in a string, since one of its "
                    "elements can't be" });
    CHECK(
        errors_of("Local k = (1, Null)") ==
        errors_t{ "Null: Null needs a type from where it's used, like 'Local e: Enemy? = Null'" });
}

// Function values

TEST_CASE("a lambda's type comes from its parameters and what it returns", "[sema]") {
    CHECK(probe("Local f = Function(x: Int) => x * 2") == "Func(Int32): Int32");
    CHECK(probe("Local f: Func(Long): Long = Function(x) => x + 1") == "Func(Int64): Int64");
    CHECK(probe("Local f = Function() => Print(\"x\")") == "Func()");
    CHECK(probe("Local f = Function()\n    Print(\"x\")\nEnd Function") == "Func()");
    CHECK(probe("Local f = Function(): String\n    Return \"x\"\nEnd Function") ==
          "Func(): String");
    CHECK(probe("Local f = Function(x: Int) => x\nLocal n = f(2)") == "Int32");
    CHECK(probe("Function Make(): Func(): Int => Function() => 1\nLocal n = Make()()") == "Int32");
}

TEST_CASE("a Function's name alone is a function value", "[sema]") {
    CHECK(probe("Function Twice(x: Int): Int => x * 2\nLocal f = Twice") == "Func(Int32): Int32");
    CHECK(probe("Function P(n: Int): Int => n\nFunction P(s: String): Int => 0\n"
                "Local f: Func(String): Int = P") == "Func(String): Int32");
    CHECK(errors_of("Function P(n: Int)\nEnd Function\nFunction P(s: String)\nEnd Function\n"
                    "Local f = P") ==
          errors_t{ "P: 'P' has overloads, so a Func type has to say which one is meant" });
    CHECK(errors_of("Class Oops Extends Error\nEnd Class\nFunction F() Throws Oops\n"
                    "End Function\nLocal f = F") ==
          errors_t{ "F: 'F' can throw, and a function value can't" });
}

TEST_CASE("a lambda captures Locals through cells, and copies what can't change", "[sema]") {
    const fb::compilation_t compilation = fb::compile("test.fb",
                                                      "Function F(p: Int)\n"
                                                      "    Local n = 1\n"
                                                      "    Const c = p + 1\n"
                                                      "    Local f = Function() => n + p + c\n"
                                                      "End Function",
                                                      {});
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    const fb::analysis_t& analysis = compilation.analysis;
    REQUIRE(analysis.closures.size() == 1);
    const fb::closure_t& closure = analysis.closures.front();
    CHECK(analysis.types.name(closure.type) == "Function at test.fb:4:15");
    REQUIRE(closure.captures.size() == 3);
    CHECK(analysis.symbol(closure.captures[0].variable).name == "n");
    CHECK(closure.captures[0].kind == fb::capture_kind_t::CELL);
    CHECK(analysis.types.name(analysis.symbol(closure.captures[0].variable).cell) == "Cell<Int32>");
    CHECK(closure.captures[1].kind == fb::capture_kind_t::VALUE);
    CHECK(closure.captures[2].kind == fb::capture_kind_t::VALUE);

    // The closure's Class has a field for each, named after the variable.
    const std::vector<fb::field_t>& fields = analysis.types.get(closure.type).fields;
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].name == "n");
    CHECK(fields[0].type == analysis.symbol(closure.captures[0].variable).cell);
    CHECK(fields[2].name == "c");
}

TEST_CASE("Self and [Weak Self] in a lambda", "[sema]") {
    constexpr std::string_view timer = "Class Timer\n"
                                       "    Field Ticks = 0\n"
                                       "    Field OnTick: (Func())?\n"
                                       "    Method Tick()\n"
                                       "        Ticks++\n"
                                       "    End Method\n";
    const auto with_timer = [&](const std::string_view text) {
        return errors_of(std::format("{}{}\nEnd Class", timer, text));
    };

    CHECK(with_timer("    Method Start()\n        OnTick = Function[Weak Self]() => Self?.Tick()\n"
                     "    End Method")
              .empty());
    CHECK(with_timer("    Method Start()\n        OnTick = Function() => Tick()\n    End Method") ==
          errors_t{ "Function: this lambda captures Self, so keeping it in a field of Self makes a "
                    "cycle that's never freed; capture it with [Weak Self]" });
    CHECK(
        with_timer("    Method Start()\n        OnTick = Function[Weak Self]() => Tick()\n"
                   "    End Method") ==
        errors_t{ "Tick: 'Tick' needs Self, which this lambda refers to weakly; use Self?.Tick" });
    CHECK(errors_of("Struct P\n    Field X = 0\n    New()\n        Local f = Function() => Self.X\n"
                    "    End New\nEnd Struct") ==
          errors_t{ "Self: a lambda in a Struct's New can't use Self, which isn't made yet" });
}

TEST_CASE("narrowing around lambdas", "[sema]") {
    // A Local that a lambda assigns can change in any call once the lambda exists.
    CHECK(errors_of("Class E\nEnd Class\n"
                    "Local e: E? = E()\n"
                    "Local f = Function()\n    e = Null\nEnd Function\n"
                    "If e != Null\n    Local g: E = e\nEnd If") ==
          errors_t{ "e: expected E, found E?, which may be Null; check it first, or give a value "
                    "for Null with '?"
                    "?'" });

    // In a lambda, other code can change a Local from outside between a check and a use.
    CHECK(errors_of("Local s: String? = \"a\"\n"
                    "Local f = Function()\n    If s != Null Then Print(s)\nEnd Function") ==
          errors_t{ "s: expected String, found String?, which may be Null; check it first, or "
                    "give a value for Null with '?"
                    "?'" });
}

TEST_CASE("lambda mistakes", "[sema]") {
    CHECK(errors_of("Local f = Function(x) => x") ==
          errors_t{ "x: 'x' needs a type, since nothing here says what this lambda takes" });
    CHECK(errors_of("Local f: Func(Int): Int = Function(x) => \"no\"") ==
          errors_t{ "\"no\": expected Int32, found String" });
    CHECK(errors_of("Local f = Function()\n    Return 5\nEnd Function") ==
          errors_t{ "5: this lambda returns nothing, so its Return can't have a value; give it a "
                    "return type, like 'Function(): Int'" });
    CHECK(errors_of("Local f: Func(Int): Int = Function(x)\n    If x > 0 Then Return 1\n"
                    "End Function") ==
          errors_t{ "Function: this lambda doesn't return a value on every path" });
    CHECK(errors_of("Function F(ByRef n: Int)\n    Local c = Function() => n\nEnd Function") ==
          errors_t{ "n: 'n' is ByRef, so a lambda can't capture it; copy it into a Local first" });
    CHECK(errors_of("Class Oops Extends Error\nEnd Class\n"
                    "Local f = Function()\n    Throw Oops(\"x\")\nEnd Function") ==
          errors_t{ "Oops(\"x\"): a lambda can't throw, so Oops has to be caught in it" });
    CHECK(errors_of("Loop\n    Local f = Function()\n        Break\n    End Function\n    Break\n"
                    "End Loop") == errors_t{ "Break: Break must be inside a loop" });
    CHECK(errors_of("Local f: Func() = Function() => 5") ==
          errors_t{ "5: this lambda returns nothing, so its '=>' needs a call, like '=> "
                    "Print(x)'" });
}

TEST_CASE("calling a function value", "[sema]") {
    constexpr std::string_view f = "Local f: Func(Int): Int = Function(x) => x\n";
    const auto with_f = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", f, text));
    };

    CHECK(with_f("f(\"a\")") == errors_t{ "\"a\": expected Int32, found String" });
    CHECK(with_f("f(1, 2)") == errors_t{ "f(1, 2): Func(Int32): Int32 takes 1 argument, not 2" });
    CHECK(with_f("Local same = f == f") ==
          errors_t{ "f == f: '==' can't be used on Func(Int32): Int32" });
    CHECK(with_f("Print(\"{f}\")") ==
          errors_t{ "f: Func(Int32): Int32 can't be shown in a string" });
    CHECK(errors_of("Local n = 5\nn(1)") == errors_t{ "n: 'n' isn't a function" });
    CHECK(errors_of("Local g: (Func())? = Null\ng()") ==
          errors_t{ "g: 'g' may be Null; check that it isn't before calling it" });
    CHECK(errors_of("Local g: (Func())? = Null\nLocal empty = g == Null").empty());
}

// Generics

constexpr std::string_view generics = "Function First<T>(xs: T[]): T => xs[0]\n"
                                      "Function Transform<T, U>(xs: T[], f: Func(T): U): U[]\n"
                                      "    Local result: U[] = []\n"
                                      "    For x In xs\n"
                                      "        result.Add(f(x))\n"
                                      "    End For\n"
                                      "    Return result\n"
                                      "End Function\n"
                                      "Function Max<T>(a: T, b: T): T => a\n"
                                      "Struct Pair<A, B>\n"
                                      "    Field First: A\n"
                                      "    Field Second: B\n"
                                      "End Struct\n"
                                      "Class Pile<T>\n"
                                      "    Field Items: T[] = []\n"
                                      "    Method Push(item: T)\n"
                                      "        Items.Add(item)\n"
                                      "    End Method\n"
                                      "    Method Top(): T => Items[Items.Count - 1]\n"
                                      "End Class\n";

namespace {

std::string probe_generic(const std::string_view text) {
    return probe(std::format("{}{}", generics, text));
}

errors_t with_generics(const std::string_view text) {
    return errors_of(std::format("{}{}", generics, text));
}

} // namespace

TEST_CASE("type arguments are inferred from the arguments", "[sema]") {
    CHECK(probe_generic("Local n = First([3, 4])") == "Int32");
    CHECK(probe_generic("Local n = First([\"a\"])") == "String");
    CHECK(probe_generic("Local p = Pair(1, \"one\")") == "Pair<Int32, String>");

    // A literal decides as an Int, or as a Float when one passed for the same T has a decimal.
    CHECK(probe_generic("Local m = Max(1, 2)") == "Int32");
    CHECK(probe_generic("Local m = Max(1, 2.5)") == "Float32");

    // A lambda's parameters take the types already known, and its result gives the rest.
    CHECK(probe_generic("Local s = Transform([1, 2], Function(x) => \"n{x}\")") == "String[]");
}

TEST_CASE("type arguments can be written, or come from the type expected", "[sema]") {
    CHECK(probe_generic("Local f = First<Float>([1, 2])") == "Float32");
    CHECK(probe_generic("Local p: Pair<Long, String> = Pair<Long, String>(2, \"two\")") ==
          "Pair<Int64, String>");
    CHECK(probe_generic("Local s: Pile<Int> = Pile()") == "Pile<Int32>");
    CHECK(probe_generic("Local f: Func(String[]): String = First") == "Func(String[]): String");
}

TEST_CASE("an instance's members have its type arguments", "[sema]") {
    CHECK(probe_generic("Local s = Pile<String>()\nLocal top = s.Top()") == "String");
    CHECK(probe_generic("Local s = Pile<Int>()\nLocal items = s.Items") == "Int32[]");
    CHECK(probe_generic("Local p = Pair(1, 2.5)\nLocal x = p.Second") == "Float32");
    CHECK(with_generics("Local s = Pile<Int>()\ns.Push(\"a\")") ==
          errors_t{ "\"a\": expected Int32, found String" });
}

TEST_CASE("a type parameter can only be passed around until interfaces", "[sema]") {
    CHECK(errors_of("Function F<T>(a: T, b: T): Bool => a == b") ==
          errors_t{ "a == b: '==' can't be used on T" });
    CHECK(errors_of("Function F<T>(a: T): String => \"{a}\"") ==
          errors_t{ "a: T can't be shown in a string without a constraint, like <T: Printable>" });
    CHECK(errors_of("Function F<T>(a: T): Int => a.Count") ==
          errors_t{ "Count: T has no members without a constraint, like <T: I>" });
    CHECK(errors_of("Function F<T>(): T\n    Local x: T\n    Return x\nEnd Function") ==
          errors_t{ "x: 'x' is read before it's assigned" });
}

TEST_CASE("generic mistakes", "[sema]") {
    // In Zero itself, the type expected, its own T, says what T is.
    CHECK(errors_of("Function Zero<T>(): T => Zero()\nLocal z = Zero()") ==
          errors_t{ "Zero: can't tell what T is in this call; write the type arguments, like "
                    "Zero<Int>(...)" });
    CHECK(with_generics("Local s: Pile") ==
          errors_t{ "Pile: Pile<T> is generic, so it needs type arguments, like Pile<Int>" });
    CHECK(with_generics("Local s: Pile<Int, Int>") ==
          errors_t{ "Pile<Int, Int>: Pile<T> takes 1 type argument, not 2" });
    CHECK(errors_of("Local d: Int<Int>") == errors_t{ "Int<Int>: 'Int' isn't generic" });
    CHECK(errors_of("Function F<T, T>()\nEnd Function") ==
          errors_t{ "T: 'T' is already a type parameter here" });
    CHECK(errors_of("Class H<T>\n    Static Field Count = 0\nEnd Class\nLocal n = H.Count") ==
          errors_t{ "H: H<T> is generic, so it needs type arguments, like H<Int>" });
    CHECK(with_generics("Local f = First") ==
          errors_t{ "First: 'First' is generic, so it needs a Func type to say what T is, or type "
                    "arguments, like First<Int>" });
}

// Operators

constexpr std::string_view money =
    "Struct Money\n"
    "    Field Cents: Long\n"
    "    Operator +(a: Money, b: Money): Money => Money(a.Cents + b.Cents)\n"
    "    Operator *(a: Money, n: Long): Money => Money(a.Cents * n)\n"
    "    Operator -(a: Money): Money => Money(-a.Cents)\n"
    "    Operator ==(a: Money, b: Money): Bool => a.Cents == b.Cents\n"
    "End Struct\n"
    "Local m = Money(5)\n";

namespace {

std::string probe_money(const std::string_view text) {
    return probe(std::format("{}{}", money, text));
}

errors_t with_money(const std::string_view text) {
    return errors_of(std::format("{}{}", money, text));
}

} // namespace

TEST_CASE("an operator on a Struct or Class calls the Operator it defines", "[sema]") {
    CHECK(probe_money("Local x = m + m") == "Money");
    CHECK(probe_money("Local x = -m") == "Money");
    // A literal takes the type of the Operator's parameter.
    CHECK(probe_money("Local x = m * 3") == "Money");
    CHECK(probe_money("Local x = m == m") == "Bool");
    // != is the opposite of ==.
    CHECK(probe_money("Local x = m != m") == "Bool");
    CHECK(with_money("m += m\nm = -m").empty());

    // A generic type's Operators take its type arguments from the operands.
    CHECK(probe("Struct Box<T>\n    Field Value: T\n"
                "    Operator <<(a: Box<T>, value: T): Box<T> => Box(value)\nEnd Struct\n"
                "Local b = Box(1) << 2") == "Box<Int32>");
}

TEST_CASE("an operator a type doesn't define stays an error", "[sema]") {
    CHECK(with_money("Local x = m / m") == errors_t{ "m / m: '/' can't be used on Money" });
    CHECK(with_money("Local x = m + \"a\"") ==
          errors_t{ "m + \"a\": no Operator + takes (Money, String)" });
    CHECK(with_money("Local x = m * 2.5") ==
          errors_t{ "m * 2.5: no Operator * takes (Money, Float32)" });
}

TEST_CASE("Operator mistakes", "[sema]") {
    const auto in_struct = [](const std::string_view member) {
        return errors_of(std::format("Struct M\n    Field V = 0\n    {}\nEnd Struct", member));
    };

    CHECK(in_struct("Operator !=(a: M, b: M): Bool => True") ==
          errors_t{ "!=: != is always the opposite of ==, so define Operator == instead" });
    CHECK(in_struct("Operator +(a: M): M => a") ==
          errors_t{ "+: Operator + takes two parameters" });
    CHECK(in_struct("Operator !(a: M, b: M): Bool => True") ==
          errors_t{ "!: Operator ! takes one parameter" });
    CHECK(in_struct("Operator ==(a: M, b: M): Int => 1") ==
          errors_t{ "==: Operator == gives a Bool, not Int32" });
    CHECK(in_struct("Operator *(a: Int, b: Int): Int => a") ==
          errors_t{ "*: an Operator of M needs a parameter of that type" });
    CHECK(in_struct("Operator +(a: M, b: M...): M => a") ==
          errors_t{ "+: an Operator takes its operands, so no parameter takes the rest of the "
                    "arguments" });
    CHECK(in_struct("Operator /(ByRef a: M, b: M): M => a") ==
          errors_t{ "/: an Operator's parameters can't be ByRef or have defaults" });
    CHECK(in_struct("Static Operator %(a: M, b: M): M => a") ==
          errors_t{ "%: an Operator can't be Abstract, Final, Override, Static, or ReadOnly; it's "
                    "always Static" });
    CHECK(errors_of("Operator +(a: Int, b: Int): Int => a") ==
          errors_t{ "+: Operators can only be declared in a Struct or Class" });
}

TEST_CASE("a type with Operator == can be compared, whatever its fields are", "[sema]") {
    CHECK(errors_of("Struct Bag\n    Field Items: Int[] = []\n"
                    "    Operator ==(a: Bag, b: Bag): Bool => a.Items.Count == b.Items.Count\n"
                    "End Struct\n"
                    "Local bags = [Bag()]\nLocal found = bags.Contains(Bag())")
              .empty());
}

// Interfaces

constexpr std::string_view shapes = "Interface Shape\n"
                                    "    Method Area(): Int\n"
                                    "    Method Twice(): Int => Area() * 2\n"
                                    "    ReadOnly Property Sides: Int\n"
                                    "End Interface\n"
                                    "Class Square Implements Shape\n"
                                    "    Field Side: Int = 1\n"
                                    "    Method Area(): Int => Side * Side\n"
                                    "    Property Sides: Int\n"
                                    "        Get => 4\n"
                                    "    End Property\n"
                                    "End Class\n"
                                    "Struct Tile Implements Shape\n"
                                    "    Method Area(): Int => 1\n"
                                    "    Property Sides: Int\n"
                                    "        Get => 4\n"
                                    "    End Property\n"
                                    "End Struct\n";

namespace {

std::string probe_shapes(const std::string_view text) {
    return probe(std::format("{}{}", shapes, text));
}

errors_t with_shapes(const std::string_view text) {
    return errors_of(std::format("{}{}", shapes, text));
}

} // namespace

TEST_CASE("an Interface's value is an object of a Class that implements it", "[sema]") {
    CHECK(probe_shapes("Local s: Shape = Square()\nLocal a = s.Area()") == "Int32");
    CHECK(probe_shapes("Local s: Shape = Square()\nLocal n = s.Sides") == "Int32");
    CHECK(probe_shapes("Local shapes: Shape[] = [Square(), Square()]\nLocal s = shapes[0]") ==
          "Shape");
    CHECK(probe_shapes("Local maybe: Shape? = Square()") == "Shape? (from Square)");
    // A default is a Method of each type that implements it.
    CHECK(probe_shapes("Local a = Square().Twice()") == "Int32");
    CHECK(probe_shapes("Local a = Tile().Twice()") == "Int32");

    // Is, As, and As? test for an Interface, and from one for a Class.
    CHECK(probe_shapes("Local s: Shape = Square()\nLocal q = s As Square") == "Square");
    CHECK(probe_shapes("Local s: Shape = Square()\nLocal q = s Is Square") == "Bool");
    CHECK(probe_shapes("Class Rock\nEnd Class\nLocal q = Rock() As? Shape") == "Shape?");
}

TEST_CASE("what an Interface's values can't do", "[sema]") {
    CHECK(
        with_shapes("Local s: Shape = Tile()") ==
        errors_t{ "Tile(): expected Shape, found Tile, which is a Struct; an Interface's values "
                  "are objects of Classes, but a type parameter like <T: Shape> takes a Struct" });
    CHECK(with_shapes("Local s: Shape = Square()\nLocal same = s == s") ==
          errors_t{ "s == s: '==' can't be used on Shape; RefEquals tells whether two are the same "
                    "object" });
    CHECK(with_shapes("Local s: Shape = Square()\nLocal same = RefEquals(s, s)").empty());
    CHECK(with_shapes("Local s = Shape()") ==
          errors_t{ "Shape: Shape is an Interface, so it can't be made; make a Struct or Class "
                    "that implements it" });
    CHECK(with_shapes("Final Class Rock\nEnd Class\nLocal q = Rock() Is Shape") ==
          errors_t{ "Rock() Is Shape: Rock and Shape aren't related, so this is always False" });
    CHECK(with_shapes("Local q = Square() Is Shape") ==
          errors_t{ "Square() Is Shape: this is always True: every Square is a Shape" });
}

TEST_CASE("an Interface that needs Statics or Operators, or uses Self, is only a constraint",
          "[sema]") {
    CHECK(errors_of("Interface Zeroed\n    Static Method Zero(): Self\nEnd Interface\n"
                    "Local n: Zeroed") ==
          errors_t{ "Zeroed: Zeroed can only be a constraint, like <T: Zeroed>, since it needs "
                    "Static Methods" });
    CHECK(errors_of("Interface Adds\n    Operator +(a: Self, b: Self): Self\nEnd Interface\n"
                    "Function F(a: Adds)\nEnd Function") ==
          errors_t{ "Adds: Adds can only be a constraint, like <T: Adds>, since it needs "
                    "Operators" });
    CHECK(errors_of("Interface Copies\n    Method Copy(): Self\nEnd Interface\n"
                    "Interface More Extends Copies\nEnd Interface\n"
                    "Function F(m: More[])\nEnd Function") ==
          errors_t{ "More: More can only be a constraint, like <T: More>, since it uses Self" });
}

TEST_CASE("a type implements each of its Interfaces' members", "[sema]") {
    CHECK(with_shapes("Class C Implements Shape\nEnd Class") ==
          errors_t{
              "Shape: C implements Shape, so it needs a Method 'Area'",
              "Shape: C implements Shape, so it needs a Property 'Sides'",
          });
    CHECK(with_shapes("Class C Implements Shape\n"
                      "    Method Area(): Float => 1\n"
                      "    Property Sides: String\n"
                      "        Get => \"\"\n"
                      "    End Property\n"
                      "End Class") ==
          errors_t{
              "Shape: C's 'Area' doesn't match Shape's, which takes () and returns Int32",
              "Shape: 'Sides' must be Int32 to implement Shape's",
          });

    // The members of the Interfaces it extends too, and Self is the type itself.
    CHECK(errors_of("Interface Adds\n    Operator +(a: Self, b: Self): Self\nEnd Interface\n"
                    "Interface Sums Extends Adds\n    Static Method Zero(): Self\nEnd Interface\n"
                    "Struct M Implements Sums\n"
                    "    Static Method Zero(): Int => 0\n"
                    "End Struct") ==
          errors_t{
              "Sums: M's 'Zero' doesn't match Sums's, which takes () and returns M",
              "Sums: M implements Adds, so it needs an Operator '+'",
          });

    // What implements a Method doesn't throw, or change a Struct when the Interface's doesn't.
    CHECK(errors_of("Interface Hurts\n    Method Hurt()\n    Property Hp: Int\nEnd Interface\n"
                    "Struct S Implements Hurts\n"
                    "    ByRef Method Hurt()\n    End Method\n"
                    "    Property Hp: Int\n        Get => 1\n    End Property\n"
                    "End Struct\n"
                    "Class C Implements Hurts\n"
                    "    Method Hurt() Throws Error\n        Throw Error()\n    End Method\n"
                    "    Property Hp: Int\n        Get => 1\n    End Property\n"
                    "End Class") ==
          errors_t{
              "Hurts: 'Hurt' is a ByRef Method, so it can't implement Hurts's, which isn't",
              "Hurts: 'Hp' needs a Set to implement Hurts's",
              "Hurts: 'Hurt' can't throw Error, since Hurts's doesn't",
              "Hurts: 'Hp' needs a Set to implement Hurts's",
          });
}

TEST_CASE("Interface mistakes", "[sema]") {
    CHECK(errors_of("Interface A Extends B\nEnd Interface\nInterface B Extends A\nEnd Interface") ==
          errors_t{ "A: B can't extend itself, directly or through other Interfaces" });
    CHECK(errors_of("Final Interface C\n"
                    "    Field X: Int\n"
                    "    New()\n    End New\n"
                    "    Method Destroy()\n"
                    "End Interface") ==
          errors_t{
              "C: an Interface doesn't take modifiers like Abstract or Final",
              "X: an Interface can't hold Fields; a Property can stand for one",
              "New: an Interface can't have a New; the types that implement it make their values",
              "Destroy: an Interface can't have a Destroy; each Class's runs by itself",
          });
    CHECK(errors_of("Interface I\n    Property P: Int\n        Get => 1\n    End Property\n"
                    "End Interface") ==
          errors_t{ "P: an Interface's Property is only its first line; the types that implement "
                    "it give its Get and Set" });
    CHECK(errors_of("Interface I\n    Operator +(a: Int, b: Int): Int\nEnd Interface") ==
          errors_t{ "+: an Operator of I needs a parameter of type Self" });
    CHECK(errors_of("Interface I\nEnd Interface\nClass C Extends I\nEnd Class") ==
          errors_t{ "I: 'I' is an Interface, so it goes after Implements" });
    CHECK(errors_of("Struct S Implements Int, S\nEnd Struct") ==
          errors_t{ "Int: Int32 isn't an Interface", "S: S isn't an Interface" });
    CHECK(errors_of("Struct S\n    Method M(x: Self)\n    End Method\nEnd Struct") ==
          errors_t{
              "Self: Self is a type only in an Interface, where it's the type implementing it" });
}

// Constraints

constexpr std::string_view numbers = "Interface Summable\n"
                                     "    Static Method Zero(): Self\n"
                                     "    Operator +(a: Self, b: Self): Self\n"
                                     "    Operator ==(a: Self, b: Self): Bool\n"
                                     "End Interface\n"
                                     "Struct Money Implements Summable\n"
                                     "    Field Cents: Long\n"
                                     "    Static Method Zero(): Money => Money(0)\n"
                                     "    Operator +(a: Money, b: Money): Money => Money(a.Cents "
                                     "+ b.Cents)\n"
                                     "    Operator ==(a: Money, b: Money): Bool => a.Cents == "
                                     "b.Cents\n"
                                     "End Struct\n"
                                     "Function Sum<T: Summable>(xs: T[]): T\n"
                                     "    Local total = T.Zero()\n"
                                     "    For x In xs\n"
                                     "        total += x\n"
                                     "    End For\n"
                                     "    Return total\n"
                                     "End Function\n";

namespace {

std::string probe_numbers(const std::string_view text) {
    return probe(std::format("{}{}", numbers, text));
}

errors_t with_numbers(const std::string_view text) {
    return errors_of(std::format("{}{}", numbers, text));
}

} // namespace

TEST_CASE("generic code uses what its constraints give", "[sema]") {
    CHECK(probe_numbers("Local s = Sum([Money(1), Money(2)])") == "Money");
    CHECK(probe_numbers("Function Same<T>(a: T, b: T): Bool\n"
                        "    Where T: Summable\n"
                        "    Return a == b && !(a != b)\n"
                        "End Function\n"
                        "Local s = Same(Money(1), Money(2))") == "Bool");
    // A type parameter with a constraint meets another's.
    CHECK(probe_numbers("Function Twice<T: Summable>(x: T): T => Sum([x, x])\n"
                        "Local m = Twice(Money(3))") == "Money");
    CHECK(with_numbers("Function F<T: Summable>(x: T): T => x.Zero()") ==
          errors_t{ "Zero: 'Zero' is Static; call it on T" });
    CHECK(with_numbers("Function F<T: Summable>(x: T): Int => x.Count") ==
          errors_t{ "Count: T's constraints have no member named 'Count'" });
}

TEST_CASE("each use of generic code meets its constraints", "[sema]") {
    CHECK(with_numbers("Local s = Sum([1, 2])") ==
          errors_t{ "Sum: Int32 doesn't implement Summable, which 'Sum' needs for T" });
    CHECK(with_numbers("Struct Box<T: Summable>\n    Field Item: T\nEnd Struct\n"
                       "Local b: Box<String>? = Null") ==
          errors_t{ "Box<String>: String doesn't implement Summable, which Box<T> needs for T" });
    CHECK(with_numbers("Function Bad(b: Later<Int>)\nEnd Function\n"
                       "Class Later<T: Summable>\nEnd Class") ==
          errors_t{ "Later<Int>: Int32 doesn't implement Summable, which Later<T> needs for T" });
    CHECK(with_numbers("Local f: Func(Money[]): Money = Sum").empty());

    // A Method with a Where line is only there for the type arguments that meet it.
    CHECK(with_numbers("Class Bag<T>\n"
                       "    Field Items: T[] = []\n"
                       "    Method Total(): T\n"
                       "        Where T: Summable\n"
                       "        Return Sum(Items)\n"
                       "    End Method\n"
                       "End Class\n"
                       "Local money = Bag<Money>().Total()\n"
                       "Local words = Bag<String>().Total()") ==
          errors_t{ "Bag<String>().Total: String doesn't implement Summable, which 'Total' needs "
                    "for T" });
}

TEST_CASE("a Class extending one gets the Interfaces it implements, unless they use Self",
          "[sema]") {
    CHECK(with_shapes("Class Big Extends Square\nEnd Class\n"
                      "Function F<T: Shape>(x: T): Int => x.Area()\n"
                      "Local s: Shape = Big()\nLocal a = F(Big())")
              .empty());
    CHECK(with_numbers("Class A Implements Summable\n"
                       "    Static Method Zero(): A => A()\n"
                       "    Operator +(a: A, b: A): A => a\n"
                       "    Operator ==(a: A, b: A): Bool => True\n"
                       "End Class\n"
                       "Class B Extends A\nEnd Class\n"
                       "Local b = Sum([B()])") ==
          errors_t{ "Sum: B doesn't implement Summable, which 'Sum' needs for T" });
}

TEST_CASE("constraint mistakes", "[sema]") {
    CHECK(errors_of("Function F<T: Int>(x: T)\nEnd Function") ==
          errors_t{ "Int: Int32 isn't an Interface" });
    CHECK(errors_of("Function F<T>(x: T)\n    Where U: Int\nEnd Function") ==
          errors_t{ "U: 'U' isn't a type parameter here" });
    CHECK(
        errors_of("Interface I\nEnd Interface\nFunction F(x: Int)\n    Where T: I\nEnd Function") ==
        errors_t{ "T: 'T' isn't a type parameter here" });
}

// Abstract and Static Properties

TEST_CASE("an Abstract Property is overridden, and a Static one belongs to its type", "[sema]") {
    const std::string_view shape = "Abstract Class Shape\n"
                                   "    Abstract ReadOnly Property Area: Float\n"
                                   "    Abstract Property Label: String\n"
                                   "    Static Property Made: Int\n"
                                   "        Get => 1\n"
                                   "    End Property\n"
                                   "End Class\n";
    CHECK(probe(std::format("{}Local s: Shape? = Null\nLocal m = Shape.Made", shape)) == "Int32");
    CHECK(errors_of(std::format("{}Class Circle Extends Shape\n"
                                "    Override Property Label: String\n"
                                "        Get => \"\"\n"
                                "    End Property\n"
                                "End Class",
                                shape)) ==
          errors_t{
              "Circle: Circle must override Shape's Abstract Property 'Area', or be Abstract",
              "Label: 'Label' needs a Set, like Shape's Abstract one",
          });
    CHECK(errors_of(std::format("{}Local s = Shape.Made\nShape.Made = 2", shape)) ==
          errors_t{ "Shape.Made: 'Made' has no Set, so it can't be assigned" });
}

TEST_CASE("Property mistakes", "[sema]") {
    const auto in_class = [](const std::string_view member) {
        return errors_of(std::format("Class C\n    {}\nEnd Class", member));
    };

    CHECK(in_class("Abstract Property P: Int") ==
          errors_t{ "P: 'P' is Abstract, so C must be Abstract too" });
    CHECK(in_class("ReadOnly Property P: Int\n        Get => 1\n    End Property") ==
          errors_t{ "P: a Property without a Set is read-only already; ReadOnly only goes on an "
                    "Abstract one or an Interface's" });
    CHECK(in_class("Static Override Property P: Int\n        Get => 1\n    End Property") ==
          errors_t{ "P: a Static Property can't be Abstract, Final, or Override; it isn't used "
                    "through an object" });
    CHECK(in_class("Abstract Property P: Int\n        Get => 1\n    End Property") ==
          errors_t{ "P: an Abstract Property is only its first line; a Class that extends it "
                    "gives its Get and Set" });
    CHECK(errors_of("Struct S\n    Abstract Property P: Int\nEnd Struct") ==
          errors_t{ "P: a Struct's Properties can't be Abstract, Final, or Override" });
}

// Built-in Interfaces

TEST_CASE("the built-in types implement the built-in Interfaces that fit them", "[sema]") {
    const auto meets = [](const std::string_view constraint, const std::string_view value) {
        return errors_of(
                   std::format("Function F<T: {}>(x: T)\nEnd Function\nF({})", constraint, value))
            .empty();
    };

    CHECK(meets("Numeric & Comparable & Hashable & Printable", "2.5"));
    CHECK(meets("Comparable & Hashable & Printable", "\"a\""));
    CHECK(meets("Equatable & Hashable & Printable", "True"));
    CHECK(meets("Hashable & Printable", "[(1, \"a\")]"));
    CHECK(meets("Hashable", "(1 As Int?)"));
    CHECK_FALSE(meets("Numeric", "\"a\""));
    CHECK_FALSE(meets("Comparable", "True"));
    CHECK_FALSE(meets("Comparable", "[1]"));
    CHECK_FALSE(meets("Printable", "(1 As Int?)"));
    CHECK_FALSE(meets("Equatable", "[Function() => 1]"));

    CHECK(errors_of("Function Max<T: Comparable>(a: T, b: T): T => a > b ? a : b\n"
                    "Local m = Max(True, False)") ==
          errors_t{ "Max: Bool doesn't implement Comparable, which 'Max' needs for T" });
}

TEST_CASE("a Struct or Class that lists Equatable or Hashable can have the built-in == and Hash",
          "[sema]") {
    CHECK(probe("Struct P Implements Hashable\n    Field X: Int\n    Field Name: String\n"
                "End Struct\nLocal h = P(1, \"a\").Hash()") == "UInt64");
    CHECK(probe("Class C Implements Hashable\nEnd Class\nLocal h = C().Hash()") == "UInt64");
    CHECK(errors_of("Struct Money Implements Hashable\n"
                    "    Field Cents: Long\n"
                    "    Operator ==(a: Money, b: Money): Bool => a.Cents == b.Cents\n"
                    "End Struct") ==
          errors_t{ "Hashable: Money has an Operator == of its own, so it needs a Hash that agrees "
                    "with it" });
    CHECK(errors_of("Class Rock\nEnd Class\n"
                    "Struct Bag Implements Hashable\n    Field Owner: Rock\nEnd Struct") ==
          errors_t{ "Hashable: 'Owner' isn't Hashable, so Bag needs a Hash of its own to implement "
                    "Hashable" });
    CHECK(errors_of("Struct Holder Implements Equatable\n    Field Run: Func(): Int\nEnd Struct") ==
          errors_t{ "Equatable: 'Run' can't be compared with ==, so Holder needs an Operator == of "
                    "its own to implement Equatable" });

    // Comparable gives the other comparisons from <.
    CHECK(probe("Struct M Implements Comparable\n"
                "    Field V: Int\n"
                "    Operator <(a: M, b: M): Bool => a.V < b.V\n"
                "End Struct\n"
                "Local x = M(1) >= M(2)") == "Bool");
}

TEST_CASE("the built-in types have the built-in Interfaces' Methods", "[sema]") {
    CHECK(probe("Local s = 5.ToString()") == "String");
    CHECK(probe("Local h = \"a\".Hash()") == "UInt64");
    CHECK(probe("Local h = [1, 2].Hash()") == "UInt64");
    CHECK(probe("Local h = (1, \"a\").ToString()") == "String");
    CHECK(errors_of("Class Rock\nEnd Class\nLocal h = Rock().Hash()") ==
          errors_t{ "Hash: Rock has no member named 'Hash'" });
    CHECK(errors_of("Local f = 1.5.Hash") ==
          errors_t{ "Hash: 'Hash' is a method; call it with ()" });
}

TEST_CASE("Printable puts a value in a string", "[sema]") {
    const std::string_view point = "Struct Point Implements Printable\n"
                                   "    Field X: Int\n"
                                   "    Method ToString(): String => \"({X})\"\n"
                                   "End Struct\n";
    CHECK(probe(std::format("{}Local s = \"{{Point(1)}} {{[Point(2)]}}\"", point)) == "String");
    CHECK(probe("Function F<T: Printable>(x: T): String => \"{x}\"\nLocal s = F([1, 2])") ==
          "String");
    CHECK(probe(std::format("{}Class C Implements Printable\n"
                            "    Method ToString(): String => \"c\"\n"
                            "End Class\n"
                            "Local p: Printable = C()\nLocal s = \"{{p}}\"",
                            point)) == "String");
    CHECK(errors_of("Local s = \"{[Function() => 1]}\"") ==
          errors_t{ "[Function() => 1]: (Func(): Int32)[] can't be shown in a string, since its "
                    "elements can't be" });
}

// Extend

TEST_CASE("an Extend adds Methods, Properties, and Operators to a type", "[sema]") {
    const std::string_view point = "Struct Point\n"
                                   "    Field X: Int\n"
                                   "End Struct\n"
                                   "Extend Point\n"
                                   "    Method Doubled(): Point => Point(X * 2)\n"
                                   "    Property Big: Bool\n"
                                   "        Get => X > 10\n"
                                   "    End Property\n"
                                   "    Operator +(a: Point, b: Point): Point => Point(a.X + b.X)\n"
                                   "    ByRef Method Grow()\n"
                                   "        X++\n"
                                   "    End Method\n"
                                   "End Extend\n";
    CHECK(probe(std::format("{}Local p = Point(1).Doubled()", point)) == "Point");
    CHECK(probe(std::format("{}Local b = Point(1).Big", point)) == "Bool");
    CHECK(probe(std::format("{}Local p = Point(1) + Point(2)", point)) == "Point");
    CHECK(errors_of(std::format("{}Point(1).Grow()", point)) ==
          errors_t{ "Point(1): 'Grow' is a ByRef Method, so it needs a variable to change, not a "
                    "value" });

    // Built-in types, Enums, and every array.
    CHECK(probe("Extend Int\n    Method Doubled(): Int => Self * 2\nEnd Extend\n"
                "Local x = 5.Doubled()") == "Int32");
    CHECK(probe("Extend Int\n    Static Method Parse(text: String): Int => 0\nEnd Extend\n"
                "Local x = Int.Parse(\"1\")") == "Int32");
    CHECK(probe("Extend String\n    Method Twice(): String => Self + Self\nEnd Extend\n"
                "Local x = \"a\".Twice()") == "String");
    CHECK(probe("Enum Mood\n    Calm, Angry\nEnd Enum\n"
                "Extend Mood\n    Method Calm(): Bool => Self == Mood.Calm\nEnd Extend\n"
                "Local x = Mood.Angry.Calm()") == "Bool");
    CHECK(probe("Extend T[]\n    Method Last(): T => Self[Self.Count - 1]\nEnd Extend\n"
                "Local x = [\"a\"].Last()") == "String");
}

TEST_CASE("an Extend's Where makes what it adds conditional", "[sema]") {
    const std::string_view sums = "Extend T[]\n"
                                  "    Method Sum(): T\n"
                                  "        Where T: Numeric\n"
                                  "        Local total = T.Zero()\n"
                                  "        For x In Self\n"
                                  "            total += x\n"
                                  "        End For\n"
                                  "        Return total\n"
                                  "    End Method\n"
                                  "End Extend\n";
    CHECK(probe(std::format("{}Local s = [1.5].Sum()", sums)) == "Float32");
    CHECK(errors_of(std::format("{}Local s = [\"a\"].Sum()", sums)) ==
          errors_t{ "[\"a\"].Sum: String doesn't implement Numeric, which 'Sum' needs for T" });

    const std::string_view box = "Class Box<T>\n"
                                 "    Field Item: T?\n"
                                 "End Class\n"
                                 "Extend Box<T> Implements Printable\n"
                                 "    Where T: Printable\n"
                                 "    Method ToString(): String => \"box\"\n"
                                 "End Extend\n";
    CHECK(probe(std::format("{}Local s = \"{{Box<Int>()}}\"", box)) == "String");
    CHECK(errors_of(std::format("{}Class Rock\nEnd Class\nLocal s = \"{{Box<Rock>()}}\"", box)) ==
          errors_t{ "Box<Rock>(): Box<Rock> can't be shown in a string; implement Printable, with "
                    "a ToString" });
    // A Class that implements an Interface through an Extend's Where is its value.
    CHECK(errors_of(std::format("{}Local p: Printable = Box<Int>()", box)).empty());
    CHECK(
        errors_of(std::format("{}Class Rock\nEnd Class\nLocal p: Printable = Box<Rock>()", box)) ==
        errors_t{ "Box<Rock>(): expected Printable, found Box<Rock>" });
}

TEST_CASE("an Extend can make a type implement an Interface", "[sema]") {
    const std::string_view shown = "Interface Shown\n"
                                   "    Method Text(): String\n"
                                   "    Method Loud(): String => Text() + \"!\"\n"
                                   "End Interface\n"
                                   "Function Show<T: Shown>(x: T): String => x.Loud()\n";
    CHECK(probe(std::format("{}Extend Int Implements Shown\n"
                            "    Method Text(): String => \"int\"\n"
                            "End Extend\n"
                            "Local s = Show(5) + 5.Loud()",
                            shown)) == "String");
    CHECK(probe(std::format("{}Extend T[] Implements Shown\n"
                            "    Method Text(): String => \"items\"\n"
                            "End Extend\n"
                            "Local s = Show([1, 2])",
                            shown)) == "String");
    CHECK(probe(std::format("{}Class Enemy\nEnd Class\n"
                            "Extend Enemy Implements Shown\n"
                            "    Method Text(): String => \"enemy\"\n"
                            "End Extend\n"
                            "Local s: Shown = Enemy()",
                            shown)) == "Shown (from Enemy)");
    CHECK(errors_of(std::format(
              "{}Class Enemy\nEnd Class\nExtend Enemy Implements Shown\nEnd Extend", shown)) ==
          errors_t{ "Shown: Enemy implements Shown, so it needs a Method 'Text'" });
    CHECK(
        errors_of("Extend Int Implements Printable\nEnd Extend") ==
        errors_t{ "Printable: Int32 comes with the built-in Interfaces that fit it; an Extend can "
                  "give it others" });
}

TEST_CASE("what an Extend adds neither overrides nor is overridden", "[sema]") {
    CHECK(errors_of("Class Enemy\nEnd Class\nClass Boss Extends Enemy\nEnd Class\n"
                    "Extend Enemy\n    Method Name(): String => \"enemy\"\nEnd Extend\n"
                    "Extend Boss\n    Method Name(): String => \"boss\"\nEnd Extend") ==
          errors_t{ "Name: 'Name' is already a member of Enemy, which an Extend can't override" });
    CHECK(errors_of("Class Enemy\n    Method Hit(): Int => 1\nEnd Class\n"
                    "Extend Enemy\n    Method Hit(): Int => 2\nEnd Extend") ==
          errors_t{ "Hit: 'Hit' is already declared with the same parameter types" });
}

TEST_CASE("Extend mistakes", "[sema]") {
    CHECK(errors_of("Class Box<T>\n    Field Item: T?\nEnd Class\nExtend Box<U>\nEnd Extend") ==
          errors_t{ "Box<U>: Box is generic, so Extend it with its own type parameters, as Box<T>, "
                    "or with types, as one instance" });
    CHECK(errors_of("Interface I\nEnd Interface\nExtend I\nEnd Extend") ==
          errors_t{ "I: an Interface can't be extended; its defaults go in the Interface" });
    CHECK(errors_of("Extend Int\n"
                    "    Field Extra: Int\n"
                    "    New()\n    End New\n"
                    "    Operator +(a: Int, b: Int): Int => a\n"
                    "    Method ToString(): String => \"x\"\n"
                    "    Override Method M()\n    End Method\n"
                    "End Extend") ==
          errors_t{
              "Extra: an Extend can't add Fields; a Property can compute a value",
              "New: an Extend can't add a New",
              "+: an Extend of Int32 can't add Operators; only a Struct's or Class's can",
              "ToString: Int32 already has a member named 'ToString'",
              "M: what an Extend adds can't be Abstract, Final, or Override; nothing overrides it",
          });
    CHECK(errors_of("Extend String\n    Where T: Printable\nEnd Extend") ==
          errors_t{ "T: 'T' isn't a type parameter here" });
    CHECK(errors_of("Function F()\n    Extend Int\n    End Extend\nEnd Function") ==
          errors_t{ "Extend Int\n    End Extend: an Extend can only be at the top level" });
}

TEST_CASE("a generic Interface's instances have its type arguments in their members", "[sema]") {
    const std::string_view pair = "Interface Pair<A, B>\n"
                                  "    Method First(): A\n"
                                  "    Method Second(): B\n"
                                  "    Method Swapped(): (B, A) => (Second(), First())\n"
                                  "End Interface\n"
                                  "Struct Named Implements Pair<String, Int>\n"
                                  "    Method First(): String => \"a\"\n"
                                  "    Method Second(): Int => 1\n"
                                  "End Struct\n";
    CHECK(probe(std::format("{}Local s = Named().Swapped()", pair)) == "(Int32, String)");
    CHECK(probe(std::format("{}Function F<T: Pair<String, Int>>(x: T): Int => x.Second()\n"
                            "Local n = F(Named())",
                            pair)) == "Int32");
    CHECK(probe("Class Words Implements Iterator<String>\n"
                "    Method Next(): String? => Null\n"
                "End Class\n"
                "Local it: Iterator<String> = Words()\n"
                "Local next = it.Next()") == "String?");

    // A generic type can pass its own type parameters on, and an Interface can extend an
    // instance.
    CHECK(probe("Struct Again<T> Implements Iterator<T>\n"
                "    Field Value: T\n"
                "    ByRef Method Next(): T? => Value\n"
                "End Struct\n"
                "Local a = Again(1.5)\n"
                "Local x = a.Next()") == "Float32?");
    CHECK(errors_of("Interface Numbers Extends Iterator<Int>\nEnd Interface\n"
                    "Function F(n: Numbers): Int? => n.Next()\n"
                    "Function G(n: Numbers): Iterator<Int> => n")
              .empty());
}

TEST_CASE("a constraint's type arguments are inferred from what the type argument implements",
          "[sema]") {
    // E is the Iterator<E> that S implements.
    CHECK(probe("Struct Count Implements Iterator<Int>\n"
                "    ByRef Method Next(): Int? => Null\n"
                "End Struct\n"
                "Function First<S, E>(items: S): E?\n"
                "    Where S: Iterator<E>\n"
                "    Local copy = items\n"
                "    Return copy.Next()\n"
                "End Function\n"
                "Local x = First(Count())") == "Int32?");
    CHECK(errors_of("Function Total<S: Iterator<Int>>(items: S): Int => 0\n"
                    "Class Words Implements Iterator<String>\n"
                    "    Method Next(): String? => Null\n"
                    "End Class\n"
                    "Local t = Total(Words())") ==
          errors_t{ "Total: Words doesn't implement Iterator<Int32>, which 'Total' needs for S" });
}

TEST_CASE("generic Interface mistakes", "[sema]") {
    CHECK(errors_of("Struct Both Implements Iterator<Int>, Iterator<String>\n"
                    "    ByRef Method Next(): Int? => Null\n"
                    "End Struct") ==
          errors_t{ "Iterator<String>: Both implements Iterator<Int32> already; a type implements "
                    "a generic Interface with one set of type arguments" });
    CHECK(errors_of("Class Base Implements Iterator<Int>\n"
                    "    Method Next(): Int? => Null\n"
                    "End Class\n"
                    "Class Derived Extends Base Implements Iterator<String>\n"
                    "End Class") ==
          errors_t{ "Iterator<String>: Derived implements Iterator<Int32> already; a type "
                    "implements a generic Interface with one set of type arguments" });
    CHECK(errors_of("Function F<T>(x: T)\n"
                    "    Where T: Iterator<Int> & Iterator<String>\n"
                    "End Function") ==
          errors_t{ "Iterator<String>: T is Iterator<Int32> already; a type implements a generic "
                    "Interface with one set of type arguments" });
    CHECK(errors_of("Function F(x: Iterator)\nEnd Function") ==
          errors_t{ "Iterator: Iterator<T> is generic, so it needs type arguments, like "
                    "Iterator<Int>" });
    CHECK(errors_of("Function F(x: Iterator<Int, Int>)\nEnd Function") ==
          errors_t{ "Iterator<Int, Int>: Iterator<T> takes 1 type argument, not 2" });
    CHECK(errors_of("Struct Wrong Implements Iterator<Int>\n"
                    "    ByRef Method Next(): String? => Null\n"
                    "End Struct") ==
          errors_t{ "Iterator<Int>: Wrong's 'Next' doesn't match Iterator<Int32>'s, which takes () "
                    "and returns Int32?" });
    CHECK(errors_of("Interface Grow<T> Extends Grow<T[]>\nEnd Interface") ==
          errors_t{ "Grow<T[]>: Grow<T> can't extend itself, directly or through other "
                    "Interfaces" });
    CHECK(errors_of("Interface Picky<T>\n    Where T: Comparable\nEnd Interface\n"
                    "Struct P\n    Field X: Int\nEnd Struct\n"
                    "Function F(x: Picky<P>)\nEnd Function") ==
          errors_t{ "Picky<P>: P doesn't implement Comparable, which Picky<T> needs for T" });
}

TEST_CASE("For ... In goes over an Iterator, or the one a value's Iterate gives", "[sema]") {
    const std::string_view count = "Struct Count Implements Iterator<Int>\n"
                                   "    Field N: Int\n"
                                   "    ByRef Method Next(): Int? => Null\n"
                                   "End Struct\n";
    CHECK(errors_of(std::format("{}For x In Count(3)\n    Local s: String = x\nEnd For", count)) ==
          errors_t{ "x: expected String, found Int32" });
    CHECK(errors_of(std::format("{}Class Bag\n"
                                "    Method Iterate(): Count => Count(2)\n"
                                "End Class\n"
                                "For x In Bag()\n    Local s: String = x\nEnd For",
                                count)) == errors_t{ "x: expected String, found Int32" });
    CHECK(errors_of("Function F(it: Iterator<Int>)\n"
                    "    For x In it\n"
                    "        Local s: String = x\n"
                    "    End For\n"
                    "End Function") == errors_t{ "x: expected String, found Int32" });

    // Two names take each pair apart, like a Map's keys and values.
    CHECK(errors_of("Struct Pairs Implements Iterator<(String, Int)>\n"
                    "    ByRef Method Next(): (String, Int)? => Null\n"
                    "End Struct\n"
                    "For key, value In Pairs()\n"
                    "    Local n: String = value\n"
                    "End For") == errors_t{ "value: expected String, found Int32" });
    CHECK(errors_of(std::format("{}For i, x In Count(3)\nEnd For", count)) ==
          errors_t{ "i: 'For a, b In' takes each of Count's elements apart, so they must be pairs, "
                    "not Int32; an index only comes with arrays" });
    CHECK(errors_of("Function F(s: String?)\n    For c In s\n    End For\nEnd Function") ==
          errors_t{ "s: 'For ... In' goes over arrays, Iterators, and values whose Iterate() gives "
                    "an Iterator, not String?" });
}

TEST_CASE("a String's Chars and Bytes are arrays of its code points and bytes", "[sema]") {
    CHECK(probe("Local c = \"abc\".Chars") == "Int32[]");
    CHECK(probe("Local b = \"abc\".Bytes") == "UInt8[]");
    CHECK(probe("Function F(s: String?): Int[]? => s?.Chars\nLocal c = F(Null)") == "Int32[]?");
    CHECK(errors_of("Local c = \"abc\".Chars()") ==
          errors_t{ "Chars: Chars isn't a Method; leave out the ()" });
    CHECK(errors_of("Function F(s: String?)\n    Local c = s.Bytes\nEnd Function") ==
          errors_t{ "s: 's' may be Null; check that it isn't, or use '?.'" });
    CHECK(errors_of("Extend String\n    Method Chars(): Int => 0\nEnd Extend") ==
          errors_t{ "Chars: String already has a member named 'Chars'" });
}

TEST_CASE("a slice is a new array of some of an array's elements", "[sema]") {
    CHECK(probe("Local s = [1, 2, 3][1..]") == "Int32[]");
    CHECK(probe("Local a: Int[3] = [1, 2, 3]\nLocal s = a[..2]") == "Int32[]");
    CHECK(probe("Local a = [\"x\"]\nLocal n: Long = 1\nLocal s = a[0..n]") == "String[]");
    CHECK(errors_of("Local a = [1, 2, 3]\nLocal s = a[2..1]") ==
          errors_t{ "a[2..1]: the slice 2..1 ends before it starts" });
    CHECK(errors_of("Local a = [1, 2, 3]\nLocal s = a[-1..]") ==
          errors_t{ "-1: a slice's bounds can't be negative" });
    CHECK(errors_of("Local a: Int[3] = [1, 2, 3]\nLocal s = a[..4]") ==
          errors_t{ "4: 4 is past the end of Int32[3]" });
    CHECK(errors_of("Local g: Int[2, 2]\nLocal s = g[0..1]") ==
          errors_t{ "g: Int32[2, 2] can't be sliced; a slice goes over one dimension" });
    CHECK(errors_of("Local s = \"abc\"[0..1]") == errors_t{ "\"abc\": String can't be sliced" });
    CHECK(errors_of("Local a = [1]\nLocal s = a[0.5..]") ==
          errors_t{ "0.5: a slice's bounds must be integers, not Float32" });
    CHECK(errors_of("Local a = [1]\na[0..1] = [2]") ==
          errors_t{ "a[0..1]: only a variable can be assigned to" });
}

TEST_CASE("a variadic parameter takes the rest of the arguments as an array", "[sema]") {
    const std::string_view sum = "Function Sum(nums: Int...): Int => nums.Count\n";
    CHECK(probe(std::format("{}Local n = Sum()", sum)) == "Int32");
    CHECK(probe(std::format("{}Local n = Sum(1, 2, 3)", sum)) == "Int32");
    CHECK(probe(std::format("{}Local xs = [1, 2]\nLocal n = Sum(xs)", sum)) == "Int32");
    CHECK(probe(std::format("{}Local n = Sum([1, 2])", sum)) == "Int32");
    CHECK(probe(std::format("{}Local n = Sum(nums: [1, 2])", sum)) == "Int32");
    CHECK(probe("Function Largest<T: Comparable>(first: T, rest: T...): T => first\n"
                "Local x = Largest(1.5, 2, 3)") == "Float32");
    CHECK(errors_of(std::format("{}Local n = Sum(1, \"x\")", sum)) ==
          errors_t{ "\"x\": expected Int32, found String" });
    CHECK(errors_of(std::format("{}Local x = 1\nLocal n = Sum(ByRef x)", sum)) ==
          errors_t{ "x: 'nums' takes the rest of the arguments, which can't be ByRef" });
    CHECK(errors_of(std::format("{}Local n = Sum(1, nums: [2])", sum)) ==
          errors_t{ "[2]: parameter 'nums' already has an argument" });
}

TEST_CASE("variadic parameter mistakes", "[sema]") {
    CHECK(errors_of("Function F(xs: Int..., y: Int)\nEnd Function") ==
          errors_t{ "xs: only the last parameter can take the rest of the arguments" });
    CHECK(errors_of("Function F(ByRef xs: Int...)\nEnd Function") ==
          errors_t{ "xs: 'xs' takes the rest of the arguments, so it can't be ByRef" });
    CHECK(errors_of("Function F(xs: Int... = [])\nEnd Function") ==
          errors_t{ "[]: 'xs' takes the rest of the arguments, so it has no default; without "
                    "any, it's an empty array" });
}

TEST_CASE("@Deprecated warns at each use outside what it's on", "[sema]") {
    CHECK(errors_of("@Deprecated(\"use G\") Function F(): Int => 1\n"
                    "Function G(): Int => F()\n"
                    "Local x = F()") ==
          errors_t{ "F: 'F' is deprecated: use G", "F: 'F' is deprecated: use G" });
    CHECK(errors_of("@Deprecated Struct Old\n    Field X: Int\nEnd Struct\nLocal o = Old(1)") ==
          errors_t{ "Old: 'Old' is deprecated" });
    CHECK(errors_of("Class E\n"
                    "    @Deprecated Method Hit()\n"
                    "    End Method\n"
                    "    Method Use()\n"
                    "        Hit()\n"
                    "    End Method\n"
                    "End Class\n"
                    "E().Hit()") == errors_t{ "Hit: 'E.Hit' is deprecated" });
    CHECK(errors_of("@Deprecated Const LIMIT = 3\nLocal x = LIMIT") ==
          errors_t{ "LIMIT: 'LIMIT' is deprecated" });
    CHECK(errors_of("@Deprecated Enum Mood\n    Calm\nEnd Enum\nLocal m = Mood.Calm") ==
          errors_t{ "Mood: 'Mood' is deprecated" });
}

TEST_CASE("attribute mistakes", "[sema]") {
    CHECK(errors_of("@Inline Struct S\nEnd Struct") ==
          errors_t{ "S: @Inline only goes before a Function, Method, Operator, or Property" });
    CHECK(errors_of("@Inline Global g = 1") ==
          errors_t{ "g: @Inline only goes before a Function, Method, Operator, or Property" });
    CHECK(errors_of("Abstract Class C\n    @Inline Abstract Method M()\nEnd Class") ==
          errors_t{ "M: 'M' has no body, so @Inline has nothing to inline" });
    CHECK(errors_of("Function F()\n    @Deprecated Local x = 1\nEnd Function") ==
          errors_t{ "x: a Local can't be @Deprecated, since only its own code uses it" });
}

TEST_CASE("a Const known at compile time can be used before its line", "[sema]") {
    CHECK(probe("Const A = B * 2\nConst B = 3\nLocal x = A") == "Int32 6");
    CHECK(probe("Struct S\n    Field a: Int[N]\nEnd Struct\n"
                "Function F(x: Int[N]): Int => x.Count\n"
                "Const N = 4\n"
                "Local x = S().a.Count") == "Int32 4");
    CHECK(errors_of("Const A = B\nConst B = A") ==
          errors_t{ "A: 'A' needs its own value to be worked out" });
    CHECK(errors_of("Function Spot(): Int => 4\nConst Z = Spot()\n"
                    "Struct T\n    Field b: Int[Z]\nEnd Struct") ==
          errors_t{ "Z: a fixed array's size must be an integer known at compile time, like 64 "
                    "or 8 * 8" });
    CHECK(errors_of("Global g = Z\nConst Z = Spot()\nFunction Spot(): Int => 4") ==
          errors_t{ "Z: 'Z' isn't initialized yet; Globals and Consts initialize in source "
                    "order" });
}

TEST_CASE("a call of a Const Function runs at compile time when its arguments are known",
          "[sema]") {
    const std::string_view fib = "Const Function Fib(n: Int): Int\n"
                                 "    Return n < 2 ? n : Fib(n - 1) + Fib(n - 2)\n"
                                 "End Function\n";
    CHECK(probe(std::format("{}Const X = Fib(10)", fib)) == "Int32 55");
    CHECK(probe(std::format("{}Local x = Fib(10) + 1", fib)) == "Int32 56");
    CHECK(probe(std::format("{}Local n = 3\nLocal x = Fib(n)", fib)) == "Int32");
    CHECK(probe(std::format("{}Local t: Int[Fib(5)]\nLocal x = t.Count", fib)) == "Int32 5");
    CHECK(probe("Const Function Shout(s: String): String => s + \"!\"\nLocal x = Shout(\"hi\")") ==
          "String hi!");
    CHECK(probe("Struct P\n    Field X: Int\nEnd Struct\n"
                "Const Function Get(): Int\n    Local p = P(4)\n    Return p.X\nEnd Function\n"
                "Local x = Get()") == "Int32 4");
    CHECK(probe("Const Function Count(n: Int): Int\n"
                "    Local seen: Bool[10]\n"
                "    For i = 0 Until n\n"
                "        seen[i] = True\n"
                "    End For\n"
                "    Local total = 0\n"
                "    For Each = 0 Until 10\n"
                "        If seen[Each] Then total += 1\n"
                "    End For\n"
                "    Return total\n"
                "End Function\n"
                "Local x = Count(4)") == "Int32 4");
}

TEST_CASE("what a Const Function can't do", "[sema]") {
    CHECK(errors_of("Global g = 1\n"
                    "Class Box\nEnd Class\n"
                    "Function Plain(): Int => 1\n"
                    "Const Function F(): Int\n"
                    "    Print(\"x\")\n"
                    "    Local a = [1, 2]\n"
                    "    Return g + Plain()\n"
                    "End Function") ==
          errors_t{
              "Print: a Const Function can only call Const Functions, and 'Print' isn't one",
              "[1, 2]: a Const Function can't use Int32[], which only the running program has",
              "g: a Const Function can't use Globals, which only the running program has",
              "Plain: a Const Function can only call Const Functions, and 'Plain' isn't one",
          });
    CHECK(errors_of("Const Function F<T>(x: T[]): Int => 1") ==
          errors_t{ "x: a Const Function takes numbers, Bools, Strings, and Enums, not T[]" });
    CHECK(errors_of("Const Function F()\nEnd Function") ==
          errors_t{ "F: a Const Function gives a value, so it needs a result type" });
    CHECK(errors_of("Const Function F(ByRef x: Int): Int => x") ==
          errors_t{ "x: a Const Function's parameters can't be ByRef" });
    CHECK(errors_of("Const Function F(x: Int[]): Int => 1") ==
          errors_t{ "x: a Const Function takes numbers, Bools, Strings, and Enums, not Int32[]" });
}

TEST_CASE("a Const Function that fails at compile time is an error at its call", "[sema]") {
    CHECK(errors_of("Const Function Div(a: Int, b: Int): Int => a / b\nConst X = Div(1, 0)") ==
          errors_t{ "Div(1, 0): 'Div' can't run at compile time: division by zero" });
    CHECK(
        errors_of("Const Function Check(n: Int): Int\n"
                  "    Assert(n > 0, \"n must be positive\")\n"
                  "    Return n\n"
                  "End Function\n"
                  "Const X = Check(-1)") ==
        errors_t{ "Check(-1): 'Check' can't run at compile time: it panics: n must be positive" });
    CHECK(errors_of("Const Function Forever(): Int\n"
                    "    Local x = 0\n"
                    "    While True\n"
                    "        x += 1\n"
                    "        If x == -5 Then Break\n"
                    "    End While\n"
                    "    Return x\n"
                    "End Function\n"
                    "Const X = Forever()") ==
          errors_t{ "Forever(): 'Forever' can't run at compile time: it runs more than 10000000 "
                    "steps" });
    CHECK(errors_of("Struct S\n"
                    "    Field a: Int[Width()]\n"
                    "End Struct\n"
                    "Struct P\n"
                    "    Field X: Int\n"
                    "End Struct\n"
                    "Const Function Width(): Int\n"
                    "    Local p = P(3)\n"
                    "    Return p.X\n"
                    "End Function") ==
          errors_t{ "Width(): this Const Function can't run here, since its code needs types that "
                    "aren't declared yet" });
}

TEST_CASE("a Const type parameter stands for an integer", "[sema]") {
    const std::string_view grid = "Struct Grid<Const W: Int, Const H: Int>\n"
                                  "    Field Cells: Float[W, H]\n"
                                  "    Method Size(): Int => W * H\n"
                                  "End Struct\n";
    CHECK(probe(std::format("{}Local g = Grid<3, 2>()", grid)) == "Grid<3, 2>");
    CHECK(probe(std::format("{}Const N = 4\nLocal g = Grid<N, N>()\nLocal x = g.Cells", grid)) ==
          "Float32[4, 4]");
    CHECK(probe("Function Sum<Const N: Int>(v: Int[N]): Int => N\n"
                "Local a: Int[3]\n"
                "Local x = Sum(a)") == "Int32");
}

TEST_CASE("Const type parameter mistakes", "[sema]") {
    const std::string_view grid = "Struct Grid<Const W: Int, Const H: Int>\n"
                                  "    Field Cells: Float[W, H]\n"
                                  "End Struct\n";
    CHECK(
        errors_of("Struct Bad<Const F: Float>\nEnd Struct") ==
        errors_t{ "Float: a Const type parameter is an integer, like Const N: Int, not Float32" });
    CHECK(errors_of("Function F<Const N: Int>()\n    Where N: Printable\nEnd Function") ==
          errors_t{ "N: 'N' is Const, so it has no constraints" });
    CHECK(errors_of("Function F<Const N: Int>()\n    Local a: Int[N] = [1, 2]\nEnd Function") ==
          errors_t{ "[1, 2]: Int32[N] can't come from an array literal, since its size is a Const "
                    "type parameter" });
    CHECK(errors_of("Interface I<Const N: Int>\n    Method Size(): Int => N\nEnd Interface\n"
                    "Struct Row Implements I<0>\nEnd Struct") ==
          errors_t{ "0: N is a size, from 1 to 2147483647, not 0" });
    CHECK(errors_of(std::format("{}Local a = Grid<Int, 2>()", grid)) ==
          errors_t{ "Int: W is Const, so it takes an integer known at compile time, like 64, not "
                    "Int" });
    CHECK(errors_of(std::format("{}Local a = Grid<-1, 2>()", grid)) ==
          errors_t{ "-1: W is a size, from 1 to 2147483647, not -1" });
    CHECK(errors_of("Struct Box<T>\n    Field Item: T?\nEnd Struct\nLocal b = Box<4>()") ==
          errors_t{ "4: T is a type, so it takes one, not a value" });
    CHECK(errors_of("Local f: 5 = 1") == errors_t{ "5: 5 is a value, not a type" });
}

TEST_CASE("a Struct's Method implements a ByRef Method only when it's ByRef too", "[sema]") {
    CHECK(errors_of("Interface Counter\n    ByRef Method Bump()\nEnd Interface\n"
                    "Struct S Implements Counter\n    Method Bump()\n    End Method\nEnd Struct") ==
          errors_t{ "Counter: 'Bump' must be a ByRef Method to implement Counter's, which changes "
                    "its value" });
    CHECK(errors_of("Interface Counter\n    ByRef Method Bump()\nEnd Interface\n"
                    "Class C Implements Counter\n    Method Bump()\n    End Method\nEnd Class")
              .empty());
}

TEST_CASE("an attribute's arguments are values known at compile time", "[sema]") {
    const std::string_view check = "Const Limit = 10\n"
                                   "Attribute Check(Max: Int = Limit, Label: String = \"\")\n";
    CHECK(errors_of(std::format("{}@Check(Limit * 2, Label: \"size\") Global size = 1", check))
              .empty());
    CHECK(errors_of(std::format("{}@Check Function F()\nEnd Function", check)).empty());
    CHECK(errors_of("Attribute Tag(Code: Int)\n@Tag Function F()\nEnd Function") ==
          errors_t{ "Tag: missing an argument for parameter 'Code'" });
    CHECK(errors_of(std::format("{}@Check(1, Max: 2) Global g = 1", check)) ==
          errors_t{ "2: parameter 'Max' already has an argument" });
    CHECK(errors_of(std::format("{}@Check(1, \"a\", 3) Global g = 1", check)) ==
          errors_t{ "3: too many arguments to '@Check'" });
    CHECK(errors_of(std::format("{}@Check(Min: 1) Global g = 1", check)) ==
          errors_t{ "Min: '@Check' has no parameter named 'Min'" });
    CHECK(errors_of(std::format("{}@Check(\"no\") Global g = 1", check)) ==
          errors_t{ "\"no\": expected Int32, found String" });
    CHECK(errors_of(std::format("{}Function Max(): Int => 3\n@Check(Max()) Global g = 1", check)) ==
          errors_t{ "Max(): an attribute's arguments must be known at compile time" });
    CHECK(errors_of(std::format("{}@Check @Check Global g = 1", check)) ==
          errors_t{ "Check: @Check is written twice" });
}

TEST_CASE("attribute declaration mistakes", "[sema]") {
    CHECK(errors_of("@Missing Function F()\nEnd Function") ==
          errors_t{ "Missing: '@Missing' isn't an attribute; an Attribute declaration makes one" });
    CHECK(errors_of("Struct Missing\nEnd Struct\n@Missing Function F()\nEnd Function") ==
          errors_t{ "Missing: '@Missing' isn't an attribute; an Attribute declaration makes one" });
    CHECK(errors_of("Attribute Inline") == errors_t{ "Inline: @Inline is a built-in attribute" });
    CHECK(errors_of("Attribute Bad(Items: Int[])") ==
          errors_t{ "Items: an Attribute's parameters are numbers, Bools, Strings, and Enums, not "
                    "Int32[]" });
    CHECK(errors_of("Function Two(): Int => 2\nAttribute Bad(Code: Int = Two())") ==
          errors_t{ "Two(): a field's default must be known at compile time" });
    CHECK(errors_of("Attribute Tag\nFunction F()\n    @Tag Local x = 1\nEnd Function") ==
          errors_t{ "x: a Local can't have attributes, since only its own code uses it" });
}

TEST_CASE("For Field goes over a type's fields, with a type parameter for each one's type",
          "[sema]") {
    const std::string_view show = "Function Show<T>(value: T)\n"
                                  "    For Field f: F In T\n"
                                  "        Where F: Printable\n"
                                  "        Print(\"{f.Name} {f.Index}: {f.Get(value)}\")\n"
                                  "    End For\n"
                                  "End Function\n";
    CHECK(errors_of(std::format("{}Struct P\n    Field X: Int\nEnd Struct\nShow(P(1))", show))
              .empty());
    CHECK(errors_of(std::format("{}Show((1, \"two\"))\nShow(5)", show)).empty());
    CHECK(errors_of(std::format("{}Struct Q\n    Field A: Int\nEnd Struct\n"
                                "Struct H\n    Field Items: Int[]\n    Field Inner: Q\nEnd Struct\n"
                                "Show(H([1], Q(1)))",
                                show)) ==
          errors_t{ "Show: 'Show' needs each field of H to implement Printable, but its field "
                    "'Inner' is Q, which doesn't" });

    // A type parameter's fields meet a constraint only when its own loops say so.
    CHECK(errors_of(
              std::format("{}Function Pass<U>(value: U)\n    Show(value)\nEnd Function", show)) ==
          errors_t{ "Show: 'Show' needs each field of U to implement Printable, but nothing says "
                    "that of U's fields" });
    CHECK(errors_of(std::format("{}Function Pass<U>(value: U)\n"
                                "    For Field f: F In U\n"
                                "        Where F: Printable\n"
                                "    End For\n"
                                "    Show(value)\n"
                                "End Function",
                                show))
              .empty());
    // A Const Function can count them at compile time.
    CHECK(probe("Struct P\n    Field X: Int\n    Field Y: Bool\nEnd Struct\n"
                "Const Function Count(): Int\n"
                "    Local n = 0\n"
                "    For Field f In P\n"
                "        n += 1\n"
                "    End For\n"
                "    Return n\n"
                "End Function\n"
                "Const FIELDS = Count()") == "Int32 2");
    CHECK(errors_of("Struct P\n    Field X: Int\n    Field Name: String\nEnd Struct\n"
                    "For Field f: F In P\n    Where F: Numeric\nEnd For") ==
          errors_t{ "P: P's field 'Name' is String, which doesn't implement Numeric, as this loop "
                    "needs" });
}

TEST_CASE("Set changes a field, and For Method calls Methods that take nothing", "[sema]") {
    CHECK(errors_of("Function Clear<T>(ByRef value: T)\n"
                    "    For Field f: F In T\n"
                    "        Where F: Numeric\n"
                    "        f.Set(ByRef value, F.Zero())\n"
                    "    End For\n"
                    "End Function")
              .empty());
    CHECK(errors_of("Function Clear<T>(value: T)\n"
                    "    For Field f: F In T\n"
                    "        Where F: Numeric\n"
                    "        f.Set(ByRef value, F.Zero())\n"
                    "    End For\n"
                    "End Function") ==
          errors_t{ "value: 'value' is a parameter and can't be passed ByRef; copy it into a "
                    "Local, or make it ByRef" });
    CHECK(errors_of("Attribute Tag\n"
                    "Struct S\n    @Tag Method M()\n    End Method\nEnd Struct\n"
                    "For Method m In S\n"
                    "    If m.Has(Tag) Then m.Call(S())\n"
                    "End For")
              .empty());
}

TEST_CASE("what a For Field's or For Method's variable can't do", "[sema]") {
    const std::string_view point = "Struct P\n    Field X: Int\nEnd Struct\n";
    CHECK(errors_of(std::format("{}Function F<T>(value: T)\n"
                                "    For Field f In T\n"
                                "        Print(f.Get(value))\n"
                                "    End For\n"
                                "End Function",
                                point)) ==
          errors_t{ "f.Get(value): expected String, found the type of f" });
    CHECK(errors_of(std::format("{}For Field f In P\n    Print(f)\nEnd For", point)) ==
          errors_t{ "f: 'f' is a field of P; it has Name, Index, Get(value), Set(ByRef value, x), "
                    "Has(A), and Attribute(A)" });
    CHECK(errors_of(std::format("{}For Method m In P\n    Print(m.Index)\nEnd For", point)) ==
          errors_t{ "Index: 'm' is a Method of P; it has Name, Has(A), Attribute(A), and "
                    "Call(value, ...)" });
    CHECK(errors_of(std::format("{}For Field f In P\n    f.Name = \"x\"\nEnd For", point)) ==
          errors_t{ "f.Name: only a variable or one of its fields can be assigned to" });
    // Each copy of the body has its own lambda.
    CHECK(errors_of(
              std::format("{}For Field f In P\n    Local g = Function() => f.Name\nEnd For", point))
              .empty());
    CHECK(
        errors_of(std::format("{}For Field f In P\n    Print(\"{{f.Has(P)}}\")\nEnd For", point)) ==
        errors_t{ "P: 'f.Has' takes the name of an Attribute, which an Attribute declaration "
                  "makes" });
    CHECK(errors_of(std::format("{}For Method m In P\n    m.Call(1)\nEnd For", point)) ==
          errors_t{ "1: expected P, found Int32" });
    CHECK(errors_of(std::format("{}For Method m In P\n    Where X: Printable\nEnd For", point)) ==
          errors_t{ "X: a For Method loop names the Methods' Func type, as 'For Method m: M In T', "
                    "to give it a Where line" });
    CHECK(errors_of(std::format("{}For Method m: M In P\nEnd For", point)) ==
          errors_t{ "M: a For Method loop says which Methods it goes over with a Where line, like "
                    "'Where M: Func(Int): String'" });
}

TEST_CASE("a Map literal is a Map of the expected type, or of its entries' common types",
          "[sema]") {
    CHECK(probe("Local m = {\"a\": 1, \"b\": 2}") == "Map<String, Int32>");
    CHECK(probe("Local m = {1: 1.5, 2: 2}") == "Map<Int32, Float32>");
    CHECK(probe("Local m: Map<String, Long> = {\"a\": 1}\nLocal n = m") == "Map<String, Int64>");
    CHECK(probe("Local m: Map<Int, String>? = {}") ==
          "Map<Int32, String>? (from Map<Int32, String>)");
    CHECK(errors_of("Function Total(scores: Map<String, Int>): Int => scores.Count\n"
                    "Local t = Total({})")
              .empty());
}

TEST_CASE("Map literal mistakes", "[sema]") {
    CHECK(errors_of("Local m = {}") ==
          errors_t{ "{}: an empty Map needs a type from where it's used, like 'Local m: "
                    "Map<String, Int> = {}'" });
    CHECK(errors_of("Local m = {\"a\": 1, 2: 3}") ==
          errors_t{ "2: the keys of a Map have different types, String and Int32" });
    CHECK(errors_of("Local m = {1: \"a\", 2: True}") ==
          errors_t{ "True: the values of a Map have different types, String and Bool" });
    CHECK(errors_of("Struct P\n    Field X: Int\nEnd Struct\nLocal m = {P(1): 2}") ==
          errors_t{ "{P(1): 2}: P doesn't implement Hashable, which Map<K, V> needs for K" });
    CHECK(errors_of("Local m: Map<String, Int> = {\"a\": \"b\"}") ==
          errors_t{ "\"b\": expected Int32, found String" });
    CHECK(errors_of("Local m: Int = {\"a\": 1}") ==
          errors_t{ "{\"a\": 1}: expected Int32, found Map<String, Int32>" });
}

TEST_CASE("an Extend of T[] uses the array's Count and Methods by name", "[sema]") {
    CHECK(errors_of("Extend T[]\n"
                    "    Method Last(): T => Self[Count - 1]\n"
                    "    Method Drop()\n"
                    "        RemoveAt(Count - 1)\n"
                    "    End Method\n"
                    "    Method Later(): Func(): Int => Function() => Count + Clone().Count\n"
                    "End Extend\n"
                    "Local a = [1, 2]\n"
                    "a.Drop()\n"
                    "Local last = a.Last()")
              .empty());
    CHECK(errors_of("Extend T[]\n    Method M()\n        Count = 1\n    End Method\nEnd Extend") ==
          errors_t{ "Count: Count is how many elements Self has, which only its Methods "
                    "change" });
}

TEST_CASE("a call's lambda argument can declare symbols while the call is checked", "[sema]") {
    // Its parameters are new symbols; checking the call keeps working on the parameter it's for.
    CHECK(probe("Local nums = [5, 3, 8]\n"
                "Local text = \"{nums.Filter(Function(n: Int) => n % 2 == 1)}\"") == "String");
}

TEST_CASE("narrowing is forgotten for what a loop changes inside any expression", "[sema]") {
    CHECK(errors_of("Function Clear(ByRef s: String?): Int\n"
                    "    s = Null\n"
                    "    Return 0\n"
                    "End Function\n"
                    "Function Pick(): String? => \"a\"\n"
                    "Local s = Pick()\n"
                    "If s != Null\n"
                    "    For i = 0 Until 2\n"
                    "        Print(s)\n"
                    "        Local a = [Clear(ByRef s)]\n"
                    "    End For\n"
                    "End If") ==
          errors_t{ "s: expected String, found String?, which may be Null; check it first, or give "
                    "a value for Null with '?"
                    "?'" });
}

TEST_CASE("a name that a later milestone declares isn't supported yet, wherever it's used",
          "[sema]") {
    CHECK(errors_of("Local t: Task") == errors_t{ "Task: 'Task' isn't supported yet" });
    CHECK(errors_of("Function F<T>()\n    Where T: Sendable\nEnd Function") ==
          errors_t{ "Sendable: 'Sendable' isn't supported yet" });
    CHECK(errors_of("Thread.Start()") == errors_t{ "Thread: 'Thread' isn't supported yet" });
    CHECK(errors_of("Local v = Vec3(1, 2, 3)") == errors_t{ "Vec3: 'Vec3' isn't supported yet" });

    // A program can still declare one of them.
    CHECK(errors_of("Function Spawn(): Int => 1\nLocal s = Spawn()").empty());
}

TEST_CASE("a generic type's field default can name its type parameters", "[sema]") {
    CHECK(errors_of("Struct S<T>\n"
                    "    Where T: Hashable\n"
                    "    Field M: Map<T, Bool> = Map<T, Bool>()\n"
                    "End Struct") ==
          errors_t{ "Map<T, Bool>(): a field's default must be known at compile time" });
}

TEST_CASE("an integer's Wrapping and Checked Methods say what overflow does", "[sema]") {
    CHECK(probe("Local x = (250 As Byte).WrappingAdd(10)") == "UInt8 4");
    CHECK(probe("Local x = (-128 As Int8).WrappingNegate()") == "Int8 -128");
    CHECK(probe("Local x = 100.CheckedMultiply(3)") == "Int32? 300");
    CHECK(probe("Local x = 2147483647.CheckedAdd(1)") == "Int32? Null");
    CHECK(probe("Local x = 7.CheckedDivide(0)") == "Int32? Null");
    CHECK(probe("Local a = 1\nLocal x = a.CheckedSubtract(2)") == "Int32?");
    CHECK(probe("Const Function Twice(n: Int): Int => n.WrappingMultiply(2)\n"
                "Const X = Twice(2147483647)") == "Int32 -2");
}

TEST_CASE("integer Method mistakes", "[sema]") {
    CHECK(errors_of("Local x = 1.WrappingDivide(2)") ==
          errors_t{ "WrappingDivide: Int32 has no member named 'WrappingDivide'" });
    CHECK(errors_of("Local x = (1.5).CheckedAdd(1)") ==
          errors_t{ "CheckedAdd: Float32 has no member named 'CheckedAdd'" });
    CHECK(errors_of("Extend Int\n    Method CheckedAdd(x: Int): Int? => x\nEnd Extend") ==
          errors_t{ "CheckedAdd: Int32 already has a member named 'CheckedAdd'" });
}

TEST_CASE("an Interface's type parameters can have constraints and be Const", "[sema]") {
    CHECK(errors_of("Interface Sorted<T: Comparable>\n"
                    "    Method Smallest(): T\n"
                    "    Method Less(a: T, b: T): Bool => a < b\n"
                    "End Interface\n"
                    "Struct Pair<T> Implements Sorted<T>\n"
                    "    Where T: Comparable\n"
                    "    Field A: T\n"
                    "    Field B: T\n"
                    "    Method Smallest(): T => A < B ? A : B\n"
                    "End Struct\n"
                    "Function Least<S: Sorted<E>, E: Comparable>(s: S): E => s.Smallest()\n"
                    "Local x = Least(Pair(3, 2))")
              .empty());
    CHECK(probe("Interface Sized<Const N: Int>\n    Method Size(): Int => N\nEnd Interface\n"
                "Struct Row Implements Sized<4>\nEnd Struct\n"
                "Local n = Row().Size()") == "Int32");
}

TEST_CASE("an Interface's Methods can throw, and be generic", "[sema]") {
    const std::string_view parser = "Class ParseError Extends Error\nEnd Class\n"
                                    "Interface Parser\n"
                                    "    Method Parse(text: String): Int Throws ParseError\n"
                                    "End Interface\n";
    CHECK(errors_of(std::format("{}Class Lenient Implements Parser\n"
                                "    Method Parse(text: String): Int => 0\n"
                                "End Class\n"
                                "Function Use(p: Parser): Int => Try? p.Parse(\"x\") ?? -1",
                                parser))
              .empty());
    CHECK(errors_of(std::format("{}Class Loud Implements Parser\n"
                                "    Method Parse(text: String): Int Throws Error\n"
                                "        Return 0\n"
                                "    End Method\n"
                                "End Class",
                                parser)) ==
          errors_t{ "Parser: 'Parse' can't throw Error, since Parser's doesn't" });
    CHECK(errors_of(std::format("{}Function Use(p: Parser): Int => p.Parse(\"x\")", parser)) ==
          errors_t{ "p.Parse(\"x\"): 'Parse' can throw ParseError, so calling it needs Try, "
                    "Try?, or a Try block" });

    const std::string_view mapper = "Interface Mapper\n"
                                    "    Method Apply<U>(f: Func(Int): U): U\n"
                                    "End Interface\n";
    CHECK(errors_of(std::format("{}Struct Box Implements Mapper\n"
                                "    Method Apply<U>(f: Func(Int): U): U => f(1)\n"
                                "End Struct\n"
                                "Function Twice<T: Mapper>(m: T): Int => m.Apply(Function(x: Int) "
                                "=> x * 2)",
                                mapper))
              .empty());
    CHECK(errors_of(std::format("{}Struct Picky Implements Mapper\n"
                                "    Method Apply<U: Printable>(f: Func(Int): U): U => f(1)\n"
                                "End Struct",
                                mapper)) ==
          errors_t{ "Mapper: 'Apply' needs U to implement Printable, which Mapper's doesn't, so it "
                    "can't implement it" });
    CHECK(errors_of(std::format("{}Function Through(m: Mapper): Int => m.Apply(Function(x: Int) => "
                                "x)",
                                mapper)) ==
          errors_t{ "m.Apply: 'Apply' is generic, so it's called on a type parameter, like <T: "
                    "Mapper>, not on an Interface's value" });
}

TEST_CASE("For ... In Try passes on the errors of a throwing Iterate", "[sema]") {
    const std::string_view levels = "Class LoadError Extends Error\nEnd Class\n"
                                    "Struct None Implements Iterator<Int>\n"
                                    "    ByRef Method Next(): Int? => Null\n"
                                    "End Struct\n"
                                    "Class Levels\n"
                                    "    Method Iterate(): None Throws LoadError => None()\n"
                                    "End Class\n";
    CHECK(errors_of(std::format("{}Function Count(l: Levels): Int Throws LoadError\n"
                                "    For n In Try l\n"
                                "    End For\n"
                                "    Return 0\n"
                                "End Function\n"
                                "Try\n"
                                "    For n In Levels()\n"
                                "    End For\n"
                                "Catch e As LoadError\n"
                                "End Try",
                                levels))
              .empty());
    CHECK(errors_of(std::format("{}Function Count(l: Levels): Int\n"
                                "    For n In l\n"
                                "    End For\n"
                                "    Return 0\n"
                                "End Function",
                                levels)) ==
          errors_t{ "l: Levels's Iterate can throw LoadError, so the loop needs 'In Try l', or a "
                    "Try block around it" });
    CHECK(errors_of("For n In Try [1, 2]\nEnd For") ==
          errors_t{ "Try [1, 2]: '[1, 2]' doesn't throw, so it doesn't need Try" });
}

TEST_CASE("an Extend can add to one instance, or one array type", "[sema]") {
    const std::string_view box = "Struct Box<T>\n    Field Value: T\nEnd Struct\n";
    CHECK(
        probe(std::format("{}Extend Box<Int>\n    Method Doubled(): Int => Value * 2\nEnd Extend\n"
                          "Local x = Box(4).Doubled()",
                          box)) == "Int32");
    CHECK(errors_of(std::format("{}Extend Box<Int>\n    Method Doubled(): Int => Value * 2\n"
                                "End Extend\nLocal x = Box(\"a\").Doubled()",
                                box)) ==
          errors_t{ "Doubled: Box<String> has no member named 'Doubled'" });
    CHECK(probe("Extend Int[]\n    Method Total(): Int => Count\nEnd Extend\n"
                "Local x = [1, 2].Total()") == "Int32");
    CHECK(errors_of("Extend Int[]\n    Method Total(): Int => Count\nEnd Extend\n"
                    "Local x = [\"a\"].Total()") ==
          errors_t{ "Total: String[] has no member named 'Total'" });
    CHECK(probe("Extend T[]\n    Property Second: T\n        Get => Self[1]\n    End Property\n"
                "End Extend\nLocal x = [\"a\", \"b\"].Second") == "String");
}

TEST_CASE("a Class can extend an instance of a generic Class", "[sema]") {
    const std::string_view container = "Class Container<T>\n"
                                       "    Field Items: T[] = []\n"
                                       "    Method Add(item: T)\n"
                                       "        Items.Add(item)\n"
                                       "    End Method\n"
                                       "    Method First(): T => Items[0]\n"
                                       "End Class\n";
    CHECK(probe(std::format("{}Class Names Extends Container<String>\n"
                            "    Method Last(): String => Items[Items.Count - 1]\n"
                            "End Class\n"
                            "Local x = Names().First()",
                            container)) == "String");
    CHECK(probe(std::format("{}Class Sorted<T> Extends Container<T>\n"
                            "    Where T: Comparable\n"
                            "    Override Method Add(item: T)\n"
                            "        Items.Insert(0, item)\n"
                            "    End Method\n"
                            "End Class\n"
                            "Local c: Container<Int> = Sorted<Int>()",
                            container)) == "Container<Int32> (from Sorted<Int32>)");
    CHECK(errors_of(std::format("{}Class Names Extends Container<String>\n"
                                "    Override Method Add(item: Int)\n"
                                "    End Method\n"
                                "End Class",
                                container)) ==
          errors_t{ "Add: 'Add' has Override, but Container<String> has no Method like it" });
    CHECK(errors_of(std::format("{}Class Bad Extends Container\nEnd Class", container)) ==
          errors_t{ "Container: Container<T> is generic, so it needs type arguments, like "
                    "Container<Int>" });
}

TEST_CASE("each instance of a generic type has its own Statics", "[sema]") {
    const std::string_view counter = "Class Counter<T>\n"
                                     "    Static Field Made: Int = 0\n"
                                     "    Static Field Last: T?\n"
                                     "    Static Method Report(): String => \"{Made}\"\n"
                                     "End Class\n";
    CHECK(probe(std::format("{}Local x = Counter<String>.Last", counter)) == "String?");
    CHECK(probe(std::format("{}Local x = Counter<Int>.Report()", counter)) == "String");
    CHECK(errors_of("Function Start(): Int => 3\n"
                    "Class Box<T>\n    Static Field Made: Int = Start()\nEnd Class") ==
          errors_t{ "Start(): a generic type's Static Field starts with a value known at compile "
                    "time, since each instance has its own" });
    CHECK(errors_of("Extend T[]\n    Static Method Make(): Int => 1\nEnd Extend") ==
          errors_t{ "Make: an array type can't come before a '.', so T[] can't have Statics" });
}

TEST_CASE("Const Functions can be generic, and give the sizes in declarations' types", "[sema]") {
    CHECK(probe("Const Function Largest<T: Comparable>(a: T, b: T): T => a > b ? a : b\n"
                "Const X = Largest(3, 9)") == "Int32 9");
    CHECK(probe("Const Function Square<T: Numeric>(x: T): T => x * x\n"
                "Const X = Square(1.5)") == "Float32 2.25");
    CHECK(probe("Struct Board\n"
                "    Field Cells: Int[Cells()]\n"
                "End Struct\n"
                "Const Function Cells(): Int => 4 * 4\n"
                "Local b = Board()\n"
                "Local x = b.Cells") == "Int32[16]");
}

TEST_CASE("a Const type argument or size can be computed from Const type parameters", "[sema]") {
    CHECK(probe("Function Join<Const N: Int, Const M: Int>(a: Int[N], b: Int[M]): Int[N + M]\n"
                "    Local joined: Int[N + M]\n"
                "    Return joined\n"
                "End Function\n"
                "Local a: Int[2]\n"
                "Local b: Int[3]\n"
                "Local x = Join(a, b)") == "Int32[5]");
    CHECK(probe("Struct Grid<Const W: Int>\n    Field Cells: Int[W * 2]\nEnd Struct\n"
                "Function Doubled<Const W: Int>(g: Grid<W>): Grid<W * 2> => Grid<W * 2>()\n"
                "Local x = Doubled(Grid<3>())") == "Grid<6>");
    CHECK(errors_of("Function F<Const N: Int>(a: Int[N - 1])\nEnd Function") ==
          errors_t{ "N - 1: a value computed from Const type parameters uses integers known at "
                    "compile time, with + and *, like N * 2" });
}

TEST_CASE("a loop can go over a type's own type parameter, or Self", "[sema]") {
    const std::string_view wrapper = "Struct Wrapper<T>\n"
                                     "    Field Item: T\n"
                                     "    Method Describe(): String\n"
                                     "        Local text = \"\"\n"
                                     "        For Field f: F In T\n"
                                     "            Where F: Printable\n"
                                     "            text = text + \"{f.Get(Item)}\"\n"
                                     "        End For\n"
                                     "        Return text\n"
                                     "    End Method\n"
                                     "End Struct\n"
                                     "Struct Q\n    Field A: Int\nEnd Struct\n"
                                     "Struct H\n    Field Inner: Q\nEnd Struct\n";
    CHECK(probe(std::format("{}Local x = Wrapper(Q(1)).Describe()", wrapper)) == "String");
    CHECK(errors_of(std::format("{}Local x = Wrapper(H(Q(1))).Describe()", wrapper)) ==
          errors_t{ "Wrapper(H(Q(1))).Describe: 'Describe' needs each field of H to implement "
                    "Printable, but its field 'Inner' is Q, which doesn't" });

    const std::string_view dumped = "Interface Dumped\n"
                                    "    Method Dump(): String\n"
                                    "        For Field f: F In Self\n"
                                    "            Where F: Printable\n"
                                    "        End For\n"
                                    "        Return \"\"\n"
                                    "    End Method\n"
                                    "End Interface\n"
                                    "Struct Q\n    Field A: Int\nEnd Struct\n";
    CHECK(
        errors_of(std::format("{}Struct P Implements Dumped\n    Field X: Int\nEnd Struct", dumped))
            .empty());
    CHECK(errors_of(std::format("{}Struct H Implements Dumped\n    Field Inner: Q\nEnd Struct",
                                dumped)) ==
          errors_t{ "Dumped: H takes Dumped's 'Dump', which needs each of its fields to implement "
                    "Printable, but its field 'Inner' is Q, which doesn't" });
}

TEST_CASE("loops over Properties, Static Fields, and Methods of a Func type", "[sema]") {
    const std::string_view player = "Class Player\n"
                                    "    Field HP: Int\n"
                                    "    Static Field Count: Int = 3\n"
                                    "    Property Half: Int\n"
                                    "        Get => HP / 2\n"
                                    "    End Property\n"
                                    "    Method Hit(amount: Int): Int => HP - amount\n"
                                    "End Class\n";
    CHECK(errors_of(std::format("{}For Property p: P In Player\n"
                                "    Where P: Printable\n"
                                "    Print(\"{{p.Name}} {{p.Get(Player())}}\")\n"
                                "End For\n"
                                "For Static Field s: S In Player\n"
                                "    Where S: Printable\n"
                                "    Print(\"{{s.Get()}}\")\n"
                                "End For\n"
                                "For Method m: M In Player\n"
                                "    Where M: Func(Int): Int\n"
                                "    Local after = m.Call(Player(), 5)\n"
                                "End For",
                                player))
              .empty());
    CHECK(errors_of(std::format("{}For Method m: M In Player\n"
                                "    Where M: Func(Int): Int\n"
                                "    Local after = m.Call(Player(), \"five\")\n"
                                "End For",
                                player)) == errors_t{ "\"five\": expected Int32, found String" });
    CHECK(
        errors_of(std::format("{}For Method m: M In Player\n    Where M: Int\nEnd For", player)) ==
        errors_t{ "Int: M is the Methods' Func type, like Func(Int): String, not Int32" });
    CHECK(errors_of(std::format("{}For Property p In Player\n    p.Set(1)\nEnd For", player)) ==
          errors_t{ "Set: 'p' is a Property of Player; it has Name, Get(value), Has(A), and "
                    "Attribute(A)" });
}

TEST_CASE("a type's own attributes", "[sema]") {
    CHECK(probe("Attribute Saved\n@Saved Struct P\nEnd Struct\nLocal x = P.Has(Saved)") == "Bool");
    CHECK(probe("Attribute Label(Text: String)\nStruct P\nEnd Struct\n"
                "Function L<T>(): Label? => T.Attribute(Label)\n"
                "Local x = L<P>()") == "Label?");
    CHECK(errors_of("Struct P\nEnd Struct\nLocal x = P.Has(P)") ==
          errors_t{ "P.Has: 'P.Has' takes the name of an Attribute, which an Attribute declaration "
                    "makes" });
}
