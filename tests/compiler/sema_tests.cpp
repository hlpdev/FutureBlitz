#include "fb/c_abi.hpp"
#include "fb/const_value.hpp"
#include "fb/driver.hpp"
#include "fb/sema.hpp"
#include "printers.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
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
    if (ast.items().empty() || ast.outside_main(ast.items().back())) {
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

/// The errors of `program`, which can import `modules` by their paths, like "./lib/a" for
/// lib/a.fb, besides the standard library's. An error in one of them starts with its path.
errors_t errors_with(const std::string_view program,
                     const std::vector<fb::library_file_t>& modules) {
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    files.insert(files.end(), modules.begin(), modules.end());
    const fb::compilation_t compilation = fb::compile("test.fb", std::string(program), files);
    errors_t errors;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        const fb::source_file_t& file = compilation.sources.get(diagnostic.span.file);
        const std::string where = file.id() == compilation.file ? "" : file.path() + ": ";
        errors.push_back(
            std::format("{}{}: {}", where, file.slice(diagnostic.span), diagnostic.message));
    }

    return errors;
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

    // A literal fits where it rounds to a float, as the largest one's shortest spelling does.
    CHECK(errors_of("Local x = 3.4028235e38\nLocal y: Float32 = -3.4028235e38").empty());
    CHECK(errors_of("Local x = 3.4028236e38") ==
          errors_t{ "3.4028236e38: 3.4028236e38 doesn't fit in Float32" });
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

TEST_CASE("C's integers widen and take literals as every target's would", "[sema]") {
    CHECK(probe("Local i = 1\nLocal x: CLong = i") == "CLong (from Int32)");
    CHECK(probe("Local c: CLong = 1\nLocal x: Long = c") == "Int64 (from CLong)");
    CHECK(probe("Local c: CLong = 1\nLocal i = 2\nLocal x = c + i") == "CLong");
    CHECK(probe("Local u: CULong = 1\nLocal x: UInt64 = u") == "UInt64 (from CULong)");
    CHECK(probe("Local w: CWChar = 'A'\nLocal x: Long = w") == "Int64 (from CWChar)");
    CHECK(probe("Local x: CLong = -2147483648") == "CLong -2147483648");
    CHECK(probe("Local x: CWChar = 65535") == "CWChar 65535");
    CHECK(probe("Const x: CLong = 1000 * 1000") == "CLong 1000000");
    const bool windows = fb::build_c_integers().c_long == fb::type_id_t::INT32;
    CHECK(probe("Const big: Long = 5_000_000_000\nConst x = big As CULong") ==
          (windows ? "CULong 705032704" : "CULong 5000000000"));
    CHECK(probe("Local x = SizeOf<CLong>()") ==
          std::format("Int64 {}", fb::bit_width(fb::build_c_integers().c_long) / 8));

    // Windows' are the smaller ones, and its wchar_t is unsigned.
    CHECK(errors_of("Local l: Long = 0\nLocal c: CLong = l") ==
          errors_t{ "l: expected CLong, found Int64" });
    CHECK(errors_of("Local c: CLong = 1\nLocal i: Int = c") ==
          errors_t{ "c: expected Int32, found CLong" });
    CHECK(errors_of("Local u: UInt32 = 1\nLocal c: CLong = u") ==
          errors_t{ "u: expected CLong, found UInt32" });
    CHECK(errors_of("Local x: CLong = 3000000000") ==
          errors_t{ "3000000000: 3000000000 doesn't fit in CLong, which is an Int32 on Windows" });
    CHECK(errors_of("Local x: CULong = 5000000000") ==
          errors_t{ "5000000000: 5000000000 doesn't fit in CULong, which is a UInt32 on Windows" });
    CHECK(errors_of("Local x: CWChar = -1") ==
          errors_t{ "-1: -1 doesn't fit in CWChar, which is a UInt16 on Windows" });
    CHECK(errors_of("Local w: CWChar = 1\nLocal x = -w") ==
          errors_t{ "-w: '-' can't be used on CWChar, which is a UInt16 on Windows" });
    CHECK(errors_of("Enum Kind: CLong\n    A\nEnd Enum") ==
          errors_t{ "CLong: an Enum's type is an integer of one size on every target, which "
                    "CLong isn't" });

    // A Const computes as the target programs are built for has them.
    CHECK(errors_of("Const a: CLong = 2000000000\nConst b = a * a * a") ==
          errors_t{ std::format("{}: the result of '*' overflows CLong ({} on this target)",
                                windows ? "a * a" : "a * a * a",
                                fb::builtin_name(fb::build_c_integers().c_long)) });
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
          errors_t{ "1.0e10 As Int: 1e10 doesn't fit in Int32" });
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

TEST_CASE("an interpolation's format spec fits its value, or is an error at the spec", "[sema]") {
    // Known values are formatted at compile time, as the program would.
    CHECK(probe("Local x = \"{3.14159:.2} {1234:,} {255:#x} {\"ab\":*^6} {True:>5}|\"") ==
          "String 3.14 1,234 0xff **ab**  True|");
    CHECK(errors_of("Local n = 5\nLocal x = \"{n:.2}\"") ==
          errors_t{ ".2: Int32 doesn't take a precision in its format spec" });
    CHECK(errors_of("Local f = 1.5\nLocal x = \"{f:x}\"") ==
          errors_t{ "x: Float32 doesn't take 'x' in its format spec" });
    CHECK(errors_of("Local s = \"a\"\nLocal x = \"{s:+}\"") ==
          errors_t{ "+: String doesn't take a sign in its format spec" });
    CHECK(errors_of("Local b = True\nLocal x = \"{b:05}\"") ==
          errors_t{ "05: Bool doesn't take '0' in its format spec" });
    CHECK(errors_of("Local n = 5\nLocal x = \"{n:#}\"") ==
          errors_t{ "#: '#' gives a base's prefix, so it needs x, X, o, or b" });

    // An error in the spec's text points at where in it.
    CHECK(errors_of("Local n = 5\nLocal x = \"{n:>5q}\"") ==
          errors_t{ "q: 'q' isn't part of a format spec" });
    CHECK(errors_of("Local n = 5\nLocal x = \"{n:1000}\"") ==
          errors_t{ "1000: a format spec's width is at most 999" });

    // A value that may be Null has no text without '??'.
    CHECK(errors_of("Function Shown(n: Int?): String => \"{n:>4}\"") ==
          errors_t{ "n: Int32? can't be shown in a string, since it may be Null; give a value for "
                    "Null with '?"
                    "?'" });
}

TEST_CASE("every value has a debug form, and DebugElements goes on an Iterable", "[sema]") {
    const std::string_view point = "Import std.core\nStruct P\n    Field x: Int\nEnd Struct\n";
    CHECK(errors_of(std::format("{}Local p = P(1)\nLocal x = \"{{p:?}} {{p:>#9?}}\"", point))
              .empty());
    CHECK(errors_of(std::format("{}Local p = P(1)\nLocal x = \"{{p:+?}} {{p:.2?}}\"", point)) ==
          errors_t{
              "+?: the debug form '?' takes only a fill, an alignment, a width, and '#'",
              ".2?: the debug form '?' takes only a fill, an alignment, a width, and '#'",
          });

    // Std's DebugElements shows an Iterable's elements, and DebugEntries its (key, value) ones.
    const auto bag = [](const std::string_view attributes, const std::string_view element) {
        return std::format("Import std.core\nImport std.collections\n{}\n"
                           "Class Bag Implements Iterable<{}>\n"
                           "    Field Items: {}[] = []\n"
                           "    Method Iterate(): ArrayIterator<{}> => Items.Iterate()\n"
                           "End Class",
                           attributes, element, element, element);
    };
    CHECK(errors_of(bag("@DebugElements", "Int")).empty());
    CHECK(errors_of(bag("@DebugEntries", "(String, Int)")).empty());
    CHECK(
        errors_of(bag("@DebugEntries", "Int")) ==
        errors_t{ "Bag: @DebugEntries goes on an Iterable of (key, value) tuples, but 'Bag' is an "
                  "Iterable<Int32>" });
    CHECK(
        errors_of(bag("@DebugEntries", "(Int, Int, Int)")) ==
        errors_t{ "Bag: @DebugEntries goes on an Iterable of (key, value) tuples, but 'Bag' is an "
                  "Iterable<(Int32, Int32, Int32)>" });
    CHECK(errors_of(bag("@DebugElements @DebugEntries", "(String, Int)")) ==
          errors_t{ "Bag: 'Bag' has @DebugElements and @DebugEntries, but its debug form shows "
                    "one" });
    CHECK(errors_of(std::format("{}@DebugElements\nStruct Q\nEnd Struct", point)) ==
          errors_t{ "Q: @DebugElements goes on a Struct or Class that implements Iterable<T>, "
                    "which 'Q' isn't" });
    CHECK(errors_of("Import std.core\n"
                    "@DebugElements\n"
                    "Interface Many Extends Iterable<Int>\n"
                    "End Interface") ==
          errors_t{ "Many: @DebugElements goes on a Struct or Class that implements Iterable<T>, "
                    "which 'Many' isn't" });
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
    CHECK(errors_of("Local x = RefEquals") ==
          errors_t{ "RefEquals: 'RefEquals' is built in, so it can't be a value; use a lambda that "
                    "calls it" });
    CHECK(errors_of("Local x: Foo") == errors_t{ "Foo: 'Foo' isn't a type" });

    // Vec3 is std.math's, which a file imports to use it.
    CHECK(errors_of("Local v: Vec3") ==
          errors_t{ "Vec3: 'Vec3' is in std.math, which this file doesn't import" });
}

TEST_CASE("functions and Globals are visible anywhere in the file", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Print(Greeting())\n"
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

    // A lambda's code runs later, so it uses any Global whose type is known by then; a Global
    // that gets its type from a later initializer needs one written.
    CHECK(errors_of("Global f: Func(): Int = Function(): Int => n * 2\n"
                    "Global n: Int = Make()\n"
                    "Function Make(): Int => 21")
              .empty());
    CHECK(errors_of("Global f: Func(): Int = Function(): Int => n * 2\n"
                    "Global n = Make()\n"
                    "Function Make(): Int => 21") ==
          errors_t{ "n: 'n' gets its type from its initializer, which comes later; give it a type "
                    "to use it in a lambda here" });
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
    CHECK(errors_of("Import std.core\n"
                    "Local hp: Int\nPrint(\"{hp}\")") ==
          errors_t{ "hp: 'hp' is read before it's assigned" });
    CHECK(errors_of("Import std.core\n"
                    "Local hp: Int\nhp = 5\nPrint(\"{hp}\")")
              .empty());
    CHECK(errors_of("Local hp: Int\nhp += 1") ==
          errors_t{ "hp: 'hp' is read before it's assigned" });

    // Reported once, not at every read.
    CHECK(errors_of("Import std.core\n"
                    "Local hp: Int\nPrint(\"{hp} {hp}\")") ==
          errors_t{ "hp: 'hp' is read before it's assigned" });
}

TEST_CASE("an ElseIf condition only sees what was assigned before the If", "[sema]") {
    // The condition only runs when the branches before it didn't, so their assignments don't
    // count, and a Return in one of them doesn't hide the read.
    CHECK(errors_of("Import std.core\n"
                    "Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    hp = 1\n"
                    "ElseIf hp > 0\n"
                    "    Print(\"positive\")\n"
                    "End If") == errors_t{ "hp: 'hp' is read before it's assigned" });
    CHECK(errors_of("Import std.core\n"
                    "Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    Return\n"
                    "ElseIf hp > 0\n"
                    "    Print(\"positive\")\n"
                    "End If") == errors_t{ "hp: 'hp' is read before it's assigned" });
}

TEST_CASE("assignment is tracked through branches and loops", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    hp = 1\n"
                    "Else\n"
                    "    hp = 2\n"
                    "End If\n"
                    "Print(\"{hp}\")")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Local c = True\n"
                    "Local hp: Int\n"
                    "If c\n"
                    "    hp = 1\n"
                    "End If\n"
                    "Print(\"{hp}\")") == errors_t{ "hp: 'hp' is read before it's assigned" });

    // A While body may run zero times. A Repeat body runs at least once, and the Loop only ends
    // at its Break.
    CHECK(errors_of("Import std.core\n"
                    "Local c = True\n"
                    "Local hp: Int\n"
                    "While c\n"
                    "    hp = 1\n"
                    "End While\n"
                    "Print(\"{hp}\")") == errors_t{ "hp: 'hp' is read before it's assigned" });
    CHECK(errors_of("Import std.core\n"
                    "Local hp: Int\n"
                    "Repeat\n"
                    "    hp = 1\n"
                    "Until True\n"
                    "Print(\"{hp}\")")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Local hp: Int\n"
                    "Loop\n"
                    "    hp = 1\n"
                    "    Break\n"
                    "End Loop\n"
                    "Print(\"{hp}\")")
              .empty());

    // Default returns, so only the Case 1 path reaches the Print.
    CHECK(errors_of("Import std.core\n"
                    "Local n = 1\n"
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
    CHECK(errors_of("Import std.core\n"
                    "Print(\"a\")\nReturn\nPrint(\"b\")")
              .empty());
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

    // A String parameter takes a String.
    CHECK(errors_of("Function Shout(text: String)\nEnd Function\nShout(1)") ==
          errors_t{ "1: expected String, found Int32" });
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
    // Resolution has two tiers: an Int literal for an Int beats one for a Double only where the
    // call's other arguments are exact too.
    constexpr std::string_view fbm =
        "Function Fbm(x: Double, y: Double, octaves: Int): Bool => True\n"
        "Function Fbm(x: Double, y: Double, z: Double): String => \"\"\n"
        "Local d: Double = 0.5\n";
    CHECK(probe(std::format("{}Local x = Fbm(d, d, 3)", fbm)) == "Bool");
    CHECK(errors_of(std::format("{}Local x = Fbm(0.5, d, 3)", fbm)) ==
          errors_t{ "Fbm: the call to 'Fbm' is ambiguous" });
    CHECK(errors_of(call("Show(True)")) == errors_t{ "Show: no overload of 'Show' takes (Bool)" });
    // A literal too big for an Int, or a generic Function's name, has no type of its own, so it
    // shows as written.
    constexpr std::string_view neither = "Function F(a: String)\nEnd Function\n"
                                         "Function F(a: Bool)\nEnd Function\n"
                                         "Function Same<T>(a: T, b: T): Bool => True\n";
    CHECK(errors_of(std::format("{}F(5_000_000_000)", neither)) ==
          errors_t{ "F: no overload of 'F' takes (5_000_000_000)" });
    CHECK(errors_of(std::format("{}F(Same, 1.5)", neither)) ==
          errors_t{ "F: no overload of 'F' takes (Same, Float32)" });
    CHECK(errors_of("Function F(a: Int)\nEnd Function\nFunction F(b: Int)\nEnd Function") ==
          errors_t{ "F: 'F' is already declared with the same parameter types" });

    // Generic overloads compare with their type parameters in order, whatever their names.
    CHECK(errors_of("Function Id<T>(x: T): T => x\nFunction Id<U>(y: U): U => y") ==
          errors_t{ "Id: 'Id' is already declared with the same parameter types" });
    CHECK(errors_of("Function Id<T>(x: T): T => x\n"
                    "Function Id<U>(y: U[], i: Int): U => y[i]\n"
                    "Local a = Id(1)\n"
                    "Local b = Id([2], 0)")
              .empty());

    // One that isn't generic beats a generic one that fits as well, exactly or with a
    // conversion.
    constexpr std::string_view either = "Function F(x: Int?): Bool => True\n"
                                        "Function F<T>(x: T?): String => \"generic\"\n"
                                        "Function G(x: Int): Bool => True\n"
                                        "Function G<T>(x: T): String => \"generic\"\n";
    CHECK(probe(std::format("{}Local n: Int = 5\nLocal x = F(n)", either)) == "Bool");
    CHECK(probe(std::format("{}Local x = G(5)", either)) == "Bool");

    // Their constraints count too, since a call picks the overload whose constraints its type
    // arguments meet, though one that extends another asks what both do.
    constexpr std::string_view describe = "Import std.core\n"
                                          "Struct OnlyEq Implements Equatable\n"
                                          "    Field X: Int\n"
                                          "End Struct\n"
                                          "Function Describe<T>(x: T): String\n"
                                          "    Where T: Comparable\n"
                                          "    Return \"comparable\"\n"
                                          "End Function\n";
    CHECK(probe(std::format("Import std.core\n"
                            "{}Function Describe<U>(y: U): Int\n"
                            "    Where U: Equatable\n"
                            "    Return 1\n"
                            "End Function\n"
                            "Local x = Describe(OnlyEq(1))",
                            describe)) == "Int32");
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Function Describe<U>(y: U): String\n"
                                "    Where U: Comparable\n"
                                "    Return \"again\"\n"
                                "End Function",
                                describe)) ==
          errors_t{ "Describe: 'Describe' is already declared with the same parameter types" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Function Describe<U>(y: U): String\n"
                                "    Where U: Equatable & Comparable\n"
                                "    Return \"again\"\n"
                                "End Function",
                                describe)) ==
          errors_t{ "Describe: 'Describe' is already declared with the same parameter types" });

    // That holds whatever is declared first: Const Functions are declared before any Interface
    // knows what it extends, and here the Interfaces come after the Functions.
    CHECK(errors_of("Import std.core\n"
                    "Const Function F<T>(x: T): Int\n"
                    "    Where T: Comparable\n"
                    "    Return 1\n"
                    "End Function\n"
                    "Const Function F<U>(y: U): Int\n"
                    "    Where U: Comparable & Equatable\n"
                    "    Return 2\n"
                    "End Function\n"
                    "Const A = F(1)") ==
          errors_t{ "F: 'F' is already declared with the same parameter types" });
    CHECK(errors_of("Function D<T>(x: T): Int\n"
                    "    Where T: B\n"
                    "    Return 1\n"
                    "End Function\n"
                    "Function D<U>(y: U): Int\n"
                    "    Where U: B & A\n"
                    "    Return 2\n"
                    "End Function\n"
                    "Interface A\n"
                    "End Interface\n"
                    "Interface B Extends A\n"
                    "End Interface\n"
                    "Struct S Implements B\n"
                    "End Struct\n"
                    "Local n = D(S())") ==
          errors_t{ "D: 'D' is already declared with the same parameter types" });

    // A Const type parameter only stands for one of the same type.
    CHECK(errors_of("Function F<Const N: Int>(x: Int[N]): Int => N\n"
                    "Function F<Const M: Int>(y: Int[M]): Int => M") ==
          errors_t{ "F: 'F' is already declared with the same parameter types" });
    CHECK(errors_of("Function F<Const N: Int>(x: Int[N]): Int => N\n"
                    "Function F<Const N: UInt8>(x: Int[N]): Int => 2\n"
                    "Local a: Int[3]\n"
                    "Local n = F(a)") == errors_t{ "F: the call to 'F' is ambiguous" });
}

TEST_CASE("std's Print and the built-ins can be overloaded, and a module's own hides them",
          "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Function Print(value: Int)\n"
                    "End Function\n"
                    "Print(1)\n"
                    "Print(\"a\")")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Function Print(text: String)\nEnd Function\nPrint(\"a\")")
              .empty());
    CHECK(errors_of("Function Panic(message: String)\n"
                    "End Function\n"
                    "Panic(\"not really\")\n"
                    "Local after = 1")
              .empty());
    CHECK(errors_of("Struct Wait\n"
                    "    Field n: Int\n"
                    "End Struct\n"
                    "Local s = Wait(2)\n"
                    "Local n: Int = s.n")
              .empty());
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
    CHECK(errors_of("Import std.core\n"
                    "For i = 0 To 9\n    Print(\"{i}\")\nEnd For")
              .empty());
    CHECK(errors_of("For i = 0.0 To 1.0 Step 0.25\nEnd For").empty());
    CHECK(errors_of("For i = 10 To 0 Step -1\nEnd For").empty());

    // A literal start takes the end's type.
    CHECK(errors_of("Local n: Long = 10\n"
                    "For i = 0 Until n\n"
                    "    Local x: Int = i\n"
                    "End For") == errors_t{ "i: expected Int32, found Int64" });

    CHECK(errors_of("Import std.core\n"
                    "For i = 0 To 9\nEnd For\nPrint(\"{i}\")") ==
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
    CHECK(errors_of("Local x: DateTime") == errors_t{ "DateTime: 'DateTime' isn't supported yet" });
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
    constexpr std::string_view box = "Class Box\n"
                                     "    Property Size: Int\n"
                                     "        Get\n"
                                     "            Return 3\n"
                                     "        End Get\n"
                                     "    End Property\n"
                                     "    Static Property Made: Int\n"
                                     "        Get\n"
                                     "            Return 1\n"
                                     "        End Get\n"
                                     "    End Property\n"
                                     "    Static Field Count: Int\n"
                                     "End Class\n";
    CHECK(errors_of(std::format("{}Local n = Box().Size()", box)) ==
          errors_t{ "Size: 'Size' is a Property, not a method" });
    CHECK(errors_of(std::format("{}Local n = Box.Made()", box)) ==
          errors_t{ "Made: 'Made' is a Property, not a method" });
    CHECK(errors_of(std::format("{}Local n = Box.Count()", box)) ==
          errors_t{ "Count: 'Count' is a field, not a method" });
    CHECK(errors_of("Class Tally\n"
                    "    Static Field Count: Int\n"
                    "    Method Bump()\n"
                    "        Count()\n"
                    "    End Method\n"
                    "End Class") == errors_t{ "Count: 'Count' is a field, not a method" });
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

    // A Property's Set changes Self too, but a Static Property belongs to its type.
    CHECK(in_struct("    Property Twice: Int\n"
                    "        Get => x * 2\n"
                    "        Set(value)\n"
                    "            x = value / 2\n"
                    "        End Set\n"
                    "    End Property\n"
                    "    Method M()\n"
                    "        Twice = 4\n"
                    "    End Method\n") ==
          errors_t{ "Twice: 'Twice' is a Property, and a Method can't change Self; make it a ByRef "
                    "Method" });
    CHECK(in_struct("    Static Field made: Int = 0\n"
                    "    Static Property Made: Int\n"
                    "        Get => made\n"
                    "        Set(value)\n"
                    "            made = value\n"
                    "        End Set\n"
                    "    End Property\n"
                    "    Method M()\n"
                    "        Made += 1\n"
                    "        Made = Made + 1\n"
                    "    End Method\n"
                    "    Static Method Reset()\n"
                    "        Made = 0\n"
                    "    End Method\n")
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

    // A generic one can't contain an instance of itself either, whatever its type arguments.
    CHECK(errors_of("Struct Wrap<T>\n    Field v: T\n    Field w: Wrap<Int>\nEnd Struct\n"
                    "Function Use(w: Wrap<String>): String => w.v") ==
          errors_t{ "w: 'Wrap<T>' can't contain itself; a Struct is stored inline" });
    CHECK(errors_of("Struct Wrap<T>\n    Field v: T\n    Field w: Wrap<T[]>\nEnd Struct\n"
                    "Function Use(w: Wrap<String>): String => w.v") ==
          errors_t{ "w: 'Wrap<T>' can't contain itself; a Struct is stored inline" });
}

TEST_CASE("Structs compare with == and != only", "[sema]") {
    constexpr std::string_view point =
        "Import std.core\n"
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

    // One can hold an array of itself, which compares element by element.
    CHECK(probe("Struct Node\n"
                "    Field children: Node[]\n"
                "End Struct\n"
                "Local same = Node([]) == Node([Node([])])") == "Bool");
    CHECK(errors_of("Struct Node\n"
                    "    Field f: Func(): Int\n"
                    "    Field children: Node[]\n"
                    "End Struct\n"
                    "Local same = Node(Function() => 1, []) == Node(Function() => 1, [])") ==
          errors_t{ "Node(Function() => 1, []) == Node(Function() => 1, []): '==' can't be used "
                    "on Node" });
    // One that holds an array of a larger instance of itself has no end to compare.
    constexpr std::string_view tree = "Struct Tree<T>\n"
                                      "    Field v: T\n"
                                      "    Field kids: Tree<T[]>[]\n"
                                      "End Struct\n"
                                      "Local a = Tree<Int>(1, [])\n";
    CHECK(errors_of(std::format("{}Local same = a == a\nLocal found = [a].Contains(a)", tree)) ==
          errors_t{ "a == a: '==' can't be used on Tree<Int32>; comparing one compares a larger "
                    "instance of Tree<T> in it each time, without end",
                    "[a].Contains: Contains compares with ==, which Tree<Int32> doesn't have; "
                    "comparing one compares a larger instance of Tree<T> in it each time, without "
                    "end" });
    CHECK(errors_of(std::format("{}Local v = a.v", tree)).empty());
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
    CHECK(errors_of("Function F()\n"
                    "    Class C\n    End Class\n"
                    "    Interface I\n    End Interface\n"
                    "End Function") == errors_t{
                                           "C: Classes can only be declared at the top level",
                                           "I: Interfaces can only be declared at the top level",
                                       });
    CHECK(errors_of("Struct P\n    Print(\"x\")\nEnd Struct") ==
          errors_t{ "Print(\"x\"): a Struct can only hold Fields, Methods, Properties, and News" });
    CHECK(errors_of("Struct Int\nEnd Struct") == errors_t{ "Int: 'Int' is a built-in type" });

    // A built-in generic type's name, like Weak, Ptr, or Task, is the code's own type when it
    // declares one, in types and calls alike.
    CHECK(probe("Class Weak<T>\n"
                "    Field x: Int = 7\n"
                "End Class\n"
                "Class Task\n"
                "    Field y: Int = 2\n"
                "End Class\n"
                "Local typed: Weak<Int> = Weak<Int>()\n"
                "Local job: Task = Task()\n"
                "Local x = typed.x + job.y") == "Int32");
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

    // It runs on every object, so it has no type arguments and no Where line to meet.
    CHECK(errors_of("Class C\n"
                    "    Method Destroy<T>()\n"
                    "        Local x: T\n"
                    "    End Method\n"
                    "End Class\n"
                    "Local c = C()") ==
          errors_t{ "Destroy: Destroy can't be generic, since it runs by itself" });
    CHECK(errors_of("Import std.core\n"
                    "Class Box<T>\n"
                    "    Method Destroy()\n"
                    "        Where T: Printable\n"
                    "    End Method\n"
                    "End Class\n"
                    "Local b = Box<Int>()") ==
          errors_t{ "Destroy: Destroy can't have a Where line, since every object's runs" });
}

TEST_CASE("Class references", "[sema]") {
    constexpr std::string_view a = "Import std.core\n"
                                   "Class A\nEnd Class\n";
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
    CHECK(errors_of("Function Shout(text: String)\nEnd Function\nShout(Null)") ==
          errors_t{ "Null: String can't be Null; String? can" });
    // A function type's '?' would go on what it gives.
    CHECK(errors_of("Local f: Func(): String = Null") ==
          errors_t{ "Null: Func(): String can't be Null; (Func(): String)? can" });
}

TEST_CASE("a T? of a T? holds one that can be Null itself", "[sema]") {
    // The messages are split around "??" so the compiler doesn't see a trigraph in them.
    const std::string twice = "Int32?"
                              "?";
    CHECK(probe("Local a: Int?"
                "? = Null\nLocal x = a") == twice);
    CHECK(probe("Function F(): Int?"
                "?? => 5\nLocal x = F()") == "Int32?"
                                             "??");
    // '??' and narrowing take off one T? at a time.
    CHECK(probe("Local a: Int?"
                "? = Null\nLocal x = a ?"
                "? Null") == "Int32?");
    CHECK(probe("Local a: Int?"
                "? = Null\nLocal x = a ?"
                "? 5") == "Int32?");
    CHECK(errors_of("Function F(): Int?"
                    "? => Null\n"
                    "Local a = F()\n"
                    "If a != Null\n"
                    "    Local x: Int? = a\n"
                    "End If")
              .empty());
    // A T?? can't lose a level without a check.
    CHECK(errors_of("Local a: Int?"
                    "? = Null\nLocal b: Int? = a") ==
          errors_t{ std::format("a: expected Int32?, found {}, which may be Null; check it first, "
                                "or give a value for Null with '?"
                                "?'",
                                twice) });
    // Generic code's T? for an Int? is an Int??, as std's Map gives for a missing key.
    CHECK(probe("Import std.collections\n"
                "Local m: Map<String, Int?> = {}\n"
                "Local x = m.Get(\"a\")") == twice);
    CHECK(probe("Function First<T>(items: T[]): T? => items.Count > 0 ? items[0] : Null\n"
                "Local holes: Int?[] = [Null]\n"
                "Local x = First(holes)") == twice);
}

TEST_CASE("a T? has to be checked before it's used as a T", "[sema]") {
    // The messages are split around "??" so the compiler doesn't see a trigraph in them.
    CHECK(errors_of("Import std.core\n"
                    "Function F(s: String?)\n    Print(s)\nEnd Function") ==
          errors_t{ "s: String? can't be shown by 'Print', since it may be Null; give a "
                    "value for Null with '?"
                    "?'" });
    CHECK(errors_of("Import std.core\n"
                    "Function F(x: Int?)\n    Print(\"{x}\")\nEnd Function") ==
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
    constexpr std::string_view enemy = "Import std.core\n"
                                       "Class E\n    Field hp: Int\nEnd Class\n";
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

TEST_CASE("T?s nested to different depths meet at the deeper one", "[sema]") {
    constexpr std::string_view values = "Class Animal\nEnd Class\n"
                                        "Class Dog Extends Animal\nEnd Class\n"
                                        "Class Cat Extends Animal\nEnd Class\n"
                                        "Function Flag(): Bool => False\n"
                                        "Function A(): Int32?? => 3\n"
                                        "Function B(): Int64? => 4\n"
                                        "Function D(): Dog? => Dog()\n"
                                        "Function C(): Cat?? => Null\n"
                                        "Function C3(): Cat??? => Null\n";
    const auto with_values = [&](const std::string_view text) {
        return std::format("{}{}", values, text);
    };

    // Each side keeps its depth, so the Null inside a Cat?? stays one.
    CHECK(probe(with_values("Local x = Flag() ? A() : B()")) == "Int64??");
    CHECK(probe(with_values("Local x = Flag() ? D() : C()")) == "Animal??");
    CHECK(probe(with_values("Local x = [A(), B()]")) == "Int64??[]");
    CHECK(probe(with_values("Local x = [D(), C()]")) == "Animal??[]");
    CHECK(probe(with_values("Local x = C3() ?? D()")) == "Animal??");
    CHECK(errors_of(with_values("Local x = A() == B()\nLocal y = C() == D()")).empty());
}

TEST_CASE("comparing with Null", "[sema]") {
    CHECK(probe("Local a: Int? = Null\nLocal x = a == Null") == "Bool");
    CHECK(probe("Local a: Int? = Null\nLocal x = Null != a") == "Bool");
    CHECK(probe("Local a: Int? = Null\nLocal x = a == 5") == "Bool");
    CHECK(probe("Const C: Int? = Null\nLocal x = C == Null") == "Bool True");
    CHECK(errors_of("Local a = 1\nLocal x = a == Null") ==
          errors_t{ "a == Null: 'a' can't be Null here, so this is always False" });
    // An expression made with a parenthesized one starts at its parenthesis, and ends at the
    // last one's.
    CHECK(errors_of("Local s: String? = Null\n"
                    "Local c = True\n"
                    "Local a = ((s) ?? \"x\") == Null\n"
                    "Local b = (\"x\" + (s ?? \"y\")) == Null\n"
                    "Local d = ((c) ? \"a\" : \"b\") == Null") ==
          errors_t{
              "((s) ?? \"x\") == Null: '(s) ?? \"x\"' can't be Null here, so this is always "
              "False",
              "(\"x\" + (s ?? \"y\")) == Null: '\"x\" + (s ?? \"y\")' can't be Null here, so this "
              "is always False",
              "((c) ? \"a\" : \"b\") == Null: '(c) ? \"a\" : \"b\"' can't be Null here, so this is "
              "always False",
          });
    CHECK(errors_of("Class Box\n    Field Name: String = \"\"\nEnd Class\n"
                    "Function Get(): (Box?, Int) => (Null, 1)\n"
                    "Local t = Get()\n"
                    "Local a = (t).0.Name\n"
                    "Local b = ((t)).0.Name") ==
          errors_t{
              "(t).0: '(t).0' may be Null; check a Local copy of it, or use '?.'",
              "((t)).0: '((t)).0' may be Null; check a Local copy of it, or use '?.'",
          });
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

    // A lambda made before then can't use Self, since New could call it before it assigns the
    // field, and a Property's Set is code of Self's too.
    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    New()\n"
                     "        Local f = Function() => e\n"
                     "        e = E()\n"
                     "        Local g = Function() => e\n"
                     "    End New\n"
                     "End Class") ==
          errors_t{ "e: Self can't be used before New assigns 'e', which this lambda would read "
                    "when it runs" });
    CHECK(with_enemy("Class H\n"
                     "    Field e: E\n"
                     "    Property P: Int\n"
                     "        Get => 1\n"
                     "        Set(value)\n"
                     "        End Set\n"
                     "    End Property\n"
                     "    New()\n"
                     "        P = 1\n"
                     "        Self.P = 2\n"
                     "        e = E()\n"
                     "        P = 3\n"
                     "    End New\n"
                     "End Class") ==
          errors_t{ "P: Self can't be used before New assigns 'e'",
                    "Self.P: Self can't be used before New assigns 'e'" });
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

    // A Static member can't hide one the Class's objects inherit, but a Static Method with
    // other parameters, or beside a Static one, is another.
    CHECK(errors_of(with_base("Class B Extends A\n"
                              "    Static Field hp: Int = 5\n"
                              "    Static Method Hit(n: Int)\n"
                              "    End Method\n"
                              "End Class")) ==
          errors_t{ "hp: 'hp' is already declared in A", "Hit: 'Hit' is already declared in A" });
    CHECK(errors_of(with_base("Class B Extends A\n"
                              "    Static Method Hit()\n"
                              "    End Method\n"
                              "End Class"))
              .empty());
    CHECK(errors_of("Class A\n"
                    "    Static Method Make(): Int => 1\n"
                    "End Class\n"
                    "Class B Extends A\n"
                    "    Static Method Make(): Int => 2\n"
                    "End Class")
              .empty());
    // Nor a Static Method an inherited Field's or Property's name, Static or not.
    CHECK(errors_of("Class A\n"
                    "    Field x: Int = 1\n"
                    "    Property P: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "    Static Field s: Int = 2\n"
                    "End Class\n"
                    "Class B Extends A\n"
                    "    Static Method x(): Int => 5\n"
                    "    Static Method P(): Int => 5\n"
                    "    Static Method s(): Int => 5\n"
                    "End Class") == errors_t{ "x: 'x' is already declared in A",
                                              "P: 'P' is already declared in A",
                                              "s: 's' is already declared in A" });

    CHECK(errors_of("Struct S\nEnd Struct\nStruct T Extends S\nEnd Struct") ==
          errors_t{ "S: only a Class can extend another; a Struct is a value" });
    CHECK(errors_of("Class B Extends Nope\nEnd Class") == errors_t{ "Nope: 'Nope' isn't a Class" });
    CHECK(errors_of("Class A Extends B\nEnd Class\nClass B Extends A\nEnd Class") ==
          errors_t{ "A: B can't extend itself, directly or through other Classes" });

    // Through an instance of the Class being declared, whatever its type arguments.
    CHECK(errors_of("Class A Extends B<Int32>\nEnd Class\nClass B<T> Extends A\nEnd Class") ==
          errors_t{ "A: B<T> can't extend itself, directly or through other Classes" });
    CHECK(errors_of("Class A<T> Extends B<Int32>\nEnd Class\n"
                    "Class B<T> Extends A<String>\nEnd Class") ==
          errors_t{ "A: B<T> can't extend itself, directly or through other Classes" });
    CHECK(errors_of("Class A<T> Extends B<T>\nEnd Class\nClass B<T> Extends A<Int32>\nEnd Class") ==
          errors_t{ "A: B<T> can't extend itself, directly or through other Classes" });
    CHECK(errors_of("Class C\nEnd Class\n"
                    "Class B<T> Extends C\nEnd Class\n"
                    "Class A Extends B<Int32>\nEnd Class\n"
                    "Class D Extends A\nEnd Class")
              .empty());
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
    CHECK(errors_of("Class C\n"
                    "    Override Method M(): Int => 1\n"
                    "    Override Property P: Int\n"
                    "        Get => 2\n"
                    "    End Property\n"
                    "End Class") == errors_t{ "M: 'M' has Override, but C extends no Class",
                                              "P: 'P' has Override, but C extends no Class" });
    CHECK(with_base("Class B Extends A\n    Override Method F(): Long => 2\nEnd Class") ==
          errors_t{ "F: 'F' must return Int32, like A's" });
    CHECK(with_base("Class B Extends A\n    Override Method G()\n    End Method\nEnd Class") ==
          errors_t{ "G: 'G' is Final in A, so it can't be overridden" });

    // Other parameters make an overload. Every Class's Destroy runs, so none overrides another.
    CHECK(with_base("Class B Extends A\n    Method F(x: Int): Int => x\nEnd Class").empty());

    // Generic Methods aren't in the Method table, so one of the same shape neither overrides
    // nor hides A's: it's another overload, and a call that fits both is ambiguous.
    constexpr std::string_view generic = "Class A\n"
                                         "    Method H<T>(x: T): String => \"a\"\n"
                                         "End Class\n"
                                         "Class B Extends A\n"
                                         "    Method H<U>(x: U): String => \"b\"\n"
                                         "End Class\n"
                                         "Local b = B()\n";
    CHECK(errors_of(std::format("{}Local s = b.H(1)", generic)) ==
          errors_t{ "b.H: the call to 'H' is ambiguous" });
    CHECK(errors_of(std::format("{}Local a: A = b\nLocal s = a.H(1)", generic)).empty());
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
    constexpr std::string_view base = "Import std.core\n"
                                      "Abstract Class A\n"
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
    // New of B's calls Super.New, once, outside any block, and Self waits for it.
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
                    "End Class")
              .empty());
    CHECK(with_base("Abstract Class B Extends A\n"
                    "    New(make: Bool)\n"
                    "        If make Then Super.New(1)\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "New: New must call Super.New(...), since A has no New without parameters",
                    "Super.New(1): Super.New can only be called once in a New, outside any "
                    "block" });
    CHECK(with_base("Abstract Class B Extends A\n"
                    "    New()\n"
                    "        Super.New(1)\n"
                    "        Super.New(2)\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "Super.New(2): Super.New can only be called once in a New, outside any "
                    "block" });
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

TEST_CASE("a Class assigns its own fields that have no zero before Super.New", "[sema]") {
    // A's New calls a Method, which runs the override of the object's Class, so a field of B's
    // that it reads is assigned before Super.New.
    constexpr std::string_view base = "Class E\n"
                                      "End Class\n"
                                      "Class A\n"
                                      "    Field hp: Int\n"
                                      "    Field e: E\n"
                                      "    New()\n"
                                      "        e = E()\n"
                                      "        Show()\n"
                                      "    End New\n"
                                      "    Method Show()\n"
                                      "    End Method\n"
                                      "End Class\n";
    const auto with_base = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", base, text));
    };

    // The code before it can do what doesn't use Self, and read the fields it has assigned.
    CHECK(with_base("Class B Extends A\n"
                    "    Field f: E\n"
                    "    Field n: Int = 2\n"
                    "    New(twice: Bool)\n"
                    "        Local made = E()\n"
                    "        f = made\n"
                    "        If twice Then n = n * 2\n"
                    "        Local g = f\n"
                    "        Super.New()\n"
                    "        hp = n\n"
                    "    End New\n"
                    "    Override Method Show()\n"
                    "        Local g = f\n"
                    "    End Method\n"
                    "End Class")
              .empty());
    CHECK(with_base("Class B Extends A\n"
                    "    Field f: E\n"
                    "    New()\n"
                    "        Super.New()\n"
                    "        f = E()\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "Super.New(): New must assign 'f' before Super.New, since E has no zero "
                    "value" });
    CHECK(with_base("Class B Extends A\n"
                    "    Field f: E\n"
                    "    New(make: Bool)\n"
                    "        If make Then f = E()\n"
                    "        Super.New()\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "Super.New(): New must assign 'f' before Super.New, since E has no zero "
                    "value" });

    // Without Super.New, A's New runs first, before B's code could assign it.
    CHECK(with_base("Class B Extends A\n"
                    "    Field f: E\n"
                    "    New()\n"
                    "        f = E()\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "New: New must call Super.New() after it assigns 'f', since E has no zero "
                    "value" });
    CHECK(errors_of(std::format("{}Class Box<T>\n"
                                "    Field count: Int\n"
                                "End Class\n"
                                "Class Pair<T> Extends Box<T>\n"
                                "    Field f: E\n"
                                "    New(n: Int)\n"
                                "        f = E()\n"
                                "        Super.New()\n"
                                "        count = n\n"
                                "    End New\n"
                                "End Class\n"
                                "Local p = Pair<String>(1)",
                                base))
              .empty());
    CHECK(errors_of(std::format("{}Class Box<T>\n"
                                "    Field count: Int\n"
                                "End Class\n"
                                "Class Pair<T> Extends Box<T>\n"
                                "    Field f: E\n"
                                "    New(n: Int)\n"
                                "        count = n\n"
                                "        f = E()\n"
                                "        Super.New()\n"
                                "    End New\n"
                                "End Class",
                                base)) ==
          errors_t{ "count: 'count' is inherited, so New can't use it before Super.New" });

    // Until Super.New, Self and what it inherits wait, and New can't return.
    CHECK(with_base("Class B Extends A\n"
                    "    Field f: E\n"
                    "    New(early: Bool)\n"
                    "        Self.f = E()\n"
                    "        hp = 1\n"
                    "        Local x = e\n"
                    "        Show()\n"
                    "        Local g = Function() => f\n"
                    "        If early Then Return\n"
                    "        Super.New()\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{
              "Self: Self can't be used before Super.New; assign the field by name",
              "hp: 'hp' is inherited, so New can't use it before Super.New",
              "e: 'e' is inherited, so New can't use it before Super.New",
              "Show: Self can't be used before Super.New",
              "f: Self can't be used before Super.New, which this lambda would read when it runs",
              "Return: New can't return before Super.New sets up what Self inherits",
          });
    // A Static Property has no Self, its own or one it inherits, as a Static Field hasn't, there
    // or before a field without a zero is assigned.
    CHECK(errors_of("Class Node\n"
                    "    Field name: String\n"
                    "    New(name: String)\n"
                    "        Self.name = name\n"
                    "    End New\n"
                    "End Class\n"
                    "Class Base\n"
                    "    Static Property Made: Int\n"
                    "        Get => 0\n"
                    "    End Property\n"
                    "End Class\n"
                    "Class D Extends Base\n"
                    "    Field part: Node\n"
                    "    Static Property Prefix: String\n"
                    "        Get => \"part\"\n"
                    "    End Property\n"
                    "    New()\n"
                    "        part = Node(\"{Prefix} {Made}\")\n"
                    "        Super.New()\n"
                    "    End New\n"
                    "End Class\n"
                    "Class Plain\n"
                    "    Field part: Node\n"
                    "    Static Property Prefix: String\n"
                    "        Get => \"part\"\n"
                    "    End Property\n"
                    "    New()\n"
                    "        part = Node(Prefix)\n"
                    "    End New\n"
                    "End Class")
              .empty());
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

    // Null and empty arrays take the type the others meet at, wherever they are among them.
    CHECK(probe("Local a = [1, Null]") == "Int32?[]");
    CHECK(probe("Local a = [Null, 1]") == "Int32?[]");
    CHECK(probe("Local m = {\"x\": Null, \"y\": 2}") == "Map<String, Int32?>");
    CHECK(probe("Local i: Int[] = [1]\nLocal a = [i, []]") == "Int32[][]");
    CHECK(probe("Local i: Int[] = [1]\nLocal a = [[], i]") == "Int32[][]");
    // So do tuples, and the parts in them.
    CHECK(probe("Local i: Long[] = [1]\nLocal a = [(2, i), (1, [])]") == "(Int32, Int64[])[]");
    CHECK(probe("Local i: Long[] = [1]\nLocal a = [(1, []), (2, i)]") == "(Int32, Int64[])[]");
    CHECK(probe("Local i: Long[] = [1]\nLocal m = {\"x\": (1, []), \"y\": (2, i)}") ==
          "Map<String, (Int32, Int64[])>");
    CHECK(probe("Local a = [(1, 2), (3, 4.5)]") == "(Int32, Float32)[]");
    CHECK(probe("Local a = [(3, 4.5), (1, 2)]") == "(Int32, Float32)[]");

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
    // A variable whose type was refused isn't reported again where it's used.
    CHECK(errors_of("Local n = 3\nLocal a: Int[n]\na[0] = 1\nLocal b = a[0]") ==
          errors_t{ "n: a fixed array's size must be an integer known at compile time, like 64 "
                    "or 8 * 8" });

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
    CHECK(errors_of("Local g: Int[2, 4]\nLocal y = g[1, 18446744073709551615 As UInt64]") ==
          errors_t{ "18446744073709551615 As UInt64: index 18446744073709551615 is out of range "
                    "for Int32[2, 4]" });
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
    // Nor ByRef, nor a fixed array's, nor a String's Chars or Bytes, which are new each time.
    const std::string takers = "Function Bump(ByRef n: Int)\n"
                               "End Function\n"
                               "Function Fill(ByRef bytes: UInt8[])\n"
                               "End Function\n"
                               "Struct Pair\n"
                               "    Field Items: Int[]\n"
                               "    Field Name: String\n"
                               "End Struct\n";
    // Count alone, in an Extend of T[], neither.
    CHECK(errors_of(takers + "Extend T[]\n"
                             "    Method Grow()\n"
                             "        Bump(ByRef Count)\n"
                             "    End Method\n"
                             "    Unsafe Method Peek(): Int\n"
                             "        Local p = AddressOf(Count)\n"
                             "        Return p.Value\n"
                             "    End Method\n"
                             "End Extend") ==
          errors_t{ "Count: Count is how many elements Self has, which only its Methods change",
                    "Count: Count is how many elements Self has, which only its Methods change" });
    CHECK(errors_of(takers + "Local a = [1]\nBump(ByRef a.Count)") ==
          errors_t{ "a.Count: an array's Count can't be changed; use Add, Insert, RemoveAt, or "
                    "Clear" });
    // Nor its Capacity, alone in an Extend of T[] or not.
    CHECK(errors_of(takers + "Local a = [1]\nBump(ByRef a.Capacity)\n"
                             "Extend T[]\n"
                             "    Method Grow()\n"
                             "        Bump(ByRef Capacity)\n"
                             "    End Method\n"
                             "End Extend") ==
          errors_t{ "a.Capacity: an array's Capacity can't be set; use Reserve or TrimExcess",
                    "Capacity: Capacity is how many elements Self has room for, which only its "
                    "Methods change" });
    CHECK(errors_of(takers + "Local g: Int[3]\nBump(ByRef g.Count)\ng.Count += 1") ==
          errors_t{ "g.Count: a fixed array's Count can't be changed",
                    "g.Count: a fixed array's Count can't be changed" });
    CHECK(errors_of(takers + "Local w = \"abc\"\nFill(ByRef w.Bytes)\nw.Bytes = [1]") ==
          errors_t{ "w.Bytes: a String's Bytes is a new array each time it's read, so it can't be "
                    "changed",
                    "w.Bytes: a String's Bytes is a new array each time it's read, so it can't be "
                    "changed" });
    CHECK(errors_of(takers + "Local p = Pair([1], \"x\")\nBump(ByRef p.Items.Count)\n"
                             "p.Name.Chars = [65]") ==
          errors_t{ "p.Items.Count: an array's Count can't be changed; use Add, Insert, RemoveAt, "
                    "or Clear",
                    "p.Name.Chars: a String's Chars is a new array each time it's read, so it "
                    "can't be changed" });
    // An element of the new array is a temporary's, like an element of what a call gives.
    CHECK(errors_of("Local w = \"abc\"\nw.Bytes[0] = 5\nLocal b = w.Bytes[0]").empty());
    // A Property an Extend gives arrays can be set.
    CHECK(errors_of("Extend T[]\n"
                    "    Property Top: T\n"
                    "        Get => Self[Count - 1]\n"
                    "        Set(value)\n"
                    "            Self[Count - 1] = value\n"
                    "        End Set\n"
                    "    End Property\n"
                    "End Extend\n"
                    "Local a = [1, 2]\n"
                    "a.Top = 5")
              .empty());
    CHECK(errors_of("Local a = [1]\nLocal n = a.Count()") ==
          errors_t{ "Count: Count isn't a Method; leave out the ()" });

    // It goes where an Interface it implements is expected, and a generic call takes T from what
    // it implements.
    const std::string imports = "Import std.core\nImport std.collections\n";
    CHECK(probe(imports + "Local items: Iterable<Int> = [1]\nLocal x = items") ==
          "Iterable<Int32>");
    CHECK(probe(imports + "Function First<T>(values: Iterable<T>): T? => values.Iterate().First()\n"
                          "Local x = First([\"a\"])") == "String?");
    CHECK(probe(imports + "Function First<T>(values: Iterable<T>): T? => values.Iterate().First()\n"
                          "Local x = First(Set<Int>())") == "Int32?");
    CHECK(errors_of(imports + "Local items: Iterable<Int> = [\"a\"]") ==
          errors_t{ "\"a\": expected Int32, found String" });
    CHECK(errors_of("Import std.core\nLocal items: Printable = [Function() => 1]") ==
          errors_t{ "[Function() => 1]: expected Printable, found (Func(): Int32)[]" });

    // Its Capacity is how many elements it has room for, which Reserve and TrimExcess change.
    CHECK(probe("Local a = [1]\nLocal x = a.Capacity") == "Int32");
    CHECK(errors_of("Local a = [1]\na.Reserve(10)\na.TrimExcess()").empty());
    CHECK(errors_of("Local a = [1]\na.Capacity = 3") ==
          errors_t{ "a.Capacity: an array's Capacity can't be set; use Reserve or TrimExcess" });
    CHECK(errors_of("Local a = [1]\nLocal n = a.Capacity()") ==
          errors_t{ "Capacity: Capacity isn't a Method; leave out the ()" });
    CHECK(errors_of("Local g: Int[3]\nLocal n = g.Capacity") ==
          errors_t{ "Capacity: Int32[3] has no member named 'Capacity'" });
    CHECK(errors_of("Local g: Int[3]\ng.Reserve(4)") ==
          errors_t{ "Reserve: Int32[3] has no member named 'Reserve'" });
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
          errors_t{ "5: 'For ... In' goes over arrays, Channels, Iterators, and values whose "
                    "Iterate() gives an Iterator, not Int32" });
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

    CHECK(errors_of("Import std.core\n"
                    "Local n = 0\n"
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
    CHECK(errors_of("Import std.core\n"
                    "Defer If True\n    Print(\"a\")\nEnd If") ==
          rejected("Defer If True\n    Print(\"a\")\nEnd If"));
    CHECK(errors_of("Import std.core\n"
                    "Defer Defer Print(\"a\")") == rejected("Defer Defer Print(\"a\")"));
}

TEST_CASE("a Defer's statement runs as its block exits", "[sema]") {
    constexpr std::string_view enemy = "Import std.core\n"
                                       "Class E\n    Field hp: Int\nEnd Class\n";
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
    CHECK(errors_of("Import std.core\n"
                    "Local n: Int\nDefer n = 1\nPrint(\"{n}\")") ==
          errors_t{ "n: 'n' is read before it's assigned" });
    CHECK(errors_of("Import std.core\n"
                    "Local n: Int\nDefer Print(\"{n}\")\nn = 1") ==
          errors_t{ "n: 'n' is read before it's assigned" });
}

// Errors

/// Two errors, and a function that can throw either.
constexpr std::string_view two_errors = "Import std.core\n"
                                        "Class FileError Extends Error\n"
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
    CHECK(probe("Import std.core\n"
                "Local e = Error(\"x\")") == "Error");
    CHECK(probe("Import std.core\n"
                "Local m = Error().Message") == "String");
    CHECK(probe("Import std.core\n"
                "Class Oops Extends Error\nEnd Class\nLocal e: Error = Oops(\"x\")") ==
          "Error (from Oops)");

    // A program's own Error hides std's for its code, but errors still extend std's.
    CHECK(errors_of("Import std.core\n"
                    "Class Error\nEnd Class")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Class Error\nEnd Class\nThrow Error()") ==
          errors_t{ "Error(): Throw takes an object of a Class that extends std.core.error.Error, "
                    "not test.Error" });

    // Where the code has an Error of its own, the one errors extend says whose it is.
    CHECK(errors_of("Import std.core\n"
                    "Class Error\nEnd Class\nClass Oops Extends Error\nEnd Class\nThrow Oops()") ==
          errors_t{ "Oops(): Throw takes an object of a Class that extends std.core.error.Error, "
                    "not Oops" });
    CHECK(errors_of("Import std.core\n"
                    "Class Error\nEnd Class\nFunction F() Throws Error\nEnd Function") ==
          errors_t{ "Error: Throws lists Classes that extend std.core.error.Error, "
                    "not test.Error" });

    // So do a Catch's, and a message that shows one beside a T? of the other.
    CHECK(errors_of("Import std.core\n"
                    "Class Error\n"
                    "End Class\n"
                    "Local last: Error? = Null\n"
                    "Try\n"
                    "    Local x = 1\n"
                    "Catch e\n"
                    "    last = e\n"
                    "End Try") ==
          errors_t{ "e: nothing in the Try block throws std.core.error.Error",
                    "e: expected test.Error?, found std.core.error.Error" });
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
    CHECK(errors_of("Import std.core\n"
                    "Function F(e: Error?) Throws Error\n    Throw e\nEnd Function") ==
          errors_t{ "e: 'e' may be Null; check that it isn't, or use '?.'" });
}

TEST_CASE("an error has a Cause and the Place Throw gives it, and each area has its own",
          "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Local io = IOError(IOErrorKind.NOT_FOUND, \"gone\", \"a.txt\")\n"
                    "Local parse = ParseError(ParseErrorKind.SYNTAX, \"bad\", 3, 4, io)\n"
                    "Local where = \"{parse.Place} {parse.Line}:{parse.Column}\"\n"
                    "Local kinds = [io.Kind == IOErrorKind.NOT_FOUND, io.Path == \"a.txt\"]\n"
                    "Local cause: Error? = parse.Cause\n"
                    "Local others: Error[] = [\n"
                    "    ArgumentError(ArgumentErrorKind.MISSING, \"no --out\", \"out\"),\n"
                    "    FormatError(FormatErrorKind.BAD_TEMPLATE, \"{{\"),\n"
                    "    TimeoutError(\"slow\"),\n"
                    "    NetError(NetErrorKind.REFUSED, \"no\", \"127.0.0.1:80\"),\n"
                    "    DatabaseError(DatabaseErrorKind.BUSY, \"locked\")\n"
                    "]")
              .empty());

    // Only Throw sets the Place, and the Kind and details are the New's to set.
    CHECK(errors_of("Import std.core\n"
                    "Local e = IOError(IOErrorKind.OTHER, \"x\")\n"
                    "e.Place = Error().Place\n"
                    "e.Kind = IOErrorKind.NOT_FOUND\n"
                    "e.Cause = Error(\"why\")") ==
          errors_t{
              "e.Place: 'Place' is ReadOnly; only Error's New can change it",
              "e.Kind: 'Kind' is ReadOnly; only IOError's New can change it",
          });
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
    CHECK(errors_of("Import std.core\n"
                    "Class Oops Extends Error\n"
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
          errors_t{ "t: String? can't be shown by 'Print', since it may be Null; give a "
                    "value for Null with '?"
                    "?'" });
}

TEST_CASE("a Defer and a Finally can't let errors out, or leave", "[sema]") {
    CHECK(with_errors("Defer Try Load()") ==
          errors_t{ "Load(): a Defer can't throw, so FileError has to be caught in it",
                    "Load(): a Defer can't throw, so ParseError has to be caught in it" });
    CHECK(with_errors("Defer Try? Load()").empty());
    CHECK(with_errors("Try\n    Print(\"x\")\nFinally\n    Load()\nEnd Try") ==
          errors_t{ "Load(): a Finally can't throw, so calling 'Load', which can throw "
                    "FileError, ParseError, needs Try?, or a Try block inside it" });
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
    CHECK(errors_of("Import std.core\n"
                    "For i = 1 To 2\n"
                    "    Try\n"
                    "        Print(\"x\")\n"
                    "    Finally\n"
                    "        Break\n"
                    "    End Try\n"
                    "End For") == errors_t{ "Break: Break can't leave a Finally" });
    CHECK(errors_of("Import std.core\n"
                    "Try\n"
                    "    Print(\"x\")\n"
                    "Finally\n"
                    "    For i = 1 To 2\n"
                    "        Continue\n"
                    "    End For\n"
                    "End Try")
              .empty());
}

TEST_CASE("a New can throw, and its callers handle it", "[sema]") {
    constexpr std::string_view base = "Import std.core\n"
                                      "Class Bad Extends Error\n"
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
          errors_t{ "New: A's New can throw, so this New has to call 'Try Super.New()'" });
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
    constexpr std::string_view node = "Import std.core\n"
                                      "Class Node\n"
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

TEST_CASE("Where T: Class says a type parameter is a Class, which a Weak<T> refers to", "[sema]") {
    constexpr std::string_view node = "Import std.core\n"
                                      "Class Node\n"
                                      "    Field name: String\n"
                                      "End Class\n"
                                      "Class Leaf Extends Node\n"
                                      "End Class\n"
                                      "Struct Point\n"
                                      "    Field x: Int\n"
                                      "End Struct\n"
                                      "Interface Shape\n"
                                      "    Method Area(): Float\n"
                                      "End Interface\n"
                                      "Class Holder<T>\n"
                                      "    Where T: Class\n"
                                      "    Field Item: Weak<T>\n"
                                      "    Method Get(): T? => Item\n"
                                      "End Class\n";
    const auto with_node = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", node, text));
    };

    // A Weak<T> is assigned its T, and [Weak x], RefEquals, 'Is', and 'Case ... As' take one.
    CHECK(probe(std::format("{}Local h = Holder<Node>()\nh.Item = Leaf()\nLocal n = h.Get()",
                            node)) == "Node?");
    CHECK(with_node("Function Keep<O: Class>(owner: O): Weak<O>\n"
                    "    Local kept: Weak<O> = owner\n"
                    "    Return kept\n"
                    "End Function\n"
                    "Function Later<O: Class>(owner: O): Func(): Bool\n"
                    "    Return Function[Weak owner]() => owner != Null\n"
                    "End Function\n"
                    "Function Same<T: Class>(a: T, b: T?): Bool => RefEquals(a, b) && a Is Leaf\n"
                    "Function Find<T: Class>(x: Node): Bool => x Is T\n"
                    "Function Name<T: Class>(x: T): String\n"
                    "    Select x\n"
                    "        Case leaf As Leaf\n"
                    "            Return leaf.name\n"
                    "        Case found As T\n"
                    "            Return \"T\"\n"
                    "    End Select\n"
                    "    Return \"\"\n"
                    "End Function\n"
                    "Local a = Same(Leaf(), Null) || Find<Leaf>(Node())\n"
                    "Local b = Name(Leaf()) + Name(Node())\n"
                    "Local c = Keep(Node())\n"
                    "Local d = Later(Leaf())")
              .empty());
    CHECK(with_node("Function Same<T: Class>(a: T, b: T): Bool => a Is T") ==
          errors_t{ "a Is T: this is always True: 'a' is T already" });

    // A Class, an instance of a generic one, and a Shared Class meet it; nothing else does.
    CHECK(with_node("Shared Class Hub\nEnd Class\n"
                    "Local a = Holder<Leaf>()\n"
                    "Local b = Holder<Holder<Node>>()\n"
                    "Local c = Holder<Hub>()")
              .empty());
    CHECK(with_node("Local a = Holder<Point>()\n"
                    "Local b = Holder<Shape>()\n"
                    "Local c = Holder<Int>()") ==
          errors_t{
              "Holder<Point>: Point isn't a Class, which Holder<T> needs for T",
              "Holder<Shape>: Shape isn't a Class, which Holder<T> needs for T; an Interface's "
              "value can be an array too",
              "Holder<Int>: Int32 isn't a Class, which Holder<T> needs for T",
          });
    CHECK(with_node("Local d = Holder<String>()\n"
                    "Local e = Holder<Node[]>()\n"
                    "Local f = Holder<Func()>()\n"
                    "Local g = Holder<Node?>()") ==
          errors_t{
              "Holder<String>: String isn't a Class, which Holder<T> needs for T",
              "Holder<Node[]>: Node[] isn't a Class, which Holder<T> needs for T",
              "Holder<Func()>: Func() isn't a Class, which Holder<T> needs for T",
              "Holder<Node?>: Node? isn't a Class, which Holder<T> needs for T",
          });

    // Without it, a type parameter may be anything, so none of these take it.
    CHECK(with_node("Function A<T>(x: T): Weak<T> => x\n"
                    "Function B<T>(x: T): Bool => RefEquals(x, x)") ==
          errors_t{
              "Weak<T>: Weak<T> refers to an object of a Class, not T; a constraint like <T: "
              "Class> says it's one",
              "x: RefEquals compares Class references, not T; a constraint like <T: Class> says "
              "it's one",
              "x: RefEquals compares Class references, not T; a constraint like <T: Class> says "
              "it's one",
          });
    CHECK(with_node("Function C<T>(x: T): Func(): Bool => Function[Weak x]() => True\n"
                    "Function D<T>(x: T): Bool => x Is Node\n"
                    "Function E<T>(x: Node): Bool => x Is T") ==
          errors_t{
              "x: [Weak x] needs a reference to an object, not T; a constraint like <T: Class> "
              "says it's one",
              "x: 'Is' tests objects of Classes, not T; a constraint like <T: Class> says it's "
              "one",
              "T: 'Is' tests for a Class or Interface, not T; a constraint like <T: Class> says "
              "it's one",
          });

    // A T? gets the same hint, and an Interface's Self, which takes no constraint, none.
    CHECK(with_node("Function B<T>(x: T?): Bool => RefEquals(x, x)\n"
                    "Function C<T>(x: T?): Func(): Bool => Function[Weak x]() => True\n"
                    "Interface Sized\n"
                    "    Method Same(other: Sized): Bool => other Is Self\n"
                    "End Interface") ==
          errors_t{
              "x: RefEquals compares Class references, not T?; a constraint like <T: Class> "
              "says it's one",
              "x: RefEquals compares Class references, not T?; a constraint like <T: Class> "
              "says it's one",
              "x: [Weak x] needs a reference to an object, not T?; a constraint like <T: Class> "
              "says it's one",
              "Self: 'Is' tests for a Class or Interface, not Self",
          });

    // A Weak<T> parameter learns T from the T or T? it's given; where nothing says what T is, the
    // error has no example, since no type every program has is a Class.
    CHECK(probe(std::format("{}Function Read<T: Class>(w: Weak<T>): T? => w\n"
                            "Local h = Holder<Leaf>()\n"
                            "Local n = Read(Leaf()) ?? Read(h.Get())",
                            node)) == "Leaf?");
    CHECK(with_node("Function Make<T: Class>(): T? => Null\nLocal m = Make()") ==
          errors_t{ "Make: can't tell what T is in this call; write the type arguments" });

    // It's only a constraint, and gives only what every Class has: it's Equatable.
    CHECK(with_node("Function Same<T: Class>(a: T, b: T): Bool => a == b && a != b").empty());
    CHECK(with_node("Function F<T: Class>(x: T): String => x.name\n"
                    "Function G<T: Class>(x: T): UInt64 => x.Hash()") ==
          errors_t{
              "name: T's constraints have no member named 'name'",
              "Hash: T's constraints have no member named 'Hash'",
          });
}

TEST_CASE("Class and Error are constraints any module's Public code can have", "[sema]") {
    const std::vector<fb::library_file_t> holder{
        {
            .path = "lib/holder.fb",
            .text = "Import std.core\n"
                    "Public Class Holder<T>\n"
                    "    Where T: Class\n"
                    "    Public Field Item: Weak<T>\n"
                    "    Public Method Keep<O: Class>(owner: O): Weak<O> => owner\n"
                    "End Class\n"
                    "Public Function Rethrow<E: Error>(error: E): Int Throws E\n"
                    "    Throw error\n"
                    "End Function\n",
        },
    };
    CHECK(errors_with("Import std.core\n"
                      "Import \"./lib/holder\"\n"
                      "Class Node\nEnd Class\n"
                      "Local h = Holder<Node>()\n"
                      "Local w = h.Keep(Node())\n"
                      "Local n = Try? Rethrow(Error(\"no\"))",
                      holder)
              .empty());
}

TEST_CASE("a Weak<T> of a Class that's Sendable is Sendable, a type parameter's too", "[sema]") {
    constexpr std::string_view hub = "Import std.core\n"
                                     "Shared Class Hub\n"
                                     "End Class\n";
    CHECK(errors_of(std::format("{}Shared Class Registry<T>\n"
                                "    Where T: Class & Sendable\n"
                                "    ReadOnly Field First: Weak<T>\n"
                                "    New(first: T)\n"
                                "        First = first\n"
                                "    End New\n"
                                "End Class\n"
                                "Local r = Registry<Hub>(Hub())",
                                hub))
              .empty());
    CHECK(errors_of(std::format("{}Shared Class Registry<T>\n"
                                "    Where T: Class\n"
                                "    ReadOnly Field First: Weak<T>\n"
                                "End Class",
                                hub)) ==
          errors_t{ "First: 'First' is ReadOnly, but threads share Registry<T>'s objects, so it "
                    "must be Sendable too: a Weak<T> stays on the thread that made it, unless T "
                    "is a Shared Class, as a type parameter that's Class & Sendable is" });
    CHECK(errors_of(std::format("{}Shared Class Registry\n"
                                "    ReadOnly Field First: Weak<Node>\n"
                                "End Class\n"
                                "Class Node\nEnd Class",
                                hub)) ==
          errors_t{ "First: 'First' is ReadOnly, but threads share Registry's objects, so it must "
                    "be Sendable too: a Weak<T> stays on the thread that made it, unless T is a "
                    "Shared Class" });
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
        return errors_of(std::format("Import std.core\n"
                                     "Class A\n"
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

    // Not before a Struct or Class itself, and a Const never changes already.
    CHECK(errors_of("Static Class C\n    Field x: Int\nEnd Class\n"
                    "ReadOnly Struct S\n    Field x: Int\nEnd Struct") ==
          errors_t{ "C: Static only goes before a Field or Method of a Struct or Class",
                    "S: ReadOnly only goes before a Field or Property" });
    CHECK(errors_of("ReadOnly Const X = 1\n"
                    "Function F()\n"
                    "    ReadOnly Const Y = 2\n"
                    "End Function") ==
          errors_t{ "X: a Const never changes, so it takes no ReadOnly",
                    "Y: a Const never changes, so it takes no ReadOnly" });
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

    // A lambda or a Parallel For's body in New runs later, or on other threads, so it can't.
    CHECK(errors_of("Class A\n"
                    "    ReadOnly Field id: Int\n"
                    "    Field f: Func(Int)?\n"
                    "    New()\n"
                    "        id = 1\n"
                    "        f = Function(v: Int)\n"
                    "            id = v\n"
                    "        End Function\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "Function: this lambda captures Self, so keeping it in a field of Self makes a "
                    "cycle that's never freed; capture it with [Weak Self]",
                    "id: 'id' is ReadOnly, and a lambda in New runs later, so only New's own code "
                    "can change it" });
    CHECK(errors_of("Shared Class A\n"
                    "    ReadOnly Field name: String\n"
                    "    New()\n"
                    "        Parallel For i = 0 Until 10\n"
                    "            name = \"{i}\"\n"
                    "        End For\n"
                    "    End New\n"
                    "End Class") ==
          errors_t{ "name: 'name' is ReadOnly, and a Parallel For's body in New runs on other "
                    "threads, so only New's own code can change it" });

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

    // One inherited from an instance has the instance's types, as a Method does.
    const std::string_view holder = "Abstract Class Holder<T>\n"
                                    "    Abstract Property Value: T\n"
                                    "End Class\n";
    CHECK(errors_of(std::format("{}Class Ints Extends Holder<Int>\n"
                                "    Override Property Value: Int\n"
                                "        Get => 3\n"
                                "        Set(value)\n"
                                "        End Set\n"
                                "    End Property\n"
                                "End Class\n"
                                "Local h: Holder<Int> = Ints()\n"
                                "Local v = h.Value",
                                holder))
              .empty());
    CHECK(errors_of(std::format("{}Class Ints Extends Holder<Int>\n"
                                "    Override Property Value: String\n"
                                "        Get => \"3\"\n"
                                "        Set(value)\n"
                                "        End Set\n"
                                "    End Property\n"
                                "End Class",
                                holder)) ==
          errors_t{ "Value: 'Value' must be Int32, like Holder<T>'s" });
    CHECK(errors_of("Property P: Int\n    Get => 1\nEnd Property") ==
          errors_t{ "P: Properties can only be declared in a Struct or Class" });
}

TEST_CASE("Panic ends the path, and Assert goes on where its condition holds", "[sema]") {
    CHECK(errors_of("Function F(): Int\n    Panic(\"no\")\nEnd Function").empty());
    CHECK(errors_of("Function F(): Int\n    Assert(False, \"never\")\nEnd Function").empty());
    CHECK(errors_of("Import std.core\n"
                    "Function F(s: String?)\n    Assert(s != Null)\n    Print(s)\nEnd Function")
              .empty());
    CHECK(errors_of("Panic(1)") == errors_t{ "1: expected String, found Int32" });
}

TEST_CASE("Unreachable and Todo end the path, and DebugAssert narrows nothing", "[sema]") {
    CHECK(errors_of("Function F(n: Int): Int\n"
                    "    If n > 0 Then Return 1\n"
                    "    Unreachable()\n"
                    "End Function\n"
                    "Function G(): String\n"
                    "    Todo(\"G\")\n"
                    "End Function\n"
                    "Function H(): Int\n"
                    "    Unreachable(\"never\")\n"
                    "End Function")
              .empty());

    // A release build doesn't check it, so code after it can't count on it.
    CHECK(errors_of("Function F(s: String?): String\n"
                    "    DebugAssert(s != Null, \"s is set\")\n"
                    "    Return s\n"
                    "End Function") ==
          errors_t{ "s: expected String, found String?, which may be Null; check it first, or "
                    "give a value for Null with '?"
                    "?'" });
    CHECK(errors_of("Function F(): Int\n    DebugAssert(False)\nEnd Function") ==
          errors_t{ "F: 'F' doesn't return a value on every path" });
    CHECK(errors_of("Todo(1)\nUnreachable(True)\nDebugAssert(1)") ==
          errors_t{
              "1: expected String, found Int32",
              "True: expected String, found Bool",
              "1: expected Bool, found Int32",
          });

    // A Const Function can use them.
    CHECK(probe("Const Function Half(n: Int): Int\n"
                "    DebugAssert(n % 2 == 0)\n"
                "    If n < 0 Then Todo()\n"
                "    Return n / 2\n"
                "End Function\n"
                "Local x = Half(8)") == "Int32 4");
}

TEST_CASE("AssertEqual compares what's Equatable and Printable", "[sema]") {
    CHECK(errors_of("Import std.core\nAssertEqual(1 + 1, 2)\nAssertEqual(\"a\", \"a\", \"same\")")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Struct P\n    Field x: Int\nEnd Struct\n"
                    "AssertEqual(P(1), P(1))") ==
          errors_t{
              "AssertEqual: P doesn't implement Equatable, which 'AssertEqual' needs for T",
              "AssertEqual: P doesn't implement Printable, which 'AssertEqual' needs for T",
          });
    CHECK(errors_of("AssertEqual(1, 1)") ==
          errors_t{ "AssertEqual: 'AssertEqual' is in std.core, which this file doesn't import" });
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
    CHECK(probe("Import std.core\n"
                "Local f = Function() => Print(\"x\")") == "Func()");
    CHECK(probe("Import std.core\n"
                "Local f = Function()\n    Print(\"x\")\nEnd Function") == "Func()");
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
          errors_t{ "P: 'P' has overloads, so a Func or Callback type has to say which one is "
                    "meant" });
    CHECK(errors_of("Import std.core\n"
                    "Class Oops Extends Error\nEnd Class\nFunction F() Throws Oops\n"
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

    // A variable is in a capture list once.
    CHECK(errors_of("Class Node\n    Field n = 1\nEnd Class\n"
                    "Local a = Node()\n"
                    "Local f = Function[Weak a, Weak a]() => a?.n ?? -1") ==
          errors_t{ "a: 'a' is already in this capture list" });
    CHECK(with_timer("    Method Start()\n"
                     "        OnTick = Function[Weak Self, Weak Self]() => Self?.Tick()\n"
                     "    End Method") ==
          errors_t{ "Self: 'Self' is already in this capture list" });
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
    CHECK(errors_of("Import std.core\n"
                    "Local s: String? = \"a\"\n"
                    "Local f = Function()\n    If s != Null Then Print(s)\nEnd Function") ==
          errors_t{ "s: String? can't be shown by 'Print', since it may be Null; give a "
                    "value for Null with '?"
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
    // An Async one's errors point at its Function too.
    CHECK(errors_of("Local c = True\n"
                    "Local f = Async  Function(): Int\n    If c Then Return 1\nEnd Function") ==
          errors_t{ "Function: this lambda doesn't return a value on every path" });
    CHECK(errors_of("Function F(ByRef n: Int)\n    Local c = Function() => n\nEnd Function") ==
          errors_t{ "n: 'n' is ByRef, so a lambda can't capture it; copy it into a Local first" });
    CHECK(errors_of("Import std.core\n"
                    "Class Oops Extends Error\nEnd Class\n"
                    "Local f = Function()\n    Throw Oops(\"x\")\nEnd Function") ==
          errors_t{ "Oops(\"x\"): a lambda can't throw, so Oops has to be caught in it" });
    CHECK(errors_of("Loop\n    Local f = Function()\n        Break\n    End Function\n    Break\n"
                    "End Loop") == errors_t{ "Break: Break must be inside a loop" });
    CHECK(errors_of("Local f: Func() = Function() => 5") ==
          errors_t{ "5: this lambda returns nothing, so its '=>' needs a call, like '=> "
                    "Print(x)'" });
}

TEST_CASE("calling a function value", "[sema]") {
    constexpr std::string_view f = "Import std.core\n"
                                   "Local f: Func(Int): Int = Function(x) => x\n";
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

    // So does a generic Function's name passed to an overload, a constructor's too, from the
    // overload's Func parameter.
    constexpr std::string_view takers = "Function Take(f: Func(Int, Int): Int): Int => f(1, 2)\n"
                                        "Function Take(): Int => 0\n"
                                        "Class Keeper\n"
                                        "    Field Pick: Func(Int, Int): Int\n"
                                        "    New()\n"
                                        "        Pick = Max<Int>\n"
                                        "    End New\n"
                                        "    New(pick: Func(Int, Int): Int)\n"
                                        "        Pick = pick\n"
                                        "    End New\n"
                                        "End Class\n";
    CHECK(probe_generic(std::format("{}Local n = Take(Max)", takers)) == "Int32");
    CHECK(probe_generic(std::format("{}Local k = Keeper(Max)", takers)) == "Keeper");
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
    CHECK(errors_of("Import std.core\nFunction F<T>(a: T): String => \"{a}\"") ==
          errors_t{ "a: T can't be shown in a string without a constraint, like <T: Printable>" });
    CHECK(errors_of("Function F<T>(a: T): Int => a.Count") ==
          errors_t{ "Count: T has no members without a constraint, like <T: I>" });
    CHECK(errors_of("Function F<T>(): T\n    Local x: T\n    Return x\nEnd Function") ==
          errors_t{ "x: 'x' is read before it's assigned" });
}

TEST_CASE("generic code that would need instances without end is an error", "[sema]") {
    // Each instance of Deep calls one with a larger T, which has its own instance, and so on.
    CHECK(errors_of("Function Deep<T>(x: T, n: Int): Int\n"
                    "    If n == 0 Then Return 0\n"
                    "    Return Deep([x], n - 1)\n"
                    "End Function\n"
                    "Local d = Deep(1, 3)") ==
          errors_t{ "Deep([x], n - 1): 'Deep' gets T[] for T here, which leads back to this code, "
                    "so it would need a larger instance each time, without end" });
    CHECK(errors_of("Function A<T>(x: T, n: Int): Int\n"
                    "    If n == 0 Then Return 0\n"
                    "    Return B((x, 1), n - 1)\n"
                    "End Function\n"
                    "Function B<U>(y: U, n: Int): Int => A(y, n)") ==
          errors_t{ "B((x, 1), n - 1): 'B' gets (T, Int32) for U here, which leads back to this "
                    "code, so it would need a larger instance each time, without end" });
    CHECK(errors_of("Class Node<T>\n"
                    "    Method Wrap()\n"
                    "        Local outer = Node<Node<T>>()\n"
                    "    End Method\n"
                    "End Class") ==
          errors_t{ "Node<Node<T>>(): Node<T> gets Node<T> for T here, which leads back to this "
                    "code, so it would need a larger instance each time, without end" });
    // Through any type that holds T, like a Channel.
    CHECK(errors_of("Import std.core\n"
                    "Function Deep<T: Sendable>(x: T, n: Int): Int\n"
                    "    If n == 0 Then Return 0\n"
                    "    Return Deep(Channel<T>(1), n - 1)\n"
                    "End Function\n"
                    "Local d = Deep(1, 3)") ==
          errors_t{ "Deep(Channel<T>(1), n - 1): 'Deep' gets Channel<T> for T here, which leads "
                    "back to this code, so it would need a larger instance each time, without "
                    "end" });

    // A call with the same T, or a larger one that doesn't lead back, has an end.
    CHECK(errors_of("Function Same<T>(x: T, n: Int): Int\n"
                    "    If n == 0 Then Return 0\n"
                    "    Return Same(x, n - 1)\n"
                    "End Function\n"
                    "Function Wrap<T>(x: T): T[] => [x]\n"
                    "Function Twice<T>(x: T): T[][] => Wrap(Wrap(x))\n"
                    "Local s = Same(Twice(1), 3)\n"
                    "Class Node<T>\n    Field next: Node<Node<T>>?\nEnd Class")
              .empty());
    // So does a Field of an instance whose argument doubles, which stops being made long before
    // its names fill the memory.
    CHECK(errors_of("Class Seq<T>\n"
                    "    Field Deeper: Seq<(T, T)>?\n"
                    "    Field Value: T\n"
                    "    New(value: T)\n"
                    "        Value = value\n"
                    "    End New\n"
                    "End Class\n"
                    "Local s = Seq<Int>(1)\n"
                    "Local none = s.Deeper == Null")
              .empty());
}

TEST_CASE("a T? has Expect and OrElse, which give its value", "[sema]") {
    constexpr std::string_view found = "Import std.core\n"
                                       "Function Find(): Int? => Null\n"
                                       "Function Deep(): String?? => Null\n";
    CHECK(probe(std::format("{}Local x = Find().Expect(\"none\")", found)) == "Int32");
    CHECK(probe(std::format("{}Local x = Find().OrElse(Function() => 7)", found)) == "Int32");
    CHECK(probe(std::format("{}Local x = Deep().Expect(\"none\")", found)) == "String?");
    CHECK(errors_of(std::format("{}Local a = Find().OrElse(7)\n"
                                "Local b = Find().Expect(1)\n"
                                "Local c = Find().Other()",
                                found)) == errors_t{
                                               "7: expected Func(): Int32, found Int32",
                                               "1: expected String, found Int32",
                                               "Other: Int32? has no member named 'Other'",
                                           });
}

TEST_CASE("arrays are Cloneable, and the numbers, Bool, String, and Enums Defaultable", "[sema]") {
    constexpr std::string_view generic = "Import std.core\n"
                                         "Function Copied<T: Cloneable>(v: T): T => v.Clone()\n"
                                         "Function Made<T: Defaultable>(): T => T.DefaultValue()\n"
                                         "Enum E\n    A\nEnd Enum\n"
                                         "Class C\nEnd Class\n";
    CHECK(errors_of(std::format("{}Local a = Copied([\"x\"])\n"
                                "Local b = Made<Int64>()\n"
                                "Local c = Made<String>()\n"
                                "Local d = Made<E>()\n"
                                "Local e = Made<Bool>()",
                                generic))
              .empty());
    CHECK(errors_of(std::format("{}Local a = Made<C>()\n"
                                "Local b = Made<Int[]>()\n"
                                "Local c = Copied(C())\n"
                                "Local d = Copied(1)",
                                generic)) ==
          errors_t{
              "Made<C>: C doesn't implement Defaultable, which 'Made' needs for T",
              "Made<Int[]>: Int32[] doesn't implement Defaultable, which 'Made' needs for T",
              "Copied: C doesn't implement Cloneable, which 'Copied' needs for T",
              "Copied: Int32 doesn't implement Cloneable, which 'Copied' needs for T",
          });
    CHECK(errors_of("Import std.core\nExtend Int Implements Defaultable\nEnd Extend") ==
          errors_t{ "Defaultable: Int32 comes with the built-in Interfaces that fit it, "
                    "Defaultable among them; an Extend can give it others" });

    // A PriorityQueue is Defaultable only where its values are Comparable.
    CHECK(errors_of(std::format("Import std.collections\n{}"
                                "Local a = Made<PriorityQueue<Int>>()\n"
                                "Local b = Made<PriorityQueue<C>>()",
                                generic)) ==
          errors_t{ "Made<PriorityQueue<C>>: PriorityQueue<C> doesn't implement Defaultable, which "
                    "'Made' needs for T" });
}

TEST_CASE("generic mistakes", "[sema]") {
    // In Zero itself, the type expected, its own T, says what T is.
    CHECK(errors_of("Function Zero<T>(): T => Zero()\nLocal z = Zero()") ==
          errors_t{ "Zero: can't tell what T is in this call; write the type arguments, like "
                    "Zero<Int>(...)" });
    // A Const type parameter's example is a value.
    CHECK(errors_of("Function Sized<Const N: Int, T>(): Int => N\nLocal s = Sized()") ==
          errors_t{ "Sized: can't tell what N, T are in this call; write the type arguments, "
                    "like Sized<1, Int>(...)" });
    // Only what the call doesn't tell, not an Interface's Self and T, which its value does, and
    // the call as written, with its object's parentheses.
    CHECK(errors_of("Import std.core\n"
                    "Local v = (1 To 3).Map(Function(n: Int) => n * 2)\n"
                    "Local m = v.Map(Function(n: Int)\n"
                    "    Print(n)\n"
                    "End Function)\n"
                    "Local w = (1 To 3).Map(Function(n: Int)\n"
                    "    Print(n)\n"
                    "End Function)") ==
          errors_t{
              "v.Map: can't tell what U is in this call; write the type arguments, like "
              "v.Map<Int>(...)",
              "(1 To 3).Map: can't tell what U is in this call; write the type arguments, like "
              "(1 To 3).Map<Int>(...)",
          });
    // The example writes every type argument, with what the call told; and a call that wrote
    // too few is told that alone.
    CHECK(errors_of("Function Pair<A, B>(a: A): B[] => []\n"
                    "Function Make<T>(): T[] => []\n"
                    "Local p = Pair(1)\n"
                    "Local q = Pair<Int>(1)\n"
                    "Local r = (Make)()") ==
          errors_t{
              "Pair: can't tell what B is in this call; write the type arguments, like "
              "Pair<Int32, Int>(...)",
              "Pair<Int>: 'Pair<Int>' takes 2 type arguments, not 1",
              "Make: can't tell what T is in this call; write the type arguments, like "
              "Make<Int>(...)",
          });
    // A type argument that's only in another's constraint, which that one can't meet, is that.
    CHECK(errors_of("Import std.core\n"
                    "Import std.collections\n"
                    "Local s = Stack<Int>()\n"
                    "Local z = (1 To 3).Zip(s)\n"
                    "Local a = (1 To 3).Zip([1, 2])\n"
                    "Local f = (1 To 6).Chunks(2).Flatten()") ==
          errors_t{
              "(1 To 3).Zip: Stack<Int32> doesn't implement Iterator<U>, which 'Zip' needs for O; "
              "its Iterate() gives one",
              "(1 To 3).Zip: Int32[] doesn't implement Iterator<U>, which 'Zip' needs for O; its "
              "Iterate() gives one",
          });
    // A copy's bound on a type made of its type parameters says so, rather than asking for the
    // type arguments.
    CHECK(errors_of("Import std.core\n"
                    "Class Pairs<T> Implements Iterator<(T, T)>\n"
                    "    Method Next(): (T, T)? => Null\n"
                    "End Class\n"
                    "Local a = Pairs<Int>().Flatten()\n"
                    "Local b = Pairs<Int>().Flatten<Int>()") ==
          errors_t{
              "Flatten: Pairs<Int32> has no member named 'Flatten'; Iterator<(Int32, Int32)>'s "
              "default needs (Int32, Int32) to implement Iterable<U>",
              "Flatten: Pairs<Int32> has no member named 'Flatten'; Iterator<(Int32, Int32)>'s "
              "default needs (Int32, Int32) to implement Iterable<U>",
          });
    CHECK(with_generics("Local s: Pile") ==
          errors_t{ "Pile: Pile<T> is generic, so it needs type arguments, like Pile<Int>" });
    CHECK(with_generics("Local s: Pile<Int, Int>") ==
          errors_t{ "Pile<Int, Int>: Pile<T> takes 1 type argument, not 2" });
    CHECK(errors_of("Local d: Int<Int>") == errors_t{ "Int<Int>: 'Int' isn't generic" });
    CHECK(errors_of("Function F<T, T>()\nEnd Function") ==
          errors_t{ "T: 'T' is already a type parameter here" });
    // Nor a type's name, Extend T[]'s T too.
    CHECK(errors_of("Struct Foo\n"
                    "    Field n: Int\n"
                    "End Struct\n"
                    "Function F<Foo>(x: Foo): Foo => x\n"
                    "Struct Box<Foo>\n"
                    "    Field v: Foo\n"
                    "End Struct") ==
          errors_t{ "Foo: 'Foo' is a type here, so a type parameter can't take its name",
                    "Foo: 'Foo' is a type here, so a type parameter can't take its name" });
    // One declared below, or the type's own name.
    CHECK(errors_of("Struct Late<Bar>\n"
                    "    Field v: Bar\n"
                    "End Struct\n"
                    "Class Later<Mode>\n"
                    "    Field v: Mode?\n"
                    "End Class\n"
                    "Interface Shows<Baz>\n"
                    "    Method Show(b: Baz)\n"
                    "End Interface\n"
                    "Struct Node<Node>\n"
                    "    Field v: Int\n"
                    "End Struct\n"
                    "Struct Bar\n"
                    "    Field n: Int\n"
                    "End Struct\n"
                    "Enum Mode\n"
                    "    A\n"
                    "End Enum\n"
                    "Class Baz\n"
                    "End Class") ==
          errors_t{
              "Bar: 'Bar' is a type here, so a type parameter can't take its name",
              "Mode: 'Mode' is a type here, so a type parameter can't take its name",
              "Baz: 'Baz' is a type here, so a type parameter can't take its name",
              "Node: 'Node' is a type here, so a type parameter can't take its name",
          });
    CHECK(errors_of("Struct T\n"
                    "    Field n: Int\n"
                    "End Struct\n"
                    "Extend T[]\n"
                    "    Method First(): Int => Self[0].n\n"
                    "End Extend") ==
          errors_t{ "T: 'T' is a type here, so Extend T[] can't name every array; a type "
                    "parameter can't take a type's name" });
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
    // A literal takes the type of the Operator's parameter, one too large for an Int too.
    CHECK(probe_money("Local x = m * 3") == "Money");
    CHECK(probe_money("Local x = m * 5_000_000_000") == "Money");
    // So does a literal expression whose value only an Int64 holds.
    CHECK(probe_money("Local x = m * (1 << 40)") == "Money");
    CHECK(probe_money("Local x = m * (2_000_000_000 + 2_000_000_000)") == "Money");
    CHECK(with_money("Local x = m * (1 << 40)").empty());
    CHECK(probe_money("Local x = m == m") == "Bool");
    // != is the opposite of ==.
    CHECK(probe_money("Local x = m != m") == "Bool");
    CHECK(with_money("m += m\nm = -m").empty());

    // A generic type's Operators take its type arguments from the operands.
    CHECK(probe("Struct Box<T>\n    Field Value: T\n"
                "    Operator <<(a: Box<T>, value: T): Box<T> => Box(value)\nEnd Struct\n"
                "Local b = Box(1) << 2") == "Box<Int32>");
    // A shift's amount takes the type of the Operator's parameter too.
    CHECK(probe("Struct Bits\n    Field Value: Long\n"
                "    Operator <<(a: Bits, count: Long): Bits => Bits(a.Value)\nEnd Struct\n"
                "Local b = Bits(1) << 5_000_000_000") == "Bits");
}

TEST_CASE("an operator a type doesn't define stays an error", "[sema]") {
    CHECK(with_money("Local x = m / m") == errors_t{ "m / m: '/' can't be used on Money" });
    CHECK(with_money("Local x = m + \"a\"") ==
          errors_t{ "m + \"a\": no Operator + takes (Money, String)" });
    CHECK(with_money("Local x = m * 2.5") ==
          errors_t{ "m * 2.5: no Operator * takes (Money, Float32)" });
    CHECK(with_money("Local x = m * 1.0e300") ==
          errors_t{ "m * 1.0e300: no Operator * takes (Money, Float64)" });
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

TEST_CASE("a range is std's Range<T> of the integers a For loop with its bounds counts", "[sema]") {
    CHECK(probe("Import std.core\nLocal r = 0 Until 10") == "Range<Int32>");
    CHECK(probe("Import std.core\nLocal n: Int64 = 5\nLocal r = 0 To n Step 2") == "Range<Int64>");
    CHECK(probe("Import std.core\nLocal r: Range<UInt8> = 0 Until 3") == "Range<UInt8>");
    CHECK(errors_of("Import std.core\nLocal a = 0.5 To 2.5") ==
          errors_t{ "0.5: a range counts with integers, not Float32" });
    CHECK(errors_of("Import std.core\nLocal b = 1 To 10 Step 0") ==
          errors_t{ "0: Step can't be 0" });
    CHECK(errors_of("Import std.core\nLocal c: UInt32 = 3\nLocal d = c To 9 Step -1") ==
          errors_t{ "-1: -1 doesn't fit in UInt32" });
    CHECK(errors_of("Import std.core\nLocal f = 0 Until 10 Until 20") ==
          errors_t{ "0 Until 10: a range counts with integers, not Range<Int32>" });
}

TEST_CASE("Operator [] and Operator []= index a type's values, as Methods of Self", "[sema]") {
    constexpr std::string_view grid = "Struct Grid\n"
                                      "    Field Cells: Int[]\n"
                                      "    Operator [](x: Int, y: Int): Int => Cells[y * 2 + x]\n"
                                      "    Operator []=(x: Int, y: Int, value: Int)\n"
                                      "        Cells[y * 2 + x] = value\n"
                                      "    End Operator\n"
                                      "    Operator [](name: String): String => name\n"
                                      "End Struct\n"
                                      "Local g = Grid([1, 2, 3, 4])\n";
    CHECK(probe(std::format("{}Local x = g[1, 0]", grid)) == "Int32");
    CHECK(probe(std::format("{}Local x = g[\"a\"]", grid)) == "String");
    CHECK(
        errors_of(std::format("{}g[1, 1] = 5\ng[0, 0] += 2\ng[0, 1]++\ng[1, 0]--", grid)).empty());

    // An Interface that requires them still has values, and a type parameter it constrains
    // indexes through them.
    constexpr std::string_view indexed = "Interface Indexed\n"
                                         "    Operator [](i: Int): Int\n"
                                         "End Interface\n";
    CHECK(errors_of(std::format("{}Function F(x: Indexed): Int => x[0]\n"
                                "Function G<T: Indexed>(x: T): Int => x[1]",
                                indexed))
              .empty());

    // A generic type's take the instance's types.
    CHECK(probe("Struct Row<T>\n    Field Items: T[]\n    Operator [](i: Int): T => Items[i]\n"
                "End Struct\nLocal x = Row<String>([\"a\"])[0]") == "String");
}

TEST_CASE("index Operator mistakes", "[sema]") {
    const auto in_struct = [](const std::string_view member) {
        return errors_of(std::format("Struct M\n    Field V = 0\n    {}\nEnd Struct", member));
    };

    CHECK(in_struct("Operator [](): Int => 1") ==
          errors_t{ "[]: Operator [] takes an index, or more" });
    CHECK(in_struct("Operator []=(value: Int)\n    End Operator") ==
          errors_t{ "[]=: Operator []= takes the indices, then the value it sets" });
    CHECK(in_struct("Operator []=(i: Int, value: Int): Int => 1") ==
          errors_t{ "[]=: Operator []= sets the element, so it gives nothing" });
    CHECK(in_struct("Static Operator [](i: Int): Int => i") ==
          errors_t{ "[]: Operator [] indexes Self, so it can't be Static" });
    CHECK(in_struct("Operator [](ByRef i: Int): Int => i") ==
          errors_t{ "[]: an Operator's parameters can't be ByRef or have defaults" });
    CHECK(in_struct("Operator [](i: Int...): Int => 1") ==
          errors_t{ "[]: an Operator takes its indices, so no parameter takes the rest of the "
                    "arguments" });

    // What Operator [] gives is a copy, which can't be changed in place or passed ByRef, and
    // assigning needs an Operator []=.
    constexpr std::string_view types = "Struct P\n    Field X = 0\nEnd Struct\n"
                                       "Struct R\n"
                                       "    Operator [](i: Int): P => P()\n"
                                       "End Struct\n"
                                       "Struct W\n"
                                       "    Operator [](i: Int): Int => i\n"
                                       "    Operator []=(i: Int, value: Int)\n"
                                       "    End Operator\n"
                                       "End Struct\n"
                                       "Function Bump(ByRef x: Int)\nEnd Function\n"
                                       "Local r = R()\nLocal w = W()\nLocal n = 0\n";
    CHECK(
        errors_of(std::format("{}r[0] = P()", types)) ==
        errors_t{ "r[0]: R has an Operator [] but no Operator []=, so 'r[0]' can't be assigned" });
    CHECK(errors_of(std::format("{}r[0].X = 1", types)) ==
          errors_t{ "r[0]: 'r[0]' is what Operator [] gives, a copy; set it whole, through "
                    "Operator []=" });
    CHECK(errors_of(std::format("{}Bump(ByRef w[0])", types)) ==
          errors_t{ "w[0]: 'w[0]' is what Operator [] gives, which has no variable to pass "
                    "ByRef" });
    CHECK(errors_of(std::format("{}w[0], n = (1, 2)", types)) ==
          errors_t{ "w[0]: 'w[0]' is set through Operator []=, which a tuple's element can't be; "
                    "assign it on a line of its own" });
    CHECK(errors_of(std::format("{}Local x = w[1.5]", types)) ==
          errors_t{ "1.5: 1.5 doesn't fit in Int32" });
    CHECK(errors_of(std::format("{}Local x = n[0]", types)) ==
          errors_t{ "n: Int32 can't be indexed" });
    // A Struct's []= changes it, as a ByRef Method does, so not what a Property gives.
    CHECK(errors_of(std::format("{}Function F(v: W)\n    v[0] = 1\nEnd Function", types)) ==
          errors_t{ "v: 'v' is a parameter and can't be changed by '[]='; copy it into a Local, "
                    "or make it ByRef" });
    constexpr std::string_view holder = "Struct Bag\n"
                                        "    Field N = 0\n"
                                        "    Operator [](i: Int): Int => N\n"
                                        "    Operator []=(i: Int, v: Int)\n"
                                        "        N = v\n"
                                        "    End Operator\n"
                                        "    ByRef Method SetN(v: Int)\n"
                                        "        N = v\n"
                                        "    End Method\n"
                                        "End Struct\n"
                                        "Class Holder\n"
                                        "    Field Inner: Bag\n"
                                        "    Property Prop: Bag\n"
                                        "        Get => Inner\n"
                                        "        Set(value)\n"
                                        "            Inner = value\n"
                                        "        End Set\n"
                                        "    End Property\n"
                                        "End Class\n"
                                        "Local h = Holder()\n";
    CHECK(errors_of(std::format("{}h.Prop[0] = 1\nh.Prop[0] += 1\nh.Prop.SetN(1)", holder)) ==
          errors_t{
              "h.Prop: 'Prop' is a Property, which gives a copy, so '[]=' would change only the "
              "copy; set the whole of it instead",
              "h.Prop: 'Prop' is a Property, which gives a copy, so '[]=' would change only the "
              "copy; set the whole of it instead",
              "h.Prop: 'Prop' is a Property, which gives a copy, so 'SetN' would change only the "
              "copy; set the whole of it instead",
          });

    // An Interface's []= is ByRef, for its Structs, but its values are objects.
    CHECK(errors_of("Interface Indexed\n"
                    "    Operator []=(i: Int, value: Int)\n"
                    "End Interface\n"
                    "Function F(items: Indexed[])\n"
                    "    items[1][0] = 4\n"
                    "End Function")
              .empty());

    // op= and ++ work the indices out once for both, so []= takes them as [] does.
    CHECK(errors_of("Struct Mixed\n"
                    "    Operator [](i: Int64): Int => 1\n"
                    "    Operator []=(i: Int, value: Int)\n"
                    "    End Operator\n"
                    "End Struct\n"
                    "Local m = Mixed()\nm[1] += 1") ==
          errors_t{ "m[1]: Operator [] takes the indices here as (Int64), which no Operator []= of "
                    "Mixed takes, so 'm[1]' can't work them out once for both" });
    CHECK(errors_of("Struct Names\n"
                    "    Operator [](i: Int): String => \"a\"\n"
                    "    Operator []=(i: Int, value: String)\n"
                    "    End Operator\n"
                    "End Struct\n"
                    "Local n = Names()\nn[1]++") ==
          errors_t{ "n[1]: '++' can't be used on String" });
    CHECK(errors_of(std::format("{}Local g = Bag()\ng[\"s\"] += 1", holder)) ==
          errors_t{ "\"s\": expected Int32, found String" });

    // The count of indices is the index's, and reading needs an Operator [].
    CHECK(errors_of("Struct Board\n"
                    "    Operator []=(x: Int, y: Int, value: String)\n"
                    "    End Operator\n"
                    "End Struct\n"
                    "Local b = Board()\nb[1] = \"X\"\nLocal s = b[0, 1]") ==
          errors_t{
              "b[1]: Board's Operator []= doesn't take 1 index",
              "b[0, 1]: Board has an Operator []= but no Operator [], so 'b[0, 1]' can't be read",
          });

    // An index Operator is overridden as one, and can't throw or have type parameters.
    CHECK(errors_of("Class Base\n    Operator [](i: Int): Int => i\nEnd Class\n"
                    "Class Child Extends Base\n    Operator [](i: Int): Int => i * 2\nEnd Class") ==
          errors_t{ "[]: '[]' overrides Base's; write 'Override Operator []'" });
    CHECK(errors_of("Import std.core\nStruct S\n    Operator [](i: Int): Int Throws Error => i\n"
                    "    Operator []<U>(u: U): Int => 1\nEnd Struct") ==
          errors_t{
              "[]: an Operator can't throw",
              "[]: an Operator can't have type parameters of its own",
          });
}

TEST_CASE("a type with Operator == can be compared, whatever its fields are", "[sema]") {
    CHECK(errors_of("Struct Bag\n    Field Items: Int[] = []\n"
                    "    Operator ==(a: Bag, b: Bag): Bool => a.Items.Count == b.Items.Count\n"
                    "End Struct\n"
                    "Local bags = [Bag()]\nLocal found = bags.Contains(Bag())")
              .empty());
}

TEST_CASE("a T? and a T compare as two T?s, with the T's Operator ==", "[sema]") {
    // A T converts to a T?, so == between them compares the values inside, as between two T?s,
    // and so does a Weak<T>, whose read is a T?.
    const std::string_view types = "Struct S\n"
                                   "    Field n: Int\n"
                                   "    Operator ==(a: S, b: S): Bool => a.n == b.n\n"
                                   "End Struct\n"
                                   "Class Box\n"
                                   "    Operator ==(a: Box, b: Box): Bool => True\n"
                                   "End Class\n";
    CHECK(errors_of(std::format("{}"
                                "Function Same(x: S?, y: S, a: Box?, b: Box): Bool\n"
                                "    Local w: Weak<Box> = b\n"
                                "    Return x == y && y != x && a == b && b != a && w == b\n"
                                "End Function",
                                types))
              .empty());

    // An Operator still takes only its own types.
    CHECK(errors_of(std::format("{}Function F(x: S?, b: Box): Bool => x == b", types)) ==
          errors_t{ "x == b: no Operator == takes (S?, Box)" });
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
    CHECK(errors_of("Import std.core\n"
                    "Interface Hurts\n    Method Hurt()\n    Property Hp: Int\nEnd Interface\n"
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

    // A constraint that names no Interface is reported once, and nothing it would allow is.
    CHECK(errors_of("Function Big<T: Ordered>(a: T, b: T): Bool => a < b || -a == b\n"
                    "Function Name<T: Named>(x: T): String => \"{x.Name} {x}\"\n"
                    "Function Same<T: Sameish>(xs: T[], x: T): Bool => xs.Contains(x)") ==
          errors_t{
              "Ordered: 'Ordered' isn't a type",
              "Named: 'Named' isn't a type",
              "Sameish: 'Sameish' isn't a type",
          });
}

TEST_CASE("a missing Import's constraint is reported once, and nothing it would allow is",
          "[sema]") {
    // Without Import std.core, a For ... In over an Iterator, a Numeric's Static, a Sendable
    // value's Spawn, and a Catch of the Error a Throw can't name aren't errors too.
    CHECK(errors_of("Function Seq<T: Iterator<Int>>(x: T): Int\n"
                    "    Local n = 0\n"
                    "    For v In x\n"
                    "        n += v\n"
                    "    End For\n"
                    "    Return n\n"
                    "End Function\n"
                    "Function Sum<T: Numeric>(xs: T[]): T => T.Zero()\n"
                    "Function Send<T: Sendable>(x: T): T => Spawn(Function() => x).Wait()\n"
                    "Function Fails()\n"
                    "    Try\n"
                    "        Throw Error(\"x\")\n"
                    "    Catch e\n"
                    "        Assert(e.Message == \"x\")\n"
                    "    End Try\n"
                    "End Function") ==
          errors_t{
              "Iterator<Int>: 'Iterator' is in std.core, which this file doesn't import",
              "Numeric: 'Numeric' is in std.core, which this file doesn't import",
              "Sendable: 'Sendable' is in std.core, which this file doesn't import",
              "Error: 'Error' is in std.core, which this file doesn't import",
          });
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
                   std::format("Import std.core\nFunction F<T: {}>(x: T)\nEnd Function\nF({})",
                               constraint, value))
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

    CHECK(errors_of("Import std.core\n"
                    "Function Max<T: Comparable>(a: T, b: T): T => a > b ? a : b\n"
                    "Local m = Max(True, False)") ==
          errors_t{ "Max: Bool doesn't implement Comparable, which 'Max' needs for T" });

    // Among overloads, the one that fits but for its Where lines says which isn't met.
    CHECK(errors_of("Import std.core\n"
                    "Class Thing\nEnd Class\n"
                    "Class Line<T>\n"
                    "    New()\n        Where T: Comparable\n    End New\n"
                    "    New(before: Func(T, T): Bool)\n    End New\n"
                    "End Class\n"
                    "Local line = Line<Thing>()") ==
          errors_t{ "Line<Thing>: Thing doesn't implement Comparable, which Line<T> needs for T" });
    CHECK(errors_of("Import std.core\n"
                    "Class Thing\nEnd Class\n"
                    "Function Sorted<T>(items: T[])\n    Where T: Comparable\nEnd Function\n"
                    "Function Sorted<T>(items: T[], before: Func(T, T): Bool)\nEnd Function\n"
                    "Sorted([Thing()])") ==
          errors_t{ "Sorted: Thing doesn't implement Comparable, which 'Sorted' needs for T" });
}

TEST_CASE("a Struct or Class that lists Equatable or Hashable can have the built-in == and Hash",
          "[sema]") {
    CHECK(probe("Import std.core\n"
                "Struct P Implements Hashable\n    Field X: Int\n    Field Name: String\n"
                "End Struct\nLocal h = P(1, \"a\").Hash()") == "UInt64");
    CHECK(probe("Import std.core\n"
                "Class C Implements Hashable\nEnd Class\nLocal h = C().Hash()") == "UInt64");
    CHECK(errors_of("Import std.core\n"
                    "Struct Money Implements Hashable\n"
                    "    Field Cents: Long\n"
                    "    Operator ==(a: Money, b: Money): Bool => a.Cents == b.Cents\n"
                    "End Struct") ==
          errors_t{ "Hashable: Money has an Operator == of its own, so it needs a Hash that agrees "
                    "with it" });
    CHECK(errors_of("Import std.core\n"
                    "Struct Rock\nEnd Struct\n"
                    "Struct Bag Implements Hashable\n    Field Owner: Rock\nEnd Struct") ==
          errors_t{ "Hashable: 'Owner' isn't Hashable, so Bag needs a Hash of its own to implement "
                    "Hashable" });
    CHECK(errors_of("Import std.core\n"
                    "Struct Holder Implements Equatable\n    Field Run: Func(): Int\nEnd Struct") ==
          errors_t{ "Equatable: 'Run' can't be compared with ==, so Holder needs an Operator == of "
                    "its own to implement Equatable" });

    // So does an == that comes after the built-in Hash, from an Interface's default an Extend
    // gives, or an Extend of one instance.
    const std::string_view keyed = "Interface Keyed\n"
                                   "    Method Key(): Int\n"
                                   "    Operator ==(a: Self, b: Self): Bool => a.Key() == b.Key()\n"
                                   "End Interface\n";
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Struct P Implements Hashable\n"
                                "    Field N: Int\n"
                                "    Method Key(): Int => N\n"
                                "End Struct\n"
                                "Extend P Implements Keyed\nEnd Extend",
                                keyed)) ==
          errors_t{ "Hashable: P has an Operator == of its own, so it needs a Hash that agrees "
                    "with it" });
    const std::string_view box = "Import std.core\n"
                                 "Struct Box<T: Hashable> Implements Hashable\n"
                                 "    Field Value: T\n"
                                 "End Struct\n";
    CHECK(errors_of(std::format("{}Extend Box<Int>\n"
                                "    Operator ==(a: Box<Int>, b: Box<Int>): Bool => True\n"
                                "End Extend",
                                box)) ==
          errors_t{ "Hashable: Box<Int32> has an Operator == of its own, so it needs a Hash that "
                    "agrees with it" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Extend Box<Int> Implements Hashable\n"
                                "    Operator ==(a: Box<Int>, b: Box<Int>): Bool => True\n"
                                "    Method Hash(): UInt64 => 0\n"
                                "End Extend",
                                box))
              .empty());

    // A Class's built-in == compares objects, whatever its fields are, and one extending an
    // instance with an == of its own has that ==.
    const std::string_view bag = "Class Bag<T>\n"
                                 "    Field Items: T[] = []\n"
                                 "End Class\n"
                                 "Extend Bag<String>\n"
                                 "    Operator ==(a: Bag<String>, b: Bag<String>): Bool => True\n"
                                 "End Extend\n";
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Class Kept<T> Extends Bag<Int> Implements Hashable\nEnd Class",
                                bag))
              .empty());
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Class Kept<T> Extends Bag<T> Implements Hashable\nEnd Class",
                                bag)) ==
          errors_t{ "Hashable: Kept<String> has an Operator == of its own, so it needs a Hash "
                    "that agrees with it" });

    // Another file's == too, since == is what == means for the type in that file's Sets.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Import std.core\n"
                    "Public Struct P Implements Hashable\n    Public Field N: Int\nEnd Struct\n",
        },
    };
    CHECK(errors_with(std::format("Import \"./lib/a\"\n{}"
                                  "Extend P Implements Keyed\n"
                                  "    Method Key(): Int => N\n"
                                  "End Extend",
                                  keyed),
                      modules) ==
          errors_t{ "lib/a.fb: Hashable: P has an Operator == of its own, so it needs a Hash "
                    "that agrees with it" });

    // Comparable gives the other comparisons from <.
    CHECK(probe("Import std.core\n"
                "Struct M Implements Comparable\n"
                "    Field V: Int\n"
                "    Operator <(a: M, b: M): Bool => a.V < b.V\n"
                "End Struct\n"
                "Local x = M(1) >= M(2)") == "Bool");
}

TEST_CASE("every Class is Equatable and Hashable without listing them", "[sema]") {
    // With its own == and Hash, or else comparing and hashing its object, generic or not, and a
    // T? of one too.
    const std::string_view classes =
        "Import std.core\n"
        "Import std.collections\n"
        "Class Enemy\n"
        "    Field Name: String\n"
        "End Class\n"
        "Class Box<T>\n"
        "    Field Item: T?\n"
        "End Class\n"
        "Class Coin\n"
        "    Field Cents: Int\n"
        "    Operator ==(a: Coin, b: Coin): Bool => a.Cents == b.Cents\n"
        "End Class\n"
        "Class Gold Extends Coin\n"
        "End Class\n"
        "Class Hashed Extends Coin\n"
        "    Method Hash(): UInt64 => Cents As UInt64\n"
        "End Class\n"
        "Local e = Enemy()\n";
    const auto with_classes = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", classes, text));
    };
    CHECK(with_classes("Local enemies = [e, Enemy()]\n"
                       "Local removed = enemies.Remove(e)\n"
                       "Local at = enemies.IndexOf(e)\n"
                       "Local seen = Set<Enemy>()\n"
                       "Local counts: Map<Enemy, Int> = {e: 1}\n"
                       "Local boxes = Set<Box<Int>>()\n"
                       "Local maybe = Set<Enemy?>()\n"
                       "Local hashed = Set<Hashed>()\n"
                       "Local coins = [Coin()].Contains(Coin())\n"
                       "Local golds = [Gold()].Contains(Gold())")
              .empty());

    // An Operator == of its own, or one it inherits, without a Hash, leaves it Equatable only.
    CHECK(with_classes("Local coins = Set<Coin>()\nLocal golds: Map<Gold, Int> = {}") ==
          errors_t{ "Set<Coin>: Coin doesn't implement Hashable, which Set<T> needs for T; Coin "
                    "has an Operator == of its own, so it's Hashable only with a Hash of its own "
                    "that agrees with it",
                    "Map<Gold, Int>: Gold doesn't implement Hashable, which Map<K, V> needs for "
                    "K; Gold has an Operator == of its own, so it's Hashable only with a Hash of "
                    "its own that agrees with it" });
    CHECK(
        errors_of("Import std.core\n"
                  "Import std.collections\n"
                  "Interface Keyed\n"
                  "    Method Key(): Int\n"
                  "    Operator ==(a: Self, b: Self): Bool => a.Key() == b.Key()\n"
                  "End Interface\n"
                  "Class Door Implements Keyed\n"
                  "    Method Key(): Int => 1\n"
                  "End Class\n"
                  "Local doors = [Door()].Contains(Door())\n"
                  "Local keyed = Set<Door>()") ==
        errors_t{ "Set<Door>: Door doesn't implement Hashable, which Set<T> needs for T; Door has "
                  "an Operator == of its own, so it's Hashable only with a Hash of its own that "
                  "agrees with it" });
    // A Hash of another kind, written or an Interface's default, is only a Method.
    CHECK(errors_of("Import std.core\n"
                    "Interface Digest\n"
                    "    Method Hash(): String => \"digest\"\n"
                    "End Interface\n"
                    "Class Doc\n"
                    "    Field text: String = \"abc\"\n"
                    "    Operator ==(a: Doc, b: Doc): Bool => a.text == b.text\n"
                    "    Method Hash(): String => \"sha:\" + text\n"
                    "End Class\n"
                    "Class Page Implements Digest\n"
                    "    Operator ==(a: Page, b: Page): Bool => True\n"
                    "End Class\n"
                    "Local same = Doc() == Doc() && Page() == Page()\n"
                    "Local digests = Doc().Hash() + Page().Hash()")
              .empty());

    // An Extend of one instance that gives it an == decides that one: with a Hash, it hashes
    // with that, and without one it's Equatable only, as are the Classes that extend it.
    constexpr std::string_view boxed = "Import std.core\n"
                                       "Import std.collections\n"
                                       "Class Box<T>\n"
                                       "    Field v: T\n"
                                       "    New(v: T)\n"
                                       "        Self.v = v\n"
                                       "    End New\n"
                                       "End Class\n"
                                       "Extend Box<String>\n"
                                       "    Operator ==(a: Box<String>, b: Box<String>): Bool => "
                                       "a.v == b.v\n"
                                       "    Method Hash(): UInt64 => v.Hash()\n"
                                       "End Extend\n"
                                       "Extend Box<Int>\n"
                                       "    Operator ==(a: Box<Int>, b: Box<Int>): Bool => "
                                       "a.v == b.v\n"
                                       "End Extend\n"
                                       "Class Named Extends Box<String>\n"
                                       "End Class\n"
                                       "Class Counted Extends Box<Int>\n"
                                       "End Class\n";
    CHECK(errors_of(std::format("{}Local a = Set<Box<Bool>>()\n"
                                "Local b = Set<Box<String>>()\n"
                                "Local c = Set<Named>()",
                                boxed))
              .empty());
    CHECK(errors_of(std::format("{}Local d = Set<Box<Int>>()\nLocal e = Set<Counted>()", boxed)) ==
          errors_t{ "Set<Box<Int>>: Box<Int32> doesn't implement Hashable, which Set<T> needs for "
                    "T; Box<Int32> has an Operator == of its own, so it's Hashable only with a "
                    "Hash of its own that agrees with it",
                    "Set<Counted>: Counted doesn't implement Hashable, which Set<T> needs for T; "
                    "Counted has an Operator == of its own, so it's Hashable only with a Hash of "
                    "its own that agrees with it" });

    // Listing Hashable says it has one, and Comparable and the Structs are as they were.
    CHECK(errors_of("Import std.core\n"
                    "Class Coin Implements Hashable\n"
                    "    Field Cents: Int\n"
                    "    Operator ==(a: Coin, b: Coin): Bool => a.Cents == b.Cents\n"
                    "End Class") ==
          errors_t{ "Hashable: Coin has an Operator == of its own, so it needs a Hash that agrees "
                    "with it" });
    CHECK(errors_of("Import std.core\n"
                    "Class Enemy\n"
                    "End Class\n"
                    "Function Least<T: Comparable>(a: T, b: T): T => a < b ? a : b\n"
                    "Local e = Least(Enemy(), Enemy())") ==
          errors_t{ "Least: Enemy doesn't implement Comparable, which 'Least' needs for T" });
    CHECK(errors_of("Import std.core\n"
                    "Import std.collections\n"
                    "Struct Point\n"
                    "    Field X: Int\n"
                    "End Struct\n"
                    "Local seen = Set<Point>()") ==
          errors_t{ "Set<Point>: Point doesn't implement Hashable, which Set<T> needs for T" });
}

TEST_CASE("the built-in types have the built-in Interfaces' Methods", "[sema]") {
    CHECK(probe("Local s = 5.ToString()") == "String");
    CHECK(probe("Local h = \"a\".Hash()") == "UInt64");
    CHECK(probe("Local h = [1, 2].Hash()") == "UInt64");
    CHECK(probe("Local h = (1, \"a\").ToString()") == "String");
    CHECK(errors_of("Struct Rock\nEnd Struct\nLocal h = Rock().Hash()") ==
          errors_t{ "Hash: Rock has no member named 'Hash'" });
    CHECK(probe("Import std.core\nClass Rock\nEnd Class\nLocal h = Rock().Hash()") == "UInt64");
    CHECK(errors_of("Local f = 1.5.Hash") ==
          errors_t{ "Hash: 'Hash' is a method; call it with ()" });

    // A literal before a member takes the first type, from its default up, that holds it and
    // has the member: a decimal Float, then Double; an integer Int, Int64, then UInt64 when only
    // it holds the value.
    CHECK(probe("Local s = 1.5.Hash()") == "UInt64");
    CHECK(probe("Import std.time\nLocal d = 0.1.Seconds") == "Duration");
    CHECK(probe("Local s = 5_000_000_000.ToString()") == "String");
    CHECK(probe("Local s = 18_000_000_000_000_000_000.ToString()") == "String");
    const std::string sizes = "Extend Float\n"
                              "    Property Narrow: Float\n        Get => Self\n    End Property\n"
                              "End Extend\n"
                              "Extend Double\n"
                              "    Property Wide: Double\n        Get => Self\n    End Property\n"
                              "    Method Doubled(): Double => Self * 2\n"
                              "End Extend\n"
                              "Extend Int\n"
                              "    Property Small: Int\n        Get => Self\n    End Property\n"
                              "End Extend\n"
                              "Extend Long\n"
                              "    Property Large: Long\n        Get => Self\n    End Property\n"
                              "    Property Small: Long\n        Get => Self\n    End Property\n"
                              "End Extend\n";
    CHECK(probe(sizes + "Local x = 1.5.Narrow") == "Float32");
    CHECK(probe(sizes + "Local x = 1.5.Wide") == "Float64");
    CHECK(probe(sizes + "Local x = 0.1.Doubled()") == "Float64");
    CHECK(probe(sizes + "Local x = 2.Small") == "Int32");
    CHECK(probe(sizes + "Local x = 2.Large") == "Int64");
    CHECK(probe(sizes + "Local x = (-2).Large") == "Int64");
    CHECK(probe(sizes + "Local x = 5_000_000_000.Small") == "Int64");
    CHECK(errors_of(sizes + "Local x = 2.Missing") ==
          errors_t{ "Missing: neither Int32 nor Int64 has a member named 'Missing'" });
    CHECK(errors_of(sizes + "Local x = 2.5.Missing()") ==
          errors_t{ "Missing: neither Float32 nor Float64 has a member named 'Missing'" });
    CHECK(errors_of("Local x = 5_000_000_000.Missing") ==
          errors_t{ "Missing: Int64 has no member named 'Missing'" });
    CHECK(errors_of("Local x = 1.0e300.Missing") ==
          errors_t{ "Missing: Float64 has no member named 'Missing'" });

    // A unit std.time's Extends give says which Import it needs, as a Method does, on a literal
    // or a variable.
    CHECK(errors_of("Import std.core\nLocal d = 2.Seconds\nLocal m = 1.5.Minutes\n"
                    "Local n: Int64 = 5\nLocal l = n.Milliseconds") ==
          errors_t{ "Seconds: 'Seconds' is in std.time, which this file doesn't import",
                    "Minutes: 'Minutes' is in std.time, which this file doesn't import",
                    "Milliseconds: 'Milliseconds' is in std.time, which this file doesn't "
                    "import" });
}

TEST_CASE("Printable puts a value in a string", "[sema]") {
    const std::string_view point = "Import std.core\n"
                                   "Struct Point Implements Printable\n"
                                   "    Field X: Int\n"
                                   "    Method ToString(): String => \"({X})\"\n"
                                   "End Struct\n";
    CHECK(probe(std::format("{}Local s = \"{{Point(1)}} {{[Point(2)]}}\"", point)) == "String");
    CHECK(probe("Import std.core\n"
                "Function F<T: Printable>(x: T): String => \"{x}\"\nLocal s = F([1, 2])") ==
          "String");
    CHECK(probe(std::format("Import std.core\n"
                            "{}Class C Implements Printable\n"
                            "    Method ToString(): String => \"c\"\n"
                            "End Class\n"
                            "Local p: Printable = C()\nLocal s = \"{{p}}\"",
                            point)) == "String");
    CHECK(errors_of("Local s = \"{[Function() => 1]}\"") ==
          errors_t{ "[Function() => 1]: (Func(): Int32)[] can't be shown in a string, since its "
                    "elements can't be" });
}

TEST_CASE("a program's own Interface named like a built-in one hides std's for its code",
          "[sema]") {
    // Each can be declared again, and the program's constraints mean its own.
    CHECK(errors_of("Import std.core\n"
                    "Interface Equatable\nEnd Interface\n"
                    "Interface Comparable\nEnd Interface\n"
                    "Interface Hashable\nEnd Interface\n"
                    "Interface Printable\nEnd Interface\n"
                    "Interface Numeric\nEnd Interface\n"
                    "Interface Sendable\nEnd Interface\n"
                    "Interface Iterator<T>\nEnd Interface")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Interface Printable\n"
                    "    Method Show(): String\n"
                    "End Interface\n"
                    "Function F<T: Printable>(x: T): String => x.Show()\n"
                    "Local s = F(1)") ==
          errors_t{ "F: Int32 doesn't implement Printable, which 'F' needs for T" });

    // The language's ==, <, strings, loops, Maps, Sets, and threads still go through std's.
    CHECK(errors_of("Import std.core\n"
                    "Import std.collections\n"
                    "Interface Equatable\nEnd Interface\n"
                    "Interface Comparable\nEnd Interface\n"
                    "Interface Hashable\nEnd Interface\n"
                    "Interface Printable\nEnd Interface\n"
                    "Interface Sendable\nEnd Interface\n"
                    "Interface Iterator<T>\nEnd Interface\n"
                    "Local m = {\"a\": 1}\n"
                    "Local names = [\"b\", \"a\"]\n"
                    "names.Sort()\n"
                    "For k, v In m\n"
                    "    Print(\"{k}={v} {m} {names} {k < \"b\"} {k == \"a\"}\")\n"
                    "End For\n"
                    "Local s = Set<Int>()\n"
                    "s.Add(1)\n"
                    "Local t = Spawn(Function() => 2)\n"
                    "Local n = t.Wait()")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Interface Sendable\nEnd Interface\n"
                    "Class Rock\n"
                    "    Field Weight: Int = 1\n"
                    "End Class\n"
                    "Function F(rock: Rock): Int\n"
                    "    Return Spawn(Function() => rock.Weight).Wait()\n"
                    "End Function") ==
          errors_t{ "Function() => rock.Weight: 'rock' is Rock, which another thread can't "
                    "have: a Class's objects stay on the thread that made them, unless it's a "
                    "Shared Class" });

    // Messages name std's Printable where the code means its own.
    CHECK(errors_of("Import std.core\n"
                    "Interface Printable\n"
                    "End Interface\n"
                    "Struct P Implements Printable\n"
                    "    Field X: Int\n"
                    "End Struct\n"
                    "Local s = \"{P(1)}\"") ==
          errors_t{ "P(1): P can't be shown in a string; implement std.core.interfaces.Printable, "
                    "with a ToString" });
    CHECK(errors_of("Interface Printable\nEnd Interface\nFunction F<T>(x: T): String => \"{x}\"") ==
          errors_t{ "x: T can't be shown in a string without a constraint, like "
                    "<T: interfaces.Printable>, after Import std.core.interfaces" });
    CHECK(errors_of("Import std.core.interfaces As I\n"
                    "Interface Printable\nEnd Interface\nFunction F<T>(x: T): String => \"{x}\"") ==
          errors_t{ "x: T can't be shown in a string without a constraint, like "
                    "<T: I.Printable>" });
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

    // An array's Methods can always change it, so a ByRef default an Interface gives it isn't.
    CHECK(errors_of("Import std.core\n"
                    "Interface Grows\n"
                    "    ByRef Method Grow()\n"
                    "        Print(\"grow\")\n"
                    "    End Method\n"
                    "End Interface\n"
                    "Extend Int[] Implements Grows\n"
                    "End Extend\n"
                    "Function Make(): Int[] => [1, 2]\n"
                    "Make().Grow()")
              .empty());

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
    const std::string_view sums = "Import std.core\n"
                                  "Extend T[]\n"
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

    const std::string_view box = "Import std.core\n"
                                 "Class Box<T>\n"
                                 "    Field Item: T?\n"
                                 "End Class\n"
                                 "Extend Box<T> Implements Printable\n"
                                 "    Where T: Printable\n"
                                 "    Method ToString(): String => \"box\"\n"
                                 "End Extend\n";
    CHECK(probe(std::format("{}Local s = \"{{Box<Int>()}}\"", box)) == "String");
    // A value whose Printable a Where line keeps from it can't be shown, and the line says why.
    CHECK(errors_of(std::format("{}Class Rock\nEnd Class\nLocal s = \"{{Box<Rock>()}}\"", box)) ==
          errors_t{ "Box<Rock>(): Box<Rock> can't be shown in a string, since Rock isn't "
                    "Printable, which its ToString needs for T" });
    CHECK(errors_of("Import std.core\nImport std.collections\nPrint(Map<String, Int?>())\n"
                    "Local s = \"{Set<Int?>()}\"") ==
          errors_t{ "Map<String, Int?>(): Map<String, Int32?> can't be shown by 'Print', since "
                    "Int32? isn't Printable, which its ToString needs for V",
                    "Set<Int?>(): Set<Int32?> can't be shown in a string, since Int32? isn't "
                    "Printable, which its ToString needs for T" });
    // A Class that implements an Interface through an Extend's Where is its value.
    CHECK(errors_of(std::format("{}Local p: Printable = Box<Int>()", box)).empty());
    CHECK(
        errors_of(std::format("{}Class Rock\nEnd Class\nLocal p: Printable = Box<Rock>()", box)) ==
        errors_t{ "Box<Rock>(): expected Printable, found Box<Rock>" });

    // So is a Class extending an instance that does, as with what a Class it extends lists.
    const std::string_view kept = "Class Kept Extends Box<Int>\n"
                                  "End Class\n"
                                  "Class Held<T> Extends Box<T>\n"
                                  "End Class\n"
                                  "Function Show<P: Printable>(p: P): String => p.ToString()\n";
    CHECK(errors_of(std::format("{}{}"
                                "Local p: Printable = Kept()\n"
                                "Local q: Printable = Held<Int>()\n"
                                "Local s = Show(Kept()) + Show(Held<Int>())",
                                box, kept))
              .empty());
    CHECK(errors_of(std::format("{}{}Class Rock\nEnd Class\nLocal p: Printable = Held<Rock>()", box,
                                kept)) ==
          errors_t{ "Held<Rock>(): expected Printable, found Held<Rock>" });
}

TEST_CASE("an Extend's Where makes the defaults it gives conditional too", "[sema]") {
    // "<owner> <Interface>" of what implements Sized's Twice for `type`.
    const auto implementer = [](const std::string& program, const std::string_view type) {
        const fb::compilation_t compilation = fb::compile("test.fb", program);
        const fb::analysis_t& analysis = compilation.analysis;
        INFO(program);
        CHECK(compilation.diagnostics.diagnostics().empty());
        for (const fb::conformance_t& conformance : analysis.conformances) {
            if (analysis.types.name(conformance.type) == type &&
                analysis.types.name(conformance.interface) == "Sized") {
                const fb::symbol_t& twice = analysis.symbol(conformance.members.at(1));
                return std::format("{} {}", analysis.types.name(twice.owner),
                                   analysis.types.name(twice.default_interface));
            }
        }

        return std::string();
    };
    const std::string doubled = "Interface Doubled\n"
                                "    Method Size(): Int\n"
                                "    Method Twice(): Int => Size() + 100\n"
                                "End Interface\n"
                                "Struct Rock\n"
                                "End Struct\n";
    const std::string box = "Import std.core\n" + doubled +
                            "Class Box<T>\n"
                            "    Method Size(): Int => 1\n"
                            "End Class\n"
                            "Extend Box<T> Implements Doubled\n"
                            "    Where T: Hashable\n"
                            "End Extend\n";

    // Only the instances that meet it have its default, as they have what it adds.
    CHECK(probe(box + "Local n = Box<Int>().Twice()") == "Int32");
    // Beside the default every instance has from an Extend without one, a call takes that.
    CHECK(probe("Import std.core\n"
                "Interface Sized\n"
                "    Method Size(): Int => 1\n"
                "End Interface\n"
                "Interface Doubled Extends Sized\n"
                "    Method Size(): Int => 10\n"
                "End Interface\n"
                "Class Tray<T>\n"
                "End Class\n"
                "Extend Tray<T> Implements Sized\n"
                "End Extend\n"
                "Extend Tray<T> Implements Doubled\n"
                "    Where T: Numeric\n"
                "End Extend\n"
                "Local n = Tray<Int>().Size()") == "Int32");
    CHECK(errors_of(box + "Local n = Box<Rock>().Twice()") ==
          errors_t{
              "Box<Rock>().Twice: Rock doesn't implement Hashable, which 'Twice' needs for T" });
    CHECK(errors_of("Import std.core\n" + doubled +
                    "Extend T[] Implements Doubled\n"
                    "    Where T: Hashable\n"
                    "    Method Size(): Int => Count\n"
                    "End Extend\n"
                    "Local n = [1].Twice()\n"
                    "Local m = [Rock()].Twice()") ==
          errors_t{ "[Rock()].Twice: Rock doesn't implement Hashable, which 'Twice' needs for T" });

    // So where every Box is Sized, Sized's default is its Twice, however Doubled extends it,
    // and it hides Doubled's in a call, which goes through Doubled.
    const std::string sized = "Interface Sized\n"
                              "    Method Size(): Int\n"
                              "    Method Twice(): Int => Size() * 2\n"
                              "End Interface\n";
    const std::string refined = "Import std.core\n" + sized +
                                "Interface Doubled Extends Sized\n"
                                "    Method Twice(): Int => Size() + 100\n"
                                "End Interface\n"
                                "Class Box<T> Implements Sized\n"
                                "    Method Size(): Int => 1\n"
                                "End Class\n"
                                "Extend Box<T> Implements Doubled\n"
                                "    Where T: Hashable\n"
                                "End Extend\n"
                                "Class Rock\n"
                                "End Class\n";
    CHECK(implementer(refined, "Box<T>") == "Box<T> Sized");
    CHECK(errors_of(refined + "Function S(s: Sized): Int => s.Twice()\n"
                              "Function D(d: Doubled): Int => d.Twice()\n"
                              "Local n = S(Box<Rock>()) + Box<Rock>().Twice() + D(Box<Int>()) +\n"
                              "    Box<Int>().Twice()")
              .empty());

    // As it is for a Class extending Box, unless its own Where says each one is Doubled.
    const std::string base = "Import std.core\n" + sized + doubled +
                             "Class Base<T>\n"
                             "    Method Size(): Int => 1\n"
                             "End Class\n"
                             "Extend Base<T> Implements Doubled\n"
                             "    Where T: Hashable\n"
                             "End Extend\n";
    CHECK(implementer(base + "Class Sub<T> Extends Base<T> Implements Sized\nEnd Class",
                      "Sub<T>") == "Sub<T> Sized");
    CHECK(errors_of(base + "Class Sub<T> Extends Base<T> Implements Sized\n"
                           "End Class\n"
                           "Local n = Sub<Rock>().Twice() + Sub<Int>().Twice()")
              .empty());
    CHECK(implementer(base + "Class Sub<T> Extends Base<T> Implements Sized\n"
                             "    Where T: Hashable\n"
                             "End Class",
                      "Sub<T>") == "Base<T> Doubled");

    // A Method or Property an Extend's Where makes conditional can't implement an Interface
    // listed for every instance, which would run it for the others.
    // Keyed with `key` for its Key, and Box with `listing` after its name.
    const auto keyed = [](const std::string_view key, const std::string_view listing) {
        return std::format("Import std.core\n"
                           "Interface Keyed\n"
                           "    {}\n"
                           "End Interface\n"
                           "Class Box<T>{}\n"
                           "    Field Item: T?\n"
                           "End Class\n"
                           "Extend Box<T>\n"
                           "    Where T: Hashable\n"
                           "    Method Key(): UInt64 => Item.Hash()\n"
                           "End Extend\n",
                           key, listing);
    };
    const std::string narrower = "Keyed: Box<T>'s 'Key' needs T to implement Hashable, so it "
                                 "can't implement Keyed's here";
    CHECK(errors_of(keyed("Method Key(): UInt64", " Implements Keyed")) == errors_t{ narrower });
    CHECK(errors_of(keyed("Method Key(): UInt64 => 0", " Implements Keyed")) ==
          errors_t{ narrower });
    CHECK(errors_of("Import std.core\n"
                    "Interface Named\n"
                    "    ReadOnly Property Name: String\n"
                    "End Interface\n"
                    "Class Box<T> Implements Named\n"
                    "End Class\n"
                    "Extend Box<T>\n"
                    "    Where T: Printable\n"
                    "    Property Name: String\n"
                    "        Get => \"box\"\n"
                    "    End Property\n"
                    "End Extend") ==
          errors_t{ "Named: Box<T>'s 'Name' needs T to implement Printable, so it can't implement "
                    "Named's here" });
    // But it implements one listed where its Where holds.
    CHECK(errors_of(keyed("Method Key(): UInt64", "") + "Extend Box<T> Implements Keyed\n"
                                                        "    Where T: Hashable\n"
                                                        "End Extend")
              .empty());

    // What overrides it would be in the Method table, which it isn't.
    CHECK(
        errors_of(base + "Class Sub<T> Extends Base<T>\n"
                         "    Override Method Twice(): Int => 7\n"
                         "End Class") ==
        errors_t{ "Twice: 'Twice' was added to Base<T> by an Extend, so it can't be overridden" });

    // Beside a default every instance has, an Override overrides that one, which a call on the
    // Class reaches, whatever instance the Class extends, and whether or not the conditional
    // one's Interface extends the other's.
    // "<owner> <Interface>" of what the Twice of the Class named `name` overrides.
    const auto overridden = [](const std::string& program, const std::string_view name) {
        const fb::compilation_t compilation = fb::compile("test.fb", program);
        const fb::analysis_t& analysis = compilation.analysis;
        INFO(program);
        CHECK(compilation.diagnostics.diagnostics().empty());
        for (const fb::symbol_t& symbol : analysis.symbols) {
            if (symbol.name == "Twice" && symbol.owner != fb::type_id_t::INVALID &&
                analysis.types.name(symbol.owner) == name && symbol.overrides) {
                const fb::symbol_t& original = analysis.symbol(*symbol.overrides);
                return std::format("{} {}", analysis.types.name(original.owner),
                                   analysis.types.name(original.default_interface));
            }
        }

        return std::string();
    };
    // `program` and a Class, declared as `header`, that overrides Twice.
    const auto with_override = [](const std::string_view program, const std::string_view header) {
        return std::format("{}{}\n"
                           "    Override Method Twice(): Int => 7\n"
                           "End Class\n",
                           program, header);
    };
    CHECK(overridden(with_override(refined, "Class Sub<T> Extends Box<T>"), "Sub<T>") ==
          "Box<T> Sized");
    CHECK(overridden(with_override(refined, "Class Sub Extends Box<Int>"), "Sub") ==
          "Box<T> Sized");
    CHECK(overridden(with_override(refined, "Class Sub Extends Box<Rock>"), "Sub") ==
          "Box<T> Sized");
    const std::string both = "Import std.core\n" + sized + doubled +
                             "Class Base<T> Implements Sized\n"
                             "    Method Size(): Int => 1\n"
                             "End Class\n"
                             "Extend Base<T> Implements Doubled\n"
                             "    Where T: Hashable\n"
                             "End Extend\n";
    CHECK(overridden(with_override(both, "Class Sub<T> Extends Base<T>"), "Sub<T>") ==
          "Base<T> Sized");

    // A loop over an instance's Properties goes over the ones it has.
    CHECK(errors_of("Import std.core\n" + doubled +
                    "Class Crate<T>\n"
                    "End Class\n"
                    "Extend Crate<T>\n"
                    "    Where T: Hashable\n"
                    "    Property Thing: Rock\n"
                    "        Get => Rock()\n"
                    "    End Property\n"
                    "End Extend\n"
                    "Function Show<C>(c: C): String\n"
                    "    Local text = \"\"\n"
                    "    For Property p: P In C\n"
                    "        Where P: Printable\n"
                    "        text += \"{p.Name} {p.Get(c)}\"\n"
                    "    End For\n"
                    "    Return text\n"
                    "End Function\n"
                    "Local a = Show(Crate<Rock>())\n"
                    "Local b = Show(Crate<Int>())") ==
          errors_t{ "Show: 'Show' needs each Property of Crate<Int32> to implement Printable, but "
                    "its Property 'Thing' is Rock, which doesn't" });
}

TEST_CASE("an instance without a conditional Operator == compares the built-in way", "[sema]") {
    // Whether the == on the program's last line calls an Operator.
    const auto calls_operator = [](const std::string& program) {
        const fb::compilation_t compilation = fb::compile("test.fb", program);
        const fb::ast_t& ast = compilation.ast;
        INFO(program);
        CHECK(compilation.diagnostics.diagnostics().empty());
        const auto& local = std::get<fb::variable_stmt_t>(ast.get(ast.items().back()).node);
        REQUIRE(local.value);
        return compilation.analysis.expr(local.value.value_or(fb::expr_id_t{})).symbol.has_value();
    };
    const std::string box = "Struct Plain\n"
                            "    Field v: Int\n"
                            "End Struct\n"
                            "Struct Box<T>\n"
                            "    Field item: T\n"
                            "    Field n: Int\n"
                            "End Struct\n";

    // One an Extend's Where adds, and a default it gives, are the instances' that meet it.
    const std::string own = "Import std.core\n" + box +
                            "Extend Box<T>\n"
                            "    Where T: Hashable\n"
                            "    Operator ==(a: Box<T>, b: Box<T>): Bool => a.n == b.n\n"
                            "End Extend\n";
    CHECK(calls_operator(own + "Local same = Box(1, 5) == Box(2, 5)"));
    CHECK_FALSE(calls_operator(own + "Local same = Box(Plain(1), 5) == Box(Plain(2), 5)"));
    CHECK_FALSE(calls_operator(own + "Local same = Box(Plain(1), 5) != Box(Plain(2), 5)"));
    const std::string keyed = "Import std.core\n" + box +
                              "Interface Keyed\n"
                              "    Method Key(): Int\n"
                              "    Operator ==(a: Self, b: Self): Bool => a.Key() == b.Key()\n"
                              "End Interface\n"
                              "Extend Box<T> Implements Keyed\n"
                              "    Where T: Hashable\n"
                              "    Method Key(): Int => n\n"
                              "End Extend\n";
    CHECK(calls_operator(keyed + "Local same = Box(1, 5) == Box(2, 5)"));
    CHECK_FALSE(calls_operator(keyed + "Local same = Box(Plain(1), 5) == Box(Plain(2), 5)"));

    // The built-in way compares the fields, which need == of their own.
    CHECK(errors_of(own + "Interface Shape\n"
                          "End Interface\n"
                          "Function Same(s: Shape): Bool => Box(s, 1) == Box(s, 1)") ==
          errors_t{ "Box(s, 1) == Box(s, 1): '==' can't be used on Box<Shape>" });
    CHECK(errors_of(own + "Interface Shape\n"
                          "End Interface\n"
                          "Function Has(s: Shape): Bool => [Box(s, 1)].Contains(Box(s, 1))") ==
          errors_t{ "[Box(s, 1)].Contains: Contains compares with ==, which Box<Shape> doesn't "
                    "have" });

    // Generic code has it where its constraints meet the Where.
    CHECK(errors_of(own + "Function Same<U>(a: Box<U>, b: Box<U>): Bool => a == b") ==
          errors_t{ "a == b: '==' can't be used on Box<U>" });
    CHECK(errors_of(own + "Function Same<U: Hashable>(a: Box<U>, b: Box<U>): Bool => a == b")
              .empty());
}

TEST_CASE("a Class declares what the instance it extends doesn't have", "[sema]") {
    // Base<Rock> has neither what an Extend's Where adds nor the default it gives, so a Class
    // extending it declares Methods and Properties of their names, which override nothing.
    const std::string base = "Import std.core\n"
                             "Interface Doubled\n"
                             "    Method Size(): Int\n"
                             "    Method Twice(): Int => Size() + 100\n"
                             "End Interface\n"
                             "Struct Rock\n"
                             "End Struct\n"
                             "Class Base<T>\n"
                             "    Method Size(): Int => 1\n"
                             "End Class\n"
                             "Extend Base<T> Implements Doubled\n"
                             "    Where T: Hashable\n"
                             "    Method Seal(): Int => 3\n"
                             "    Property Mark: Int\n"
                             "        Get => 3\n"
                             "    End Property\n"
                             "End Extend\n";
    const std::string members = "    Method Twice(): Int => 7\n"
                                "    Method Seal(): Int => 7\n"
                                "    Property Mark: Int\n"
                                "        Get => 7\n"
                                "    End Property\n"
                                "End Class\n";
    CHECK(errors_of(std::format("{}Class RockSub Extends Base<Rock>\n"
                                "{}Local n = RockSub().Twice() + RockSub().Seal() + RockSub().Mark",
                                base, members))
              .empty());

    // One that has them, or with the Class's own type parameters in it, has what can't be
    // overridden.
    const errors_t added{
        "Twice: 'Twice' was added to Base<T> by an Extend, so it can't be overridden",
        "Seal: 'Seal' was added to Base<T> by an Extend, so it can't be overridden",
        "Mark: 'Mark' was added to Base<T> by an Extend, so it can't be overridden",
    };
    CHECK(errors_of(std::format("{}Class IntSub Extends Base<Int>\n{}", base, members)) == added);
    CHECK(errors_of(std::format("{}Class Sub<T> Extends Base<T>\n{}", base, members)) == added);

    // Like a ToString beside Map's, which is for keys and values that can be shown.
    CHECK(errors_of("Import std.core\n"
                    "Import std.collections\n"
                    "Class Item\n"
                    "End Class\n"
                    "Class Inventory Extends Map<String, Item> Implements Printable\n"
                    "    Method ToString(): String => \"inventory of {Count}\"\n"
                    "End Class\n"
                    "Local text = \"{Inventory()}\"")
              .empty());
    CHECK(errors_of("Import std.collections\n"
                    "Class Names Extends Map<String, String>\n"
                    "    Method ToString(): String => \"names\"\n"
                    "End Class") ==
          errors_t{ "ToString: 'ToString' was added to Map<K, V> by an Extend, so it can't be "
                    "overridden" });
}

TEST_CASE("an Interface that's only a constraint isn't had from the Class extended", "[sema]") {
    // Low has Comparable from neither Ranked<Int>'s Extend nor Ordered's listing, so a Pair of
    // one isn't Printable, statically or as fb::implements() tells lowering.
    const std::string program = "Import std.core\n"
                                "Class Ranked<T>\n"
                                "    Operator <(a: Ranked<T>, b: Ranked<T>): Bool => False\n"
                                "End Class\n"
                                "Extend Ranked<T> Implements Comparable\n"
                                "    Where T: Hashable\n"
                                "End Extend\n"
                                "Class Ordered Implements Comparable\n"
                                "    Operator <(a: Ordered, b: Ordered): Bool => False\n"
                                "End Class\n"
                                "Class Low Extends Ranked<Int>\n"
                                "End Class\n"
                                "Class Lower Extends Ordered\n"
                                "End Class\n"
                                "Class Pair<T>\n"
                                "End Class\n"
                                "Extend Pair<T> Implements Printable\n"
                                "    Where T: Comparable\n"
                                "    Method ToString(): String => \"pair\"\n"
                                "End Extend\n"
                                "Local plain = Pair<Int>()\n"
                                "Local ranked = Pair<Ranked<Int>>()\n"
                                "Local low = Pair<Low>()\n"
                                "Local lower = Pair<Lower>()\n";
    CHECK(errors_of(program + "Local p: Printable = Pair<Ranked<Int>>()").empty());
    CHECK(errors_of(program + "Local p: Printable = Pair<Low>()") ==
          errors_t{ "Pair<Low>(): expected Printable, found Pair<Low>" });
    CHECK(errors_of(program + "Local p: Printable = Pair<Lower>()") ==
          errors_t{ "Pair<Lower>(): expected Printable, found Pair<Lower>" });

    fb::compilation_t compilation = fb::compile("test.fb", program);
    REQUIRE(compilation.diagnostics.diagnostics().empty());
    fb::type_table_t& types = compilation.analysis.types;
    const auto type_named = [&](const std::string_view name) {
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (types.name(static_cast<fb::type_id_t>(i)) == name) {
                return static_cast<fb::type_id_t>(i);
            }
        }

        return fb::type_id_t::INVALID;
    };
    const fb::type_id_t printable = compilation.analysis.interfaces.printable;
    const fb::type_id_t comparable = compilation.analysis.interfaces.comparable;
    CHECK(
        fb::implements(compilation.analysis, types, type_named("Pair<Ranked<Int32>>"), printable));
    CHECK(fb::implements(compilation.analysis, types, type_named("Ranked<Int32>"), comparable));
    CHECK_FALSE(fb::implements(compilation.analysis, types, type_named("Low"), comparable));
    CHECK_FALSE(fb::implements(compilation.analysis, types, type_named("Lower"), comparable));
    CHECK_FALSE(fb::implements(compilation.analysis, types, type_named("Pair<Low>"), printable));
    CHECK_FALSE(fb::implements(compilation.analysis, types, type_named("Pair<Lower>"), printable));
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
    CHECK(errors_of("Import std.core\n"
                    "Extend Int Implements Printable\nEnd Extend") ==
          errors_t{ "Printable: Int32 comes with the built-in Interfaces that fit it, Printable "
                    "among them; an Extend can give it others" });
    CHECK(errors_of("Import std.core\n"
                    "Extend CString Implements Printable\nEnd Extend") ==
          errors_t{ "Printable: CString doesn't fit Printable, a built-in Interface, which only "
                    "the built-in types it fits implement; an Extend can give it others" });
    // One that extends those it comes with keeps them as they are.
    CHECK(errors_of("Import std.core\n"
                    "Interface Ranked Extends Comparable\n"
                    "    Method Rank(): Int\n"
                    "End Interface\n"
                    "Extend Int Implements Ranked\n"
                    "    Method Rank(): Int => Self\n"
                    "End Extend\n"
                    "Interface Real Extends Numeric\n"
                    "    Method Half(): Self\n"
                    "End Interface\n"
                    "Extend Float Implements Real\n"
                    "    Method Half(): Float => Self / 2\n"
                    "End Extend\n"
                    "Enum Mode\n"
                    "    A\n"
                    "End Enum\n"
                    "Interface Tagged Extends Printable, Hashable\n"
                    "End Interface\n"
                    "Extend Mode Implements Tagged\n"
                    "End Extend")
              .empty());

    // A second listing labels the first, which can be another module's.
    const fb::compilation_t twice =
        fb::compile("test.fb",
                    "Interface D\n    Method Describe(): String\nEnd Interface\n"
                    "Extend Int Implements D\n    Method Describe(): String => \"a\"\nEnd Extend\n"
                    "Extend Int Implements D\nEnd Extend",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    const std::vector<fb::diagnostic_t>& diagnostics = twice.diagnostics.diagnostics();
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message == "Int32 implements D already");
    REQUIRE(diagnostics[0].labels.size() == 1);
    CHECK(diagnostics[0].labels[0].span.begin < diagnostics[0].span.begin);
    CHECK(diagnostics[0].labels[0].message == "listed here");
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
    CHECK(probe("Import std.core\n"
                "Class Words Implements Iterator<String>\n"
                "    Method Next(): String? => Null\n"
                "End Class\n"
                "Local it: Iterator<String> = Words()\n"
                "Local next = it.Next()") == "String?");

    // A generic type can pass its own type parameters on, and an Interface can extend an
    // instance.
    CHECK(probe("Import std.core\n"
                "Struct Again<T> Implements Iterator<T>\n"
                "    Field Value: T\n"
                "    ByRef Method Next(): T? => Value\n"
                "End Struct\n"
                "Local a = Again(1.5)\n"
                "Local x = a.Next()") == "Float32?");
    CHECK(errors_of("Import std.core\n"
                    "Interface Numbers Extends Iterator<Int>\nEnd Interface\n"
                    "Function F(n: Numbers): Int? => n.Next()\n"
                    "Function G(n: Numbers): Iterator<Int> => n")
              .empty());
}

TEST_CASE("a constraint's type arguments are inferred from what the type argument implements",
          "[sema]") {
    // E is the Iterator<E> that S implements.
    CHECK(probe("Import std.core\n"
                "Struct Count Implements Iterator<Int>\n"
                "    ByRef Method Next(): Int? => Null\n"
                "End Struct\n"
                "Function First<S, E>(items: S): E?\n"
                "    Where S: Iterator<E>\n"
                "    Local copy = items\n"
                "    Return copy.Next()\n"
                "End Function\n"
                "Local x = First(Count())") == "Int32?");
    CHECK(errors_of("Import std.core\n"
                    "Function Total<S: Iterator<Int>>(items: S): Int => 0\n"
                    "Class Words Implements Iterator<String>\n"
                    "    Method Next(): String? => Null\n"
                    "End Class\n"
                    "Local t = Total(Words())") ==
          errors_t{ "Total: Words doesn't implement Iterator<Int32>, which 'Total' needs for S" });
}

TEST_CASE("generic Interface mistakes", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Struct Both Implements Iterator<Int>, Iterator<String>\n"
                    "    ByRef Method Next(): Int? => Null\n"
                    "End Struct") ==
          errors_t{ "Iterator<String>: Both implements Iterator<Int32> already; a type implements "
                    "a generic Interface with one set of type arguments" });
    CHECK(errors_of("Import std.core\n"
                    "Class Base Implements Iterator<Int>\n"
                    "    Method Next(): Int? => Null\n"
                    "End Class\n"
                    "Class Derived Extends Base Implements Iterator<String>\n"
                    "End Class") ==
          errors_t{ "Iterator<String>: Derived implements Iterator<Int32> already; a type "
                    "implements a generic Interface with one set of type arguments" });
    CHECK(errors_of("Import std.core\n"
                    "Function F<T>(x: T)\n"
                    "    Where T: Iterator<Int> & Iterator<String>\n"
                    "End Function") ==
          errors_t{ "Iterator<String>: T is Iterator<Int32> already; a type implements a generic "
                    "Interface with one set of type arguments" });

    // Nor can an Extend of one instance list another instance of one its generic type gives
    // every instance, by its declaration or by an Extend, with constraints the instance meets.
    const std::string_view holder = "Interface Holder<U>\n"
                                    "    Method Put(x: U): Int\n"
                                    "End Interface\n";
    const std::string_view put_string = "Extend Box<Int> Implements Holder<String>\n"
                                        "    Method Put(x: String): Int => 2\n"
                                        "End Extend";
    CHECK(errors_of(std::format("{}"
                                "Class Box<T> Implements Holder<T>\n"
                                "    Method Put(x: T): Int => 1\n"
                                "End Class\n"
                                "{}",
                                holder, put_string)) ==
          errors_t{ "Holder<String>: Box<Int32> implements Holder<Int32> already; a type "
                    "implements a generic Interface with one set of type arguments" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}"
                                "Class Base\n"
                                "End Class\n"
                                "Class Box<T> Extends Base\n"
                                "End Class\n"
                                "Extend Box<T> Implements Holder<T>\n"
                                "    Where T: Hashable\n"
                                "    Method Put(x: T): Int => 1\n"
                                "End Extend\n"
                                "{}",
                                holder, put_string)) ==
          errors_t{ "Holder<String>: Box<Int32> implements Holder<Int32> already; a type "
                    "implements a generic Interface with one set of type arguments" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}"
                                "Struct Box<T> Implements Holder<T>\n"
                                "    Field V: T\n"
                                "    Method Put(x: T): Int => 1\n"
                                "End Struct\n"
                                "Extend Box<Int> Implements Holder<Int>\n"
                                "End Extend\n"
                                "Extend Box<String> Implements Printable\n"
                                "    Method ToString(): String => V\n"
                                "End Extend",
                                holder))
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Function F(x: Iterator)\nEnd Function") ==
          errors_t{ "Iterator: Iterator<T> is generic, so it needs type arguments, like "
                    "Iterator<Int>" });
    CHECK(errors_of("Import std.core\n"
                    "Function F(x: Iterator<Int, Int>)\nEnd Function") ==
          errors_t{ "Iterator<Int, Int>: Iterator<T> takes 1 type argument, not 2" });
    CHECK(errors_of("Import std.core\n"
                    "Struct Wrong Implements Iterator<Int>\n"
                    "    ByRef Method Next(): String? => Null\n"
                    "End Struct") ==
          errors_t{ "Iterator<Int>: Wrong's 'Next' doesn't match Iterator<Int32>'s, which takes () "
                    "and returns Int32?" });
    CHECK(errors_of("Interface Grow<T> Extends Grow<T[]>\nEnd Interface") ==
          errors_t{ "Grow<T[]>: Grow<T> can't extend itself, directly or through other "
                    "Interfaces" });
    CHECK(errors_of("Import std.core\n"
                    "Interface Picky<T>\n    Where T: Comparable\nEnd Interface\n"
                    "Struct P\n    Field X: Int\nEnd Struct\n"
                    "Function F(x: Picky<P>)\nEnd Function") ==
          errors_t{ "Picky<P>: P doesn't implement Comparable, which Picky<T> needs for T" });
}

TEST_CASE("For ... In goes over an Iterator, or the one a value's Iterate gives", "[sema]") {
    const std::string_view count = "Import std.core\n"
                                   "Struct Count Implements Iterator<Int>\n"
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
    CHECK(errors_of("Import std.core\n"
                    "Function F(it: Iterator<Int>)\n"
                    "    For x In it\n"
                    "        Local s: String = x\n"
                    "    End For\n"
                    "End Function") == errors_t{ "x: expected String, found Int32" });

    // Two names take each pair apart, like a Map's keys and values.
    CHECK(errors_of("Import std.core\n"
                    "Struct Pairs Implements Iterator<(String, Int)>\n"
                    "    ByRef Method Next(): (String, Int)? => Null\n"
                    "End Struct\n"
                    "For key, value In Pairs()\n"
                    "    Local n: String = value\n"
                    "End For") == errors_t{ "value: expected String, found Int32" });
    CHECK(errors_of(std::format("{}For i, x In Count(3)\nEnd For", count)) ==
          errors_t{ "i: 'For a, b In' takes each of Count's elements apart, so they must be pairs, "
                    "not Int32; an index only comes with arrays" });
    CHECK(errors_of("Function F(s: String?)\n    For c In s\n    End For\nEnd Function") ==
          errors_t{ "s: 'For ... In' goes over arrays, Channels, Iterators, and values whose "
                    "Iterate() gives an Iterator, not String?" });

    // A Channel? may be Null, as an Int[]? may.
    CHECK(errors_of("Function F(ch: Channel<Int>?)\n    For x In ch\n    End For\nEnd Function") ==
          errors_t{ "ch: 'ch' may be Null; check that it isn't, or use '?.'" });
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
    CHECK(errors_of("Local a: Int[3] = [1, 2, 3]\nLocal s = a[..18446744073709551615 As UInt64]") ==
          errors_t{ "18446744073709551615 As UInt64: 18446744073709551615 is past the end of "
                    "Int32[3]" });
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
    CHECK(probe("Import std.core\n"
                "Function Largest<T: Comparable>(first: T, rest: T...): T => first\n"
                "Local x = Largest(1.5, 2, 3)") == "Float32");
    CHECK(errors_of(std::format("{}Local n = Sum(1, \"x\")", sum)) ==
          errors_t{ "\"x\": expected Int32, found String" });
    CHECK(errors_of(std::format("{}Local x = 1\nLocal n = Sum(ByRef x)", sum)) ==
          errors_t{ "x: 'nums' takes the rest of the arguments, which can't be ByRef" });
    CHECK(errors_of(std::format("{}Local n = Sum(1, nums: [2])", sum)) ==
          errors_t{ "[2]: parameter 'nums' already has an argument" });

    // A call that fits a function without packing its arguments takes that one, as C#'s does.
    CHECK(errors_of(std::format("{}Function Sum(n: Int): Int => 100\n"
                                "Function Sum(a: Long, rest: Long...): Int => 200\n"
                                "Local one = Sum(1)\n"
                                "Local two = Sum(1, 2)",
                                sum))
              .empty());
}

TEST_CASE("what work and threads share has no text, so it can't be shown", "[sema]") {
    constexpr std::string_view seven = "Import std.core\n"
                                       "Async Function Seven(): Int\n"
                                       "    Await NextFrame()\n"
                                       "    Return 7\n"
                                       "End Function\n";
    CHECK(errors_of(std::format("{}Print(Seven())", seven)) ==
          errors_t{ "Seven(): Task<Int32> can't be shown by 'Print'; show what it gives, with "
                    "Await or Wait()" });
    CHECK(errors_of(std::format("{}Local t = Seven()\nLocal s = \"{{t}}\"", seven)) ==
          errors_t{ "t: Task<Int32> can't be shown in a string; show what it gives, with Await "
                    "or Wait()" });
    CHECK(errors_of(std::format("{}Print(Channel<Int>(2))\nPrint(Scheduler())", seven)) ==
          errors_t{
              "Channel<Int>(2): Channel<Int32> can't be shown by 'Print', since it has no text "
              "of its own",
              "Scheduler(): Scheduler can't be shown by 'Print', since it has no text of its own",
          });
}

TEST_CASE("'values: Printable...' takes values of any types a string can show", "[sema]") {
    // Each argument becomes its text at the call, so the function gets a String[].
    const std::string_view log = "Import std.core\n"
                                 "Function Log(level: Int, values: Printable...): String[] => "
                                 "values\n";
    CHECK(probe(std::format("{}Local texts = Log(1, \"a\", 2, 3.5, True, [1], (1, \"x\"))", log)) ==
          "String[]");
    CHECK(probe(std::format("{}Local texts = Log(1)", log)) == "String[]");
    CHECK(probe(std::format("{}Local texts = Log(1, values: [\"given\"])", log)) == "String[]");
    // An array is a value too, so it's shown, not passed as the texts.
    CHECK(probe(std::format("{}Local xs = [1, 2]\nLocal texts = Log(1, xs)", log)) == "String[]");
    CHECK(errors_of(std::format("{}Struct P\n    Field x: Int\nEnd Struct\n"
                                "Local s: String? = Null\n"
                                "Local texts = Log(1, P(1), s)",
                                log)) ==
          errors_t{
              "P(1): P can't be shown by 'Log'; implement Printable, with a ToString",
              "s: String? can't be shown by 'Log', since it may be Null; give a value for Null "
              "with '?"
              "?'",
          });

    // Only std's Printable means it: a program's own is an Interface of objects.
    CHECK(errors_of("Interface Printable\n"
                    "    Method ToString(): String\n"
                    "End Interface\n"
                    "Function Log(values: Printable...): Int => values.Count\n"
                    "Local n = Log(1)") == errors_t{ "1: expected Printable, found Int32" });
}

TEST_CASE("'values: Formattable...' takes values of any Formattable types", "[sema]") {
    // The function gets a Formattable[], whatever the types, which needn't be objects.
    const std::string_view fill =
        "Import std.core\n"
        "Function Fill(values: Formattable...): Formattable[] => values\n";
    CHECK(probe(std::format("{}Local all = Fill(1, 2.5, \"a\", True)", fill)) == "Formattable[]");
    CHECK(probe(std::format("{}Local all = Fill()", fill)) == "Formattable[]");
    CHECK(errors_of(std::format("{}Struct P\n    Field x: Int\nEnd Struct\n"
                                "Local all = Fill(1, P(1), [2])",
                                fill)) ==
          errors_t{
              "P(1): P isn't Formattable, which 'values' takes",
              "[2]: Int32[] isn't Formattable, which 'values' takes",
          });
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

TEST_CASE("a Property's Override takes @PanicsAtCaller from what it overrides", "[sema]") {
    // So one further down can be @PanicsAtCaller too.
    CHECK(errors_of("Class A\n"
                    "    @PanicsAtCaller\n"
                    "    Property Size: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "End Class\n"
                    "Class B Extends A\n"
                    "    Override Property Size: Int\n"
                    "        Get => 2\n"
                    "    End Property\n"
                    "End Class\n"
                    "Class C Extends B\n"
                    "    @PanicsAtCaller\n"
                    "    Override Property Size: Int\n"
                    "        Get => 3\n"
                    "    End Property\n"
                    "End Class")
              .empty());
}

TEST_CASE("@PanicsAtCaller goes on what code calls", "[sema]") {
    CHECK(errors_of("@PanicsAtCaller Function F()\nEnd Function\n"
                    "Struct S\n"
                    "    Field n: Int\n"
                    "    @PanicsAtCaller New(n: Int)\n"
                    "        Self.n = n\n"
                    "    End New\n"
                    "    @PanicsAtCaller Method M()\n"
                    "    End Method\n"
                    "    @PanicsAtCaller Static Method Make(): S => S(1)\n"
                    "    @PanicsAtCaller Operator +(a: S, b: S): S => S(a.n + b.n)\n"
                    "    @PanicsAtCaller Property Twice: Int\n"
                    "        Get => n * 2\n"
                    "    End Property\n"
                    "End Struct\n"
                    "Extend Int\n"
                    "    @PanicsAtCaller Method Half(): Int => Self / 2\n"
                    "End Extend")
              .empty());
    // An Interface's default that a call runs directly, through its value too.
    CHECK(errors_of("Import std.core\n"
                    "Interface J<T>\n"
                    "    @PanicsAtCaller Final Method F(): Int => 1\n"
                    "    @PanicsAtCaller Method G<U>(x: U): U => x\n"
                    "    @PanicsAtCaller Method H(): Int\n"
                    "        Where T: Numeric\n"
                    "        Return 1\n"
                    "    End Method\n"
                    "End Interface")
              .empty());
    const std::string where =
        "@PanicsAtCaller only goes before a Function, Method, Operator, New, or Property";
    CHECK(errors_of("@PanicsAtCaller Struct S\nEnd Struct\n"
                    "Struct T\n    @PanicsAtCaller Field x: Int\nEnd Struct\n"
                    "@PanicsAtCaller Global g = 1\n"
                    "@PanicsAtCaller Enum E\n    A\nEnd Enum\n"
                    "@PanicsAtCaller Interface I\nEnd Interface") ==
          errors_t{ "S: " + where, "x: " + where, "g: " + where, "E: " + where, "I: " + where });
    const std::string only_direct =
        "of an Interface's members, only a Final, generic, or conditional default can be "
        "@PanicsAtCaller, which a call runs directly; what implements one can be";
    CHECK(errors_of("Interface I\n"
                    "    @PanicsAtCaller Method M()\n"
                    "    @PanicsAtCaller Method D(): Int => 1\n"
                    "    @PanicsAtCaller ReadOnly Property P: Int\n"
                    "End Interface\n"
                    "Class C\n"
                    "    @PanicsAtCaller Method Destroy()\n"
                    "    End Method\n"
                    "End Class\n"
                    "Extern \"C\"\n"
                    "    @PanicsAtCaller Function abs(n: Int32): Int32\n"
                    "End Extern") ==
          errors_t{
              "M: " + only_direct,
              "D: " + only_direct,
              "P: an Interface's members can't be @PanicsAtCaller; what implements one can be",
              "Destroy: Destroy can't be @PanicsAtCaller, since no code calls it",
              "abs: an Extern Function can't be @PanicsAtCaller, since its code is C's",
          });

    // An Override takes it from what it overrides, and can't add it.
    CHECK(errors_of("Class A\n"
                    "    @PanicsAtCaller Method M()\n"
                    "    End Method\n"
                    "    Method N()\n"
                    "    End Method\n"
                    "    Property P: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "End Class\n"
                    "Class B Extends A\n"
                    "    Override Method M()\n"
                    "    End Method\n"
                    "    @PanicsAtCaller Override Method N()\n"
                    "    End Method\n"
                    "    @PanicsAtCaller Override Property P: Int\n"
                    "        Get => 2\n"
                    "    End Property\n"
                    "End Class") ==
          errors_t{
              "N: 'N' is @PanicsAtCaller, so it can't override A's, which isn't, since a call "
              "through A gives no place",
              "P: 'P' is @PanicsAtCaller, so it can't override A's, which isn't, since a use "
              "through A gives no place",
          });
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

    // A parameter's default counts as known where Consts and types run it too.
    const std::string_view add = "Const Function Add(a: Int, b: Int = 10): Int\n"
                                 "    Return a + b\n"
                                 "End Function\n";
    CHECK(probe(std::format("{}Const A = Add(1)", add)) == "Int32 11");
    CHECK(probe(std::format("{}Const A = Add(1)\nLocal t: Int[A]\nLocal x = t.Count", add)) ==
          "Int32 11");
    CHECK(probe(std::format("Struct S\n    Field cells: Int[Add(1)]\nEnd Struct\n{}"
                            "Local x = S().cells.Count",
                            add)) == "Int32 11");
    // A default names what its own function sees, wherever the first call that runs it is, and
    // its errors are reported though that call is tried quietly.
    const std::string_view top = "Const N = 10\n"
                                 "Const Function Add(a: Int, b: Int = N): Int => a + b\n";
    CHECK(probe(std::format("{}Const Function Outer(): Int\n"
                            "    Const N = 5\n"
                            "    Return Add(1)\n"
                            "End Function\n"
                            "Const X = Outer()\n"
                            "Local x = X + Add(1)",
                            top)) == "Int32 22");
    CHECK(probe(std::format("{}Struct G<Const N: Int>\n"
                            "    Field c: Int8[Add(1)]\n"
                            "End Struct\n"
                            "Local x = G<3>().c.Count",
                            top)) == "Int32 11");
    CHECK(errors_of("Const Function Add(a: Int, b: Int = \"x\"): Int => a + b\n"
                    "Function F(x: Int): Int => x\n"
                    "Function F(x: String): Int => 0\n"
                    "Global g = F(Add(1))") == errors_t{ "\"x\": expected Int32, found String" });
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
    CHECK(errors_of("Import std.core\n"
                    "Global g = 1\n"
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
    // A For ... In over a range or any Iterator calls its Next, a Method, which a Const Function
    // can't, whether it runs in a Const, a Global, or code.
    CHECK(
        errors_of("Import std.core\n"
                  "Const Function SumTo(n: Int): Int\n"
                  "    Local total = 0\n"
                  "    For x In 1 To n\n"
                  "        total += x\n"
                  "    End For\n"
                  "    Return total\n"
                  "End Function\n"
                  "Const A = SumTo(10)\n"
                  "Global g = SumTo(4)\n"
                  "Print(A, g, SumTo(3))") ==
        errors_t{ "1 To n: a Const Function can only call Const Functions, and 'Next' isn't one" });
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
    // A call whose arguments don't fit is reported as any call's is, and doesn't run.
    CHECK(errors_of("Const Function Twice(a: Int): Int => a * 2\n"
                    "Function F()\n"
                    "    Local x = Twice(1.5)\n"
                    "    Local y = Twice(\"no\")\n"
                    "    Local z = Twice(True)\n"
                    "End Function\n"
                    "Const W = Twice(2.5)") ==
          errors_t{ "1.5: 1.5 doesn't fit in Int32", "\"no\": expected Int32, found String",
                    "True: expected Int32, found Bool", "2.5: 2.5 doesn't fit in Int32" });
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
    // Its values are Null or a value, so a T?'s Null inside a T?? only the program can hold.
    CHECK(errors_of("Const Function Held(): Int\n"
                    "    Local inner: Int? = Null\n"
                    "    Local held: Int?"
                    "? = inner\n"
                    "    Return 1\n"
                    "End Function\n"
                    "Const X = Held()") ==
          errors_t{ "Held(): 'Held' can't run at compile time: it puts a Null inside Int32?"
                    "?, "
                    "which only the running program can hold" });
    // What its variables hold has a limit too: a String that doubles, a fixed array too big to
    // make, and copies of one String.
    const std::string_view holds = "can't run at compile time: it holds more than 256 MiB";
    CHECK(errors_of("Const Function Big(n: Int): String\n"
                    "    Local s = \"x\"\n"
                    "    For i = 1 To n\n"
                    "        s = s + s\n"
                    "    End For\n"
                    "    Return s\n"
                    "End Function\n"
                    "Const X = Big(40)") == errors_t{ std::format("Big(40): 'Big' {}", holds) });
    CHECK(errors_of("Const Function Wide(): Int\n"
                    "    Local a: Int64[100000000]\n"
                    "    Return 1\n"
                    "End Function\n"
                    "Const X = Wide()") == errors_t{ std::format("Wide(): 'Wide' {}", holds) });
    CHECK(errors_of("Const Function Copies(): Int\n"
                    "    Local s = \"x\"\n"
                    "    For i = 1 To 20\n"
                    "        s = s + s\n"
                    "    End For\n"
                    "    Local a: String[1000]\n"
                    "    For i = 0 Until 1000\n"
                    "        a[i] = s\n"
                    "    End For\n"
                    "    Return 1\n"
                    "End Function\n"
                    "Const X = Copies()") ==
          errors_t{ std::format("Copies(): 'Copies' {}", holds) });
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
    // A Const type argument is any value of its type; one that's a size is checked where the
    // instance is made.
    CHECK(errors_of("Interface I<Const N: Int>\n    Method Size(): Int => N\nEnd Interface\n"
                    "Struct Row Implements I<0>\nEnd Struct")
              .empty());
    CHECK(errors_of("Struct Small<Const N: Int8>\nEnd Struct\nLocal s = Small<300>()") ==
          errors_t{ "300: 300 doesn't fit in Int8" });
    // Each instance works one out in an Int64, so a UInt64's goes up to Int64's highest.
    CHECK(errors_of("Struct Mask<Const M: UInt64>\nEnd Struct\n"
                    "Local a = Mask<9223372036854775807>()\n"
                    "Local b = Mask<18446744073709551615>()") ==
          errors_t{ "18446744073709551615: M takes a value up to 9223372036854775807, not "
                    "18446744073709551615" });
    CHECK(errors_of(std::format("{}Local a = Grid<Int, 2>()", grid)) ==
          errors_t{ "Int: W is Const, so it takes an integer known at compile time, like 64, not "
                    "Int" });
    CHECK(errors_of(std::format("{}Local a = Grid<-1, 2>()", grid)).empty());
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
    // An Attribute can have a built-in's name, which code reaches through its module's
    // qualifier, since "@Inline" alone is always the built-in.
    CHECK(errors_of("Attribute Inline\n@Inline Function F()\nEnd Function").empty());
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
    const std::string_view show = "Import std.core\n"
                                  "Function Show<T>(value: T)\n"
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
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Function Pass<U>(value: U)\n"
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
    CHECK(errors_of("Import std.core\n"
                    "Struct P\n    Field X: Int\n    Field Name: String\nEnd Struct\n"
                    "For Field f: F In P\n    Where F: Numeric\nEnd For") ==
          errors_t{ "P: P's field 'Name' is String, which doesn't implement Numeric, as this loop "
                    "needs" });
}

TEST_CASE("Set changes a field, and For Method calls Methods that take nothing", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Function Clear<T>(ByRef value: T)\n"
                    "    For Field f: F In T\n"
                    "        Where F: Numeric\n"
                    "        f.Set(ByRef value, F.Zero())\n"
                    "    End For\n"
                    "End Function")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Function Clear<T>(value: T)\n"
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
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Function F<T>(value: T)\n"
                                "    For Field f In T\n"
                                "        Print(f.Get(value))\n"
                                "    End For\n"
                                "End Function",
                                point)) ==
          errors_t{ "f.Get(value): the type of f can't be shown by 'Print' without a constraint; "
                    "name it, as in 'For Field f: F In T', then add 'Where F: Printable'" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}For Field f In P\n    Print(f)\nEnd For",
                                point)) ==
          errors_t{ "f: 'f' is a field of P; it has Name, Index, Get(value), Set(ByRef value, x), "
                    "Has(A), and Attribute(A)" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}For Method m In P\n    Print(m.Index)\nEnd For",
                                point)) ==
          errors_t{ "Index: 'm' is a Method of P; it has Name, Has(A), Attribute(A), and "
                    "Call(value, ...)" });
    CHECK(errors_of(std::format("{}For Field f In P\n    f.Name = \"x\"\nEnd For", point)) ==
          errors_t{ "f.Name: only a variable or one of its fields can be assigned to" });
    // Each copy of the body has its own lambda.
    CHECK(errors_of(
              std::format("{}For Field f In P\n    Local g = Function() => f.Name\nEnd For", point))
              .empty());
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}For Field f In P\n    Print(\"{{f.Has(P)}}\")\nEnd For",
                                point)) ==
          errors_t{ "P: 'f.Has' takes the name of an Attribute, which an Attribute declaration "
                    "makes" });
    CHECK(errors_of(std::format("{}For Method m In P\n    m.Call(1)\nEnd For", point)) ==
          errors_t{ "1: expected P, found Int32" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}For Method m In P\n    Where X: Printable\nEnd For",
                                point)) ==
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
    CHECK(probe("Import std.collections\nLocal m: Map<String, Long> = {\"a\": 1}\nLocal n = m") ==
          "Map<String, Int64>");
    CHECK(probe("Import std.collections\nLocal m: Map<Int, String>? = {}") ==
          "Map<Int32, String>? (from Map<Int32, String>)");
    CHECK(errors_of("Import std.collections\n"
                    "Function Total(scores: Map<String, Int>): Int => scores.Count\n"
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
    // The instance that can't be made is reported, not each use of its Methods.
    CHECK(errors_of("Import std.collections\n"
                    "Struct P\n"
                    "    Field X: Int\n"
                    "End Struct\n"
                    "Local s = Set<P>()\n"
                    "s.Add(P(1))\n"
                    "s.Add(P(2))\n"
                    "Local m = Map<P, Int>()\n"
                    "m.Add(P(1), 2)") ==
          errors_t{ "Set<P>: P doesn't implement Hashable, which Set<T> needs for T",
                    "Map<P, Int>: P doesn't implement Hashable, which Map<K, V> needs for K" });
    CHECK(errors_of("Import std.collections\nLocal m: Map<String, Int> = {\"a\": \"b\"}") ==
          errors_t{ "\"b\": expected Int32, found String" });
    CHECK(errors_of("Local m: Int = {\"a\": 1}") ==
          errors_t{ "{\"a\": 1}: expected Int32, found Map<String, Int32>" });

    // A Map literal makes std's Map, which a program's own Map isn't; two declarations of one
    // name show their modules, whatever their type arguments, and inside other types too.
    CHECK(errors_of("Class Map\nEnd Class\nLocal m: Map = {\"a\": 1}") ==
          errors_t{ "{\"a\": 1}: expected test.Map, found "
                    "std.collections.map.Map<String, Int32>" });
    CHECK(errors_of("Class Map\nEnd Class\nLocal m: Map? = {\"a\": 1}") ==
          errors_t{ "{\"a\": 1}: expected test.Map?, found "
                    "std.collections.map.Map<String, Int32>" });
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
    CHECK(errors_of("Extend T[]\n    Method Room(): Int => Capacity - Count\nEnd Extend\n"
                    "Local room = [1].Room()")
              .empty());
    CHECK(
        errors_of("Extend T[]\n    Method M()\n        Capacity = 1\n    End Method\nEnd Extend") ==
        errors_t{ "Capacity: Capacity is how many elements Self has room for, which only its "
                  "Methods change" });
}

TEST_CASE("a call's lambda argument can declare symbols while the call is checked", "[sema]") {
    // Its parameters are new symbols; checking the call keeps working on the parameter it's for.
    CHECK(probe("Import std.collections\n"
                "Local nums = [5, 3, 8]\n"
                "Local text = \"{nums.Filter(Function(n: Int) => n % 2 == 1)}\"") == "String");
}

TEST_CASE("narrowing is forgotten for what a loop changes inside any expression", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Function Clear(ByRef s: String?): Int\n"
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
          errors_t{ "s: String? can't be shown by 'Print', since it may be Null; give a "
                    "value for Null with '?"
                    "?'" });
}

TEST_CASE("only Unsafe code uses what a Ptr points at", "[sema]") {
    const std::string_view unsafe = "needs Unsafe code: an Unsafe block, Function, or Method";
    CHECK(errors_of("Import std.core\n"
                    "Local n = 5\n"
                    "Local p = AddressOf(n)\n"
                    "Local q: Ptr<Int32> = Null\n"
                    "Print(\"{q.Value}\")\n"
                    "q[0] = 1\n"
                    "Local r = q + 1\n"
                    "Local s = q As Int64\n"
                    "Local v: Ptr<Void> = q\n"
                    "Local back: Ptr<Int32> = v\n"
                    "Print(\"{q == Null} {q == back} {SizeOf<Int32>()}\")\n") ==
          errors_t{
              std::format("AddressOf: AddressOf {}", unsafe),
              std::format("Value: a Ptr's Value {}", unsafe),
              std::format("q[0]: indexing a Ptr {}", unsafe),
              std::format("q + 1: '+' on a Ptr {}", unsafe),
              std::format("q As Int64: 'As' from Ptr<Int32> to Int64 {}", unsafe),
          });

    // In Unsafe code, or a lambda made there, they're fine.
    CHECK(errors_of("Local n = 5\n"
                    "Unsafe\n"
                    "    Local p = AddressOf(n)\n"
                    "    p.Value = 6\n"
                    "    p[0] += 1\n"
                    "    Local f = Function() => (p + 1) - p\n"
                    "End Unsafe\n")
              .empty());

    // A known Null Ptr isn't a number to fold.
    CHECK(errors_of("Const p: Ptr<Int32> = Null\n"
                    "Unsafe\n"
                    "    Local q = p + 1\n"
                    "    Local d = (Null As Ptr<Int32>) - (Null As Ptr<Int32>)\n"
                    "    Local e = (Null As Ptr<Int32>) + 1\n"
                    "End Unsafe\n")
              .empty());

    // A Ptr<Void> points at nothing in particular.
    CHECK(errors_of("Local v: Ptr<Void> = Null\n"
                    "Unsafe\n"
                    "    Local a = v.Value\n"
                    "    Local b = v + 1\n"
                    "    Local c = v[0]\n"
                    "End Unsafe\n") ==
          errors_t{
              "Value: a Ptr<Void> points at nothing in particular; make it a Ptr<T> with 'As' to "
              "use its Value",
              "v + 1: a Ptr<Void> has no size to step by; make it a Ptr<UInt8> with 'As'",
              "v: a Ptr<Void> points at nothing in particular; make it a Ptr<T> with 'As' to index "
              "it",
          });
    CHECK(errors_of("Local p: Ptr<Int32> = Null\n"
                    "Local q: Ptr<Int64> = Null\n"
                    "Unsafe\n"
                    "    Local a = 1 + p\n"
                    "    Local b = p - q\n"
                    "    Local c = p.Size\n"
                    "    Local d = p?.Value\n"
                    "    Local e = AddressOf(5)\n"
                    "End Unsafe\n") ==
          errors_t{
              "1 + p: a Ptr steps with p + n, the Ptr first",
              std::string("p - q: '-' counts the values between two Ptrs of one type, not "
                          "Ptr<Int32> and Ptr<Int64>"),
              "Size: a Ptr has only Value, what it points at, which Unsafe code reads and sets",
              "p?.Value: a Ptr's Value takes '.', not '?.'; compare the Ptr with Null first",
              std::string("5: AddressOf takes a variable, a field, or an element, which is "
                          "somewhere in memory, not a value"),
          });
}

TEST_CASE("a @CLayout Struct sized by SizeOf of one declared later is laid out whole", "[sema]") {
    CHECK(probe("@CLayout Struct Header\n"
                "    Field raw: UInt8[SizeOf<Body>() * 2 + 8]\n"
                "End Struct\n"
                "@CLayout Struct Body\n"
                "    Field a: Int64\n"
                "    Field b: Float64\n"
                "End Struct\n"
                "Local size = SizeOf<Header>()") == "Int64 40");
    CHECK(errors_of("@CLayout Struct Huge\n"
                    "    Field cells: Int64[536870912]\n"
                    "End Struct\n"
                    "Extern\n"
                    "    Function Take(huge: Huge)\n"
                    "End Extern\n") ==
          errors_t{ "Huge: Huge takes 4 GiB or more, so it goes to and from C through a Ptr, not "
                    "by value" });
}

TEST_CASE("a @CLayout Struct that contains itself is an error, not a crash", "[sema]") {
    CHECK(errors_of("@CLayout Struct R\n"
                    "    Field r: R\n"
                    "End Struct\n") ==
          errors_t{ "r: 'R' can't contain itself; a Struct is stored inline" });
    CHECK(errors_of("@CLayout Struct R\n"
                    "    Field r: R[2]\n"
                    "End Struct\n"
                    "Struct S\n"
                    "    Field s: S\n"
                    "End Struct\n"
                    "Local p: Ptr<S> = Null\n") ==
          errors_t{
              "r: 'R' can't contain itself; a Struct is stored inline",
              "s: 'S' can't contain itself; a Struct is stored inline",
          });
}

TEST_CASE("C code doesn't see a generic Struct's instance, which can't be @CLayout", "[sema]") {
    CHECK(errors_of("Struct Box<T>\n"
                    "    Field value: T\n"
                    "End Struct\n"
                    "@CLayout Struct Outer\n"
                    "    Field box: Box<Int32>\n"
                    "End Struct\n"
                    "Extern\n"
                    "    Function Take(box: Box<Int32>)\n"
                    "End Extern\n") ==
          errors_t{
              std::string("box: a @CLayout Struct's Field holds other Structs only when they're "
                          "@CLayout too, and Box<Int32> is a generic Struct's instance, which "
                          "can't be"),
              std::string("Box<Int32>: C code sees Box<Int32> here, a generic Struct's instance, "
                          "which can't be @CLayout; declare a @CLayout Struct of the types C has"),
          });
}

TEST_CASE("a Struct C gives that holds a CString takes no String's copy", "[sema]") {
    const std::string escapes =
        "this Extern Function gives, or sets through ByRef or a Ptr, what could point into text, "
        "a CString or a Ptr to Int8s or UInt8s, and a String's copy goes right after the call; it "
        "takes only CStrings";
    CHECK(errors_of("@CLayout Struct Named\n"
                    "    Field name: CString\n"
                    "    Field n: Int32\n"
                    "End Struct\n"
                    "@CLayout Struct Sized\n"
                    "    Field n: Int32\n"
                    "End Struct\n"
                    "Extern\n"
                    "    Function Keep(text: CString): Named\n"
                    "    Function KeepInto(ByRef named: Named, text: CString)\n"
                    "    Function Measure(text: CString): Sized\n"
                    "End Extern\n"
                    "Local a = Keep(\"a\")\n"
                    "Local b = Named(Null, 0)\n"
                    "KeepInto(ByRef b, \"b\")\n"
                    "Local c = Measure(\"c\")\n") == errors_t{
                                                         "\"a\": " + escapes,
                                                         "\"b\": " + escapes,
                                                     });
}

TEST_CASE("C's pointers aren't T?s, so ?? and ?. don't take them", "[sema]") {
    CHECK(errors_of("Local p: Ptr<Int32> = Null\n"
                    "Local q: Ptr<Int32> = Null\n"
                    "Local c: CString = Null\n"
                    "Local r = p ?? q\n"
                    "Local d = c ?? c\n"
                    "Unsafe\n"
                    "    Local t = c?.ToString()\n"
                    "End Unsafe\n") ==
          errors_t{
              std::string("p: Ptr<Int32> is C's pointer, not a T?, so '\?\?' doesn't take it; "
                          "compare it with Null instead"),
              std::string("c: CString is C's pointer, not a T?, so '\?\?' doesn't take it; compare "
                          "it with Null instead"),
              std::string("c: CString is C's pointer, not a T?, so '?.' doesn't take it; compare "
                          "it with Null instead"),
          });

    // A generic's T? of a Ptr is a T?, whose Null is apart from the Ptr's.
    CHECK(
        errors_of("Function First<T>(items: T[]): T?\n"
                  "    If items.Count == 0 Then Return Null\n"
                  "    Return items[0]\n"
                  "End Function\n"
                  "Local p: Ptr<Int32> = Null\n"
                  "Local x = First([p])\n"
                  "Local y = x ?? p\n"
                  "Unsafe\n"
                  "    Local v = x?.Value\n"
                  "End Unsafe\n") ==
        errors_t{ "x?.Value: a Ptr's Value takes '.', not '?.'; compare the Ptr with Null first" });
}

TEST_CASE("a Ptr isn't shown in a string", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Local p: Ptr<Int32> = Null\n"
                    "Print(\"{p} {[p]} {(1, p)}\")\n") ==
          errors_t{
              std::string("p: Ptr<Int32> can't be shown in a string; 'As UInt64' gives its address "
                          "as a number, in Unsafe code"),
              "[p]: Ptr<Int32>[] can't be shown in a string, since its elements can't be",
              std::string("(1, p): (Int32, Ptr<Int32>) can't be shown in a string, since one of "
                          "its elements can't be"),
          });
}

TEST_CASE("For ... In calls an Unsafe Iterate only in Unsafe code", "[sema]") {
    const std::string iterating = "Import std.core\n"
                                  "Struct Counting Implements Iterator<Int32>\n"
                                  "    Field at: Int32\n"
                                  "    ByRef Method Next(): Int32?\n"
                                  "        Return Null\n"
                                  "    End Method\n"
                                  "End Struct\n"
                                  "Class Numbers\n"
                                  "    Unsafe Method Iterate(): Counting\n"
                                  "        Return Counting(0)\n"
                                  "    End Method\n"
                                  "End Class\n"
                                  "Local box = Numbers()\n";
    CHECK(
        errors_of(iterating + "For x In box\nEnd For\n") ==
        errors_t{ "box: going over Numbers, whose Iterate is Unsafe, needs Unsafe code: an Unsafe "
                  "block, Function, or Method" });
    CHECK(errors_of(iterating + "Unsafe\n    For x In box\n    End For\nEnd Unsafe\n").empty());
}

TEST_CASE("AddressOf follows the rules for a change to what it points at", "[sema]") {
    // Each pass of a Parallel For changes what's outside it only at its own index.
    CHECK(errors_of("Function Main()\n"
                    "    Local n: Int32 = 0\n"
                    "    Local cells: Int32[] = [0, 0]\n"
                    "    Unsafe\n"
                    "        Parallel For i = 0 Until 2\n"
                    "            Local a = AddressOf(n)\n"
                    "            Local b = AddressOf(cells[i])\n"
                    "            Local c = AddressOf(cells[0])\n"
                    "        End For\n"
                    "    End Unsafe\n"
                    "End Function\n") ==
          errors_t{
              "n: a Parallel For's body can't change 'n', which is outside it",
              std::string("cells: a Parallel For's body changes 'cells', an array from outside the "
                          "body, only at its own index, cells[i]"),
          });

    // A ReadOnly Field and a loop variable don't change, and the Ptr's type is the variable's.
    // A Struct's Self is somewhere in memory too.
    CHECK(errors_of("Class Box\n"
                    "    ReadOnly Field id: Int32 = 1\n"
                    "End Class\n"
                    "Struct P\n"
                    "    Field x: Int32\n"
                    "    ByRef Method M()\n"
                    "        Unsafe\n"
                    "            Local p = AddressOf(Self)\n"
                    "        End Unsafe\n"
                    "    End Method\n"
                    "End Struct\n"
                    "Local box = Box()\n"
                    "Local n: Int32 = 0\n"
                    "Unsafe\n"
                    "    Local a = AddressOf(box.id)\n"
                    "    For i = 0 Until 2\n"
                    "        Local b = AddressOf(i)\n"
                    "    End For\n"
                    "    Local c = AddressOf<Int64>(n)\n"
                    "End Unsafe\n") ==
          errors_t{
              "box.id: 'id' is ReadOnly; only Box's New can change it",
              std::string("i: AddressOf takes a variable, a field, or an element, which is "
                          "somewhere in memory, not a loop variable, which can't be changed"),
              std::string("AddressOf<Int64>: AddressOf takes no type arguments; its Ptr's type is "
                          "the variable's"),
          });

    CHECK(
        errors_of("Class Holder\n"
                  "    Public Static ReadOnly Field limit: Int32 = 5\n"
                  "    Method Poke()\n"
                  "        Unsafe\n"
                  "            Local p = AddressOf(limit)\n"
                  "        End Unsafe\n"
                  "    End Method\n"
                  "End Class\n"
                  "Function Make(): (Int32, Int32) => (1, 2)\n"
                  "Struct P\n"
                  "    Field x: Int32\n"
                  "    Method M()\n"
                  "        Unsafe\n"
                  "            Local g = Function() => AddressOf(Self)\n"
                  "        End Unsafe\n"
                  "    End Method\n"
                  "End Struct\n"
                  "Unsafe\n"
                  "    Local a = AddressOf(Holder.limit)\n"
                  "    Local b = AddressOf(Make().0)\n"
                  "End Unsafe\n") ==
        errors_t{
            "limit: 'limit' is ReadOnly and Static, so it keeps its first value",
            std::string("Self: AddressOf(Self) here would point at this body's copy of Self; take "
                        "the address outside it"),
            "Holder.limit: 'limit' is ReadOnly and Static, so it keeps its first value",
            std::string("Make().0: AddressOf takes a variable, or a field or element of one, not a "
                        "part of a value the statement makes"),
        });

    // A part of a Const, a loop variable, or a parameter can't change, as they can't.
    CHECK(
        errors_of("Struct S\n    Field a: Int32\nEnd Struct\n"
                  "Const C = S(1)\n"
                  "Const F: Int32[2] = [1, 2]\n"
                  "Function Poke(s: S, ByRef t: S, n: Int32, ByRef m: Int32)\n"
                  "    Unsafe\n"
                  "        Local p = AddressOf(s.a)\n"
                  "        Local q = AddressOf(t.a)\n"
                  "        Local r = AddressOf(n)\n"
                  "        Local u = AddressOf(m)\n"
                  "    End Unsafe\n"
                  "End Function\n"
                  "Unsafe\n"
                  "    Local a = AddressOf(C.a)\n"
                  "    Local b = AddressOf(F[0])\n"
                  "    For each In [S(2)]\n"
                  "        Local c = AddressOf(each.a)\n"
                  "    End For\n"
                  "End Unsafe\n") ==
        errors_t{
            "s.a: 's' is a parameter and can't be changed; copy it into a Local, or make it ByRef",
            "n: 'n' is a parameter and can't be changed; copy it into a Local, or make it ByRef",
            "C.a: 'C' is a Const and can't be changed",
            "F[0]: 'F' is a Const and can't be changed",
            "each.a: 'each' is a loop variable and can't be changed",
        });
}

TEST_CASE("a T?'s Expect and OrElse, Try on a value-only built-in, and what's INVALID are quiet "
          "where they should be",
          "[sema]") {
    // An array's T?, and a variable declared as a T? that holds a value here, have them.
    CHECK(probe("Local a: Int[]? = Null\nLocal b = a.OrElse(Function(): Int[] => [])") ==
          "Int32[]");
    CHECK(probe("Local f: Int[2]? = Null\nLocal g = f.Expect(\"none\")") == "Int32[2]");
    CHECK(probe("Local n: Int? = 3\nLocal m = n.OrElse(Function() => 4)") == "Int32");
    // Try only warns, and alone it does nothing.
    CHECK(errors_of("Local t = Try TypeName<Int>()") ==
          errors_t{ "Try TypeName<Int>(): 'TypeName<Int>()' doesn't throw, so it doesn't need "
                    "Try" });
    CHECK(errors_of("Try TypeName<Int>()") ==
          errors_t{ "Try TypeName<Int>(): 'TypeName<Int>()' doesn't throw, so it doesn't need Try",
                    "Try TypeName<Int>(): expression has no effect" });
    // What's INVALID converts quietly, an Interface too, so each mistake is one error.
    CHECK(errors_of("Import std.core\nLocal p: Printable = Nope") ==
          errors_t{ "Nope: 'Nope' isn't declared" });
    const std::string iterable = "Import std.core\nImport std.collections\n";
    CHECK(errors_of(iterable + "Function F(x: Iterable<Int64>)\nEnd Function\nF([])\nF([1, 2])")
              .empty());
    CHECK(errors_of(iterable + "Function F<T>(x: Iterable<T>)\nEnd Function\nF([])") ==
          errors_t{ "F: can't tell what T is in this call; write the type arguments, like "
                    "F<Int>(...)",
                    "[]: an empty array needs a type from where it's used, like 'Local a: Int[] = "
                    "[]'" });
}

TEST_CASE("TypeName gives a type's name, known at compile time", "[sema]") {
    CHECK(probe("Local x = TypeName<Int>()") == "String Int32");
    CHECK(errors_of("Const n = TypeName<(Int, String?)>()\n"
                    "Function F<T>(): String => TypeName<T[]>()\n"
                    "Local s = F<Int>()")
              .empty());
    const std::string takes =
        ": TypeName takes one type argument, and no arguments, like TypeName<Int32>()";
    CHECK(errors_of("Local a = TypeName()\n"
                    "Local b = TypeName<Int>(1)\n"
                    "Local c = TypeName<Int, Int>()\n"
                    "Local d = TypeName<Nowhere>()\n"
                    "Local e = TypeName\n"
                    "TypeName<Int>()") == errors_t{
                                              "TypeName()" + takes,
                                              "TypeName<Int>(1)" + takes,
                                              "TypeName<Int, Int>()" + takes,
                                              "Nowhere: 'Nowhere' isn't a type",
                                              "TypeName: 'TypeName' is called, as in TypeName<T>()",
                                              "TypeName<Int>(): expression has no effect",
                                          });
    // A program's own TypeName takes the place of the built-in.
    CHECK(probe("Function TypeName(): Int => 1\nLocal x = TypeName()") == "Int32");
}

TEST_CASE("Ptrs and SizeOf say what's wrong with them", "[sema]") {
    // SizeOf and AddressOf look like calls, but only give a value, so alone they do nothing.
    CHECK(errors_of("SizeOf<Int32>()\n"
                    "Defer SizeOf<Int64>()\n"
                    "Unsafe\n    Local x = 1\n    AddressOf(x)\nEnd Unsafe\n"
                    "Local f: Func() = Function() => SizeOf<Int32>()") ==
          errors_t{
              "SizeOf<Int32>(): expression has no effect",
              "SizeOf<Int64>(): expression has no effect",
              "AddressOf(x): expression has no effect",
              "SizeOf<Int32>(): this lambda returns nothing, so its '=>' needs a call, like '=> "
              "Print(x)'",
          });
    CHECK(errors_of("Struct Nothing\n"
                    "End Struct\n"
                    "Struct Big\n"
                    "    Field cells: Int64[2147483647]\n"
                    "End Struct\n"
                    "Struct Bigger\n"
                    "    Field rows: Big[2147483647]\n"
                    "End Struct\n"
                    "Local a = [Nothing(), Nothing()]\n"
                    "Local p: Ptr<Int32> = Null\n"
                    "Unsafe\n"
                    "    p -= p\n"
                    "    Local q = AddressOf(a[0])\n"
                    "    Local count = q - q\n"
                    "    Local big = SizeOf<Big>()\n"
                    "    Local bigger = SizeOf<Bigger>()\n"
                    "    Local r = SizeOf\n"
                    "End Unsafe\n"
                    "Extend Ptr<Int32>\n"
                    "End Extend\n") ==
          errors_t{
              "p -= p: '-=' gives Int64, which can't be assigned to Ptr<Int32>",
              "q - q: '-' counts the values between two Ptrs, and Nothing takes no bytes",
              "SizeOf<Bigger>(): Bigger takes more bytes than an Int64 counts",
              "SizeOf: 'SizeOf' is called, as in SizeOf<T>()",
              "Ptr<Int32>: a Ptr can't be extended; it has only its own operations",
          });
    CHECK(errors_of("Struct Big\n"
                    "    Field cells: Int64[2147483647]\n"
                    "End Struct\n"
                    "Struct Bigger\n"
                    "    Field rows: Big[2147483647]\n"
                    "End Struct\n"
                    "Local p: Ptr<Bigger> = Null\n"
                    "Local q: Ptr<Int32> = Null\n"
                    "Unsafe\n"
                    "    Local r = p + 1\n"
                    "    Local s = q.ToString()\n"
                    "End Unsafe\n") ==
          errors_t{
              std::string("p + 1: Bigger takes more bytes than an Int64 counts, so a Ptr at it "
                          "doesn't step"),
              "ToString: a Ptr has only Value, what it points at, which Unsafe code reads and sets",
          });

    // What a Ptr points at stays where it is, so its elements go ByRef, and an Unsafe
    // Function's defaults are its Unsafe code's.
    CHECK(errors_of("Function Add(ByRef target: Int32, amount: Int32)\n"
                    "    target += amount\n"
                    "End Function\n"
                    "Global g: Int32 = 3\n"
                    "Unsafe Function Read(p: Ptr<Int32> = AddressOf(g)): Int32 => p.Value\n"
                    "Local n: Int32 = 1\n"
                    "Unsafe\n"
                    "    Local p = AddressOf(n)\n"
                    "    Add(ByRef p[0], 1)\n"
                    "    Add(ByRef p.Value, 1)\n"
                    "End Unsafe\n") ==
          errors_t{ "AddressOf(g): a default value must be known at compile time" });

    // Mem is only its Static Methods.
    CHECK(errors_of("Import std.ffi\nLocal m = Mem()") ==
          errors_t{ "Mem: Mem's New is private to std/ffi/mem.fb" });
}

TEST_CASE("SizeOf measures types declared after it, and refuses one that holds itself", "[sema]") {
    CHECK(probe("Struct A\n"
                "    Field a: Int8[SizeOf<B>() + 1]\n"
                "End Struct\n"
                "Struct B\n"
                "    Field x: Int64\n"
                "    Field y: Int64\n"
                "End Struct\n"
                "Local size = SizeOf<A>()") == "Int64 17");
    CHECK(probe("Struct A\n"
                "    Field a: Int8[SizeOf<E>()]\n"
                "    Field b: Int8[SizeOf<B[2]>() + 1]\n"
                "End Struct\n"
                "Enum E: Int64\n"
                "    X\n"
                "End Enum\n"
                "Struct B\n"
                "    Field x: Int32\n"
                "End Struct\n"
                "Local size = SizeOf<A>()") == "Int64 17");
    CHECK(probe("Const N = SizeOf<B>()\n"
                "Const Function Room(): Int64 => SizeOf<B>() + 1\n"
                "Global buffer: Int8[N + 1]\n"
                "Struct A\n"
                "    Field a: Int8[Room()]\n"
                "End Struct\n"
                "Struct B\n"
                "    Field x: Int64\n"
                "    Field y: Int64\n"
                "End Struct\n"
                "Local size = SizeOf<A>() + N") == "Int64 33");

    CHECK(errors_of("Struct R\n"
                    "    Field r: R\n"
                    "End Struct\n"
                    "Struct U\n"
                    "    Field a: Int8[SizeOf<R>()]\n"
                    "End Struct\n"
                    "Struct V\n"
                    "    Field a: Int8[SizeOf<V>()]\n"
                    "End Struct\n") ==
          errors_t{
              "r: 'R' can't contain itself; a Struct is stored inline",
              "R: SizeOf can't measure R, whose size depends on itself",
              "V: SizeOf can't measure V, whose size depends on itself",
          });
    // Nor one holding a fixed array that an instance works out past what one holds.
    CHECK(errors_of("Struct G<Const W: Int, Const H: Int>\n"
                    "    Field c: Int8[W, H]\n"
                    "End Struct\n"
                    "Local size = SizeOf<G<65536, 65536>>()\n"
                    "Local more = SizeOf<G<65536, 65536>[2]>()\n"
                    "Local fits = SizeOf<G<46340, 46340>>()") ==
          errors_t{
              "SizeOf<G<65536, 65536>>(): G<65536, 65536> holds Int8[65536, 65536], with more "
              "elements than a fixed array can hold, at most 2147483647",
              "SizeOf<G<65536, 65536>[2]>(): G<65536, 65536>[2] holds Int8[65536, 65536], with "
              "more elements than a fixed array can hold, at most 2147483647",
          });

    // A Const Function waits for the cycle to be reported, rather than running into it.
    CHECK(errors_of("Struct R\n"
                    "    Field x: Int32\n"
                    "    Field r: R\n"
                    "End Struct\n"
                    "Const Function Make(): Int32\n"
                    "    Local r = R()\n"
                    "    Return r.x + 1\n"
                    "End Function\n"
                    "Struct U\n"
                    "    Field a: Int8[Make()]\n"
                    "End Struct\n") ==
          errors_t{
              "r: 'R' can't contain itself; a Struct is stored inline",
              std::string("Make(): this Const Function can't run here, since its code needs types "
                          "that aren't declared yet"),
          });
}

TEST_CASE("what a Ptr points at goes ByRef twice, as C's memory, which may be shared", "[sema]") {
    CHECK(errors_of("Function Swap(ByRef a: Int32, ByRef b: Int32)\n"
                    "    Local c = a\n"
                    "    a = b\n"
                    "    b = c\n"
                    "End Function\n"
                    "Local n = [1, 2]\n"
                    "Unsafe\n"
                    "    Local p = AddressOf(n[0])\n"
                    "    Swap(ByRef p[0], ByRef p[1])\n"
                    "    Swap(ByRef p.Value, ByRef p[0])\n"
                    "End Unsafe\n")
              .empty());

    // A ByRef parameter is a pointer to C, as a Ptr is.
    CHECK(
        errors_of("Extern\n"
                  "    Function frexp(value: Float64, ByRef exponent: Int32): Float64\n"
                  "    Function Split(value: Float64, exponent: Ptr<Int32>): Float64 = \"frexp\"\n"
                  "End Extern\n")
            .empty());
}

TEST_CASE("one C symbol's declarations can differ in what their pointers point at", "[sema]") {
    CHECK(errors_of("Extern\n"
                    "    Function free(memory: Ptr<Void>)\n"
                    "    Function FreeText(text: CString) = \"free\"\n"
                    "    Function FreeBytes(bytes: Ptr<UInt8>) = \"free\"\n"
                    "End Extern\n")
              .empty());
}

TEST_CASE("a Ptr points at a value C code has, and Void is only a Ptr's", "[sema]") {
    const std::string_view c_data = "a Ptr points at a value C code has: a number, Bool, Enum, "
                                    "CString, Ptr, Callback, or a fixed array or Struct of these";
    CHECK(errors_of("Local a: Ptr<String> = Null") ==
          errors_t{ std::format("String: {}; String isn't one", c_data) });
    CHECK(errors_of("Struct S\n    Field name: String\nEnd Struct\nLocal b: Ptr<S> = Null") ==
          errors_t{ std::format("S: {}; S isn't one, since its Field name is String", c_data) });
    CHECK(errors_of("Function F<T>(p: Ptr<T>)\nEnd Function") ==
          errors_t{ std::format("T: {}; T is a type parameter, which could be any type unless "
                                "'Where T: CData' keeps it to C's",
                                c_data) });
    CHECK(errors_of("Local c: Void") ==
          errors_t{ "Void: Void is only a Ptr's type, as Ptr<Void>, which points at anything" });
    CHECK(errors_of("Local d: Ptr<Int32>? = Null") ==
          errors_t{ "Ptr<Int32>?: Ptr<Int32> can be Null already, as C's NULL" });
    CHECK(errors_of("Local e = SizeOf<String>()") ==
          errors_t{ "String: SizeOf measures a value C code has: a number, Bool, Enum, CString, "
                    "Ptr, Callback, or a fixed array or Struct of these; String isn't one" });

    // A Struct's Fields are checked once they're all declared, so one can point at its own
    // Struct.
    CHECK(
        errors_of("Struct Node\n    Field next: Ptr<Node>\n    Field value: Int32\nEnd Struct\n"
                  "Struct Late\n    Field p: Ptr<Later>\nEnd Struct\n"
                  "Struct Later\n    Field s: String\nEnd Struct\n") ==
        errors_t{ std::format("Later: {}; Later isn't one, since its Field s is String", c_data) });

    // SizeOf is known at compile time, as C lays the type out.
    CHECK(
        errors_of("Struct P\n    Field a: Int8\n    Field b: Int32\n    Field c: Int8\nEnd Struct\n"
                  "Const size = SizeOf<P>()\n"
                  "Local fits: Int8[size] \n")
            .empty());
}

TEST_CASE("a type parameter that's CData can be a Ptr's T, and SizeOf's", "[sema]") {
    // Its instances are measured as they're made, and a Ptr at one steps and counts by it.
    CHECK(errors_of("Import std.ffi\n"
                    "@CLayout Struct Point\n    Field x: Int32\nEnd Struct\n"
                    "Struct Run<T: CData>\n    Field first: Ptr<T>\n    Field count: Int64\n"
                    "End Struct\n"
                    "Unsafe Function Last<T: CData>(run: Run<T>): T\n"
                    "    Local size = SizeOf<T>() + SizeOf<T[2]>()\n"
                    "    Local steps = (run.first + run.count) - run.first\n"
                    "    Return run.first[run.count - 1]\n"
                    "End Function\n"
                    "Function Wrap<T>(p: Ptr<T>): Run<T>\n"
                    "    Where T: CData\n"
                    "    Return Run<T>(p, 1)\n"
                    "End Function\n"
                    "Local a = Run<Point>(Null, 0)\n"
                    "Local b = Run<Int32[4]>(Null, 0)\n"
                    "Local c = Run<Ptr<Point>>(Null, 0)\n"
                    "Local d = Wrap<CString>(Null)\n")
              .empty());

    // CData is what C code has: a Struct only when it's @CLayout, and it's only a constraint.
    const std::string_view what = "CData is a number, Bool, Enum, CString, Ptr, Callback, or a "
                                  "@CLayout Struct or fixed array of these";
    const auto isnt = [](const std::string_view type, const std::string_view why) {
        return std::format("Size<{0}>: {0} isn't CData, which 'Size' needs for T: {1}", type, why);
    };
    CHECK(errors_of("Import std.ffi\n"
                    "Struct Plain\n    Field x: Int32\nEnd Struct\n"
                    "Struct Box<T>\n    Field v: T\nEnd Struct\n"
                    "Function Size<T: CData>(): Int64 => SizeOf<T>()\n"
                    "Local a = Size<String>()\n"
                    "Local b = Size<Plain>()\n"
                    "Local c = Size<String[4]>()\n"
                    "Local d = Size<Box<Int32>>()\n") ==
          errors_t{
              isnt("String", what),
              isnt("Plain", "a Struct is CData only when it's @CLayout"),
              isnt("String[4]", std::format("its elements are String, and {}", what)),
              isnt("Box<Int32>", "it's a generic Struct's instance, which can't be @CLayout"),
          });
    CHECK(errors_of("Import std.ffi\nLocal e: CData\n") ==
          errors_t{ "CData: CData can only be a constraint, like <T: CData>, since it says what C "
                    "code has" });

    // A Struct of a CData T takes a byte at least, so its Ptrs count, in the generic code too.
    CHECK(errors_of("Import std.ffi\n"
                    "Struct Wrap<T: CData>\n    Field v: T\nEnd Struct\n"
                    "Struct Four<T: CData>\n    Field items: T[4]\nEnd Struct\n"
                    "Unsafe Function Count<T: CData>(a: Ptr<Wrap<T>>, b: Ptr<Wrap<T>>): Int64 => "
                    "a - b\n"
                    "Unsafe Function Count4<T: CData>(a: Ptr<Four<T>>, b: Ptr<Four<T>>): Int64 => "
                    "a - b\n")
              .empty());

    // A T's size is known for each instance, but not in the generic code, where an empty
    // Struct's Ptrs don't count either.
    CHECK(errors_of("Import std.ffi\n"
                    "Struct Raw<T: CData>\n    Field bytes: UInt8[SizeOf<T>()]\nEnd Struct\n"
                    "Struct Empty<T>\nEnd Struct\n"
                    "Unsafe Function Count<T>(a: Ptr<Empty<T>>, b: Ptr<Empty<T>>): Int64 => a - b\n"
                    "Function Size<T: CData>(): Int64\n"
                    "    Const size = SizeOf<T>()\n"
                    "    Return size\n"
                    "End Function\n") ==
          errors_t{
              "SizeOf<T>(): a fixed array's size must be an integer known at compile time, like "
              "64 or 8 * 8",
              "a - b: '-' counts the values between two Ptrs, and Empty<T> takes no bytes",
          });
    CHECK(errors_of("Import std.core\nImport std.ffi\n"
                    "Struct Listed Implements CData\n    Field x: Int32\nEnd Struct\n"
                    "Function Peek<T: Printable>(p: Ptr<T>)\nEnd Function\n") ==
          errors_t{
              "CData: Listed isn't CData: a Struct is CData only when it's @CLayout",
              "T: a Ptr points at a value C code has: a number, Bool, Enum, CString, Ptr, "
              "Callback, or a fixed array or Struct of these; T is a type parameter, which could "
              "be any type unless 'Where T: CData' keeps it to C's",
          });
}

TEST_CASE("only Unsafe code calls an Unsafe Function or Method", "[sema]") {
    CHECK(errors_of("Unsafe Function Danger(): Int32 => 1\n"
                    "Local d = Danger()\n"
                    "Local f: Func(): Int32 = Danger\n"
                    "Unsafe\n"
                    "    Local e = Danger()\n"
                    "    Local g = Function() => Danger()\n"
                    "End Unsafe\n") ==
          errors_t{
              "Danger: calling 'Danger', which is Unsafe, needs Unsafe code: an Unsafe block, "
              "Function, or Method",
              "Danger: 'Danger' is Unsafe, so it isn't a function value, which safe code could "
              "call; a lambda in Unsafe code can call it",
          });

    // What code calls without saying so can't be Unsafe.
    CHECK(errors_of("Class Base\n"
                    "    Public Method Step()\n"
                    "    End Method\n"
                    "    Public Unsafe Method Risky()\n"
                    "    End Method\n"
                    "End Class\n"
                    "Class Child Extends Base\n"
                    "    Public Override Unsafe Method Step()\n"
                    "    End Method\n"
                    "    Public Override Method Risky()\n"
                    "    End Method\n"
                    "End Class\n") ==
          errors_t{ "Step: 'Step' is Unsafe, so it can't override Base's, which isn't, since a "
                    "call through Base needs no Unsafe code" });
    CHECK(errors_of("Interface Shape\n"
                    "    Method Area(): Int32\n"
                    "    Unsafe Method Corners(): Int32\n"
                    "End Interface\n"
                    "Class Box Implements Shape\n"
                    "    Public Unsafe Method Area(): Int32 => 1\n"
                    "    Public Method Corners(): Int32 => 4\n"
                    "End Class\n") ==
          errors_t{
              "Corners: an Interface's Method can't be Unsafe, since code calls it through "
              "objects whose own Methods may not be",
              "Shape: 'Area' is Unsafe, so it can't implement Shape's, since a call through the "
              "Interface needs no Unsafe code",
          });
    CHECK(errors_of("Struct S\n"
                    "    Field x: Int32\n"
                    "    Unsafe Operator +(a: S, b: S): S => a\n"
                    "End Struct\n"
                    "Unsafe Const Function C(): Int32 => 1\n"
                    "Const Function D(): Int32\n"
                    "    Unsafe\n"
                    "    End Unsafe\n"
                    "    Return 1\n"
                    "End Function\n") ==
          errors_t{
              "+: an Operator can't be Unsafe, since it's used like any operator",
              "C: a Const Function runs in the compiler, so it can't be Unsafe",
              "Unsafe: a Const Function can't have Unsafe code, since it runs in the compiler",
          });
}

TEST_CASE("a CString is Null as C's NULL, and Unsafe code reads its text", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Local cs: CString = Null\n"
                    "Print(\"{cs == Null} {cs != Null}\")\n"
                    "Print(cs.ToString())\n"
                    "Local raw = cs As Ptr<UInt8>\n"
                    "Local bad = cs As Ptr<Int32>\n"
                    "Unsafe\n"
                    "    Print(cs.ToString())\n"
                    "    Local back = raw As CString\n"
                    "End Unsafe\n") ==
          errors_t{
              "ToString: reading a CString's text needs Unsafe code: an Unsafe block, Function, or "
              "Method",
              "cs As Ptr<UInt8>: 'As' from CString to Ptr<UInt8> needs Unsafe code: an Unsafe "
              "block, Function, or Method",
              "cs As Ptr<Int32>: 'As' can't convert CString to Ptr<Int32>",
          });

    // A Ptr isn't Sendable.
    CHECK(
        errors_of("Local p: Ptr<Int32> = Null\nLocal t = Spawn(Function() => p == Null)").size() ==
        1);
}

TEST_CASE("a name that a later milestone declares isn't supported yet, wherever it's used",
          "[sema]") {
    CHECK(errors_of("Local t: Clock") == errors_t{ "Clock: 'Clock' isn't supported yet" });
    CHECK(errors_of("Function F<T>()\n    Where T: DateTime\nEnd Function") ==
          errors_t{ "DateTime: 'DateTime' isn't supported yet" });
    CHECK(errors_of("Local d = DateTime(1, 2, 3)") ==
          errors_t{ "DateTime: 'DateTime' isn't supported yet" });

    // A program can still declare one of them.
    CHECK(errors_of("Function Spawn(): Int => 1\nLocal s = Spawn()").empty());
}

TEST_CASE("std.math's types say which Import they need, wherever they're used", "[sema]") {
    CHECK(errors_of("Local v = Vec3(1, 2, 3)") ==
          errors_t{ "Vec3: 'Vec3' is in std.math, which this file doesn't import" });
    CHECK(errors_of("Function F<T>()\n    Where T: Mat3\nEnd Function") ==
          errors_t{ "Mat3: 'Mat3' is in std.math, which this file doesn't import" });
    CHECK(errors_of("Extend Vec2\n    Method Twice(): Int => 2\nEnd Extend") ==
          errors_t{ "Vec2: 'Vec2' is in std.math, which this file doesn't import" });
}

TEST_CASE("a generic type's field default can name its type parameters", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Import std.collections\n"
                    "Struct S<T>\n"
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
    CHECK(
        errors_of("Local x = 1.WrappingDivide(2)") ==
        errors_t{ "WrappingDivide: neither Int32 nor Int64 has a member named 'WrappingDivide'" });
    CHECK(errors_of("Local a = 1\nLocal x = a.WrappingDivide(2)") ==
          errors_t{ "WrappingDivide: Int32 has no member named 'WrappingDivide'" });
    CHECK(errors_of("Local x = (1.5).CheckedAdd(1)") ==
          errors_t{ "CheckedAdd: neither Float32 nor Float64 has a member named 'CheckedAdd'" });
    CHECK(errors_of("Extend Int\n    Method CheckedAdd(x: Int): Int? => x\nEnd Extend") ==
          errors_t{ "CheckedAdd: Int32 already has a member named 'CheckedAdd'" });

    // A known argument of another type is reported, and isn't folded.
    CHECK(errors_of("Const a: Int = 5\nLocal x = a.WrappingAdd(1.5)") ==
          errors_t{ "1.5: 1.5 doesn't fit in Int32" });
    CHECK(errors_of("Const a: Int = 5\nLocal x = a.CheckedAdd(\"x\")") ==
          errors_t{ "\"x\": expected Int32, found String" });
}

TEST_CASE("an Interface's type parameters can have constraints and be Const", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Interface Sorted<T: Comparable>\n"
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
    const std::string_view parser = "Import std.core\n"
                                    "Class ParseError Extends Error\nEnd Class\n"
                                    "Interface Parser\n"
                                    "    Method Parse(text: String): Int Throws ParseError\n"
                                    "End Interface\n";
    CHECK(errors_of(std::format("{}Class Lenient Implements Parser\n"
                                "    Method Parse(text: String): Int => 0\n"
                                "End Class\n"
                                "Function Use(p: Parser): Int => Try? p.Parse(\"x\") ?? -1",
                                parser))
              .empty());
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Class Loud Implements Parser\n"
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
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Struct Picky Implements Mapper\n"
                                "    Method Apply<U: Printable>(f: Func(Int): U): U => f(1)\n"
                                "End Struct",
                                mapper)) ==
          errors_t{ "Mapper: 'Apply' needs U to implement Printable, which Mapper's doesn't, so it "
                    "can't implement it" });
    // No table can hold a generic Method that no default gives, so Mapper is only a constraint.
    CHECK(errors_of(std::format("{}Function Through(m: Mapper): Int => m.Apply(Function(x: Int) => "
                                "x)",
                                mapper)) ==
          errors_t{ "Mapper: Mapper can only be a constraint, like <T: Mapper>, since it needs "
                    "generic Methods, which no table can hold" });
}

TEST_CASE("a generic Method's constraints compare as the type implements the Interface", "[sema]") {
    // Holder<Int32>'s O: Holder<T> is O: Holder<Int32>, written or taken from the default.
    constexpr std::string_view holder = "Import std.core\n"
                                        "Interface Holder<T>\n"
                                        "    Method Get(): T\n"
                                        "    Method Both<O: Holder<T>>(other: O): T[]\n"
                                        "End Interface\n";
    CHECK(errors_of(std::format("{}Class Base Implements Holder<Int>\n"
                                "    Method Get(): Int => 4\n"
                                "    Method Both<O: Holder<Int>>(other: O): Int[] => [Get()]\n"
                                "End Class\n"
                                "Class Child Extends Base Implements Holder<Int>\n"
                                "End Class",
                                holder))
              .empty());
    CHECK(errors_of(std::format("{}Class Base Implements Holder<Int>\n"
                                "    Method Get(): Int => 4\n"
                                "    Method Both<O: Holder<String>>(other: O): Int[] => [Get()]\n"
                                "End Class",
                                holder)) ==
          errors_t{ "Holder<Int>: 'Both' needs O to implement Holder<String>, which "
                    "Holder<Int32>'s doesn't, so it can't implement it" });
}

TEST_CASE("a Method can give what implements the Iterator<T> an Interface's Method gives",
          "[sema]") {
    constexpr std::string_view iterators = "Import std.core\n"
                                           "Struct Counting Implements Iterator<Int>\n"
                                           "    ByRef Method Next(): Int? => Null\n"
                                           "End Struct\n"
                                           "Struct Words Implements Iterator<String>\n"
                                           "    ByRef Method Next(): String? => Null\n"
                                           "End Struct\n"
                                           "Struct Plain\nEnd Struct\n";
    // A Struct or Class that implements it, where the Interface's Method throws too.
    CHECK(errors_of(std::format("{}Class A Implements Iterable<Int>\n"
                                "    Method Iterate(): Counting => Counting()\n"
                                "End Class\n"
                                "Interface Lines\n"
                                "    Method Iterate(): Iterator<Int> Throws Error\n"
                                "End Interface\n"
                                "Class B Implements Lines\n"
                                "    Method Iterate(): Counting Throws Error\n"
                                "        Return Counting()\n"
                                "    End Method\n"
                                "End Class",
                                iterators))
              .empty());

    // Not one of another T, or what isn't one, or for any other Interface.
    CHECK(errors_of(std::format("{}Class A Implements Iterable<Int>\n"
                                "    Method Iterate(): Words => Words()\n"
                                "End Class",
                                iterators)) ==
          errors_t{ "Iterable<Int>: A's 'Iterate' doesn't match Iterable<Int32>'s, which takes () "
                    "and returns Iterator<Int32>" });
    CHECK(errors_of(std::format("{}Class A Implements Iterable<Int>\n"
                                "    Method Iterate(): Plain => Plain()\n"
                                "End Class",
                                iterators)) ==
          errors_t{ "Iterable<Int>: A's 'Iterate' doesn't match Iterable<Int32>'s, which takes () "
                    "and returns Iterator<Int32>" });
    CHECK(errors_of("Interface Shape\n    Method Area(): Int\nEnd Interface\n"
                    "Class Square Implements Shape\n    Method Area(): Int => 4\nEnd Class\n"
                    "Interface Maker\n    Method Make(): Shape\nEnd Interface\n"
                    "Class M Implements Maker\n    Method Make(): Square => Square()\nEnd Class") ==
          errors_t{ "Maker: M's 'Make' doesn't match Maker's, which takes () and returns Shape" });
}

TEST_CASE("a generic, conditional, or Final default runs as written, on an Interface's value too",
          "[sema]") {
    constexpr std::string_view source = "Import std.core\n"
                                        "Interface Source<T>\n"
                                        "    Method Take(): T\n"
                                        "    Method With<U>(u: U): (T, U) => (Take(), u)\n"
                                        "    Final Method Twice(): (T, T) => (Take(), Take())\n"
                                        "    Method Total(): T\n"
                                        "        Where T: Numeric\n"
                                        "        Return Take() + Take()\n"
                                        "    End Method\n"
                                        "End Interface\n"
                                        "Class Counter Implements Source<Int>\n"
                                        "    Field n = 0\n"
                                        "    Method Take(): Int\n"
                                        "        n += 1\n"
                                        "        Return n\n"
                                        "    End Method\n"
                                        "End Class\n"
                                        "Class Words Implements Source<String>\n"
                                        "    Method Take(): String => \"w\"\n"
                                        "End Class\n";
    CHECK(probe(std::format("{}Local s: Source<Int> = Counter()\nLocal p = s.With(\"a\")",
                            source)) == "(Int32, String)");
    CHECK(probe(std::format("{}Local s: Source<Int> = Counter()\nLocal p = s.Twice()", source)) ==
          "(Int32, Int32)");
    CHECK(probe(std::format("{}Local s: Source<Int> = Counter()\nLocal p = s.Total()", source)) ==
          "Int32");
    CHECK(probe(std::format("{}Local p = Counter().Total()", source)) == "Int32");
    CHECK(errors_of(std::format("{}Function F<S: Source<Int>>(s: S): Int => s.Total()", source))
              .empty());

    // A conditional default is only where its Where lines hold.
    CHECK(errors_of(std::format("{}Function F(w: Source<String>): String => w.Total()", source)) ==
          errors_t{ "w.Total: String doesn't implement Numeric, which 'Total' needs for T" });
    CHECK(errors_of(std::format("{}Local w = Words().Total()", source)) ==
          errors_t{ "Total: Words has no member named 'Total'; Source<String>'s default needs "
                    "String to implement Numeric" });
    // One on a type made of the type's own type parameters holds for the instances it holds for.
    CHECK(errors_of("Import std.core\n"
                    "Struct Wrap<T>\n"
                    "    Field Value: T\n"
                    "End Struct\n"
                    "Extend Wrap<T> Implements Comparable\n"
                    "    Where T: Comparable\n"
                    "    Operator <(a: Wrap<T>, b: Wrap<T>): Bool => a.Value < b.Value\n"
                    "End Extend\n"
                    "Class Each<T> Implements Iterator<Wrap<T>>\n"
                    "    Method Next(): Wrap<T>? => Null\n"
                    "End Class\n"
                    "Local a = Each<Int>().Min()\n"
                    "Local b = Each<Bool>().Min()") ==
          errors_t{ "Each<Bool>().Min: Wrap<Bool> doesn't implement Comparable, which 'Min' "
                    "needs" });
    // So a type that can't meet them can have a Method of its own of that name, of any type,
    // while one whose type parameters could still meet them can't.
    CHECK(probe(std::format("{}Class Joined Implements Source<String>\n"
                            "    Method Take(): String => \"w\"\n"
                            "    Method Total(): String => \"t\"\n"
                            "End Class\n"
                            "Class Counted Implements Source<String>\n"
                            "    Method Take(): String => \"w\"\n"
                            "    Method Total(): Int => 2\n"
                            "End Class\n"
                            "Local p = Joined().Total()",
                            source)) == "String");
    CHECK(errors_of(std::format("{}Class Risky Implements Source<String>\n"
                                "    Method Take(): String => \"w\"\n"
                                "    Method Total(): String Throws Error\n"
                                "        Return \"t\"\n"
                                "    End Method\n"
                                "End Class\n"
                                "Struct Changing Implements Source<String>\n"
                                "    Method Take(): String => \"w\"\n"
                                "    ByRef Method Total(): String => \"t\"\n"
                                "End Struct",
                                source))
              .empty());
    CHECK(errors_of(std::format("{}Class Box<T> Implements Source<T>\n"
                                "    Field Items: T[] = []\n"
                                "    Method Take(): T => Items[0]\n"
                                "    Method Total(): T => Items[0]\n"
                                "End Class",
                                source)) ==
          errors_t{ "Source<T>: Box<T>'s 'Total' would take the place of Source<T>'s conditional "
                    "default, which is what a call through Source<T> runs; give it another "
                    "name" });

    // What a call through the Interface runs can't be replaced or overridden.
    CHECK(errors_of(std::format("{}Class Own Implements Source<Int>\n"
                                "    Method Take(): Int => 1\n"
                                "    Method Twice(): (Int, Int) => (2, 2)\n"
                                "End Class",
                                source)) ==
          errors_t{ "Source<Int>: Own's 'Twice' would take the place of Source<Int32>'s Final "
                    "default, which is what a call through Source<Int32> runs; give it another "
                    "name" });
    CHECK(errors_of(std::format("{}Class Own Implements Source<Int>\n"
                                "    Method Take(): Int => 1\n"
                                "    Method With<U>(u: U): (Int, U) => (2, u)\n"
                                "End Class",
                                source)) ==
          errors_t{ "Source<Int>: Own's 'With' would take the place of Source<Int32>'s generic "
                    "default, which is what a call through Source<Int32> runs; give it another "
                    "name" });
    // So can't one that takes what a generic one does with some types for its own, which a call
    // on the type would pick, in the Class or one extending it.
    CHECK(errors_of(std::format("{}Class Own Implements Source<Int>\n"
                                "    Method Take(): Int => 1\n"
                                "    Method With(u: String): (Int, String) => (2, u)\n"
                                "End Class",
                                source)) ==
          errors_t{ "Source<Int>: Own's 'With' would take the place of Source<Int32>'s generic "
                    "default, which is what a call through Source<Int32> runs; give it another "
                    "name" });
    // Nor can another Interface's default the type has, whichever it lists first.
    const std::string doubled = std::format("{}Interface Doubler\n"
                                            "    Method With(u: Bool): (Int, Bool) => (2, u)\n"
                                            "End Interface\n",
                                            source);
    const std::string taken = "Both: Both gets 'With' from Doubler's default, which would take the "
                              "place of Source<Int32>'s generic default, which is what a call "
                              "through Source<Int32> runs";
    CHECK(errors_of(doubled + "Class Both Implements Doubler, Source<Int>\n"
                              "    Method Take(): Int => 1\n"
                              "End Class") == errors_t{ taken });
    CHECK(errors_of(doubled + "Class Both Implements Source<Int>, Doubler\n"
                              "    Method Take(): Int => 1\n"
                              "End Class") == errors_t{ taken });
    // Nor where a Class it extends has the generic default, or an Extend lists the other, of the
    // Class or of one instance.
    const std::string base = doubled + "Class Base Implements Source<Int>\n"
                                       "    Method Take(): Int => 1\n"
                                       "End Class\n";
    CHECK(errors_of(base + "Class Both Extends Base Implements Doubler\nEnd Class") ==
          errors_t{ taken });
    // A generic Class extending the instance its own T makes, which some of its instances meet
    // the default's Where lines for.
    CHECK(errors_of(std::format("{}Interface Totaler\n"
                                "    Method Total(): String => \"t\"\n"
                                "End Interface\n"
                                "Class Base<T> Implements Source<T>\n"
                                "    Field Items: T[] = []\n"
                                "    Method Take(): T => Items[0]\n"
                                "End Class\n"
                                "Class Sub<T> Extends Base<T> Implements Totaler\n"
                                "End Class",
                                source)) ==
          errors_t{ "Sub: Sub<T> gets 'Total' from Totaler's default, which would take the "
                    "place of Source<T>'s conditional default, which is what a call through "
                    "Source<T> runs" });
    CHECK(errors_of(base + "Class Both Extends Base\nEnd Class\n"
                           "Extend Both Implements Doubler\nEnd Extend") ==
          errors_t{ "Doubler: Both gets 'With' from Doubler's default, which would take the "
                    "place of Source<Int32>'s generic default, which is what a call through "
                    "Source<Int32> runs" });
    CHECK(errors_of(doubled + "Class Gen<T> Implements Source<T>\n"
                              "    Field Items: T[] = []\n"
                              "    Method Take(): T => Items[0]\n"
                              "End Class\n"
                              "Extend Gen<Int> Implements Doubler\nEnd Extend") ==
          errors_t{ "Doubler: Gen<Int32> gets 'With' from Doubler's default, which would take "
                    "the place of Source<Int32>'s generic default, which is what a call through "
                    "Source<Int32> runs" });
    // The other order, a Class it extends listing the other and this one Source, once.
    CHECK(errors_of(doubled + "Class Base Implements Doubler\nEnd Class\n"
                              "Class Both Extends Base Implements Source<Int>\n"
                              "    Method Take(): Int => 1\n"
                              "End Class")
              .size() == 1);
    // Nor can an Extend of the type in a module where the listing doesn't see it.
    const std::vector<fb::library_file_t> counter{
        {
            .path = "lib/counter.fb",
            .text = "Import std.core\n"
                    "Public Interface Source<T>\n"
                    "    Method Take(): T\n"
                    "    Method With<U>(u: U): (T, U) => (Take(), u)\n"
                    "End Interface\n"
                    "Public Class Counter Implements Source<Int>\n"
                    "    Public Method Take(): Int => 1\n"
                    "End Class\n",
        },
    };
    CHECK(errors_with("Import \"./lib/counter\"\n"
                      "Extend Counter\n"
                      "    Method With(u: Bool): (Int, Bool) => (42, u)\n"
                      "End Extend",
                      counter) ==
          errors_t{ "With: 'With' would take the place of Source<Int32>'s generic default, which "
                    "is what a call through Source<Int32> runs; give it another name" });
    // Not one whose types the generic default's Where lines can't take, which no call that fits
    // the default would pick.
    CHECK(errors_of("Import std.core\n"
                    "Class Counting Implements Iterator<Int>\n"
                    "    Method Next(): Int? => Null\n"
                    "    Method Chain(label: String): String => label\n"
                    "    Method Zip(other: Int): Int => other\n"
                    "End Class\n"
                    "Local c = Counting().Chain(Counting())")
              .empty());
    // Nor one a type doesn't get, since no types for its own make its Where lines hold, like
    // Flatten for an Iterator of Int32s: the type's own, an Extend's, or a Class extending it.
    constexpr std::string_view counting = "Import std.core\n"
                                          "Class Counting Implements Iterator<Int>\n"
                                          "    Method Next(): Int? => Null\n"
                                          "{}"
                                          "End Class\n";
    CHECK(errors_of(std::format(counting, "    Method Flatten(): Int => 42\n") +
                    "Local f = Counting().Flatten()")
              .empty());
    CHECK(errors_of(std::format(counting, "") +
                    "Extend Counting\n    Method Flatten(): Int => 7\nEnd Extend")
              .empty());
    CHECK(errors_of(std::format(counting, "") +
                    "Class Sub Extends Counting\n    Method Flatten(): Int => 9\nEnd Class")
              .empty());
    CHECK(errors_with("Import \"./lib/counting\"\n"
                      "Extend Counting\n    Method Flatten(): Int => 5\nEnd Extend\n"
                      "Local f = Counting().Flatten()",
                      { { .path = "lib/counting.fb",
                          .text = "Import std.core\n"
                                  "Public Class Counting Implements Iterator<Int>\n"
                                  "    Public Method Next(): Int? => Null\n"
                                  "End Class\n" } })
              .empty());
    CHECK(errors_of(std::format(counting, "") + "Local f = Counting().Flatten()") ==
          errors_t{ "Flatten: Counting has no member named 'Flatten'; Iterator<Int32>'s default "
                    "needs Int32 to implement Iterable<U>" });
    // One whose instance has it, though: its T: Iterable<U> holds for a U.
    constexpr std::string_view bag = "Import std.core\n"
                                     "Import std.collections\n"
                                     "Class Bag<T> Implements Iterator<T>\n"
                                     "    Method Next(): T? => Null\n"
                                     "End Class\n";
    CHECK(errors_of(std::string(bag) +
                    "Extend Bag<Stack<Int>>\n    Method Flatten(): Int => 99\nEnd Extend") ==
          errors_t{ "Flatten: 'Flatten' would take the place of Iterator<Stack<Int32>>'s generic "
                    "default, which is what a call through Iterator<Stack<Int32>> runs; give it "
                    "another name" });
    CHECK(errors_of(std::string(bag) +
                    "Class Rows Extends Bag<Stack<Int>>\n    Method Flatten(): Int => 99\n"
                    "End Class") ==
          errors_t{ "Flatten: 'Flatten' is Iterator<T>'s generic default, which a call through "
                    "Iterator<T> runs as written, so it can't be overridden; give this one another "
                    "name" });
    CHECK(errors_of(std::string(bag) +
                    "Extend Bag<Int>\n    Method Flatten(): Int => 99\nEnd Extend\n"
                    "Local f = Bag<Int>().Flatten()")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Class Counting Implements Iterator<Int>\n"
                    "    Method Next(): Int? => Null\n"
                    "    Method Chain(other: Counting): Int => 0\n"
                    "End Class") ==
          errors_t{ "Iterator<Int>: Counting's 'Chain' would take the place of Iterator<Int32>'s "
                    "generic default, which is what a call through Iterator<Int32> runs; give it "
                    "another name" });
    CHECK(errors_of(std::format("{}Class More Extends Counter\n"
                                "    Method With(u: Bool): (Int, Bool) => (0, u)\n"
                                "End Class\n"
                                "Class Most Extends Counter\n"
                                "    Method With<V>(v: V): (Int, V) => (0, v)\n"
                                "End Class\n"
                                "Class Other Extends Counter\n"
                                "    Method With(a: Int, b: Int): Int => a\n"
                                "End Class",
                                source)) ==
          errors_t{
              "With: 'With' is Source<T>'s generic default, which a call through Source<T> runs "
              "as written, so it can't be overridden; give this one another name",
              "With: 'With' is Source<T>'s generic default, which a call through Source<T> runs "
              "as written, so it can't be overridden; give this one another name",
          });
    CHECK(errors_of(std::format("{}Class More Extends Counter\n"
                                "    Override Method Total(): Int => 0\n"
                                "End Class",
                                source)) ==
          errors_t{ "Total: 'Total' is Source<T>'s conditional default, which a call through "
                    "Source<T> runs as written, so it can't be overridden; give this one another "
                    "name" });
    // Nor can an Extend of one instance, which goes, so a use finds the default.
    CHECK(errors_of(std::format("{}Struct Gen<T> Implements Source<T>\n"
                                "    Field Items: T[] = []\n"
                                "    Method Take(): T => Items[0]\n"
                                "End Struct\n"
                                "Extend Gen<Int>\n"
                                "    Method Twice(): (Int, Int) => (0, 0)\n"
                                "    Method With(u: String): (Int, String) => (0, u)\n"
                                "End Extend\n"
                                "Local p: (Int, Int) = Gen<Int>().Twice()",
                                source)) ==
          errors_t{
              "Twice: 'Twice' would take the place of Source<Int32>'s Final default, which is "
              "what a call through Source<Int32> runs; give it another name",
              "With: 'With' would take the place of Source<Int32>'s generic default, which is "
              "what a call through Source<Int32> runs; give it another name",
          });
    // Nor can an Interface extending it, required or with a default of its own.
    CHECK(errors_of(std::format("{}Interface Counted<T> Extends Source<T>\n"
                                "    Method Twice(): (T, T)\n"
                                "End Interface\n"
                                "Interface Paired<T> Extends Source<T>\n"
                                "    Method With<U>(u: U): (T, U) => (Take(), u)\n"
                                "End Interface",
                                source)) ==
          errors_t{
              "Twice: 'Twice' would take the place of Source<T>'s Final default, which a call "
              "through Source<T> runs as written; give it another name",
              "With: 'With' would take the place of Source<T>'s generic default, which a call "
              "through Source<T> runs as written; give it another name",
          });
    CHECK(errors_of(std::format("{}Interface Paired Extends Source<Int>\n"
                                "    Method With(u: String): (Int, String)\n"
                                "End Interface",
                                source)) ==
          errors_t{ "With: 'With' would take the place of Source<Int32>'s generic default, which a "
                    "call through Source<Int32> runs as written; give it another name" });
    CHECK(errors_of("Interface I\n    Final Method F()\nEnd Interface") ==
          errors_t{ "F: 'F' is Final, so it needs a body: a Final default is what every type that "
                    "implements the Interface has" });

    // A default runs with what each call gives it for Self, and the copies in a Class's table run
    // when one of its objects is made, so one that wraps Self in a Class implementing the
    // Interface and runs again on it would need a larger instance each time, without end.
    constexpr std::string_view shown = "Interface Shown\n"
                                       "    Method Show(): String\n"
                                       "{}"
                                       "End Interface\n"
                                       "Class Holder<S: Shown> Implements Shown\n"
                                       "    Field Inner: S\n"
                                       "    New(inner: S)\n"
                                       "        Inner = inner\n"
                                       "    End New\n"
                                       "    Method Show(): String => \"x\"\n"
                                       "End Class\n"
                                       "{}";
    const auto wrapping = [&](const std::string_view members, const std::string_view rest = "") {
        return errors_of(std::vformat(shown, std::make_format_args(members, rest)));
    };
    // In the table, it runs for each Holder it makes.
    CHECK(wrapping("    Method Boxed(): Shown => Holder<Self>(Self)\n") ==
          errors_t{ "Holder<Self>(Self): 'Boxed' gets Holder<Self> for Self here, which leads back "
                    "to this code, so it would need a larger instance each time, without end" });
    // Through a generic Function, or a Final default it calls.
    CHECK(wrapping("    Method Boxed(): Shown => Wrap(Self)\n",
                   "Function Wrap<S: Shown>(s: S): Shown => Holder<S>(s)") ==
          errors_t{ "Holder<S>(s): 'Boxed' gets Holder<S> for Self here, which leads back to this "
                    "code, so it would need a larger instance each time, without end" });
    CHECK(wrapping("    Method Boxed(): Shown => Wrapped()\n"
                   "    Final Method Wrapped(): Shown => Holder<Self>(Self)\n") ==
          errors_t{ "Holder<Self>(Self): 'Boxed' gets Holder<Self> for Self here, which leads back "
                    "to this code, so it would need a larger instance each time, without end" });
    // Out of the table, it runs only where it's called, so only calling itself on one does.
    CHECK(wrapping("    Final Method Boxed(): Shown => Holder<Self>(Self)\n").empty());
    CHECK(wrapping("    Final Method Nest(n: Int): String\n"
                   "        If n == 0 Then Return Show()\n"
                   "        Return Holder<Self>(Self).Nest(n - 1)\n"
                   "    End Method\n") ==
          errors_t{ "Holder<Self>(Self).Nest(n - 1): 'Nest' gets Holder<Self> for Self here, which "
                    "leads back to this code, so it would need a larger instance each time, "
                    "without end" });
    CHECK(wrapping("    Final Method Once(): String => Holder<Self>(Self).Show()\n").empty());
    // A copy runs with the type that has it for Self, here a Class that a Wrapper extends, and
    // a generic Method's own type arguments are its own, not its type's.
    CHECK(errors_of("Interface Shown\n"
                    "    Method Show(): String\n"
                    "    Final Method Nest(n: Int): String\n"
                    "        If n == 0 Then Return Show()\n"
                    "        Return Wrapper<Self>(Self).Nest(n - 1)\n"
                    "    End Method\n"
                    "End Interface\n"
                    "Class Base Implements Shown\n"
                    "    Method Show(): String => \"base\"\n"
                    "End Class\n"
                    "Class Wrapper<Q> Extends Base\n"
                    "    New(inner: Q)\n"
                    "    End New\n"
                    "End Class\n"
                    "Class Maker<X>\n"
                    "    Method Make<Q>(q: Q): String => \"made\"\n"
                    "    Method Go(x: X): String => Make([x])\n"
                    "End Class\n"
                    "Local a = Base().Nest(2)\n"
                    "Local b = Maker<Int>().Go(1)")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Class Each<T> Implements Iterator<T>\n"
                    "    Method Next(): T? => Null\n"
                    "    Method AndThen(other: Each<T>): Iterator<T> => Chain(other)\n"
                    "    Method Doubled(): Iterator<(T, T)> => Map(Function(x: T) => (x, x))\n"
                    "End Class\n"
                    "Local e = Each<Int>().AndThen(Each<Int>())")
              .empty());
}

TEST_CASE("For ... In Try passes on the errors of a throwing Iterate", "[sema]") {
    const std::string_view levels = "Import std.core\n"
                                    "Class LoadError Extends Error\nEnd Class\n"
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
    const std::string_view box = "Import std.core\nStruct Box<T>\n    Field Value: T\nEnd Struct\n";
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

    // Its Operators are the instance's, and implement what its Interfaces need.
    const std::string_view less =
        "Extend Box<Int> Implements Comparable\n"
        "    Operator <(a: Box<Int>, b: Box<Int>): Bool => a.Value < b.Value\n"
        "End Extend\n";
    CHECK(probe(std::format("{}{}Local x = Box(1) < Box(2)", box, less)) == "Bool");
    CHECK(probe(std::format("{}{}Local x = Box(1) >= Box(2)", box, less)) == "Bool");
    CHECK(errors_of(std::format("{}{}Local x = Box(\"a\") < Box(\"b\")", box, less)) ==
          errors_t{ "Box(\"a\") < Box(\"b\"): '<' can't be used on Box<String>" });

    // One that isn't generic beats the type's own for every instance, as a call would choose.
    CHECK(probe(std::format("Import std.core\n"
                            "Struct Box<T>\n"
                            "    Field Value: T\n"
                            "    Operator <(a: Box<T>, b: Box<T>): Bool => False\n"
                            "End Struct\n"
                            "{}Local x = Box(1) < Box(2)",
                            less)) == "Bool");

    // Its other members can't be what the generic type has, with the instance's types: a
    // Field's or Property's name, or a Method's, its own or a default's, and its parameters.
    CHECK(errors_of("Import std.core\n"
                    "Interface Named\n"
                    "    Method Hello(): String => \"named\"\n"
                    "End Interface\n"
                    "Struct Box<T> Implements Named\n"
                    "    Field Value: T\n"
                    "    Method Take(x: T): String => \"generic\"\n"
                    "    Property Size: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "End Struct\n"
                    "Extend Box<String>\n"
                    "    Method Take(x: String): String => \"string\"\n"
                    "    Method Take(x: Int): String => \"an overload\"\n"
                    "    Property Value: Int\n"
                    "        Get => 5\n"
                    "    End Property\n"
                    "    Property Size: Int\n"
                    "        Get => 2\n"
                    "    End Property\n"
                    "    Method Hello(): String => \"own\"\n"
                    "End Extend\n"
                    "Extend Box<Int>\n"
                    "    Method Take(ByRef x: Int): String => \"another\"\n"
                    "End Extend") ==
          errors_t{ "Take: 'Take' is already declared in Box<T>, so Box<String> has it",
                    "Value: 'Value' is already declared in Box<T>, so Box<String> has it",
                    "Size: 'Size' is already declared in Box<T>, so Box<String> has it",
                    "Hello: 'Hello' is already declared in Box<T>, so Box<String> has it" });
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
    CHECK(probe(std::format("Import std.core\n"
                            "{}Class Sorted<T> Extends Container<T>\n"
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

    // It has what an Extend of the instance adds, and what an Interface's defaults give it there,
    // which it can't override.
    const std::string_view sized = "Interface Sized\n"
                                   "    Method Size(): Int => 1\n"
                                   "End Interface\n"
                                   "Extend Container<String> Implements Sized\n"
                                   "    Method Joined(): String => \"\"\n"
                                   "End Extend\n";
    CHECK(probe(std::format("{}{}Class Names Extends Container<String>\nEnd Class\n"
                            "Local x = Names().Size() + Names().Joined().Chars.Count",
                            container, sized)) == "Int32");
    CHECK(probe(std::format("{}{}Class Kept<T> Extends Container<T>\nEnd Class\n"
                            "Local x: Sized = Kept<String>()",
                            container, sized)) == "Sized (from Kept<String>)");
    CHECK(errors_of(std::format("{}{}Class Names Extends Container<String>\n"
                                "    Method Size(): Int => 2\n"
                                "End Class",
                                container, sized)) ==
          errors_t{ "Size: 'Size' was added to Container<String> by an Extend, so it can't be "
                    "overridden" });

    // Super.New sets up the instance, and a field it inherits is assigned by name with the
    // instance's types.
    const std::string_view box = "Class Box<T>\n"
                                 "    Field Open: T[] = []\n"
                                 "    Field One: T? = Null\n"
                                 "    New(first: T)\n"
                                 "        Open.Add(first)\n"
                                 "    End New\n"
                                 "End Class\n";
    CHECK(errors_of(std::format("{}Class IntBox Extends Box<Int>\n"
                                "    New()\n"
                                "        Super.New(41)\n"
                                "        Open = [2, 3]\n"
                                "        One = 5\n"
                                "    End New\n"
                                "End Class\n"
                                "Local b = IntBox()",
                                box))
              .empty());
    CHECK(errors_of(std::format("{}Class Other<T> Extends Box<T>\n"
                                "    New(x: T)\n"
                                "        Super.New(x)\n"
                                "        One = x\n"
                                "    End New\n"
                                "End Class\n"
                                "Local o = Other<String>(\"a\")",
                                box))
              .empty());
    CHECK(errors_of(std::format("{}Class IntBox Extends Box<Int>\n"
                                "    New()\n"
                                "        Super.New(\"a\")\n"
                                "        Open = [\"b\"]\n"
                                "    End New\n"
                                "End Class",
                                box)) ==
          errors_t{ "\"a\": expected Int32, found String", "\"b\": expected Int32, found String" });
    CHECK(errors_of("Import std.collections\n"
                    "Class Named Extends Queue<String>\n"
                    "    Field Label: String\n"
                    "    New(label: String)\n"
                    "        Super.New()\n"
                    "        Self.Label = label\n"
                    "    End New\n"
                    "End Class\n"
                    "Local n = Named(\"q\")")
              .empty());
}

TEST_CASE("what a generic type has implements what an Extend of one instance lists", "[sema]") {
    // Its own Operators, Methods, and Properties, with the instance's types.
    CHECK(probe("Import std.core\n"
                "Struct Box<T>\n"
                "    Field V: Int\n"
                "    Operator <(a: Box<T>, b: Box<T>): Bool => a.V < b.V\n"
                "    Operator >(a: Box<T>, b: Box<T>): Bool => True\n"
                "End Struct\n"
                "Extend Box<Int> Implements Comparable\n"
                "End Extend\n"
                "Function More<T: Comparable>(a: T, b: T): Bool => a > b\n"
                "Local x = More(Box<Int>(1), Box<Int>(2))") == "Bool");
    CHECK(errors_of("Interface Holder<U>\n"
                    "    Method Put(x: U): Int\n"
                    "End Interface\n"
                    "Interface Alike\n"
                    "    Method Same(other: Self): Bool\n"
                    "    ReadOnly Property Size: Int\n"
                    "End Interface\n"
                    "Struct Box<T>\n"
                    "    Field V: T\n"
                    "    Method Put(x: T): Int => 1\n"
                    "    Method Same(other: Box<T>): Bool => True\n"
                    "    Property Size: T\n"
                    "        Get => V\n"
                    "    End Property\n"
                    "End Struct\n"
                    "Extend Box<Int> Implements Holder<Int>, Alike\n"
                    "End Extend")
              .empty());

    // An instance of a Class with a base keeps what the Extend lists, and so do the Classes
    // extending it.
    const std::string_view named = "Interface Named\n"
                                   "    Method Name(): String\n"
                                   "End Interface\n"
                                   "Class A<T>\n"
                                   "    Field Items: T[] = []\n"
                                   "End Class\n"
                                   "Class B<T> Extends A<T>\n"
                                   "End Class\n"
                                   "Extend B<String> Implements Named\n"
                                   "    Method Name(): String => \"named\"\n"
                                   "End Extend\n";
    CHECK(probe(std::format("{}Local m: Named = B<String>()", named)) == "Named (from B<String>)");
    CHECK(probe(std::format("{}Class C Extends B<String>\nEnd Class\nLocal m: Named = C()",
                            named)) == "Named (from C)");
    CHECK(errors_of(std::format("{}Function Show<T: Named>(x: T): String => x.Name()\n"
                                "Local s = Show(B<String>())",
                                named))
              .empty());
    CHECK(errors_of(std::format("{}Local m: Named = B<Int>()", named)) ==
          errors_t{ "B<Int>(): expected Named, found B<Int32>" });
}

TEST_CASE("an Operator an Extend of one instance adds beats the generic type's", "[sema]") {
    // Through a Class extending the instance too, where both convert the same way.
    const std::string_view box = "Class Box<T>\n"
                                 "    Field V: Int = 0\n"
                                 "    Operator <(a: Box<T>, b: Box<T>): Bool => a.V < b.V\n"
                                 "End Class\n"
                                 "Extend Box<String>\n"
                                 "    Operator <(a: Box<String>, b: Box<String>): Bool => True\n"
                                 "End Extend\n"
                                 "Class Names Extends Box<String>\n"
                                 "End Class\n";
    CHECK(probe(std::format("{}Local x = Names() < Names()", box)) == "Bool");

    // A default an Interface gives the instance beats nothing, but it's taken only where what the
    // generic type has doesn't implement the Interface's member.
    CHECK(errors_of("Import std.core\n"
                    "Struct Box<T>\n"
                    "    Field V: T\n"
                    "    Operator >(a: Box<T>, b: Box<T>): Bool => True\n"
                    "End Struct\n"
                    "Extend Box<Int> Implements Comparable\n"
                    "    Operator <(a: Box<Int>, b: Box<Int>): Bool => a.V < b.V\n"
                    "End Extend\n"
                    "Local x = Box(1) > Box(2)")
              .empty());
}

TEST_CASE("an Alias is the type it names, everywhere", "[sema]") {
    // A type, which converts to and from what it names, and its News and Statics.
    const std::string_view q = "Struct Q<Const A: Int>\n"
                               "    Field V: Double\n"
                               "    Static Method Zero(): Q<A> => Q<A>(0)\n"
                               "End Struct\n"
                               "Alias Meters = Q<1>\n";
    CHECK(probe(std::format("{}Local x: Meters = Q<1>(2)", q)) == "Q<1>");
    CHECK(probe(std::format("{}Local x = Meters(2)", q)) == "Q<1>");
    CHECK(probe(std::format("{}Local x = Meters.Zero()", q)) == "Q<1>");
    CHECK(probe("Alias Score = Int\nAlias Scores = Score[]\nLocal x: Scores = [1, 2]") ==
          "Int32[]");
    // One declared later, or naming another declared later, is the same.
    CHECK(errors_of("Local x: Later = 1.5\nAlias Later = Sooner\nAlias Sooner = Double").empty());
    CHECK(probe("Alias Later = Sooner\nAlias Sooner = Double\nLocal x: Later = 1.5") ==
          "Float64 1.5");
    CHECK(errors_of(std::format("{}Local x: Meters = Q<2>(2)", q)) ==
          errors_t{ "Q<2>(2): expected Q<1>, found Q<2>" });

    // One that names itself, through others or not, once each.
    CHECK(errors_of("Alias A = B[]\nAlias B = A\nAlias C = C?") ==
          errors_t{ "A: 'A' names itself through 'B', so it's no type",
                    "C: 'C' names itself, so it's no type" });
    CHECK(errors_of("Alias D = Int\nLocal x: D<Int> = 1") ==
          errors_t{ "D<Int>: 'D' is an Alias, so it takes no type arguments" });
    CHECK(errors_of("Alias D = Int\nLocal y = D") == errors_t{ "D: 'D' is a type, not a value" });
    CHECK(errors_of("Alias D = Int[]\nLocal y = D()") ==
          errors_t{ "D: 'D' names Int32[], which isn't made with a call" });

    // An Enum's values through it, an Extend of it, and a Class that extends it.
    CHECK(probe("Enum Color\n    Red\n    Green\nEnd Enum\nAlias Hue = Color\n"
                "Local x: Hue = Hue.Green") == "Color 1");
    CHECK(probe("Struct Box<T>\n    Field Item: T\nEnd Struct\nAlias IntBox = Box<Int>\n"
                "Extend IntBox\n    Method Twice(): Int => Item * 2\nEnd Extend\n"
                "Local x = Box<Int>(4).Twice()") == "Int32");
    CHECK(probe("Import std.math\n"
                "Extend Meters\n    Method Doubled(): Meters => Meters(Value * 2)\nEnd Extend\n"
                "Local x = Quantity<1, 0, 0>(1).Doubled()") == "Quantity<1, 0, 0>");
    CHECK(errors_of("Alias Ints = Int[]\nExtend Ints\nEnd Extend") ==
          errors_t{ "Ints: Extend adds to a Struct, Class, Enum, built-in type, or T[], not "
                    "Int32[]" });
    CHECK(
        errors_of("Interface Shape\nEnd Interface\nAlias Form = Shape\nExtend Form\nEnd Extend") ==
        errors_t{ "Form: an Interface can't be extended; its defaults go in the Interface" });
    CHECK(errors_of("Class Animal\n    Method Sound(): String => \"...\"\nEnd Class\n"
                    "Alias Base = Animal\n"
                    "Class Dog Extends Base\n"
                    "    Override Method Sound(): String => \"woof\"\n"
                    "End Class\n"
                    "Local a: Animal = Dog()\n"
                    "Class Holder<T>\nEnd Class\nAlias Ints = Holder<Int>\n"
                    "Class Named Extends Ints\nEnd Class\n"
                    "Local h: Holder<Int> = Named()")
              .empty());
    CHECK(
        errors_of("Struct Point\nEnd Struct\nAlias P = Point\nClass C Extends P\nEnd Class\n"
                  "Interface Shape\nEnd Interface\nAlias S = Shape\nClass D Extends S\nEnd Class\n"
                  "Final Class F\nEnd Class\nAlias G = F\nClass H Extends G\nEnd Class") ==
        errors_t{ "P: 'P' isn't a Class", "S: 'S' is an Interface, so it goes after Implements",
                  "G: F is Final, so it can't be extended" });
    CHECK(errors_of("Function F()\n    Alias E = Int\nEnd Function") ==
          errors_t{ "E: Aliases can only be declared at the top level" });
    CHECK(errors_of("Alias Int = Double") == errors_t{ "Int: 'Int' is a built-in type" });

    // It takes the usual visibility, and a Public one can't name what other modules can't see.
    const std::vector<fb::library_file_t> names{
        {
            .path = "lib/names.fb",
            .text = "Struct Secret\nEnd Struct\n"
                    "Public Struct Shown\nEnd Struct\n"
                    "Public Alias Seen = Shown\n"
                    "Alias Hidden = Shown\n",
        },
    };
    CHECK(errors_with(
              "Import \"./lib/names\"\nLocal a: Seen = Seen()\nLocal c: names.Seen = Seen()", names)
              .empty());
    CHECK(errors_with("Import \"./lib/names\"\nLocal b: Hidden = Shown()", names) ==
          errors_t{ "Hidden: 'Hidden' is private to lib/names.fb" });
    const std::vector<fb::library_file_t> open{
        {
            .path = "lib/open.fb",
            .text = "Struct Secret\nEnd Struct\nPublic Alias Open = Secret\n",
        },
    };
    CHECK(errors_with("Import \"./lib/open\"", open) ==
          errors_t{ "lib/open.fb: Open: 'Open' is Public, but it names Secret, which is private to "
                    "lib/open.fb" });
}

TEST_CASE("std.math's units are the Quantities their exponents make", "[sema]") {
    // Meters / Seconds is MetersPerSecond, and Meters + Seconds is no Quantity at all.
    CHECK(probe("Import std.math\nLocal v: MetersPerSecond = Meters(100) / Seconds(10)") ==
          "Quantity<1, 0, -1>");
    CHECK(errors_of("Import std.math\nLocal x = Meters(1) + Seconds(1)") ==
          errors_t{ "Meters(1) + Seconds(1): no Operator + takes (Quantity<1, 0, 0>, "
                    "Quantity<0, 0, 1>)" });
    CHECK(errors_of("Import std.math\nLocal v: MetersPerSecond = Meters(1) * Seconds(1)") ==
          errors_t{ "Meters(1) * Seconds(1): expected Quantity<1, 0, -1>, found "
                    "Quantity<1, 0, 1>" });
    CHECK(errors_of("Import std.math\nLocal x = Meters(1) < Kilograms(1)") ==
          errors_t{ "Meters(1) < Kilograms(1): no Operator < takes (Quantity<1, 0, 0>, "
                    "Quantity<0, 1, 0>)" });

    // Generic code over units, whose exponents combine as each instance's do: L - 0 is L, and
    // L + L is L * 2.
    CHECK(errors_of("Import std.math\n"
                    "Function PerSecond<Const L: Int>(a: Quantity<L, 0, 0>, t: Seconds): "
                    "Quantity<L, 0, -1> => a / t\n"
                    "Function Squared<Const L: Int>(a: Quantity<L, 0, 0>): "
                    "Quantity<L * 2, 0, 0> => a * a\n"
                    "Function Rate<Const L: Int, Const M: Int, Const T: Int>(a: Quantity<L, M, T>, "
                    "t: Seconds): Quantity<L, M, T - 1> => a / t")
              .empty());
    CHECK(errors_of("Import std.math\n"
                    "Function Wrong<Const L: Int>(a: Quantity<L, 0, 0>): Quantity<L * 3, 0, 0> => "
                    "a * a") ==
          errors_t{ "a * a: expected Quantity<L * 3, 0, 0>, found Quantity<L * 2, 0, 0>" });
}

TEST_CASE("an Operator takes type parameters of its own from its operands", "[sema]") {
    // Const ones and types, as a Function call infers them, with their constraints.
    const std::string_view q = "Struct Q<Const A: Int>\n"
                               "    Field V: Double\n"
                               "    Operator *<Const B: Int>(x: Q<A>, y: Q<B>): Q<A + B> => "
                               "Q<A + B>(x.V * y.V)\n"
                               "    Operator /<Const B: Int>(x: Q<A>, y: Q<B>): Q<A - B> => "
                               "Q<A - B>(x.V / y.V)\n"
                               "    Operator +(x: Q<A>, y: Q<A>): Q<A> => Q<A>(x.V + y.V)\n"
                               "End Struct\n";
    CHECK(probe(std::format("{}Local x = Q<1>(2) * Q<-3>(4)", q)) == "Q<-2>");
    CHECK(probe(std::format("{}Local x = Q<1>(2) / Q<-3>(4) * Q<1>(1)", q)) == "Q<5>");
    const std::string_view box = "Import std.core\n"
                                 "Struct Box<T>\n"
                                 "    Field Item: T\n"
                                 "    Operator &<U: Comparable>(a: Box<T>, b: Box<U>): (T, U) => "
                                 "(a.Item, b.Item)\n"
                                 "End Struct\n"
                                 "Class Plain\nEnd Class\n";
    CHECK(probe(std::format("{}Local x = Box(1) & Box(\"a\")", box)) == "(Int32, String)");

    // One whose operands don't fit says which they are.
    CHECK(errors_of(std::format("{}Local x = Q<1>(2) + Q<2>(3)", q)) ==
          errors_t{ "Q<1>(2) + Q<2>(3): no Operator + takes (Q<1>, Q<2>)" });
    CHECK(errors_of(std::format("{}Local x = Box(1) & Box(Plain())", box)) ==
          errors_t{ "Box(1) & Box(Plain()): no Operator & takes (Box<Int32>, Box<Plain>)" });

    // Only the operands give them, so each is in one of their types.
    CHECK(errors_of("Struct Q<Const A: Int>\n"
                    "    Operator *<Const B: Int>(x: Q<A>, y: Q<A>): Q<B> => Q<B>()\n"
                    "End Struct") ==
          errors_t{ "*: Operator * takes B from its operands' types, so one of them needs it" });
    // Every == on a type's values uses its Operator ==, so that compares two of them.
    CHECK(errors_of("Struct Box<T>\n"
                    "    Field Item: Int\n"
                    "    Operator ==<U>(a: Box<T>, b: Box<U>): Bool => a.Item == b.Item\n"
                    "End Struct") ==
          errors_t{ "==: an Operator == can't have type parameters of its own, since every == on "
                    "its type's values uses it" });

    // One a Class inherits from an instance of a generic one takes that instance's type
    // arguments.
    const std::string_view base = "Class Base<T>\n"
                                  "    Operator &<U>(a: Base<T>, b: Base<U>): (T[], U[]) => "
                                  "([], [])\n"
                                  "    Operator +(a: Base<T>, b: Base<T>): T[] => []\n"
                                  "End Class\n"
                                  "Class Derived Extends Base<Int>\nEnd Class\n";
    CHECK(probe(std::format("{}Local x = Derived() & Base<String>()", base)) ==
          "(Int32[], String[])");
    CHECK(probe(std::format("{}Local x = Derived() + Derived()", base)) == "Int32[]");

    // An Extend's, found as the type's own are, and one that isn't generic still beats one that
    // is where they take the same types.
    CHECK(probe(std::format("{}Extend Q<A>\n"
                            "    Operator %<Const B: Int>(x: Q<A>, y: Q<B>): Q<A % B> => "
                            "Q<A % B>(x.V)\n"
                            "End Extend\n"
                            "Local x = Q<7>(1) % Q<4>(1)",
                            q)) == "Q<3>");
    CHECK(probe("Struct R<Const A: Int>\n"
                "    Field V: Int\n"
                "    Operator -<Const B: Int>(x: R<A>, y: R<B>): R<A - B> => R<A - B>(0)\n"
                "End Struct\n"
                "Extend R<2>\n"
                "    Operator -(x: R<2>, y: R<2>): Int => 9\n"
                "End Extend\n"
                "Local x = R<2>(1) - R<2>(1)") == "Int32");

    // Making a Struct whose New runs no code is no step to a larger instance, but calling a
    // Method of one still is.
    CHECK(errors_of("Struct S<Const A: Int>\n"
                    "    Method Next(): Int => S<A + 1>().Next()\n"
                    "End Struct\n"
                    "Local x = S<1>().Next()") ==
          errors_t{ "S<A + 1>().Next(): S<A> gets A + 1 for A here, which leads back to this code, "
                    "so it would need a different instance each time, without end" });
    CHECK(errors_of("Function Down<Const N: Int>(): Int\n"
                    "    If N == 0 Then Return 0\n"
                    "    Return Down<N - 1>()\n"
                    "End Function") ==
          errors_t{ "Down<N - 1>(): 'Down' gets N - 1 for N here, which leads back to this code, "
                    "so it would need a different instance each time, without end" });
    // One computed with / or % can come back to an instance it made, so only lowering's chain
    // of them stops one that doesn't.
    CHECK(errors_of("Function Bits<Const N: Int>(): Int\n"
                    "    If N == 0 Then Return 0\n"
                    "    Return 1 + Bits<N / 2>()\n"
                    "End Function\n"
                    "Function Cycle<Const N: Int>(n: Int): Int\n"
                    "    If n == 0 Then Return N\n"
                    "    Return Cycle<(N + 1) % 3>(n - 1)\n"
                    "End Function")
              .empty());
}

TEST_CASE("what an Extend of one instance adds can't override what the instance inherits",
          "[sema]") {
    const auto in_extend = [](const std::string_view base, const std::string_view member) {
        return errors_of(std::format("Class Base\n    {}\nEnd Class\n"
                                     "Class Box<T> Extends Base\nEnd Class\n"
                                     "Extend Box<Int>\n    {}\nEnd Extend",
                                     base, member));
    };

    CHECK(in_extend("Method Size(): Int => 1", "Method Size(): Int => 2") ==
          errors_t{ "Size: 'Size' is already a member of Base, which an Extend can't override" });
    CHECK(in_extend("Field Size: Int = 1", "Method Size(): Int => 2") ==
          errors_t{ "Size: 'Size' is already declared in Base" });
    CHECK(in_extend("Property Size: Int\n        Get => 1\n    End Property",
                    "Property Size: Int\n        Get => 2\n    End Property") ==
          errors_t{ "Size: 'Size' is already a member of Base, which an Extend can't override" });
    CHECK(in_extend("Method Size(): Int => 1", "Method Size(extra: Int): Int => 2").empty());
    CHECK(
        errors_of("Class A<T>\n    Method Size(): Int => 1\nEnd Class\n"
                  "Class B<T> Extends A<T>\nEnd Class\n"
                  "Extend B<Int>\n    Method Size(): Int => 2\nEnd Extend") ==
        errors_t{ "Size: 'Size' is already a member of A<Int32>, which an Extend can't override" });
    CHECK(errors_of("Class A<T>\n    Method Size(): Int => 1\nEnd Class\n"
                    "Class B Extends A<String>\n    Method Size(): Int => 2\nEnd Class") ==
          errors_t{ "Size: 'Size' overrides A<String>'s; write 'Override Method Size'" });
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
    CHECK(probe("Import std.core\n"
                "Const Function Largest<T: Comparable>(a: T, b: T): T => a > b ? a : b\n"
                "Const X = Largest(3, 9)") == "Int32 9");
    CHECK(probe("Import std.core\n"
                "Const Function Square<T: Numeric>(x: T): T => x * x\n"
                "Const X = Square(1.5)") == "Float32 2.25");
    CHECK(probe("Struct Board\n"
                "    Field Cells: Int[Cells()]\n"
                "End Struct\n"
                "Const Function Cells(): Int => 4 * 4\n"
                "Local b = Board()\n"
                "Local x = b.Cells") == "Int32[16]");

    // An Enum declared later has its values there too, used directly, through a Const, or in a
    // Const Function.
    constexpr std::string_view grid =
        "Struct Grid\n"
        "    Field cells: Int[Extra.BONUS As Int]\n"
        "    Field more: Int[SIZE]\n"
        "    Field most: Int[Size(4)]\n"
        "End Struct\n"
        "Const SIZE = (Extra.BONUS As Int) * 3\n"
        "Const Function Size(n: Int): Int => n + (Extra.BONUS As Int)\n"
        "Enum Extra\n    BONUS = 2\nEnd Enum\n"
        "Local g = Grid()\n";
    CHECK(probe(std::format("{}Local x = g.cells", grid)) == "Int32[2]");
    CHECK(probe(std::format("{}Local x = g.more", grid)) == "Int32[6]");
    CHECK(probe(std::format("{}Local x = g.most", grid)) == "Int32[6]");
    // So does one an integer converts to, there or in a Const Function's run.
    constexpr std::string_view converted =
        "Struct Grid\n"
        "    Field cells: Int[((2 As Extra) As Int) + 1]\n"
        "    Field more: Int[Pick(1)]\n"
        "End Struct\n"
        "Const Function Pick(n: Int): Int => ((n As Extra) As Int) + 1\n"
        "Enum Extra\n    A, B, C\nEnd Enum\n"
        "Local g = Grid()\n";
    CHECK(probe(std::format("{}Local x = g.cells", converted)) == "Int32[3]");
    CHECK(probe(std::format("{}Local x = g.more", converted)) == "Int32[2]");

    // One whose type argument doesn't meet its constraint doesn't run, inferred or written.
    CHECK(errors_of("Import std.core\n"
                    "Const Function Largest<T: Comparable>(a: T, b: T): T => a < b ? b : a\n"
                    "Const Function Twice<T: Numeric>(a: T): T => a + a\n"
                    "Function Main()\n"
                    "    Local x = Largest(True, False)\n"
                    "    Local y = Largest<Bool>(True, False)\n"
                    "    Local z = Twice<Bool>(True)\n"
                    "End Function") ==
          errors_t{
              "Largest: Bool doesn't implement Comparable, which 'Largest' needs for T",
              "Largest<Bool>: Bool doesn't implement Comparable, which 'Largest' needs for T",
              "Twice<Bool>: Bool doesn't implement Numeric, which 'Twice' needs for T",
          });
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
    // -, /, and % too, rounding toward 0 as an Int's do, and -N, which is 0 - N.
    const std::string_view q = "Struct Q<Const A: Int>\nEnd Struct\n";
    CHECK(probe(std::format("{}Function Over<Const A: Int, Const B: Int>(x: Q<A>, y: Q<B>): "
                            "Q<A - B> => Q<A - B>()\nLocal x = Over(Q<1>(), Q<3>())",
                            q)) == "Q<-2>");
    CHECK(probe(std::format("{}Function Flip<Const A: Int>(x: Q<A>): Q<-A> => Q<-A>()\n"
                            "Local x = Flip(Q<5>())",
                            q)) == "Q<-5>");
    CHECK(probe("Function Halves<Const N: Int>(a: Int[N]): Int[N / 2, N % 3 + 1, N - 1]\n"
                "    Local halves: Int[N / 2, N % 3 + 1, N - 1]\n"
                "    Return halves\n"
                "End Function\n"
                "Local a: Int[7]\n"
                "Local x = Halves(a)") == "Int32[3, 2, 6]");
    CHECK(probe(std::format("{}Function Mixed<Const A: Int>(x: Q<A>): Q<-7 / A + -7 % A> => "
                            "Q<-7 / A + -7 % A>()\nLocal x = Mixed(Q<2>())",
                            q)) == "Q<-4>");
    CHECK(probe(std::format("{}Function Paren<Const A: Int>(x: Q<A>): Q<(A - 3) * 2> => "
                            "Q<(A - 3) * 2>()\nLocal x = Paren(Q<2>())",
                            q)) == "Q<-2>");
    CHECK(errors_of("Function F<Const N: Int>(a: Int[N ** 2])\nEnd Function") ==
          errors_t{ "N ** 2: a value computed from Const type parameters uses integers known at "
                    "compile time, with +, -, *, /, and %, like N * 2" });
    // Its known parts are worked out in an Int64, so only what it comes to has to fit, and each
    // is reported once.
    CHECK(errors_of("Struct Q8<Const A: Int8>\nEnd Struct\n"
                    "Function F<Const N: Int>(): Q8<N - 200> => Q8<N - 200>()\n"
                    "Local q = F<100>()")
              .empty());
    CHECK(errors_of("Struct Q8<Const A: Int8>\nEnd Struct\n"
                    "Function F<Const N: Int>(): Q8<N - 1.5> => Q8<N - 1.5>()") ==
          errors_t{ "1.5: a value computed from Const type parameters uses integers known at "
                    "compile time, with +, -, *, /, and %, like N * 2",
                    "1.5: a value computed from Const type parameters uses integers known at "
                    "compile time, with +, -, *, /, and %, like N * 2" });
    CHECK(errors_of("Struct Q8<Const A: Int8>\nEnd Struct\n"
                    "Function F<Const N: Int>(): Q8<N - Missing> => Q8<N - 1>()") ==
          errors_t{ "Missing: 'Missing' isn't declared" });

    // A value is one type whatever its parameter's type, and a size computed as an argument is
    // computed is that type too.
    CHECK(errors_of("Struct Box8<Const M: Int8>\nEnd Struct\n"
                    "Function Make<Const N: Int>(): Box8<N> => Box8<N>()\n"
                    "Local a: Box8<5> = Make<5>()")
              .empty());
    CHECK(errors_of("Struct Box<Const M: Int>\n    Field Items: Int[M]\nEnd Struct\n"
                    "Function F<Const N: Int>(): Int\n"
                    "    Local b = Box<N - 1>()\n"
                    "    Local t: Int[N - 1] = b.Items\n"
                    "    Return t.Count\n"
                    "End Function")
              .empty());
    // Generic code's array, whose size is a Const type parameter or computed from one, infers
    // that.
    CHECK(probe("Function Count<Const N: Int>(a: Int[N]): Int[N] => a\n"
                "Function Trimmed<Const N: Int>(a: Int[N]): Int[N - 1]\n"
                "    Local t: Int[N - 1]\n"
                "    Return t\n"
                "End Function\n"
                "Function Wrap<Const N: Int>(a: Int[N]): Int[N - 1]\n"
                "    Local same: Int[N] = Count(a)\n"
                "    Return Count(Trimmed(a))\n"
                "End Function\n"
                "Local a: Int[5]\n"
                "Local x = Wrap(a)") == "Int32[4]");
}

TEST_CASE("a loop can go over a type's own type parameter, or Self", "[sema]") {
    const std::string_view wrapper = "Import std.core\n"
                                     "Struct Wrapper<T>\n"
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

    const std::string_view dumped = "Import std.core\n"
                                    "Interface Dumped\n"
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
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}For Property p: P In Player\n"
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

TEST_CASE("Sendable is what the compiler says can go to another thread", "[sema]") {
    const std::string_view send = "Import std.core\n"
                                  "Function Send<T: Sendable>(value: T): T => value\n"
                                  "Class Enemy\n    Field HP: Int\nEnd Class\n"
                                  "Enum State\n    IDLE\n    RUNNING\nEnd Enum\n"
                                  "Struct Point\n    Field X: Int\n    Field Y: Float\nEnd Struct\n"
                                  "Struct Holder\n    Field Target: Enemy?\nEnd Struct\n"
                                  "Struct Outer\n    Field Inner: Holder\nEnd Struct\n";
    for (const std::string_view value : {
             "1",
             "2.5",
             "True",
             "\"text\"",
             "State.IDLE",
             "Point(1, 2.0)",
             "(1, \"a\")",
             "Point(1, 2.0) As Point?",
         }) {
        CHECK(errors_of(std::format("{}Local x = Send({})", send, value)).empty());
    }

    CHECK(errors_of(std::format("{}Local p: Point[2]\nLocal x = Send(p)", send)).empty());
    CHECK(errors_of(std::format("{}Local x = Send(Enemy())", send)) ==
          errors_t{ "Send: Enemy isn't Sendable, which 'Send' needs for T: a Class's objects stay "
                    "on the thread that made them, unless it's a Shared Class" });
    CHECK(errors_of(std::format("{}Local x = Send([1, 2])", send)) ==
          errors_t{ "Send: Int32[] isn't Sendable, which 'Send' needs for T: an array "
                    "stays on the thread that made it" });
    CHECK(errors_of(std::format("{}Local x = Send(Function() => 1)", send)) ==
          errors_t{ "Send: Func(): Int32 isn't Sendable, which 'Send' needs for "
                    "T: a function value stays on the thread that made it, unless it's a "
                    "Sendable Func" });
    CHECK(errors_of(std::format("{}Local x = Send(Holder(Null))", send)) ==
          errors_t{ "Send: Holder isn't Sendable, which 'Send' needs for T: its field 'Target' is "
                    "Enemy?, and a Class's objects stay on the thread that made them, unless it's "
                    "a Shared Class" });
    CHECK(errors_of(std::format("{}Local x = Send(Outer(Holder(Null)))", send)) ==
          errors_t{ "Send: Outer isn't Sendable, which 'Send' needs for T: its field 'Inner' is "
                    "Holder, whose field 'Target' is Enemy?, and a Class's objects stay on the "
                    "thread that made them, unless it's a Shared Class" });
    CHECK(errors_of(std::format("{}Local x = Send((1, Enemy()))", send)) ==
          errors_t{ "Send: (Int32, Enemy) isn't Sendable, which 'Send' needs for T: its element "
                    "'1' is Enemy, and a Class's objects stay on the thread that made them, unless "
                    "it's a Shared Class" });

    // A type parameter is Sendable when its constraints say so.
    CHECK(errors_of(std::format("{}Function Pass<U>(value: U): U => Send(value)", send)) ==
          errors_t{ "Send: U isn't Sendable, which 'Send' needs for T: nothing says U is "
                    "Sendable" });
    CHECK(errors_of(std::format("{}Function Pass<U: Sendable>(value: U): U => Send(value)", send))
              .empty());

    // A generic Struct's instance is Sendable when its fields are.
    const std::string_view box = "Struct Box<T>\n    Field Item: T\nEnd Struct\n";
    CHECK(errors_of(std::format("{}{}Local x = Send(Box(1))", send, box)).empty());
    CHECK(errors_of(std::format("{}{}Local x = Send(Box(Enemy()))", send, box)) ==
          errors_t{ "Send: Box<Enemy> isn't Sendable, which 'Send' needs for T: its field 'Item' "
                    "is Enemy, and a Class's objects stay on the thread that made them, unless "
                    "it's a Shared Class" });

    // It's only a constraint.
    CHECK(errors_of("Import std.core\n"
                    "Local x: Sendable = 1") ==
          errors_t{ "Sendable: Sendable can only be a constraint, like <T: Sendable>, since it "
                    "says what can go to another thread" });
}

TEST_CASE("a type that lists Sendable, or an Interface that extends it, is checked", "[sema]") {
    const std::string_view enemy = "Class Enemy\n    Field HP: Int\nEnd Class\n";
    CHECK(errors_of("Import std.core\n"
                    "Struct Packet Implements Sendable\n    Field Id: Int\nEnd Struct")
              .empty());
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Struct Packet Implements Sendable\n"
                                "    Field Id: Int\n"
                                "    Field Sender: Enemy\n"
                                "End Struct",
                                enemy)) ==
          errors_t{ "Sendable: Packet isn't Sendable: its field 'Sender' is Enemy, and a Class's "
                    "objects stay on the thread that made them, unless it's a Shared Class" });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Class Boss Extends Enemy Implements Sendable\nEnd Class",
                                enemy)) ==
          errors_t{ "Sendable: Boss isn't Sendable: a Class's objects stay on the thread that made "
                    "them, unless it's a Shared Class" });

    // A generic Struct says its type parameters are Sendable.
    CHECK(errors_of("Import std.core\n"
                    "Struct Box<T> Implements Sendable\n    Field Item: T\nEnd Struct") ==
          errors_t{ "Sendable: Box<T> isn't Sendable: its field 'Item' is T, and nothing says T "
                    "is Sendable" });
    CHECK(errors_of("Import std.core\n"
                    "Struct Box<T: Sendable> Implements Sendable\n    Field Item: T\nEnd Struct")
              .empty());

    // An Interface that extends it can be a constraint that implies it.
    const std::string_view message = "Import std.core\n"
                                     "Interface Message Extends Sendable\n"
                                     "    Method Kind(): Int\n"
                                     "End Interface\n"
                                     "Function Send<T: Sendable>(value: T): T => value\n"
                                     "Function Post<M: Message>(value: M): M => Send(value)\n";
    CHECK(errors_of(std::format("{}Struct Ping Implements Message\n"
                                "    Field Id: Int\n"
                                "    Method Kind(): Int => 1\n"
                                "End Struct\n"
                                "Post(Ping(1))",
                                message))
              .empty());
    CHECK(errors_of(std::format("{}{}Struct Ping Implements Message\n"
                                "    Field From: Enemy\n"
                                "    Method Kind(): Int => 1\n"
                                "End Struct",
                                message, enemy)) ==
          errors_t{ "Message: Ping can't implement Message, which extends Sendable: its field "
                    "'From' is Enemy, and a Class's objects stay on the thread that made them, "
                    "unless it's a Shared Class" });
    CHECK(errors_of(std::format("{}Extend Int[] Implements Message\n"
                                "    Method Kind(): Int => 2\n"
                                "End Extend",
                                message)) ==
          errors_t{ "Message: Int32[] can't implement Message, which extends Sendable: an array "
                    "stays on the thread that made it" });
}

TEST_CASE("an Atomic and a Mutex stay where they're made", "[sema]") {
    const std::string_view shared = "Import std.core\n"
                                    "Global frames = Atomic<Int>(0)\n"
                                    "Global ready: Atomic<Bool>\n"
                                    "Global score = Mutex<Int>(0)\n"
                                    "Class Enemy\n    Field HP: Int\nEnd Class\n";
    CHECK(errors_of(std::format("{}Local f = frames.Add(1)\nLocal r: Bool = ready.Load()\n"
                                "Local swapped = frames.CompareExchange(1, 2)\n"
                                "Lock score As s\n    s += f\nEnd Lock",
                                shared))
              .empty());
    CHECK(probe(std::format("{}Local x = frames.Exchange(3)", shared)) == "Int32");

    // Copies, which would each be a variable of their own.
    CHECK(errors_of(std::format("{}Local copy = frames", shared)) ==
          errors_t{ "frames: Atomic<Int32> stays where it's made, so it can't be copied; use its "
                    "Methods, like Load()" });
    CHECK(errors_of(std::format("{}Print(\"{{score}}\")", shared)) ==
          errors_t{ "score: Mutex<Int32> stays where it's made, so it can't be copied; use its "
                    "value in a Lock block, like 'Lock m As value'" });
    CHECK(errors_of("Local a = Atomic<Int>(0)") ==
          errors_t{ "Atomic<Int>(0): Atomic<Int32> stays where it's made, so it's made only as "
                    "what a Global, a Static Field, or a Class's Field starts with" });
    CHECK(errors_of("Local a: Atomic<Int>") ==
          errors_t{ "Atomic<Int>: Atomic<Int32> stays where it's made, so a Local or Const can't "
                    "hold one; make it a Global, a Static Field, or a Class's Field" });
    CHECK(errors_of("Function F(a: Atomic<Int>)\nEnd Function") ==
          errors_t{ "Atomic<Int>: Atomic<Int32> stays where it's made, so a parameter can't hold "
                    "one; make it ByRef" });
    CHECK(errors_of("Function F(ByRef a: Atomic<Int>): Int => a.Load()").empty());
    CHECK(errors_of("Function F(): Mutex<Int>\nEnd Function") ==
          errors_t{ "Mutex<Int>: Mutex<Int32> stays where it's made, so a function can't give "
                    "one back" });
    CHECK(errors_of("Struct S\n    Field Count: Atomic<Int>\nEnd Struct") ==
          errors_t{ "Atomic<Int>: Atomic<Int32> stays where it's made, so a Struct, which is "
                    "copied, can't hold one; make S a Class" });
    CHECK(errors_of("Global counts: Atomic<Int>[]") ==
          errors_t{ "Atomic<Int>: Atomic<Int32> stays where it's made, so it can't be part of "
                    "another type" });
    CHECK(errors_of(std::format("{}frames = Atomic<Int>(1)", shared)) ==
          errors_t{ "frames: Atomic<Int32> stays where it's made, so it can't be assigned; use its "
                    "Store(value)" });

    // What they hold.
    CHECK(errors_of("Global name: Atomic<String>") ==
          errors_t{ "Atomic<String>: an Atomic holds an integer or a Bool, not String; a Mutex "
                    "holds any Sendable value" });
    CHECK(errors_of(std::format("{}Global enemies: Mutex<Enemy[]>", shared)) ==
          errors_t{ "Mutex<Enemy[]>: a Mutex holds a Sendable value, which Enemy[] isn't: an array "
                    "stays on the thread that made it" });

    // A Struct declared further down is checked once it has its fields, and a type parameter
    // with the constraints where it's written.
    CHECK(errors_of("Global m: Mutex<S>\n"
                    "Function Feed(ch: Channel<S>)\n"
                    "End Function\n"
                    "Struct S\n"
                    "    Field t: Thing?\n"
                    "End Struct\n"
                    "Class Thing\n"
                    "End Class") ==
          errors_t{
              "Mutex<S>: a Mutex holds a Sendable value, which S isn't: its field 't' is Thing?, "
              "and a Class's objects stay on the thread that made them, unless it's a Shared Class",
              "Channel<S>: a Channel carries Sendable values to other threads, which S isn't: its "
              "field 't' is Thing?, and a Class's objects stay on the thread that made them, "
              "unless it's a Shared Class",
          });
    CHECK(errors_of("Import std.core\n"
                    "Global shared: Mutex<P>\n"
                    "Function Feed<T: Sendable>(ch: Channel<T>)\n"
                    "End Function\n"
                    "Function Bad<T>(ch: Channel<T>)\n"
                    "End Function\n"
                    "Struct P\n"
                    "    Field n: Int\n"
                    "End Struct") ==
          errors_t{ "Channel<T>: a Channel carries Sendable values to other threads, which T "
                    "isn't: nothing says T is Sendable" });
    CHECK(errors_of("Global m = Mutex((1, \"one\"))\n"
                    "Lock m As pair\n"
                    "    Local text: String = pair.1\n"
                    "End Lock")
              .empty());

    // Their members.
    CHECK(errors_of(std::format("{}Local x = ready.Add(1)", shared)) ==
          errors_t{ "Add: Atomic<Bool> has no member named 'Add'; it has Load, Store, Exchange, "
                    "CompareExchange" });
    CHECK(errors_of("Extend Atomic<Int>\n    Method Twice(): Int => 2\nEnd Extend") ==
          errors_t{ "Atomic<Int>: an Atomic can't be extended; it has only its own operations" });
    CHECK(errors_of("Class C\nEnd Class\n"
                    "Extend Task<Int>\nEnd Extend\n"
                    "Extend Thread\nEnd Extend\n"
                    "Extend Channel<Int>\nEnd Extend\n"
                    "Extend Scheduler\nEnd Extend\n"
                    "Extend Weak<C>\nEnd Extend") ==
          errors_t{ "Task<Int>: a Task can't be extended; it has only its own operations",
                    "Thread: a Thread can't be extended; it has only its own operations",
                    "Channel<Int>: a Channel can't be extended; it has only its own operations",
                    "Scheduler: a Scheduler can't be extended; it has only its own operations",
                    "Weak<C>: a Weak<T> can't be extended; it has only its own operations" });
    CHECK(errors_of(std::format("{}Local x = score.value", shared)) ==
          errors_t{ "value: a Mutex's value is used in a Lock block, like 'Lock m As value', where "
                    "only one thread has it at a time" });
}

TEST_CASE("a Lock block holds a Mutex while it uses the value", "[sema]") {
    const std::string_view shared = "Import std.core\n"
                                    "Global frames = Atomic<Int>(0)\n"
                                    "Global score = Mutex<Int>(0)\n";
    CHECK(errors_of(std::format("{}Lock frames As f\nEnd Lock", shared)) ==
          errors_t{ "frames: Lock takes a Mutex, not Atomic<Int32>" });
    CHECK(errors_of(
              std::format("{}Lock score As s\n    Local f = Function() => s\nEnd Lock", shared)) ==
          errors_t{ "s: 's' is ByRef, so a lambda can't capture it; copy it into a Local first" });

    // Return leaves the Lock, so nothing comes after it.
    CHECK(errors_of(std::format("{}Function Take(): Int\n"
                                "    Lock score As s\n"
                                "        Return s\n"
                                "    End Lock\n"
                                "End Function",
                                shared))
              .empty());
    CHECK(errors_of(std::format("{}Const Function F(): Int\n"
                                "    Lock score As s\n"
                                "    End Lock\n"
                                "    Return 1\n"
                                "End Function",
                                shared)) ==
          errors_t{ "score: a Const Function can't Lock, since only the running program has "
                    "Mutexes" });

    // A Class's Field can be one, starting with its zero or, for an Atomic, a known value.
    CHECK(errors_of("Class Counter\n"
                    "    Field Total: Atomic<Int64> = Atomic<Int64>(5)\n"
                    "    Field Log: Mutex<String>\n"
                    "    Method Note(text: String)\n"
                    "        Lock Log As log\n"
                    "            log = log + text\n"
                    "        End Lock\n"
                    "        Total.Add(1)\n"
                    "    End Method\n"
                    "End Class")
              .empty());
    CHECK(errors_of("Class Counter\n    Field Log: Mutex<Int> = Mutex<Int>(3)\nEnd Class") ==
          errors_t{ "Mutex<Int>(3): a Mutex Field starts with its value's zero, which a Lock block "
                    "in New can change" });
}

TEST_CASE("a Shared Class's objects can go to another thread", "[sema]") {
    const std::string_view hub = "Import std.core\n"
                                 "Shared Class Hub\n"
                                 "    Field Visits: Atomic<Int>\n"
                                 "    Field Names: Mutex<String>\n"
                                 "    ReadOnly Field Title: String\n"
                                 "    New(title: String)\n"
                                 "        Title = title\n"
                                 "    End New\n"
                                 "End Class\n"
                                 "Function Hand<T: Sendable>(value: T): T => value\n";
    CHECK(errors_of(std::format("{}Local h = Hand(Hub(\"main\"))\nLocal n = h.Visits.Add(1)", hub))
              .empty());
    CHECK(errors_of("Shared Class Bad\n    Field Count: Int\nEnd Class") ==
          errors_t{ "Count: 'Count' must be an Atomic, a Mutex, or ReadOnly, since threads share "
                    "Bad's objects" });
    CHECK(errors_of("Class Enemy\nEnd Class\n"
                    "Shared Class Bad\n    ReadOnly Field Target: Enemy?\nEnd Class") ==
          errors_t{ "Target: 'Target' is ReadOnly, but threads share Bad's objects, so it must be "
                    "Sendable too: a Class's objects stay on the thread that made them, unless "
                    "it's a Shared Class" });
    CHECK(errors_of(
              std::format("{}Shared Class Room\n    ReadOnly Field Owner: Hub?\nEnd Class", hub))
              .empty());

    // Shared Classes extend each other only.
    CHECK(errors_of(std::format("{}Class Plain Extends Hub\nEnd Class", hub)) ==
          errors_t{ "Plain: Hub is a Shared Class, so only a Shared Class can extend it" });
    CHECK(errors_of("Class Plain\nEnd Class\nShared Class Hub Extends Plain\nEnd Class") ==
          errors_t{ "Hub: a Shared Class extends only a Shared Class, and Plain isn't one" });

    // A generic one says its type parameters are Sendable.
    const std::string_view made = "    New(item: T)\n        Item = item\n    End New\n";
    CHECK(errors_of(
              std::format("Shared Class Box<T>\n    ReadOnly Field Item: T\n{}End Class", made)) ==
          errors_t{ "Item: 'Item' is ReadOnly, but threads share Box<T>'s objects, so it must be "
                    "Sendable too: nothing says T is Sendable" });
    CHECK(errors_of(
              std::format("Import std.core\n"
                          "Shared Class Box<T: Sendable>\n    ReadOnly Field Item: T\n{}End Class",
                          made))
              .empty());

    // Shared only goes before a Class.
    CHECK(errors_of("Shared Struct S\nEnd Struct") ==
          errors_t{ "S: a Struct can't be Shared, since it's copied; it's Sendable when its "
                    "fields are" });
    CHECK(errors_of("Shared Function F()\nEnd Function") ==
          errors_t{ "Shared: Shared only goes before a Class, whose objects threads share" });
}

TEST_CASE("Spawn runs work on another thread, and gives its Task", "[sema]") {
    CHECK(probe("Local x = Spawn(Function() => 1)") == "Task<Int32>");
    CHECK(probe("Local x = Spawn(Function() => \"a\").Wait()") == "String");
    CHECK(probe("Local x = Spawn(Function() => 1).IsDone()") == "Bool");
    CHECK(probe("Function Work()\nEnd Function\nLocal x = Spawn(Work)") == "Task");
    CHECK(probe("Import std.core\n"
                "Local x = Thread.Start(Function() => Print(\"hi\"))") == "Thread");
    CHECK(errors_of("Local t: Task<Int64> = Spawn(Function() => 1)\nLocal x: Int64 = t.Wait()")
              .empty());
    CHECK(probe("Local t: Task<Int64>? = Spawn(Function() => 5)") ==
          "Task<Int64>? (from Task<Int64>)");
    CHECK(errors_of("Import std.core\n"
                    "Local t = Thread.Start(Function() => Print(\"hi\"))\nt.Join()")
              .empty());

    // What it captures can't change, and is Sendable.
    CHECK(errors_of("Const name = \"a\"\n"
                    "Function Run(count: Int): Task<Int>\n"
                    "    Return Spawn(Function() => count + name.Bytes.Count)\n"
                    "End Function")
              .empty());
    CHECK(errors_of("Function Run(): Task<Int>\n"
                    "    Local count = 1\n"
                    "    Return Spawn(Function() => count)\n"
                    "End Function") ==
          errors_t{ "Function() => count: 'count' is a Local, which another thread can't share; "
                    "make it a Const" });
    CHECK(errors_of("Class Enemy\n    Field HP: Int\nEnd Class\n"
                    "Function Run(enemy: Enemy): Task<Int>\n"
                    "    Return Spawn(Function() => enemy.HP)\n"
                    "End Function") ==
          errors_t{ "Function() => enemy.HP: 'enemy' is Enemy, which another thread can't have: "
                    "a Class's objects stay on the thread that made them, unless it's a Shared "
                    "Class" });
    CHECK(errors_of("Shared Class Hub\n    Field Hits: Atomic<Int>\nEnd Class\n"
                    "Function Run(hub: Hub): Task<Int>\n"
                    "    Return Spawn(Function() => hub.Hits.Add(1))\n"
                    "End Function")
              .empty());

    // It takes a lambda, a Function's name, or a Sendable Func, of no arguments.
    CHECK(errors_of("Local f = Function() => 1\nLocal t = Spawn(f)") ==
          errors_t{ "f: Spawn takes a lambda, a Function's name, or a Sendable Func, since a "
                    "function value it was given would stay on this thread too" });
    CHECK(probe("Local f: Sendable Func(): Int = Function() => 1\nLocal t = Spawn(f)") ==
          "Task<Int32>");
    CHECK(errors_of("Local t = Spawn(Function(x: Int) => x)") ==
          errors_t{ "Function(x: Int) => x: the work Spawn runs takes no arguments; a lambda can "
                    "pass them" });
    CHECK(errors_of("Local t = Spawn()") ==
          errors_t{ "Spawn(): Spawn takes the work to run, like Spawn(Function() => Work())" });

    // What the work gives says the Task's type, so neither takes type arguments.
    CHECK(errors_of("Local t = Spawn<String>(Function() => 5)") ==
          errors_t{ "Spawn<String>: Spawn takes no type arguments; a Task<T> expected says what "
                    "the work gives" });
    CHECK(errors_of("Import std.core\n"
                    "Local t = Thread.Start<Int, Bool>(Function() => Print(\"hi\"))") ==
          errors_t{ "Thread.Start<Int, Bool>: Thread.Start takes no type arguments, since a "
                    "Thread gives nothing" });
    CHECK(errors_of("Import std.core\n"
                    "Local t = Thread<Int>.Start(Function() => Print(\"hi\"))") ==
          errors_t{ "Thread<Int>.Start: Thread.Start takes no type arguments, since a Thread "
                    "gives nothing" });

    // A Task stays on the thread that spawned it.
    CHECK(errors_of("Import std.core\n"
                    "Function Hand<T: Sendable>(value: T): T => value\n"
                    "Local t = Hand(Spawn(Function() => 1))") ==
          errors_t{ "Hand: Task<Int32> isn't Sendable, which 'Hand' needs for T: a Task stays on "
                    "the thread that spawned it" });
    CHECK(errors_of("Local t = Spawn(Function() => 1)\nLocal x = t.Join()") ==
          errors_t{ "Join: Task<Int32> has no member named 'Join'; it has Wait and IsDone" });
}

TEST_CASE("@MainThread on a ThreadLocal Global is reported once, where it's written", "[sema]") {
    CHECK(errors_of("@MainThread ThreadLocal Global x = 1\n@MainThread Global y = 2") ==
          errors_t{ "x: @MainThread only goes before a Function or Method",
                    "y: @MainThread only goes before a Function or Method" });
    // And its initializer is still checked as work on another thread.
    CHECK(
        errors_of("@MainThread Function OnMain(): Int => 1\n"
                  "@MainThread ThreadLocal Global x = OnMain()") ==
        errors_t{ "x: @MainThread only goes before a Function or Method",
                  "OnMain(): a ThreadLocal Global's initializer runs on each thread that uses it, "
                  "but it calls 'OnMain', which is @MainThread" });
}

TEST_CASE("a ThreadLocal Global is each thread's own and made on any thread", "[sema]") {
    // Work on another thread uses its own, and top-level code the main thread's.
    CHECK(errors_of("ThreadLocal Global hits = 0\n"
                    "Function Bump(): Int\n    hits += 1\n    Return hits\nEnd Function\n"
                    "Local t = Spawn(Bump)\n"
                    "hits = t.Wait()")
              .empty());
    CHECK(errors_of("Import std.core\nThreadLocal Global names: String[] = []\n"
                    "Local t = Thread.Start(Function()\n    names.Add(\"worker\")\nEnd Function)")
              .empty());

    // Its initializer runs on whichever thread uses it first, so it's work on another thread.
    CHECK(errors_of("Global score = 3\nThreadLocal Global copy = score") ==
          errors_t{ "score: a ThreadLocal Global's initializer runs on each thread that uses it, "
                    "but it uses the Global 'score', which only the main thread can, unless it's "
                    "an Atomic or a Mutex" });
    CHECK(errors_of("Global score = 3\nFunction Get(): Int => score\n"
                    "ThreadLocal Global copy = Get()") ==
          errors_t{ "Get(): a ThreadLocal Global's initializer runs on each thread that uses it, "
                    "but it calls 'Get', which uses the Global 'score', which only the main "
                    "thread can, unless it's an Atomic or a Mutex" });
    CHECK(errors_of("Global hits = Atomic<Int>(0)\nThreadLocal Global copy = hits.Load()").empty());
    // A lambda it runs, and what it uses after a Const it needs early, are its too.
    CHECK(errors_of("Global score = 3\nFunction Run(f: Func(): Int): Int => f()\n"
                    "ThreadLocal Global copy = Run(Function(): Int => score)") ==
          errors_t{ "Run(Function(): Int => score): a ThreadLocal Global's initializer runs on "
                    "each thread that uses it, but it uses the Global 'score', which only the "
                    "main thread can, unless it's an Atomic or a Mutex" });
    CHECK(errors_of("Global score = 3\nThreadLocal Global copy = Size + score\n"
                    "Const Size = 4") ==
          errors_t{ "Size + score: a ThreadLocal Global's initializer runs on each thread that "
                    "uses it, but it uses the Global 'score', which only the main thread can, "
                    "unless it's an Atomic or a Mutex" });

    // What threads share isn't each one's own, and it's only a Global at the top level.
    CHECK(errors_of("ThreadLocal Global count = Atomic<Int>(0)") ==
          errors_t{ "count: 'count' is ThreadLocal, so each thread has its own, but Atomic<Int32> "
                    "is for what threads share; make it a Global" });
    CHECK(errors_of("Function F()\n    ThreadLocal Global x = 1\nEnd Function") ==
          errors_t{ "Global x = 1: Globals can only be declared at the top level" });
    CHECK(errors_of("Static ThreadLocal Global x = 1") ==
          errors_t{ "x: Static only goes before a Field or Method of a Struct or Class" });
}

TEST_CASE("work on another thread reaches nothing only the main thread may", "[sema]") {
    const std::string_view globals = "Global score = 0\n"
                                     "Global hits = Atomic<Int>(0)\n"
                                     "Global names = Mutex<String>(\"\")\n"
                                     "Const title = \"game\"\n"
                                     "Class Enemy\n    Field HP: Int\nEnd Class\n"
                                     "Const boss = Enemy()\n";
    CHECK(errors_of(std::format("{}Local t = Spawn(Function() => score)", globals)) ==
          errors_t{ "Spawn: this work runs on another thread, but it uses the Global 'score', "
                    "which only the main thread can, unless it's an Atomic or a Mutex" });
    CHECK(errors_of(std::format("{}Local t = Thread.Start(Function()\n"
                                "    Lock names As n\n"
                                "        n = n + title\n"
                                "    End Lock\n"
                                "    hits.Add(1)\n"
                                "End Function)",
                                globals))
              .empty());
    CHECK(errors_of(std::format("{}Local t = Spawn(Function() => boss.HP)", globals)) ==
          errors_t{ "Spawn: this work runs on another thread, but it uses the Const 'boss', which "
                    "holds Enemy, which isn't Sendable" });

    // Through what it calls, and assigning counts.
    CHECK(errors_of(std::format("{}Function Bump()\n    score += 1\nEnd Function\n"
                                "Function Step()\n    Bump()\nEnd Function\n"
                                "Local t = Spawn(Step)",
                                globals)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Bump', which uses the "
                    "Global 'score', which only the main thread can, unless it's an Atomic or a "
                    "Mutex" });

    // A Static Field is a Global, and top-level code is the main thread's.
    CHECK(errors_of("Class Enemy\n"
                    "    Static Field Count: Int\n"
                    "    Method Think(): Int => Count\n"
                    "End Class\n"
                    "Function Run(): Task<Int>\n"
                    "    Return Spawn(Function() => Enemy().Think())\n"
                    "End Function\n"
                    "Enemy.Count = 3") ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Enemy.Think', which "
                    "uses the Static Field 'Enemy.Count', which only the main thread can, unless "
                    "it's an Atomic or a Mutex" });

    // @MainThread
    const std::string_view render = "@MainThread Function Render()\nEnd Function\n";
    CHECK(errors_of(std::format("{}Local t = Spawn(Function() => Render())", render)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Render', which is "
                    "@MainThread" });
    CHECK(errors_of(std::format("{}Local t = Spawn(Render)", render)) ==
          errors_t{ "Spawn: this work runs on another thread, but 'Render' is @MainThread" });
    CHECK(errors_of(std::format("{}Render()", render)).empty());
    CHECK(errors_of("@MainThread Struct S\nEnd Struct") ==
          errors_t{ "S: @MainThread only goes before a Function or Method" });
    CHECK(errors_of("Class Engine\n"
                    "    @MainThread Property Frame: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "End Class\n"
                    "@MainThread Interface Drawn\n"
                    "    Method Draw(): Int\n"
                    "End Interface\n"
                    "@MainThread Enum Mode\n"
                    "    A\n"
                    "End Enum") ==
          errors_t{ "Frame: @MainThread only goes before a Function or Method",
                    "Drawn: @MainThread only goes before a Function or Method",
                    "Mode: @MainThread only goes before a Function or Method" });
}

TEST_CASE("what a call can run on another thread includes overrides and lambdas", "[sema]") {
    const std::string_view score = "Global score = 0\n";
    CHECK(errors_of(std::format("{}Class Enemy\n"
                                "    Method Think(): Int => 1\n"
                                "End Class\n"
                                "Class Grunt Extends Enemy\n"
                                "    Override Method Think(): Int => score\n"
                                "End Class\n"
                                "Local t = Spawn(Function() => Enemy().Think())",
                                score)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Enemy.Think', which "
                    "calls 'Grunt.Think', which uses the Global 'score', which only the main "
                    "thread can, unless it's an Atomic or a Mutex" });
    CHECK(errors_of(std::format("{}Interface Scored\n"
                                "    Method Points(): Int\n"
                                "End Interface\n"
                                "Struct Coin Implements Scored\n"
                                "    Method Points(): Int => score\n"
                                "End Struct\n"
                                "Function Total<S: Scored>(item: S): Int => item.Points()\n"
                                "Local t = Spawn(Function() => Total(Coin()))",
                                score)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Total', which calls "
                    "'Points', which calls 'Coin.Points', which uses the Global 'score', which "
                    "only the main thread can, unless it's an Atomic or a Mutex" });

    // An Interface's Property, through its value or a type parameter, runs the Get or Set of
    // what implements it.
    const std::string_view valued = "Interface Valued\n"
                                    "    Property Value: Int\n"
                                    "End Interface\n"
                                    "Class Thing Implements Valued\n"
                                    "    Property Value: Int\n"
                                    "        Get => score\n"
                                    "        Set(value)\n"
                                    "            score = value\n"
                                    "        End Set\n"
                                    "    End Property\n"
                                    "End Class\n";
    CHECK(errors_of(std::format("{}{}Function Give(v: Valued)\n"
                                "    v.Value = 2\n"
                                "End Function\n"
                                "Function Take<T: Valued>(v: T): Int => v.Value\n"
                                "Local t = Spawn(Function() => Give(Thing()))\n"
                                "Local u = Spawn(Function() => Take(Thing()))",
                                score, valued)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Give', which calls "
                    "'Value', which calls 'Thing.Value', which uses the Global 'score', which "
                    "only the main thread can, unless it's an Atomic or a Mutex",
                    "Spawn: this work runs on another thread, but it calls 'Take', which calls "
                    "'Value', which calls 'Thing.Value', which uses the Global 'score', which "
                    "only the main thread can, unless it's an Atomic or a Mutex" });

    // Showing a value runs its ToString, and == on what holds values their Operator ==.
    const std::string_view parts = "Import std.core\n"
                                   "Struct Shown Implements Printable\n"
                                   "    Field n: Int\n"
                                   "    Method ToString(): String => \"{score}\"\n"
                                   "End Struct\n"
                                   "Struct Eq\n"
                                   "    Field n: Int\n"
                                   "    Operator ==(a: Eq, b: Eq): Bool => a.n == score\n"
                                   "End Struct\n"
                                   "Struct Pair\n"
                                   "    Field e: Eq\n"
                                   "End Struct\n";
    const std::string reached = "Spawn: this work runs on another thread, but it calls "
                                "'{}', which uses the Global 'score', which only the main thread "
                                "can, unless it's an Atomic or a Mutex";
    CHECK(errors_of(std::format("{}{}Local t = Spawn(Function() => \"{{(Shown(1), 2)}}\")", parts,
                                score)) ==
          errors_t{ std::vformat(reached, std::make_format_args("Shown.ToString")) });
    CHECK(errors_of(
              std::format("{}{}Local t = Spawn(Function() => Print(Shown(1)))", parts, score)) ==
          errors_t{ std::vformat(reached, std::make_format_args("Shown.ToString")) });
    CHECK(errors_of(
              std::format("{}{}Local t = Spawn(Function() => [Eq(1)] == [Eq(2)])", parts, score)) ==
          errors_t{ std::vformat(reached, std::make_format_args("Eq.==")) });
    CHECK(errors_of(std::format("{}{}Local t = Spawn(Function() => Pair(Eq(1)) == Pair(Eq(2)))",
                                parts, score)) ==
          errors_t{ std::vformat(reached, std::make_format_args("Eq.==")) });
    // So do a T[]'s Contains and generic code's == on a Struct that compares its fields, as
    // generic code's Hash does on one that combines its fields'.
    CHECK(errors_of(std::format("{}{}Local t = Spawn(Function() => [Eq(1)].Contains(Eq(2)))", parts,
                                score)) ==
          errors_t{ std::vformat(reached, std::make_format_args("Eq.==")) });
    CHECK(errors_of(std::format("{}{}Struct Listed Implements Equatable\n"
                                "    Field e: Eq\n"
                                "End Struct\n"
                                "Function Same<T: Equatable>(a: T, b: T): Bool => a == b\n"
                                "Local t = Spawn(Function() => Same(Listed(Eq(1)), Listed(Eq(2))))",
                                parts, score)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Same', which calls "
                    "'==', which calls 'Eq.==', which uses the Global 'score', which only the "
                    "main thread can, unless it's an Atomic or a Mutex" });
    CHECK(errors_of(std::format("{}{}Struct Key Implements Hashable\n"
                                "    Field n: Int\n"
                                "    Operator ==(a: Key, b: Key): Bool => a.n == b.n\n"
                                "    Method Hash(): UInt64 => score As UInt64\n"
                                "End Struct\n"
                                "Struct Keys Implements Hashable\n"
                                "    Field k: Key\n"
                                "End Struct\n"
                                "Function HashOf<T: Hashable>(a: T): UInt64 => a.Hash()\n"
                                "Local t = Spawn(Function() => HashOf(Keys(Key(1))))",
                                parts, score)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'HashOf', which calls "
                    "'Hash', which calls 'Key.Hash', which uses the Global 'score', which only the "
                    "main thread can, unless it's an Atomic or a Mutex" });
    CHECK(errors_of(std::format("{}Struct Node\n"
                                "    Field children: Node[]\n"
                                "End Struct\n"
                                "Local t = Spawn(Function() => Node([]) == Node([]))",
                                score))
              .empty());
    CHECK(errors_of(std::format("{}Function Apply(f: Func(): Int): Int => f()\n"
                                "Local t = Spawn(Function() => Apply(Function() => score))",
                                score)) ==
          errors_t{ "Spawn: this work runs on another thread, but it uses the Global 'score', "
                    "which only the main thread can, unless it's an Atomic or a Mutex" });

    // A Shared Class's Destroy() runs on whichever thread lets go last.
    CHECK(errors_of(std::format("{}Shared Class Hub\n"
                                "    Method Destroy()\n"
                                "        score += 1\n"
                                "    End Method\n"
                                "End Class",
                                score)) ==
          errors_t{ "Destroy: a Shared Class's Destroy() runs on whichever thread lets go last, "
                    "but it uses the Global 'score', which only the main thread can, unless it's "
                    "an Atomic or a Mutex" });
}

TEST_CASE("a Sendable Func is a lambda or a Function's name that other threads can call",
          "[sema]") {
    constexpr std::string_view twice = "Import std.core\n"
                                       "Function Twice(x: Int): Int => x * 2\n"
                                       "Function Twice(x: String): String => x + x\n"
                                       "Function Send<T: Sendable>(value: T): T => value\n";
    const auto with_twice = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", twice, text));
    };

    // Where one is expected, a lambda or a Function's name is one, and it converts to a Func.
    CHECK(probe(std::format("{}Local f: Sendable Func(Int): Int = Function(x) => x * 2\n"
                            "Local g = f",
                            twice)) == "Sendable Func(Int32): Int32");
    CHECK(probe(std::format("{}Local f: Sendable Func(String): String = Twice\n"
                            "Local g: Func(String): String = f",
                            twice)) == "Func(String): String (from Sendable Func(String): String)");
    CHECK(probe(std::format("{}Local f: (Sendable Func(Int): Int)? = Twice\n"
                            "Local g = Send(f)",
                            twice)) == "Sendable Func(Int32): Int32");
    // Spawn takes one that may be Null only once it's checked.
    CHECK(with_twice("Function Get(): (Sendable Func(): Int)? => Function() => 1\n"
                     "Local t = Spawn(Get())") ==
          errors_t{ "Get(): expected Sendable Func(): Int32, found (Sendable Func(): Int32)?, "
                    "which may be Null; check it first, or give a value for Null with '?"
                    "?'" });

    // One that captures Self, kept in a Lock of a Mutex that's one of Self's fields, keeps Self
    // alive, as in the field itself.
    CHECK(with_twice("Shared Class Node\n"
                     "    ReadOnly Field Hits: Int\n"
                     "    Field Work: Mutex<(Sendable Func(): Int)?>\n"
                     "    Method Arm()\n"
                     "        Lock Work As work\n"
                     "            work = Function() => Hits\n"
                     "        End Lock\n"
                     "    End Method\n"
                     "End Class") ==
          errors_t{ "Function: this lambda captures Self, so keeping it in a field of Self makes a "
                    "cycle that's never freed; capture it with [Weak Self]" });

    // An overloaded function or New takes one too.
    CHECK(with_twice("Shared Class Lazy<T: Sendable>\n"
                     "    ReadOnly Field Maker: Sendable Func(): T\n"
                     "    New(maker: Sendable Func(): T)\n"
                     "        Maker = maker\n"
                     "    End New\n"
                     "    New(maker: Sendable Func(): T, label: String)\n"
                     "        Maker = maker\n"
                     "    End New\n"
                     "End Class\n"
                     "Function Answer(): Int => 42\n"
                     "Function Inc(x: Int): Int => x + 1\n"
                     "Function Run(f: Sendable Func(Int): Int): Int => f(1)\n"
                     "Function Run(f: (Sendable Func(Int): Int)?, n: Int): Int => n\n"
                     "Local a = Lazy<Int>(Answer)\n"
                     "Local b = Lazy<String>(Function(): String => \"x\", \"label\")\n"
                     "Local c = Run(Inc) + Run(Function(x: Int) => x + 1, 5) + Run(Inc, 2)")
              .empty());
    CHECK(with_twice("Local f: Func(Int): Int = Twice\n"
                     "Local g: Sendable Func(Int): Int = f") ==
          errors_t{ "f: expected Sendable Func(Int32): Int32, found Func(Int32): Int32, which "
                    "other threads can't call; a Sendable Func is a lambda or a Function's name, "
                    "written where it's expected" });

    // It's Sendable: a Shared Class holds one, and Spawn's work and a Parallel For call one.
    CHECK(with_twice("Shared Class Maker<T: Sendable>\n"
                     "    ReadOnly Field Make: Sendable Func(): T\n"
                     "    New(make: Sendable Func(): T)\n"
                     "        Make = make\n"
                     "    End New\n"
                     "End Class\n"
                     "Function Each(f: Sendable Func(Int): Int, values: Int[])\n"
                     "    Parallel For i = 0 Until values.Count\n"
                     "        values[i] = f(values[i])\n"
                     "    End For\n"
                     "    Local t = Spawn(Function() => f(1)).Wait()\n"
                     "End Function\n"
                     "Local m = Maker<Int>(Function() => 5)")
              .empty());

    // What it captures can't change, and is Sendable: a Const, a parameter, a loop variable, or
    // a Shared Class's Self.
    CHECK(with_twice("Shared Class Hub\n"
                     "    ReadOnly Field Hits: Int\n"
                     "    Method Later(): Sendable Func(): Int => Function() => Hits\n"
                     "End Class\n"
                     "Function Add(by: Int): Sendable Func(Int): Int\n"
                     "    Const name = \"add\"\n"
                     "    Return Function(x: Int): Int => x + by + name.Bytes.Count\n"
                     "End Function\n"
                     "For i = 1 To 3\n"
                     "    Local f: Sendable Func(): Int = Function() => i\n"
                     "End For")
              .empty());
    CHECK(with_twice("Class Enemy\n"
                     "    Field HP: Int\n"
                     "    Method Later(): Sendable Func(): Int => Function() => HP\n"
                     "End Class\n"
                     "Function Count(): Sendable Func(): Int\n"
                     "    Local n = 1\n"
                     "    Return Function() => n\n"
                     "End Function") ==
          errors_t{
              "Function() => HP: a Sendable Func can run on any thread, but 'Self' is Enemy, "
              "which another thread can't have: a Class's objects stay on the thread that made "
              "them, unless it's a Shared Class",
              "Function() => n: a Sendable Func can run on any thread, but 'n' is a Local, which "
              "another thread can't share; make it a Const",
          });

    // Its code runs on another thread, and an Async lambda's can't.
    CHECK(with_twice("Global score = 0\n"
                     "@MainThread Function Render()\n"
                     "End Function\n"
                     "Function Bump(): Sendable Func() => Function() => Print(\"{score}\")\n"
                     "Function Draw(): Sendable Func() => Render\n"
                     "Function Wait(): Sendable Func(): Task<Int> => Async Function() => 1") ==
          errors_t{
              "Function: a Sendable Func can run on any thread, but it uses the Global 'score', "
              "which only the main thread can, unless it's an Atomic or a Mutex",
              "Render: a Sendable Func can run on any thread, but 'Render' is @MainThread",
              "Function: an Async lambda's code runs on the main thread, so it can't be a "
              "Sendable Func",
          });
}

TEST_CASE("a Parallel For's passes share only copies and their own elements", "[sema]") {
    const std::string_view setup = "Local values = [1, 2, 3]\n"
                                   "Local scale = 2\n"
                                   "Local results = [0, 0, 0]\n";
    CHECK(errors_of(std::format("{}Parallel For i = 0 Until values.Count\n"
                                "    results[i] = values[2 - i] * scale\n"
                                "End For",
                                setup))
              .empty());

    // What's outside is a copy, which it can't change.
    // A body that changes an array's elements reads only its own, since another pass may be
    // changing any other at once; one that changes none reads any.
    CHECK(errors_of(std::format("{}Parallel For i = 0 Until 3\n"
                                "    Local next = results[(i + 1) % 3]\n"
                                "    results[i] = next + values[i]\n"
                                "    results[i] += results[i]\n"
                                "End For",
                                setup)) ==
          errors_t{ "results[(i + 1) % 3]: a Parallel For's body that changes elements of "
                    "'results', an array from outside the body, reads it only at its own index, "
                    "results[i], since another pass may be changing this one at once" });
    CHECK(errors_of("Struct Cell\n    Field hp = 0\nEnd Struct\n"
                    "Local cells = [Cell(), Cell()]\n"
                    "Parallel For i = 0 Until 2\n"
                    "    Local first = cells[0].hp\n"
                    "    cells[i].hp = first\n"
                    "End For") ==
          errors_t{ "cells[0]: a Parallel For's body that changes elements of 'cells', an array "
                    "from outside the body, reads it only at its own index, cells[i], since "
                    "another pass may be changing this one at once" });
    CHECK(errors_of(std::format("{}Parallel For i = 0 Until 3\n"
                                "    results[0] = results[1]\n"
                                "End For",
                                setup)) ==
          errors_t{ "results: a Parallel For's body changes 'results', an array from outside the "
                    "body, only at its own index, results[i]" });
    CHECK(errors_of(std::format("{}Parallel For i = 0 Until 3\n    scale += i\nEnd For", setup)) ==
          errors_t{ "scale: a Parallel For's body can't change 'scale', which is outside it" });

    // An array from outside changes only at the pass's own index, and is used only through its
    // elements.
    CHECK(errors_of(
              std::format("{}Parallel For i = 0 Until 3\n    results[0] = i\nEnd For", setup)) ==
          errors_t{ "results: a Parallel For's body changes 'results', an array from outside the "
                    "body, only at its own index, results[i]" });
    CHECK(errors_of(
              std::format("{}Parallel For i = 0 Until 3\n    results.Add(i)\nEnd For", setup)) ==
          errors_t{ "results: a Parallel For's body uses 'results', an array from outside the "
                    "body, only through its elements, like results[i], and its Count" });
    CHECK(errors_of(std::format("{}Parallel For i = 0 Until 3\n"
                                "    Local f = Function() => values[i]\n"
                                "End For",
                                setup)) ==
          errors_t{ "values: a lambda in a Parallel For's body can't take 'values', an array "
                    "from outside the body" });

    // What it uses is Sendable.
    CHECK(errors_of("Class Enemy\n    Field HP: Int\nEnd Class\n"
                    "Local boss = Enemy()\n"
                    "Parallel For i = 0 Until 3\n    Local hp = boss.HP\nEnd For") ==
          errors_t{ "Parallel: 'boss' is Enemy, which another thread can't have: a Class's "
                    "objects stay on the thread that made them, unless it's a Shared Class" });
    CHECK(errors_of("Class Enemy\n    Field HP: Int\nEnd Class\n"
                    "Local enemies = [Enemy()]\n"
                    "Parallel For i = 0 Until 1\n    Local hp = enemies[i].HP\nEnd For") ==
          errors_t{ "Parallel: 'enemies' is Enemy[], whose elements another thread can't have: "
                    "a Class's objects stay on the thread that made them, unless it's a Shared "
                    "Class" });

    // It counts with integers, by 1, and its passes don't leave it.
    CHECK(errors_of("Parallel For x = 0.0 Until 1.0\nEnd For") ==
          errors_t{ "0.0: a Parallel For counts with integers, not Float32" });
    CHECK(errors_of("Parallel For i = 0 Until 10 Step 2\nEnd For") ==
          errors_t{ "2: a Parallel For counts up by 1, so it has no Step" });
    CHECK(errors_of("Parallel For i = 0 Until 10\n    If i == 3 Then Break\nEnd For") ==
          errors_t{ "Break: Break can't leave a Parallel For, whose passes run at once" });
    CHECK(errors_of("Function F()\n"
                    "    Parallel For i = 0 Until 10\n"
                    "        Return\n"
                    "    End For\n"
                    "End Function") ==
          errors_t{ "Return: a Parallel For's body can't Return; Continue goes on to its next "
                    "index" });

    // The main-thread rules hold for it.
    CHECK(errors_of("Global score = 0\nParallel For i = 0 Until 10\n    score += i\nEnd For") ==
          errors_t{ "Parallel: a Parallel For's body runs on other threads, but it uses the "
                    "Global 'score', which only the main thread can, unless it's an Atomic or a "
                    "Mutex" });
}

TEST_CASE("a Channel carries Sendable values between threads", "[sema]") {
    CHECK(probe("Local x = Channel<String>(8)") == "Channel<String>");
    CHECK(probe("Const ch = Channel<Int>(2)\nLocal x = ch.Receive()") == "Int32?");
    CHECK(probe("Const ch = Channel<Int>(2)\nLocal x = ch.TrySend(1)") == "Bool");
    CHECK(errors_of("Import std.core\n"
                    "Const ch = Channel<Int>(2)\n"
                    "Local value = 0\n"
                    "Local got = ch.TryReceive(ByRef value)\n"
                    "ch.Send(value)\n"
                    "ch.Close()\n"
                    "For x In ch\n    Print(\"{x}\")\nEnd For")
              .empty());

    // A Channel is Sendable, so tasks can share one, and it carries only Sendable values.
    CHECK(errors_of("Const ch = Channel<Int>(4)\n"
                    "Function Fill(out: Channel<Int>)\n    out.Send(1)\nEnd Function\n"
                    "Local t = Spawn(Function() => Fill(ch))")
              .empty());
    CHECK(
        errors_of("Class Enemy\nEnd Class\nLocal ch = Channel<Enemy>(2)") ==
        errors_t{ "Channel<Enemy>: a Channel carries Sendable values to other threads, which Enemy "
                  "isn't: a Class's objects stay on the thread that made them, unless it's a "
                  "Shared Class" });

    // It's made with its type and its room.
    CHECK(errors_of("Local ch = Channel(2)") ==
          errors_t{ "Channel(2): a Channel is made with its type and how many it has room for, "
                    "like Channel<Packet>(64)" });
    CHECK(errors_of("Local ch = Channel<Int>(2.5)") ==
          errors_t{ "2.5: a Channel's room is an integer, not Float32" });
    CHECK(errors_of("Const ch = Channel<Int>(2)\nLocal x = ch.Peek()") ==
          errors_t{ "Peek: Channel<Int32> has no member named 'Peek'; it has Send, TrySend, "
                    "Receive, TryReceive, and Close" });
    CHECK(errors_of("Const ch = Channel<Int>(2)\nFor i, x In ch\nEnd For") ==
          errors_t{ "i: a Channel gives one value at a time, so 'For x In' takes one name" });
}

TEST_CASE("an Async Function's call gives its Task, and its code can Await", "[sema]") {
    const std::string_view sum = "Async Function Sum(n: Int): Int\n"
                                 "    Return Await Spawn(Function() => n * 2)\n"
                                 "End Function\n";
    CHECK(probe(std::format("{}Local x = Sum(3)", sum)) == "Task<Int32>");
    CHECK(probe("Async Function Log()\nEnd Function\nLocal x = Log()") == "Task");

    // A generic one's type arguments can come from the Task expected.
    const std::string_view zero = "Import std.core\n"
                                  "Async Function Zero<T: Numeric>(): T\n"
                                  "    Await NextFrame()\n"
                                  "    Return T.Zero()\n"
                                  "End Function\n";
    CHECK(probe(std::format("{}Local t: Task<Int64> = Zero()", zero)) == "Task<Int64>");
    CHECK(probe(std::format("{}Local t: Task<Float>? = Zero()", zero)) ==
          "Task<Float32>? (from Task<Float32>)");
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Async Function Twice(n: Int): Int\n"
                                "    Local first: Int = Await Sum(n)\n"
                                "    Await Spawn(Function() => Print(\"done\"))\n"
                                "    Return first * 2\n"
                                "End Function",
                                sum))
              .empty());

    // Await waits only in an Async Function, or in top-level code, which waits as Wait does,
    // for a Task.
    CHECK(probe(std::format("{}Local x = Await Sum(1)", sum)) == "Int32");
    CHECK(errors_of(std::format("{}Function F(): Int => Await Sum(1)", sum)) ==
          errors_t{ "Await Sum(1): Await goes in an Async Function or lambda, whose code can "
                    "wait without blocking its thread, or in top-level code" });
    CHECK(errors_of("Async Function F(): Int\n    Return Await 5\nEnd Function") ==
          errors_t{ "5: Await waits for a Task, not Int32" });
    CHECK(errors_of(std::format("{}Async Function F()\n"
                                "    Local f = Function() => Await Sum(1)\n"
                                "End Function",
                                sum)) ==
          errors_t{ "Await Sum(1): Await goes in an Async Function or lambda, whose code can "
                    "wait without blocking its thread, or in top-level code" });
    CHECK(errors_of(std::format("{}Global m = Mutex<Int>(0)\n"
                                "Async Function F()\n"
                                "    Lock m As value\n"
                                "        value = Await Sum(1)\n"
                                "    End Lock\n"
                                "End Function",
                                sum)) ==
          errors_t{ "Await Sum(1): Await can't wait in a Lock block, which would hold the Mutex "
                    "meanwhile" });
    // A Const that a lambda's Lock block needs before its turn isn't in that block.
    CHECK(errors_of(std::format("{}Global m = Mutex<Int>(0)\n"
                                "Global f = Function(): Int\n"
                                "    Lock m As v\n"
                                "        Return v + k\n"
                                "    End Lock\n"
                                "End Function\n"
                                "Const k = Await Sum(1)",
                                sum))
              .empty());
    CHECK(errors_of("Async Function Log()\nEnd Function\n"
                    "Async Function F(): Int\n    Return Await Log()\nEnd Function") ==
          errors_t{ "Await Log(): this Await gives nothing, since what it waits for gives "
                    "nothing" });

    // What can be Async.
    CHECK(errors_of("Async Function F(ByRef n: Int)\nEnd Function") ==
          errors_t{ "n: an Async Function's parameter can't be ByRef, since the Function can go "
                    "on after its caller's variable is gone" });
    CHECK(errors_of(std::format("{}Local f: Func(Int): Task<Int> = Sum", sum)) ==
          errors_t{ "Sum: 'Sum' is Async, so it isn't a function value; a lambda can call it" });

    // Its code runs on the main thread.
    CHECK(errors_of(std::format("{}Local t = Spawn(Function() => Sum(1))", sum)) ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Sum', which is Async, "
                    "so its code runs on the main thread" });
}

TEST_CASE("NextFrame, Wait, and Schedulers time coroutines on the main thread", "[sema]") {
    CHECK(probe("Import std.time\nLocal x = Wait(2.Seconds)") == "Task");
    CHECK(probe("Local x = Scheduler()") == "Scheduler");
    CHECK(errors_of("Import std.time\n"
                    "Async Function Door()\n"
                    "    Await Wait(1.5.Seconds)\n"
                    "    Await RealWait(1.Seconds)\n"
                    "    Await NextFrame()\n"
                    "    Await MainThread()\n"
                    "End Function\n"
                    "Global gameplay = Scheduler()\n"
                    "gameplay.Run(Door())\n"
                    "gameplay.Tick(0.016)\n"
                    "Local idle: Bool = gameplay.IsIdle()")
              .empty());

    // A Scheduler runs an Async Function's Task.
    CHECK(errors_of("Global s = Scheduler()\ns.Run(5)") ==
          errors_t{ "5: a Scheduler runs an Async Function's Task, not Int32" });
    // A Run without its Task is one error.
    CHECK(errors_of("Global s = Scheduler()\ns.Run()") ==
          errors_t{ "s.Run(): missing an argument for parameter 'task'" });
    CHECK(errors_of("Global s = Scheduler()\ns.Pause()") ==
          errors_t{ "Pause: a Scheduler has no member named 'Pause'; it has Run, Tick, and "
                    "IsIdle" });
    CHECK(errors_of("Local s = Scheduler(60)") ==
          errors_t{ "Scheduler(60): a Scheduler is made without arguments, like Scheduler()" });
    CHECK(errors_of("Local s: Scheduler<Int> = Scheduler()") ==
          errors_t{ "Scheduler<Int>: 'Scheduler' isn't generic" });

    // Wait and RealWait take std.time's Duration, which needs its Import.
    CHECK(errors_of("Import std.time\nLocal t = Wait(2)") ==
          errors_t{ "2: expected Duration, found Int32" });
    CHECK(errors_of("Local t = RealWait(2)") ==
          errors_t{ "RealWait: RealWait takes a Duration, from std.time, like RealWait(2.Seconds); "
                    "add 'Import std.time'" });

    // The timers and the Schedulers stay on the main thread, but MainThread() can go anywhere.
    CHECK(errors_of("Import std.time\nLocal t = Spawn(Function() => Wait(1.Seconds))") ==
          errors_t{ "Spawn: this work runs on another thread, but it calls 'Wait', which is "
                    "@MainThread" });
    CHECK(errors_of("Local t = Spawn(Function() => Scheduler().IsIdle())") ==
          errors_t{ "Spawn: this work runs on another thread, but it makes a Scheduler, whose "
                    "coroutines run on the main thread" });
    CHECK(errors_of("Function F(s: Scheduler)\n"
                    "    Local t = Spawn(Function() => s.IsIdle())\n"
                    "End Function") ==
          errors_t{ "Function() => s.IsIdle(): 's' is Scheduler, which another thread can't "
                    "have: a Scheduler runs coroutines, which stay on the main thread" });
    CHECK(errors_of("Local t = Spawn(Function() => MainThread())").empty());

    // A program can still declare the names.
    CHECK(errors_of("Class Scheduler\nEnd Class\nLocal s = Scheduler()").empty());
}

TEST_CASE("an Async Function's Task keeps its error, which Await and Wait re-throw", "[sema]") {
    const std::string_view load = "Import std.core\n"
                                  "Class Oops Extends Error\nEnd Class\n"
                                  "Async Function Load(): Int Throws Oops\n"
                                  "    Throw Oops(\"no\")\n"
                                  "End Function\n";
    CHECK(probe(std::format("{}Local x = Load()", load)) == "Task<Int32> Throws Oops");
    CHECK(errors_of(std::format("{}Local x = Load()\n"
                                "Local y = Try x.Wait()\n"
                                "Async Function Twice(): Int Throws Oops\n"
                                "    Return 2 * Try Await Load()\n"
                                "End Function\n"
                                "Async Function Quiet(): Int\n"
                                "    Return (Try? Await Load()) ?? 0\n"
                                "End Function",
                                load))
              .empty());

    // Waiting for it is what throws.
    CHECK(errors_of(std::format("{}Local x = Load().Wait()", load)) ==
          errors_t{ "Load().Wait(): 'Wait' can throw Oops, so calling it needs Try, Try?, or a "
                    "Try block" });
    CHECK(errors_of(std::format("{}Async Function F(): Int\n"
                                "    Return Await Load()\n"
                                "End Function",
                                load)) ==
          errors_t{ "Await Load(): this Await can throw Oops, as its Task's Async Function can, "
                    "so it needs Try, Try?, or a Try block" });
    CHECK(errors_of(std::format("{}Async Function F(): Int\n"
                                "    Return Try Await Load()\n"
                                "End Function",
                                load)) ==
          errors_t{ "Await Load(): Oops isn't caught; catch it in a Try block, or add it to the "
                    "Throws of 'F'" });
}

TEST_CASE("Task<T> Throws E is a type, whose E can be a type parameter that's an error", "[sema]") {
    const std::string_view load = "Import std.core\n"
                                  "Class Oops Extends Error\nEnd Class\n"
                                  "Class Other Extends Error\nEnd Class\n"
                                  "Async Function Load(): Int Throws Oops\n"
                                  "    Throw Oops(\"no\")\n"
                                  "End Function\n"
                                  "Async Function All<T, E: Error>(tasks: (Task<T> Throws E)[]): "
                                  "T[] Throws E\n"
                                  "    Local results: T[] = []\n"
                                  "    For task In tasks\n"
                                  "        results.Add(Try Await task)\n"
                                  "    End For\n"
                                  "    Return results\n"
                                  "End Function\n";
    const auto with_load = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", load, text));
    };

    // Written out, it's the type an Async Function's call gives; after a function's result,
    // Throws is the function's, so a Task's type there goes in parentheses.
    CHECK(probe(std::format("{}Local t: Task<Int> Throws Oops = Load()", load)) ==
          "Task<Int32> Throws Oops");
    CHECK(probe(std::format("{}Local t: (Task<Int> Throws Oops)[] = [Load()]", load)) ==
          "(Task<Int32> Throws Oops)[]");
    CHECK(probe(std::format("{}Function Start(): (Task<Int> Throws Oops) => Load()\n"
                            "Local t = Start()",
                            load)) == "Task<Int32> Throws Oops");
    CHECK(with_load("Function Start(): Task<Int> Throws Oops => Load()") ==
          errors_t{ "Load(): expected Task<Int32>, found Task<Int32> Throws Oops" });

    // E comes from the Tasks, and what waiting throws is the instance's.
    CHECK(probe(std::format("{}Local t = All([Load(), Load()])", load)) ==
          "Task<Int32[]> Throws Oops");
    CHECK(with_load("Async Function F(): Int\n"
                    "    Return (Await All([Load()]))[0]\n"
                    "End Function") ==
          errors_t{ "Await All([Load()]): this Await can throw Oops, as its Task's Async Function "
                    "can, so it needs Try, Try?, or a Try block" });
    CHECK(with_load("Function Pass<E: Error>(error: E): Int Throws E\n"
                    "    Throw error\n"
                    "End Function\n"
                    "Function Name<E: Error>(task: Task<Int> Throws E): String\n"
                    "    Try\n"
                    "        Return \"{task.Wait()}\"\n"
                    "    Catch failed As E\n"
                    "        Local error: Error = failed\n"
                    "        Return error.Message\n"
                    "    End Try\n"
                    "End Function\n"
                    "Local x = Try? Pass(Oops(\"a\"))\n"
                    "Local y = Name(Load())")
              .empty());
    CHECK(with_load("Function Pass<E: Error>(error: E): Int Throws E\n"
                    "    Throw error\n"
                    "End Function\n"
                    "Local x = Pass(Oops(\"a\"))") ==
          errors_t{ "Pass(Oops(\"a\")): 'Pass' can throw Oops, so calling it needs Try, Try?, or "
                    "a Try block" });
    CHECK(with_load("Class Box<E: Error>\n"
                    "    Method Fail(error: E): Int Throws E\n"
                    "        Throw error\n"
                    "    End Method\n"
                    "End Class\n"
                    "Local x = Try? Box<Oops>().Fail(Oops(\"a\"))\n"
                    "Local y = Box<Other>().Fail(Other(\"b\"))") ==
          errors_t{ "Box<Other>().Fail(Other(\"b\")): 'Fail' can throw Other, so calling it needs "
                    "Try, Try?, or a Try block" });

    // A Task that throws converts to one that throws what covers its errors, and no further.
    CHECK(with_load("Local t: Task<Int> Throws Error = Load()\n"
                    "Local u: Task<Int> Throws Other, Oops = Load()")
              .empty());
    CHECK(with_load("Local t: Task<Int> Throws Error = Load()\n"
                    "Local u: Task<Int> Throws Oops = t\n"
                    "Local v: Task<Int> Throws Other = Load()\n"
                    "Local w: Task<Int> = Load()") ==
          errors_t{
              "t: expected Task<Int32> Throws Oops, found Task<Int32> Throws Error",
              "Load(): expected Task<Int32> Throws Other, found Task<Int32> Throws Oops",
              "Load(): expected Task<Int32>, found Task<Int32> Throws Oops",
          });
}

TEST_CASE("a Task's Throws lists errors, and a type parameter there says it's one", "[sema]") {
    const std::string_view oops = "Import std.core\n"
                                  "Class Oops Extends Error\nEnd Class\n";
    const auto with_oops = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", oops, text));
    };

    CHECK(with_oops("Local a: Task<Int> Throws Int\n"
                    "Local b: Task<Int> Throws Oops?\n"
                    "Local c: Task<Int> Throws Oops, Error") ==
          errors_t{
              "Int: Throws lists Classes that extend Error, not Int32",
              "Oops?: Throws lists Classes that extend Error, not Oops?; a Task's type with a "
              "Throws goes in parentheses before a '?', as in (Task<T> Throws Oops)?",
              "Oops: Oops is already covered by this Throws",
          });
    CHECK(with_oops("Function F<E>(task: Task<Int> Throws E)\n"
                    "End Function\n"
                    "Function G<E>() Throws E\n"
                    "End Function") ==
          errors_t{
              "E: E is in a Throws, so it needs a constraint that says it's an error, like <E: "
              "Error>",
              "E: E is in a Throws, so it needs a constraint that says it's an error, like <E: "
              "Error>",
          });
    CHECK(with_oops("Function Make<E: Error>(): Int => 0\nLocal x = Make()") ==
          errors_t{ "Make: can't tell what E is in this call; write the type arguments, like "
                    "Make<Error>(...)" });
    CHECK(with_oops("Function F<E: Error>(task: Task<Int> Throws E)\n"
                    "End Function\n"
                    "Local f = F<String>") ==
          errors_t{ "F<String>: String isn't a Class that extends Error, which 'F' needs for E" });
}

TEST_CASE("a Task's Throws, written out, is checked, inferred, and named as a function's is",
          "[sema]") {
    const std::string_view oops = "Import std.core\n"
                                  "Class Oops Extends Error\nEnd Class\n"
                                  "Class Bad Extends Error\nEnd Class\n"
                                  "Async Function Two(): Int Throws Bad, Oops\n"
                                  "    Throw Bad(\"two\")\n"
                                  "End Function\n";
    const auto with_oops = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", oops, text));
    };

    // A type parameter covered by another, or listed twice, is reported as a Class is.
    CHECK(with_oops("Function G<E: Error>(task: Task<Int> Throws E, Error): Int => 0\n"
                    "Function H<E: Error>(task: Task<Int> Throws E, E): Int => 0") ==
          errors_t{
              "E: E is already covered by this Throws",
              "E: E is already covered by this Throws",
          });

    // E is what a Task throws besides what the written Classes cover, in whatever order.
    CHECK(with_oops("Function Take<E: Error>(task: Task<Int> Throws Oops, E): Int => 0\n"
                    "Local x = Take(Two())")
              .empty());

    // A Task with a Throws goes in parentheses in a name where a ',' follows it.
    CHECK(with_oops("Local y: ((Task<Int> Throws Oops), Bad, Int) = (Two(), Bad(\"b\"), 4)\n"
                    "Local z: ((Task<Int> Throws Oops, Bad), Int) = y\n"
                    "Local t: Task<Int> Throws Oops = Null") ==
          errors_t{
              "(Two(), Bad(\"b\"), 4): expected ((Task<Int32> Throws Oops), Bad, Int32), found "
              "((Task<Int32> Throws Bad, Oops), Bad, Int32)",
              "y: expected ((Task<Int32> Throws Oops, Bad), Int32), found ((Task<Int32> Throws "
              "Oops), Bad, Int32)",
              "Null: Task<Int32> Throws Oops can't be Null; (Task<Int32> Throws Oops)? can",
          });

    // A bad Throws is reported once, and nothing follows from it.
    CHECK(with_oops("Local t: Task<Int> Throws String = Two()\n"
                    "Function F<T>(x: T)\n"
                    "    Where T: Task<Int> Throws String\n"
                    "End Function") ==
          errors_t{
              "String: Throws lists Classes that extend Error, not String",
              "Task<Int> Throws String: Task<Int32> Throws String isn't an Interface",
              "String: Throws lists Classes that extend Error, not String",
          });

    // An E still being inferred tells a lambda's parameter nothing.
    CHECK(with_oops("Function Run<E: Error>(f: Func(Task<Int> Throws E): Int): Int => 1\n"
                    "Local x = Run(Function(t) => 0)") ==
          errors_t{
              "Run: can't tell what E is in this call; write the type arguments, like "
              "Run<Error>(...)",
              "t: 't' needs a type, since nothing here says what this lambda takes",
          });

    // And a type that grows only through a Throws grows all the same.
    CHECK(with_oops("Class Wrapped<T> Extends Error\nEnd Class\n"
                    "Function Deep<T>(n: Int, x: T): Int\n"
                    "    If n == 0 Then Return 0\n"
                    "    Local t: (Task<Int> Throws Wrapped<T>)? = Null\n"
                    "    Return Deep(n - 1, t)\n"
                    "End Function\n"
                    "Local d = Deep(3, 1)") ==
          errors_t{ "Deep(n - 1, t): 'Deep' gets (Task<Int32> Throws Wrapped<T>)? for T here, "
                    "which leads back to this code, so it would need a larger instance each "
                    "time, without end" });

    // Waiting for one that's written out throws what it lists.
    CHECK(with_oops("Function W(t: Task<Int> Throws Oops): Int => t.Wait()\n"
                    "Async Function A(t: Task<Int> Throws Oops): Int\n"
                    "    Return Await t\n"
                    "End Function") ==
          errors_t{
              "t.Wait(): 'Wait' can throw Oops, so calling it needs Try, Try?, or a Try block",
              "Await t: this Await can throw Oops, as its Task's Async Function can, so it needs "
              "Try, Try?, or a Try block",
          });

    // A Catch of E covers what Error doesn't, and is reached by any error.
    CHECK(with_oops("Function C<E: Error>(e: E): Int\n"
                    "    Try\n"
                    "        Throw e\n"
                    "    Catch c As Error\n"
                    "        Return 1\n"
                    "    Catch d As E\n"
                    "        Return 2\n"
                    "    End Try\n"
                    "End Function\n"
                    "Function D<E: Error>(e: E): Int\n"
                    "    Try\n"
                    "        Throw Oops(\"x\")\n"
                    "    Catch d As E\n"
                    "        Return 2\n"
                    "    Catch o As Oops\n"
                    "        Return 3\n"
                    "    End Try\n"
                    "End Function") ==
          errors_t{ "E: this Catch is already covered by an earlier Catch" });

    // A Throws goes only on the built-in Task's type.
    CHECK(with_oops("Class Task<T>\n"
                    "    Field Value: T?\n"
                    "End Class\n"
                    "Local t: Task<Int> Throws Oops = Task<Int>()") ==
          errors_t{ "Oops: only a Task's type takes a Throws, and 'Task' here is a type of the "
                    "code's" });
    CHECK(with_oops("Function F<Task>(x: Task Throws Oops)\nEnd Function") ==
          errors_t{ "Task Throws Oops: 'Task' is a type parameter, so it takes no type arguments "
                    "or Throws" });
}

TEST_CASE("a generic type's Throws E is its instance's, through bases, Interfaces, and loops",
          "[sema]") {
    const std::string_view oops = "Import std.core\n"
                                  "Class Oops Extends Error\nEnd Class\n"
                                  "Class Base<E: Error>\n"
                                  "    Method Fail(error: E): Int Throws E\n"
                                  "        Throw error\n"
                                  "    End Method\n"
                                  "End Class\n"
                                  "Interface Runner<E: Error>\n"
                                  "    Method Run(): Int Throws E\n"
                                  "    Method Twice(): Int Throws E\n"
                                  "        Return (Try Run()) * 2\n"
                                  "    End Method\n"
                                  "End Interface\n";
    const auto with_oops = [&](const std::string_view text) {
        return errors_of(std::format("{}{}", oops, text));
    };

    // An Override, of an instance or of the Class's own type parameter.
    CHECK(with_oops("Class Derived Extends Base<Oops>\n"
                    "    Override Method Fail(error: Oops): Int Throws Oops\n"
                    "        Throw error\n"
                    "    End Method\n"
                    "End Class\n"
                    "Class Generic<F: Error> Extends Base<F>\n"
                    "    Override Method Fail(error: F): Int Throws F\n"
                    "        Throw error\n"
                    "    End Method\n"
                    "End Class")
              .empty());
    CHECK(with_oops("Class Wrong<F: Error> Extends Base<F>\n"
                    "    Override Method Fail(error: F): Int Throws Oops\n"
                    "        Throw Oops(\"x\")\n"
                    "    End Method\n"
                    "End Class") ==
          errors_t{ "Fail: 'Fail' can't throw Oops, since Base<F>'s doesn't" });

    // What implements an Interface's Method, and its default, called on the implementing Class.
    CHECK(with_oops("Class Job Implements Runner<Oops>\n"
                    "    Method Run(): Int Throws Oops\n"
                    "        Throw Oops(\"job\")\n"
                    "    End Method\n"
                    "End Class\n"
                    "Function Go(): Int Throws Oops\n"
                    "    Return (Try Job().Run()) + (Try Job().Twice())\n"
                    "End Function\n"
                    "Local x = Job().Twice()") ==
          errors_t{ "Job().Twice(): 'Twice' can throw Oops, so calling it needs Try, Try?, or a "
                    "Try block" });

    // A New taken from a generic base, and a For ... In over a generic type's Iterate.
    CHECK(with_oops("Struct Counting Implements Iterator<Int>\n"
                    "    Field at: Int\n"
                    "    ByRef Method Next(): Int?\n"
                    "        at += 1\n"
                    "        Return at < 3 ? at : Null\n"
                    "    End Method\n"
                    "End Struct\n"
                    "Class Box<E: Error>\n"
                    "    New(failure: E?) Throws E\n"
                    "        If failure != Null Then Throw failure\n"
                    "    End New\n"
                    "    Method Iterate(): Counting Throws E => Counting()\n"
                    "End Class\n"
                    "Class Taker Extends Box<Oops>\n"
                    "End Class\n"
                    "Function Sum(items: Box<Oops>): Int Throws Oops\n"
                    "    Local taker = Try Taker(Null)\n"
                    "    Local total = 0\n"
                    "    For n In Try items\n"
                    "        total += n\n"
                    "    End For\n"
                    "    Return total\n"
                    "End Function\n"
                    "Local t = Taker(Null)\n"
                    "For n In Box<Oops>(Null)\n"
                    "End For") ==
          errors_t{
              "Taker(Null): 'Taker' can throw Oops, so calling it needs Try, Try?, or a Try "
              "block",
              "Box<Oops>(Null): 'Box<E>' can throw Oops, so calling it needs Try, Try?, or a "
              "Try block",
              "Box<Oops>(Null): Box<Oops>'s Iterate can throw Oops, so the loop needs 'In Try "
              "Box<Oops>(Null)', or a Try block around it",
          });
}

TEST_CASE("a Method can be Async, and its overrides are too", "[sema]") {
    CHECK(probe("Class C\n"
                "    Async Method M(): Int\n"
                "        Return 1\n"
                "    End Method\n"
                "End Class\n"
                "Local x = C().M()") == "Task<Int32>");

    // "c?.M()" gives a Task<T>?, Null when c is.
    const std::string_view maybe = "Class C\n"
                                   "    Async Method M(): Int\n"
                                   "        Return 1\n"
                                   "    End Method\n"
                                   "    Async Method N()\n"
                                   "    End Method\n"
                                   "End Class\n"
                                   "Function Pick(): C? => Null\n";
    CHECK(probe(std::format("{}Local x = Pick()?.M()", maybe)) == "Task<Int32>?");
    CHECK(probe(std::format("{}Local x = Pick()?.N()", maybe)) == "Task?");
    CHECK(errors_of(std::format("{}Local x = Pick()?.M()\nLocal y = x.Wait()", maybe)) ==
          errors_t{ "x: 'x' may be Null; check that it isn't, or use '?.'" });
    CHECK(errors_of("Class A\n"
                    "    Async Method M(): Int\n"
                    "        Return 1\n"
                    "    End Method\n"
                    "End Class\n"
                    "Class B Extends A\n"
                    "    Override Async Method M(): Int\n"
                    "        Return 2 * Await Super.M()\n"
                    "    End Method\n"
                    "End Class\n"
                    "Struct S\n"
                    "    Field x: Int\n"
                    "    Async Method M(): Int\n"
                    "        Return x\n"
                    "    End Method\n"
                    "    Static Async Method N()\n"
                    "    End Method\n"
                    "End Struct")
              .empty());

    CHECK(errors_of("Class A\n    Async Method M()\n    End Method\nEnd Class\n"
                    "Class B Extends A\n    Override Method M()\n    End Method\nEnd Class") ==
          errors_t{ "M: 'M' overrides A's, which is Async, so it has to be Async too" });
    CHECK(
        errors_of("Class A\n    Method M()\n    End Method\nEnd Class\n"
                  "Class B Extends A\n    Override Async Method M()\n    End Method\nEnd Class") ==
        errors_t{ "M: 'M' is Async, so it can't override A's, which isn't" });
    CHECK(errors_of("Interface I\n    Method M()\nEnd Interface\n"
                    "Class C Implements I\n    Async Method M()\n    End Method\nEnd Class") ==
          errors_t{ "I: 'M' is Async, so it can't implement I's, which isn't" });
    CHECK(errors_of("Interface Loader\n    Async Method Load(): Int\nEnd Interface") ==
          errors_t{ "Load: an Interface's Method can't be Async yet" });
    CHECK(errors_of("Struct S\n    Field x: Int\n    Async ByRef Method M()\n    End Method\n"
                    "End Struct") ==
          errors_t{ "M: a ByRef Method can't be Async, since it can go on after its caller's "
                    "variable is gone" });
    CHECK(errors_of("Class C\n    Async Method Destroy()\n    End Method\nEnd Class") ==
          errors_t{ "Destroy: Destroy can't be Async, since it runs when the last reference "
                    "goes" });
}

TEST_CASE("an Async lambda's call gives its Task, and its code can Await", "[sema]") {
    CHECK(probe("Local x = Async Function() => 5") == "Func(): Task<Int32>");
    CHECK(probe("Import std.core\n"
                "Local x = Async Function(text: String)\n"
                "    Await NextFrame()\n"
                "    Print(text)\n"
                "End Function") == "Func(String): Task");
    CHECK(errors_of("Local f: Func(Int): Task<Int> = Async Function(n) => n + 1\n"
                    "Local t = f(2)\n"
                    "Global m = Mutex<Int>(0)\n"
                    "Lock m As value\n"
                    "    Local g = Async Function()\n"
                    "        Await NextFrame()\n"
                    "    End Function\n"
                    "End Lock")
              .empty());

    // One that returns nothing can wait after its '=>', as it can make a call.
    CHECK(errors_of("Local f: Func(): Task = Async Function() => Await NextFrame()\n"
                    "Local g: Func(): Task = Async Function() => Await Spawn(Function() => 5)\n"
                    "Local h = Async Function() => Await NextFrame()")
              .empty());
    CHECK(errors_of("Local f: Func(): Task = Async Function() => 5") ==
          errors_t{ "5: this lambda returns nothing, so its '=>' needs a call, like "
                    "'=> Print(x)'" });

    // Await goes in its own code, not in a lambda inside it.
    CHECK(errors_of("Local f = Async Function()\n"
                    "    Local g = Function() => Await NextFrame()\n"
                    "End Function") ==
          errors_t{ "Await NextFrame(): Await goes in an Async Function or lambda, whose code can "
                    "wait without blocking its thread, or in top-level code" });

    // Its code runs on the main thread, so it isn't work for another one either.
    CHECK(errors_of("Local t = Spawn(Function() => (Async Function() => 1)())") ==
          errors_t{ "Spawn: this work runs on another thread, but it makes an Async lambda, "
                    "whose code runs on the main thread" });
    CHECK(errors_of("Import std.core\n"
                    "Local t = Spawn(Async Function()\n"
                    "    Await NextFrame()\n"
                    "End Function)\n"
                    "Local th = Thread.Start(Async Function() => 1)") ==
          errors_t{ "Spawn: this work runs on another thread, but it's an Async lambda, whose "
                    "code runs on the main thread",
                    "Thread.Start: this work runs on another thread, but it's an Async lambda, "
                    "whose code runs on the main thread" });
}

TEST_CASE("a lambda in generic code can loop over the members of its type parameters", "[sema]") {
    const std::string_view describe = "Import std.core\n"
                                      "Function Describe<T>(value: T): String\n"
                                      "    Local text = \"\"\n"
                                      "    Local add = Function()\n"
                                      "        For Field f: F In T\n"
                                      "            Where F: Printable\n"
                                      "            text = text + \"{f.Get(value)}\"\n"
                                      "        End For\n"
                                      "    End Function\n"
                                      "    add()\n"
                                      "    Return text\n"
                                      "End Function\n";
    CHECK(errors_of(std::format("{}Local s = Describe((1, 2))", describe)).empty());
    CHECK(errors_of("Function Names<T>(): String\n"
                    "    Local names = Function(): String\n"
                    "        Local text = \"\"\n"
                    "        For Field f In T\n"
                    "            text = text + f.Name\n"
                    "        End For\n"
                    "        Return text\n"
                    "    End Function\n"
                    "    Return names()\n"
                    "End Function")
              .empty());

    // Each call checks the loop's Where lines.
    CHECK(errors_of(std::format("{}Class Thing\nEnd Class\nStruct Holder\n    Field T: Thing\n"
                                "End Struct\nLocal s = Describe(Holder(Thing()))",
                                describe)) ==
          errors_t{ "Describe: 'Describe' needs each field of Holder to implement Printable, but "
                    "its field 'T' is Thing, which doesn't" });
}

// Modules

TEST_CASE("std's names need their Import, std.core's too", "[sema]") {
    CHECK(errors_of("Print(\"hi\")") ==
          errors_t{ "Print: 'Print' is in std.core, which this file doesn't import" });
    CHECK(errors_of("Class Oops Extends Error\nEnd Class") ==
          errors_t{ "Error: 'Error' is in std.core, which this file doesn't import" });
    CHECK(errors_of("Function Show<T: Printable>(x: T): String => \"{x}\"") ==
          errors_t{ "Printable: 'Printable' is in std.core, which this file doesn't import" });
    CHECK(errors_of("Struct Steps Implements Iterator<Int>\n"
                    "    Method Next(): Int? => Null\n"
                    "End Struct") ==
          errors_t{ "Iterator<Int>: 'Iterator' is in std.core, which this file doesn't import" });
    CHECK(errors_of("Import std.core\n"
                    "Class Oops Extends Error\n"
                    "End Class\n"
                    "Print(\"hi\")")
              .empty());

    // What a message suggests from std.core says it needs the Import.
    CHECK(errors_of("Function Show<T>(x: T): String => \"{x}\"") ==
          errors_t{ "x: T can't be shown in a string without a constraint, like <T: Printable>, "
                    "after Import std.core" });
    CHECK(errors_of("Struct P\n    Field x: Int\nEnd Struct\nLocal s = \"{P(1)}\"") ==
          errors_t{ "P(1): P can't be shown in a string; implement Printable, with a ToString, "
                    "after Import std.core" });

    // The language's own features go through std.core without it: Throw and Catch, ==,
    // strings, For ... In, and a Map literal.
    CHECK(errors_with("Import \"./lib/oops\"\n"
                      "Try\n"
                      "    Fail()\n"
                      "Catch e\n"
                      "    Local message = e.Message\n"
                      "End Try\n"
                      "Local m = {1: \"a\"}\n"
                      "Local same = [1] == [1] && m.Count == 1\n"
                      "For x In [1, 2]\n"
                      "    Local text = \"{x} {m.Count}\"\n"
                      "End For",
                      {
                          {
                              .path = "lib/oops.fb",
                              .text = "Import std.core\n"
                                      "Public Function Fail() Throws Error\n"
                                      "    Throw Error(\"oops\")\n"
                                      "End Function\n",
                          },
                      })
              .empty());
}

TEST_CASE("a name the file doesn't import says which module has it", "[sema]") {
    // std.collections isn't loaded, or only std.collections.map is, which a Map literal needs.
    CHECK(errors_of("Local s = Stack<Int>()") ==
          errors_t{ "Stack<Int>: 'Stack' is in std.collections, which this file doesn't import" });
    CHECK(errors_of("Local m: Map<String, Int>? = Null") ==
          errors_t{ "Map<String, Int>: 'Map' is in std.collections, which this file doesn't "
                    "import" });
    CHECK(errors_of("Local xs = [2, 1]\nxs.Sort()") ==
          errors_t{ "Sort: 'Sort' is in std.collections, which this file doesn't import" });
    CHECK(errors_of("Import std.collections\n"
                    "Local s = Stack<Int>()\n"
                    "Local xs = [2, 1]\n"
                    "xs.Sort()")
              .empty());

    // What its import leaves out.
    CHECK(errors_of("Import std.collections As C\nLocal s = Stack<Int>()") ==
          errors_t{ "Stack<Int>: 'Stack' is in std.collections, which this file imports as C, so "
                    "it's C.Stack" });
    CHECK(errors_of("From std.collections Import Queue\nLocal s = Stack<Int>()") ==
          errors_t{ "Stack<Int>: 'Stack' is in std.collections, but this file's From line "
                    "doesn't name it" });
}

TEST_CASE("a type or variable two imports give is ambiguous where it's used", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Public Class Node\nEnd Class\nPublic Global count = 1\n" },
        { .path = "lib/b.fb", .text = "Public Class Node\nEnd Class\nPublic Global count = 2\n" },
    };
    const std::string_view both = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    CHECK(errors_with(std::format("{}Local n: Node? = Null\nLocal c = count", both), modules) ==
          errors_t{
              "Node: 'Node' is ambiguous here: lib.a and lib.b each declare it",
              "count: 'count' is ambiguous here: lib.a and lib.b each declare it",
          });

    // Neither one's type follows from it, so what's made or read gives no more errors.
    CHECK(errors_with(std::format("{}Local m = Node()\nLocal y = m.y\nLocal c = count + 1\n"
                                  "Local d = c.Missing",
                                  both),
                      modules) ==
          errors_t{
              "Node: 'Node' is ambiguous here: lib.a and lib.b each declare it",
              "count: 'count' is ambiguous here: lib.a and lib.b each declare it",
          });

    // Qualified, or from one import, it isn't.
    CHECK(errors_with(
              std::format("{}Local n: a.Node? = b.Node() As? a.Node\nLocal c = b.count", both),
              modules) == errors_t{ "b.Node() As? a.Node: lib.b.Node and lib.a.Node aren't "
                                    "related, so this is always Null" });
    CHECK(
        errors_with("Import \"./lib/a\"\nLocal n: Node? = Null\nLocal c = count", modules).empty());
}

TEST_CASE("a module's own declaration hides an imported one", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Class Node\nEnd Class\nPublic Function Make(): Node => Node()\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Class Node\n"
                      "    Field mine: Int = 1\n"
                      "End Class\n"
                      "Local n = Node()\n"
                      "Local m = n.mine\n"
                      "Local theirs: a.Node = Make()",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Class Node\n"
                      "End Class\n"
                      "Local n: Node = Make()",
                      modules) == errors_t{ "Make(): expected test.Node, found lib.a.Node" });
}

TEST_CASE("Functions of one name across a module and its imports are one overload set", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Public Function Show(x: Int): String => \"a\"\n" },
        { .path = "lib/b.fb", .text = "Public Function Show(x: String): String => x\n" },
    };
    const std::string_view both = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    CHECK(errors_with(std::format("{}"
                                  "Function Show(x: Bool): String => \"own\"\n"
                                  "Local s = Show(1) + Show(\"b\") + Show(True)",
                                  both),
                      modules)
              .empty());
    CHECK(errors_with(std::format("{}Local s = Show(1.5)", both), modules) ==
          errors_t{ "Show: no overload of 'Show' takes (Float32)" });
}

TEST_CASE("Import and From lines come before a file's other statements", "[sema]") {
    CHECK(errors_of("// What it uses.\n"
                    "\n"
                    "Import std.collections\n"
                    "From std.collections Import Stack\n"
                    "Local s = Stack<Int>()")
              .empty());
    CHECK(errors_of("Local x = 1\nImport std.collections") ==
          errors_t{ "Import std.collections: Import lines go at the top of a file, before its "
                    "other statements" });
    CHECK(errors_of("Function F()\n    From std.collections Import Stack\nEnd Function") ==
          errors_t{ "From std.collections Import Stack: From lines go at the top of a file, "
                    "before its other statements" });
}

TEST_CASE("a From line's names are declared in its module", "[sema]") {
    CHECK(errors_of("From std.collections Import Stack, Nope") ==
          errors_t{ "Nope: 'Nope' isn't declared in std.collections" });

    // It brings in only those.
    CHECK(errors_of("From std.collections Import Stack\n"
                    "Local s = Stack<Int>()\n"
                    "Local q = Queue<Int>()") ==
          errors_t{ "Queue<Int>: 'Queue' is in std.collections, but this file's From line "
                    "doesn't name it" });
}

TEST_CASE("a qualifier stands for its module only before a '.'", "[sema]") {
    const std::string_view alias = "Import std.collections As C\n";
    CHECK(errors_of(std::format("{}Local s: C.Stack<Int> = C.Stack<Int>()\ns.Push(1)", alias))
              .empty());
    CHECK(errors_of(std::format("{}Local c = C", alias)) ==
          errors_t{ "C: 'C' is an imported module, not a value; use one of its names, like "
                    "C.Name" });
    CHECK(errors_of(std::format("{}C()", alias)) ==
          errors_t{ "C: 'C' is an imported module, not a function; call one of its Functions, "
                    "like C.Name()" });
    CHECK(errors_of(std::format("{}Local x: C? = Null", alias)) ==
          errors_t{ "C: 'C' is an imported module, not a type; use one of its types, like "
                    "C.Name" });
    CHECK(errors_of(std::format("{}Local s = C.Nope()", alias)) ==
          errors_t{ "Nope: 'Nope' isn't declared in std.collections" });
    CHECK(errors_of("Local s: D.Stack<Int>? = Null") ==
          errors_t{ "D: 'D' isn't an imported module, so it can't come before '.'" });

    // The dotted name's last part is a qualifier too, which a declaration can't take.
    CHECK(errors_of("Import std.collections\nLocal s = collections.Stack<Int>()").empty());
    CHECK(errors_of(std::format("{}Class C\nEnd Class", alias)) ==
          errors_t{ "C: 'C' is already the name of an imported module" });
}

TEST_CASE("a Global can't read one that initializes after it", "[sema]") {
    // x imports y, which imports x back, so y's Globals initialize first. A Const known at
    // compile time is known before any of them.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/x.fb",
            .text = "Import \"./y\"\n"
                    "Public Global first = Make()\n"
                    "Function Make(): Int => 1\n"
                    "Public Const KNOWN = 4\n",
        },
        {
            .path = "lib/y.fb",
            .text = "Import \"./x\"\nGlobal second = first + 1\nGlobal third = KNOWN + 1\n",
        },
    };
    CHECK(errors_with("Import \"./lib/x\"\nGlobal mine = first + 1", modules) ==
          errors_t{ "lib/y.fb: first: 'first' isn't initialized yet; lib.x's Globals and Consts "
                    "initialize after this module's, since a module's imports initialize first, "
                    "depth-first from the main module" });
}

TEST_CASE("an Extend's Methods are seen where its module is imported", "[sema]") {
    // user.fb imports twice.fb, whose Twice it sees, and so does the main file only when it
    // imports twice.fb too. Int implements Shape everywhere.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/shape.fb",
            .text = "Public Interface Shape\n    Method Area(): Int\nEnd Interface\n",
        },
        {
            .path = "lib/twice.fb",
            .text = "Import \"./shape\"\n"
                    "Extend Int Implements Shape\n"
                    "    Public Method Twice(): Int => Self * 2\n"
                    "    Method Area(): Int => Self\n"
                    "End Extend\n",
        },
        {
            .path = "lib/user.fb",
            .text = "Import \"./twice\"\nPublic Function Four(): Int => (2).Twice()\n",
        },
        {
            .path = "lib/again.fb",
            .text = "Extend Int\n    Public Method Twice(): Int => Self + Self\nEnd Extend\n",
        },
    };
    const std::string_view total = "Function Total<T: Shape>(x: T): Int => x.Area()\n";
    CHECK(errors_with(std::format("Import \"./lib/shape\"\n"
                                  "Import \"./lib/user\"\n"
                                  "{}"
                                  "Local t = Total(3) + Four()\n"
                                  "Local n = (2).Twice()",
                                  total),
                      modules) ==
          errors_t{ "Twice: 'Twice' is in lib.twice, which this file doesn't import: Import "
                    "\"./lib/twice\"" });
    CHECK(errors_with("Import \"./lib/twice\"\nLocal n = (2).Twice()", modules).empty());

    // Two modules' Extends can each add a Twice, which a call that sees both can't choose.
    CHECK(errors_with("Import \"./lib/again\"\nLocal n = (2).Twice()", modules).empty());
    CHECK(errors_with("Import \"./lib/twice\"\n"
                      "Import \"./lib/again\"\n"
                      "Local two = 2\n"
                      "Local n = two.Twice()",
                      modules) == errors_t{ "two.Twice: the call to 'Twice' is ambiguous" });
}

TEST_CASE("what two modules' Extends add to a Struct is ambiguous only where both are seen",
          "[sema]") {
    // a declares Node and adds a Twice and a Big to it, and so does b, which imports a, with
    // another Twice too. c only names Node.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Struct Node\n"
                    "    Public Field V: Int = 1\n"
                    "End Struct\n"
                    "Extend Node\n"
                    "    Public Method Twice(): Int => V * 2\n"
                    "    Public Property Big: Int\n"
                    "        Get => V * 10\n"
                    "    End Property\n"
                    "End Extend\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./a\"\n"
                    "Extend Node\n"
                    "    Public Method Twice(): Int => V * 200\n"
                    "    Public Method Twice(times: Int): Int => V * times\n"
                    "    Public Property Big: Int\n"
                    "        Get => V * 1000\n"
                    "    End Property\n"
                    "End Extend\n"
                    "Public Function UseB(): Int => Node().Twice() + Node().Big\n",
        },
        {
            .path = "lib/c.fb",
            .text = "Import \"./a\"\nPublic Function MakeNode(): Node => Node()\n",
        },
    };
    const std::string_view both = "Import \"./lib/a\"\nImport \"./lib/b\"\n";
    CHECK(errors_with(std::format("{}Local n = UseB() + Node().Twice(3)", both), modules).empty());
    // Each Twice is an overload, so only a call that two fit is ambiguous, but a Property is
    // ambiguous wherever it's used.
    CHECK(errors_with(std::format("{}Local n = Node()\nLocal t = n.Twice() + n.Big", both),
                      modules) ==
          errors_t{
              "n.Twice: the call to 'Twice' is ambiguous",
              "Big: 'Big' is ambiguous here: lib.a and lib.b each add it to Node",
          });
    CHECK(errors_with(std::format("{}"
                                  "Extend Node\n"
                                  "    Method Huge(): Int => Big * 2\n"
                                  "End Extend",
                                  both),
                      modules) ==
          errors_t{ "Big: 'Big' is ambiguous here: lib.a and lib.b each add it to Node" });

    // Seeing only b's, it's b's.
    CHECK(errors_with("Import \"./lib/c\"\n"
                      "Import \"./lib/b\"\n"
                      "Local t = MakeNode().Twice() + MakeNode().Big",
                      modules)
              .empty());
}

TEST_CASE("what an Extend adds that the code doesn't see hides nothing", "[sema]") {
    // names.fb adds a Name to Base, and derived.fb, which doesn't see it, one to Derived, which
    // hides Base's where both are seen. user.fb has the program load both.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/base.fb",
            .text = "Public Class Base\nEnd Class\nPublic Class Derived Extends Base\nEnd Class\n",
        },
        {
            .path = "lib/names.fb",
            .text = "Import \"./base\"\n"
                    "Extend Base\n"
                    "    Public Method Name(): String => \"base\"\n"
                    "End Extend\n",
        },
        {
            .path = "lib/derived.fb",
            .text = "Import \"./base\"\n"
                    "Extend Derived\n"
                    "    Public Method Name(): String => \"derived\"\n"
                    "End Extend\n",
        },
        {
            .path = "lib/user.fb",
            .text = "Import \"./derived\"\nImport \"./names\"\nFunction Use()\nEnd Function\n",
        },
    };
    CHECK(errors_with("Import \"./lib/base\"\n"
                      "Import \"./lib/names\"\n"
                      "Import \"./lib/user\"\n"
                      "Local s = Derived().Name()",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/base\"\n"
                      "Import \"./lib/names\"\n"
                      "Import \"./lib/derived\"\n"
                      "Local s = Derived().Name() + Base().Name()",
                      modules)
              .empty());

    // A Class whose module doesn't see names.fb has a Name of its own.
    CHECK(errors_with("Import \"./lib/base\"\n"
                      "Import \"./lib/user\"\n"
                      "Class Mine Extends Base\n"
                      "    Method Name(): String => \"mine\"\n"
                      "End Class\n"
                      "Local s = Mine().Name()",
                      modules)
              .empty());
}

TEST_CASE("an Interface an Extend lists is implemented by what the Extend's module sees",
          "[sema]") {
    // plain.fb and other.fb each add a Show without listing Shown. listed.fb sees plain's, but
    // its own hides it; bare.fb sees neither, and both.fb sees both. c.fb's C lists Shown, and
    // only cshow.fb, which c.fb doesn't import, adds its Show.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/shown.fb",
            .text = "Public Struct P\n"
                    "    Public Field X: Int\n"
                    "End Struct\n"
                    "Public Interface Shown\n"
                    "    Method Show(): String\n"
                    "End Interface\n",
        },
        {
            .path = "lib/plain.fb",
            .text = "Import \"./shown\"\n"
                    "Extend P\n"
                    "    Public Method Show(): String => \"plain\"\n"
                    "End Extend\n",
        },
        {
            .path = "lib/other.fb",
            .text = "Import \"./shown\"\n"
                    "Extend P\n"
                    "    Public Method Show(): String => \"other\"\n"
                    "End Extend\n",
        },
        {
            .path = "lib/listed.fb",
            .text = "Import \"./shown\"\n"
                    "Import \"./plain\"\n"
                    "Extend P Implements Shown\n"
                    "    Method Show(): String => \"listed\"\n"
                    "End Extend\n",
        },
        {
            .path = "lib/bare.fb",
            .text = "Import \"./shown\"\nExtend P Implements Shown\nEnd Extend\n",
        },
        {
            .path = "lib/both.fb",
            .text = "Import \"./plain\"\n"
                    "Import \"./other\"\n"
                    "Import \"./shown\"\n"
                    "Extend P Implements Shown\n"
                    "End Extend\n",
        },
        {
            .path = "lib/c.fb",
            .text = "Import \"./shown\"\nPublic Class C Implements Shown\nEnd Class\n",
        },
        {
            .path = "lib/cshow.fb",
            .text = "Import \"./c\"\n"
                    "Extend C\n"
                    "    Public Method Show(): String => \"C\"\n"
                    "End Extend\n",
        },
    };

    // listed's Show implements it, whichever module loads first.
    const auto implementer = [&](const std::string_view program) {
        std::vector<fb::library_file_t> files =
            fb::standard_library().value_or(std::vector<fb::library_file_t>{});
        files.insert(files.end(), modules.begin(), modules.end());
        const fb::compilation_t compilation = fb::compile("test.fb", std::string(program), files);
        const fb::analysis_t& analysis = compilation.analysis;
        CHECK(compilation.diagnostics.diagnostics().empty());
        for (const fb::conformance_t& conformance : analysis.conformances) {
            if (analysis.types.name(conformance.interface) == "Shown") {
                REQUIRE(conformance.members.size() == 1);
                return compilation.sources.get(analysis.symbol(conformance.members[0]).span.file)
                    .path();
            }
        }

        return std::string();
    };
    CHECK(implementer("Import \"./lib/plain\"\nImport \"./lib/listed\"") == "lib/listed.fb");
    CHECK(implementer("Import \"./lib/listed\"\nImport \"./lib/plain\"") == "lib/listed.fb");

    // What an Extend the listing's module doesn't see adds doesn't implement it.
    CHECK(errors_with("Import \"./lib/plain\"\nImport \"./lib/bare\"", modules) ==
          errors_t{ "lib/bare.fb: Shown: P implements Shown, so it needs a Method 'Show'; 'Show' "
                    "is in lib.plain, which this file doesn't import: Import \"./plain\"" });
    CHECK(errors_with("Import \"./lib/both\"", modules) ==
          errors_t{ "lib/both.fb: Shown: 'Show' is ambiguous here: lib.plain and lib.other each "
                    "add it to P" });

    // Nor does it for a type that lists the Interface itself, though one in its own module does.
    CHECK(errors_with("Import \"./lib/c\"\nImport \"./lib/cshow\"", modules) ==
          errors_t{ "lib/c.fb: Shown: C implements Shown, so it needs a Method 'Show'; 'Show' is "
                    "in lib.cshow, which this file doesn't import: Import \"./cshow\"" });
    CHECK(errors_with("Import \"./lib/shown\"\n"
                      "Class Mine Implements Shown\n"
                      "End Class\n"
                      "Extend Mine\n"
                      "    Method Show(): String => \"mine\"\n"
                      "End Extend",
                      modules)
              .empty());
}

TEST_CASE("a For loop takes the Iterate its file sees", "[sema]") {
    // ones.fb and tens.fb each add an Iterate to Bag, and user.fb imports ones.fb.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/bag.fb",
            .text = "Import std.core\n"
                    "Public Class Bag\n"
                    "End Class\n"
                    "Public Class Steps Implements Iterator<Int>\n"
                    "    Field At: Int = 0\n"
                    "    Field Step: Int\n"
                    "    Public New(step: Int)\n"
                    "        Step = step\n"
                    "    End New\n"
                    "    Method Next(): Int?\n"
                    "        If At >= 2 Then Return Null\n"
                    "        At += 1\n"
                    "        Return At * Step\n"
                    "    End Method\n"
                    "End Class\n",
        },
        {
            .path = "lib/ones.fb",
            .text = "Import \"./bag\"\n"
                    "Extend Bag\n"
                    "    Public Method Iterate(): Steps => Steps(1)\n"
                    "End Extend\n",
        },
        {
            .path = "lib/tens.fb",
            .text = "Import \"./bag\"\n"
                    "Extend Bag\n"
                    "    Public Method Iterate(): Steps => Steps(10)\n"
                    "End Extend\n",
        },
        { .path = "lib/user.fb", .text = "Import \"./ones\"\n" },
    };
    const std::string_view loop = "For x In Bag()\n    Print(\"{x}\")\nEnd For";
    CHECK(errors_with(
              std::format("Import std.core\nImport \"./lib/bag\"\nImport \"./lib/user\"\n{}", loop),
              modules) ==
          errors_t{ "Bag(): 'For ... In' goes over arrays, Channels, Iterators, and values "
                    "whose Iterate() gives an Iterator, not Bag; 'Iterate' is in lib.ones, which "
                    "this file doesn't import: Import \"./lib/ones\"" });
    CHECK(errors_with(
              std::format("Import std.core\nImport \"./lib/bag\"\nImport \"./lib/ones\"\n{}", loop),
              modules)
              .empty());
    CHECK(errors_with(std::format("Import std.core\nImport \"./lib/bag\"\n"
                                  "Import \"./lib/ones\"\n"
                                  "Import \"./lib/tens\"\n"
                                  "{}",
                                  loop),
                      modules) ==
          errors_t{ "Bag(): 'Iterate' is ambiguous here: lib.ones and lib.tens each add it to "
                    "Bag" });

    // The file's own Extend hides both.
    CHECK(errors_with(std::format("Import std.core\nImport \"./lib/bag\"\n"
                                  "Import \"./lib/ones\"\n"
                                  "Import \"./lib/tens\"\n"
                                  "Extend Bag\n"
                                  "    Method Iterate(): Steps => Steps(100)\n"
                                  "End Extend\n"
                                  "{}",
                                  loop),
                      modules)
              .empty());
}

TEST_CASE("an Extend's Operators are seen where its module is imported, and its == everywhere",
          "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/p.fb", .text = "Public Struct P\n    Public Field X: Int\nEnd Struct\n" },
        {
            .path = "lib/ops.fb",
            .text = "Import \"./p\"\n"
                    "Extend P\n"
                    "    Operator +(a: P, b: P): P => P(a.X + b.X)\n"
                    "    Operator ==(a: P, b: P): Bool => a.X == b.X\n"
                    "End Extend\n",
        },
        {
            .path = "lib/more.fb",
            .text = "Import \"./p\"\n"
                    "Import \"./ops\"\n"
                    "Extend P\n"
                    "    Operator +(a: P, b: P): P => P(a.X * b.X)\n"
                    "End Extend\n"
                    "Public Function Product(): P => P(2) + P(3)\n",
        },
        {
            .path = "lib/user.fb",
            .text = "Import \"./p\"\nImport \"./ops\"\nPublic Function Sum(): P => P(1) + P(2)\n",
        },
        {
            .path = "lib/same.fb",
            .text = "Import \"./p\"\n"
                    "Extend P\n"
                    "    Operator ==(a: P, b: P): Bool => True\n"
                    "End Extend\n",
        },
    };

    // user.fb sees ops.fb's +, and more.fb's own + hides it there.
    CHECK(errors_with("Import \"./lib/p\"\n"
                      "Import \"./lib/user\"\n"
                      "Import \"./lib/more\"\n"
                      "Local same = Sum() == Product()",
                      modules)
              .empty());
    CHECK(
        errors_with("Import \"./lib/p\"\nImport \"./lib/user\"\nLocal a = P(1) + P(2)", modules) ==
        errors_t{ "P(1) + P(2): Operator + for P is in lib.ops, which this file doesn't "
                  "import: Import \"./lib/ops\"" });
    CHECK(errors_with("Import \"./lib/p\"\n"
                      "Import \"./lib/ops\"\n"
                      "Import \"./lib/more\"\n"
                      "Local a = P(1) + P(2)",
                      modules) == errors_t{ "P(1) + P(2): Operator + is ambiguous for (P, P)" });

    // A type has one ==, which an Extend adds for the whole program.
    CHECK(errors_with("Import \"./lib/p\"\n"
                      "Import \"./lib/ops\"\n"
                      "Import \"./lib/same\"\n"
                      "Local same = P(1) == P(2)",
                      modules) ==
          errors_t{ "lib/same.fb: ==: '==' is already declared with the same parameter types" });
}

TEST_CASE("a declaration's messages show a type's module where its name means another type",
          "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Class Node\n"
                    "End Class\n"
                    "Public Class Base\n"
                    "    Public Method Make(): Node => Node()\n"
                    "End Class\n"
                    "Public Interface Maker\n"
                    "    Method Build(): Node\n"
                    "End Interface\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Class Node\n"
                      "End Class\n"
                      "Class D Extends Base\n"
                      "    Public Override Method Make(): Int => 1\n"
                      "End Class\n"
                      "Class M Implements Maker\n"
                      "    Method Build(): Int => 1\n"
                      "End Class\n"
                      "Local n = Base().Make().Nope",
                      modules) ==
          errors_t{
              "Make: 'Make' must return lib.a.Node, like Base's",
              "Maker: M's 'Build' doesn't match Maker's, which takes () and returns lib.a.Node",
              "Nope: lib.a.Node has no member named 'Nope'",
          });

    // Where the name means that type, it's shown as written.
    CHECK(errors_with("Import \"./lib/a\"\nLocal n = Base().Make().Nope", modules) ==
          errors_t{ "Nope: Node has no member named 'Nope'" });
}

TEST_CASE("a Function named through its module is a value, not a variable", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/w.fb", .text = "Public Function Work(): Int => 42\n" },
    };
    const std::string_view take = "Function Take(ByRef f: Func(): Int)\nEnd Function\n";
    CHECK(errors_with("Import \"./lib/w\" As W\n"
                      "Local f: Func(): Int = W.Work\n"
                      "Local n = f() + W.Work()",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/w\" As W\nFunction Own(): Int => 1\nW.Work = Own", modules) ==
          errors_t{ "W.Work: 'Work' is a function and can't be changed" });
    CHECK(errors_with("Import \"./lib/w\"\nw.Work += 1", modules) ==
          errors_t{ "w.Work: 'Work' is a function and can't be changed" });
    CHECK(
        errors_with(std::format("Import \"./lib/w\" As W\n{}Take(ByRef W.Work)", take), modules) ==
        errors_t{ "W.Work: 'Work' is a function and can't be passed ByRef" });

    // So is one of the module's own.
    CHECK(errors_of(std::format("Function Work(): Int => 1\n{}Take(ByRef Work)", take)) ==
          errors_t{ "Work: 'Work' is a function and can't be passed ByRef" });
}

TEST_CASE("an Import's qualifier names one import's modules", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a/util.fb", .text = "Public Function F(): Int => 1\n" },
        { .path = "lib/b/util.fb", .text = "Public Function G(): Int => 2\n" },
    };
    CHECK(errors_with("Import \"./lib/a/util\"\nImport \"./lib/b/util\"", modules) ==
          errors_t{ "util: 'util' names another Import's module already; give one of them its "
                    "own name with As" });
    CHECK(errors_with("Import \"./lib/a/util\"\n"
                      "Import \"./lib/b/util\" As other\n"
                      "Local n = util.F() + other.G()",
                      modules)
              .empty());

    // An Import names a module of std, not std itself, and a quoted path stays out of a folder
    // called std.
    CHECK(errors_of("Import std") ==
          errors_t{ "std: std is the standard library's folder; import one of its modules, like "
                    "std.collections" });
    CHECK(errors_with("Import \"./std/x\"", modules) ==
          errors_t{ "\"./std/x\": \"./std/x\" names a file in a folder called std, which is "
                    "the standard library's name" });
}

TEST_CASE("a Class can extend one through its module's qualifier", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/shapes.fb",
            .text = "Public Class Shape\n"
                    "    Public Method Area(): Int => 0\n"
                    "End Class\n"
                    "Public Class Holder<T>\n"
                    "    Public Field Item: T\n"
                    "    Public New(item: T)\n"
                    "        Item = item\n"
                    "    End New\n"
                    "End Class\n",
        },
    };
    const std::string_view alias = "Import \"./lib/shapes\" As S\n";
    CHECK(errors_with(std::format("{}"
                                  "Class Circle Extends S.Shape\n"
                                  "    Public Override Method Area(): Int => 3\n"
                                  "End Class\n"
                                  "Class Tag Extends S.Holder<String>\n"
                                  "End Class\n"
                                  "Local s: S.Shape = Circle()\n"
                                  "Local n = s.Area()\n"
                                  "Local t = Tag(\"x\").Item",
                                  alias),
                      modules)
              .empty());
    CHECK(errors_with(std::format("{}Class A Extends S.Nope\nEnd Class", alias), modules) ==
          errors_t{ "Nope: 'Nope' isn't declared in \"./lib/shapes\"" });
    CHECK(errors_with(std::format("{}Class A Extends D.Shape\nEnd Class", alias), modules) ==
          errors_t{ "D: 'D' isn't an imported module, so it can't come before '.'" });
    CHECK(errors_with(std::format("{}Class A Extends Shape\nEnd Class", alias), modules) ==
          errors_t{ "Shape: 'Shape' is in lib.shapes, which this file imports as S, so it's "
                    "S.Shape" });
}

TEST_CASE("a qualifier that hides the type its module declares says so", "[sema]") {
    // Circle is the Import's qualifier, which hides the Class Circle that Make is on.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/Circle.fb",
            .text = "Public Class Circle\n"
                    "    Public Static Method Make(): Int => 1\n"
                    "End Class\n",
        },
    };
    CHECK(errors_with("Import \"./lib/Circle\"\nLocal n = Circle.Make()", modules) ==
          errors_t{ "Make: 'Make' isn't declared in \"./lib/Circle\"; Circle here is the Import's "
                    "qualifier, so the type Circle the module declares, whose 'Make' this may be, "
                    "is Circle.Circle" });
    CHECK(errors_with("Import \"./lib/Circle\"\nLocal n = Circle.Circle.Make()", modules).empty());
}

// Visibility

TEST_CASE("what's private to a file can't be used from another module", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Class Enemy\n"
                    "    Field secret: Int = 1\n"
                    "    Public Field Name: String = \"enemy\"\n"
                    "    Method Hidden(): Int => secret\n"
                    "    Public Method Shown(): Int => Hidden()\n"
                    "    Property Mood: Int\n"
                    "        Get => secret\n"
                    "    End Property\n"
                    "    Property Quiet: Int\n"
                    "        Get => secret\n"
                    "        Set(value)\n"
                    "            secret = value\n"
                    "        End Set\n"
                    "    End Property\n"
                    "    Public Property Level: Int\n"
                    "        Get => secret\n"
                    "        Set(value)\n"
                    "            secret = value\n"
                    "        End Set\n"
                    "    End Property\n"
                    "End Class\n"
                    "Class Secret\n"
                    "End Class\n"
                    "Function Helper(): Int => 1\n"
                    "Global count = 0\n"
                    "Public Function Make(): Enemy => Enemy()\n",
        },
    };
    const std::string_view import = "Import \"./lib/a\"\n";
    CHECK(errors_with(
              std::format("{}Local e = Make()\nLocal n = e.Name\nLocal s = e.Shown()", import),
              modules)
              .empty());

    // A Field, a Method, a Property, a type, a Function, and a Global.
    CHECK(errors_with(std::format("{}"
                                  "Local e = Make()\n"
                                  "Local a = e.secret\n"
                                  "Local b = e.Hidden()\n"
                                  "Local c = e.Mood\n"
                                  "Local d: Secret? = Null\n"
                                  "Local f = Helper() + count",
                                  import),
                      modules) == errors_t{
                                      "secret: 'secret' is private to lib/a.fb",
                                      "Hidden: 'Hidden' is private to lib/a.fb",
                                      "Mood: 'Mood' is private to lib/a.fb",
                                      "Secret: 'Secret' is private to lib/a.fb",
                                      "Helper: 'Helper' is private to lib/a.fb",
                                      "count: 'count' is private to lib/a.fb",
                                  });

    // Through the module's qualifier, or a From line, it's the same.
    CHECK(errors_with(std::format("{}Local h = a.Helper()", import), modules) ==
          errors_t{ "Helper: 'Helper' is private to lib/a.fb" });
    CHECK(errors_with("From \"./lib/a\" Import Make, Secret", modules) ==
          errors_t{ "Secret: 'Secret' is private to lib/a.fb" });

    // Changing one is using it too, through a Property's Set as well.
    CHECK(errors_with(std::format("{}Local e = Make()\ne.secret = 2", import), modules) ==
          errors_t{ "secret: 'secret' is private to lib/a.fb" });
    CHECK(errors_with(std::format("{}"
                                  "Local e = Make()\n"
                                  "e.Level = 3\n"
                                  "e.Level += 1\n"
                                  "e.Quiet = 3\n"
                                  "e.Quiet += 1",
                                  import),
                      modules) == errors_t{
                                      "Quiet: 'Quiet' is private to lib/a.fb",
                                      "Quiet: 'Quiet' is private to lib/a.fb",
                                  });
}

TEST_CASE("a declaration a file can't see hides nothing there", "[sema]") {
    // a and b each have a private Node, and a Public one would be ambiguous.
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Class Node\nEnd Class\nPublic Function A(): Int => 1\n" },
        { .path = "lib/b.fb", .text = "Class Node\nEnd Class\nPublic Function B(): Int => 2\n" },
        {
            .path = "lib/c.fb",
            .text = "Public Class Base\n"
                    "    Field hp: Int = 1\n"
                    "End Class\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\nImport \"./lib/b\"\nLocal n = A() + B()", modules)
              .empty());
    CHECK(errors_with("Import \"./lib/a\"\nImport \"./lib/b\"\nLocal n: Node? = Null", modules) ==
          errors_t{ "Node: 'Node' is private to lib/a.fb" });

    // A Class extending one sees the Global its own module has, not a private Field of the base.
    CHECK(errors_with("Import \"./lib/c\"\n"
                      "Global hp = 5\n"
                      "Class Mine Extends Base\n"
                      "    Method Twice(): Int => hp * 2\n"
                      "End Class",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/c\"\n"
                      "Class Mine Extends Base\n"
                      "    Method Twice(): Int => hp * 2\n"
                      "End Class",
                      modules) == errors_t{ "hp: 'hp' is private to lib/c.fb" });
}

TEST_CASE("an overload a file can't use isn't one of its calls' choices", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/show.fb",
            .text = "Public Function Show(x: Int): String => \"int\"\n"
                    "Function Show(x: String): String => x\n"
                    "Public Class Log\n"
                    "    Public Method Add(x: Int)\n"
                    "    End Method\n"
                    "    Method Add(x: String)\n"
                    "    End Method\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/show\"\n";
    CHECK(errors_with(std::format("{}Local s = Show(1)\nLog().Add(2)", import), modules).empty());

    // A call that only one of them fits says why it can't use it.
    CHECK(errors_with(std::format("{}Local s = Show(\"a\")\nLog().Add(\"b\")", import), modules) ==
          errors_t{
              "Show: 'Show' is private to lib/show.fb",
              "Add: 'Add' is private to lib/show.fb",
          });
}

TEST_CASE("a Protected member is used by the Classes extending its Class", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/enemy.fb",
            .text = "Public Class Enemy\n"
                    "    Protected Field hp: Int = 10\n"
                    "    Protected Static Field made: Int = 0\n"
                    "    Public New()\n"
                    "    End New\n"
                    "    Protected New(hp: Int)\n"
                    "        Self.hp = hp\n"
                    "    End New\n"
                    "    Protected Method Heal(): Int => hp + 1\n"
                    "    Protected Static Method Spawned(): Int => made\n"
                    "End Class\n",
        },
    };

    // From a Class extending it in another module, through Self, Super, or one of its own
    // objects, and its Statics.
    const std::string_view import = "Import \"./lib/enemy\"\n";
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    New()\n"
                                  "        Super.New(50)\n"
                                  "    End New\n"
                                  "    Protected Override Method Heal(): Int => Super.Heal() * 2\n"
                                  "    Method Total(other: Boss): Int\n"
                                  "        Local f = Function() => hp\n"
                                  "        Return Self.hp + other.hp + Heal() + f() + Spawned() +\n"
                                  "               Enemy.made\n"
                                  "    End Method\n"
                                  "End Class",
                                  import),
                      modules)
              .empty());

    // Not from code that isn't a Class extending it.
    CHECK(errors_with(std::format("{}Local e = Enemy()\nLocal n = e.hp", import), modules) ==
          errors_t{ "hp: 'hp' is Protected in lib/enemy.fb, so only that file and the Classes "
                    "extending Enemy can use it" });
    CHECK(errors_with(std::format("{}"
                                  "Class Other\n"
                                  "    Method Peek(e: Enemy): Int => e.Heal() + Enemy.Spawned()\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{
              "Heal: 'Heal' is Protected in lib/enemy.fb, so only that file and the Classes "
              "extending Enemy can use it",
              "Spawned: 'Spawned' is Protected in lib/enemy.fb, so only that file and the Classes "
              "extending Enemy can use it",
          });

    // Nor through an object that isn't one of the extending Class's, or to make one.
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Method Peek(other: Enemy): Int => other.hp\n"
                                  "    Method Make(): Enemy => Enemy(5)\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{
              "hp: 'hp' is Protected, so Boss's code can use it only through Self or another Boss",
              "Enemy: Enemy's New is Protected, so Boss's code can use it only to start a New of "
              "its own, with Super.New",
          });
    CHECK(errors_with(std::format("{}Local e = Enemy(5)", import), modules) ==
          errors_t{ "Enemy: Enemy's New is Protected in lib/enemy.fb, so only that file and the "
                    "Classes extending Enemy can use it, to start their News" });

    // An Extend of the Class is not the Class's own code.
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "End Class\n"
                                  "Extend Boss\n"
                                  "    Method Peek(): Int => hp\n"
                                  "End Extend",
                                  import),
                      modules) == errors_t{ "hp: 'hp' is Protected in lib/enemy.fb, so only that "
                                            "file and the Classes extending Enemy can use it" });
}

TEST_CASE("Protected only goes before a Class's members", "[sema]") {
    const std::string_view only =
        "Protected only goes before a Class's members, which the Classes extending it can use";
    CHECK(errors_of("Struct S\n    Protected Field x: Int\nEnd Struct") ==
          errors_t{ std::format("Protected: {}", only) });
    CHECK(errors_of("Protected Function F()\nEnd Function\n"
                    "Protected Global g = 1\n"
                    "Protected Class C\nEnd Class") == errors_t{
                                                           std::format("Protected: {}", only),
                                                           std::format("Protected: {}", only),
                                                           std::format("Protected: {}", only),
                                                       });
    CHECK(errors_of("Struct S\nEnd Struct\nExtend S\n    Protected Method M()\n    End Method\n"
                    "End Extend") ==
          errors_t{ "Protected: what an Extend adds can't be Protected, since its code isn't the "
                    "Class's own" });

    // A Class's members can be.
    CHECK(errors_of("Class C\n"
                    "    Protected Field x: Int\n"
                    "    Protected Method M()\n"
                    "    End Method\n"
                    "    Protected Property P: Int\n"
                    "        Get => x\n"
                    "    End Property\n"
                    "End Class")
              .empty());
}

TEST_CASE("Package is for the modules of a package", "[sema]") {
    // std is a package, so one std module uses another's Package Function, but a program doesn't.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "std/extra/helper.fb",
            .text = "Package Function Common(): Int => 2\n"
                    "Public Class Box\n"
                    "    Package Field Size: Int = 1\n"
                    "End Class\n",
        },
        {
            .path = "std/extra/user.fb",
            .text = "Import std.extra.helper\n"
                    "Public Function Double(): Int => Common() * 2 + Box().Size\n",
        },
        { .path = "lib/own.fb", .text = "Package Function Mine(): Int => 3\n" },
    };
    CHECK(errors_with("Import std.extra\nLocal n = Double()", modules).empty());
    CHECK(errors_with("Import std.extra\nLocal n = Common() + Box().Size", modules) ==
          errors_t{
              "Common: 'Common' is Package in std/extra/helper.fb, so only the package std can use "
              "it",
              "Size: 'Size' is Package in std/extra/helper.fb, so only the package std can use it",
          });

    // A program without a project.toml is a package of its main file and what it imports by path.
    CHECK(errors_with("Import \"./lib/own\"\nLocal n = Mine()", modules).empty());
}

TEST_CASE("a Struct's own constructor is as visible as its least visible Field", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/points.fb",
            .text = "Public Struct Point\n"
                    "    Public Field X: Int\n"
                    "    Public Field Y: Int = 2\n"
                    "End Struct\n"
                    "Public Struct Handle\n"
                    "    Public Field Index: Int\n"
                    "    Field Generation: Int\n"
                    "End Struct\n"
                    "Public Function Origin(): Handle => Handle(0, 0)\n"
                    "Public Class Made\n"
                    "End Class\n"
                    "Public Class Built\n"
                    "    New()\n"
                    "    End New\n"
                    "    Public Static Method Make(): Built => Built()\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/points\"\n";
    CHECK(errors_with(std::format("{}"
                                  "Local p = Point(1, 2)\n"
                                  "Local q = Point(Y: 3, X: 1)\n"
                                  "Local i = Origin().Index\n"
                                  "Local m = Made()\n"
                                  "Local b = Built.Make()",
                                  import),
                      modules)
              .empty());
    CHECK(errors_with(std::format("{}Local h = Handle(1, 2)", import), modules) ==
          errors_t{ "Handle: Handle's constructor takes its Fields, and 'Generation' is private "
                    "to lib/points.fb" });

    // A written New is Private unless it says otherwise.
    CHECK(errors_with(std::format("{}Local b = Built()", import), modules) ==
          errors_t{ "Built: Built's New is private to lib/points.fb" });
}

TEST_CASE("a Class takes only the News of its base that its code can use", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/base.fb",
            .text = "Public Class Shut\n"
                    "    New()\n"
                    "    End New\n"
                    "End Class\n"
                    "Public Class Half\n"
                    "    Public New(x: Int)\n"
                    "    End New\n"
                    "    New(x: String)\n"
                    "    End New\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/base\"\n";
    CHECK(
        errors_with(std::format("{}Class Mine Extends Half\nEnd Class\nLocal m = Mine(1)", import),
                    modules)
            .empty());
    CHECK(errors_with(
              std::format("{}Class Mine Extends Half\nEnd Class\nLocal m = Mine(\"x\")", import),
              modules) == errors_t{ "\"x\": expected Int32, found String" });
    CHECK(errors_with(std::format("{}Class Mine Extends Shut\nEnd Class", import), modules) ==
          errors_t{ "Mine: Shut's New is private to lib/base.fb" });
    CHECK(errors_with(std::format("{}"
                                  "Class Mine Extends Shut\n"
                                  "    New()\n"
                                  "    End New\n"
                                  "End Class",
                                  import),
                      modules) == errors_t{ "New: Shut's New is private to lib/base.fb" });
}

TEST_CASE("a Public or Package declaration shows only types as visible", "[sema]") {
    const std::string_view hidden = "Struct Hidden\n    Field X: Int\nEnd Struct\n"
                                    "Interface Face\nEnd Interface\n"
                                    "Class Base\nEnd Class\n";
    CHECK(errors_of(std::format("{}"
                                "Public Function Leak(): Hidden => Hidden(1)\n"
                                "Public Function Take(h: Hidden?, f: Func(Hidden): Int)\n"
                                "End Function\n"
                                "Public Global shown: Hidden[] = []\n"
                                "Package Function Inside(): Hidden? => Null\n"
                                "Public Class Derived Extends Base Implements Face\n"
                                "    Public Field Item: Hidden?\n"
                                "    Public Method Get(): Hidden? => Item\n"
                                "End Class",
                                hidden)) ==
          errors_t{
              "Leak: 'Leak' is Public, but it gives Hidden, which is private to test.fb",
              "Take: 'Take' is Public, but it takes Hidden, which is private to test.fb",
              "shown: 'shown' is Public, but it holds Hidden, which is private to test.fb",
              "Inside: 'Inside' is Package, but it gives Hidden, which is private to test.fb",
              "Derived: 'Derived' is Public, but it extends Base, which is private to test.fb",
              "Derived: 'Derived' is Public, but it implements Face, which is private to test.fb",
              "Item: 'Item' is Public, but it holds Hidden, which is private to test.fb",
              "Get: 'Get' is Public, but it gives Hidden, which is private to test.fb",
          });

    // So does a Public Interface's member, and an Operator of a Public type.
    CHECK(errors_of(std::format("{}"
                                "Public Interface Maker\n"
                                "    Method Make(): Hidden\n"
                                "End Interface\n"
                                "Public Struct Money\n"
                                "    Public Field Cents: Int\n"
                                "    Operator +(a: Money, b: Hidden): Money => a\n"
                                "End Struct",
                                hidden)) ==
          errors_t{
              "Make: 'Make' is Public, but it gives Hidden, which is private to test.fb",
              "+: Operator + is Public, but it takes Hidden, which is private to test.fb",
          });

    // A Public member of a type that isn't, or a private one of a Public type, can.
    CHECK(errors_of(std::format("{}"
                                "Struct Inner\n"
                                "    Public Field Item: Hidden?\n"
                                "End Struct\n"
                                "Public Struct Outer\n"
                                "    Field Item: Hidden?\n"
                                "    Method Get(): Hidden? => Item\n"
                                "End Struct",
                                hidden))
              .empty());

    // A Package declaration can show a Package type, but a Public one can't.
    CHECK(errors_of("Package Struct Wide\nEnd Struct\n"
                    "Package Function Fine(): Wide => Wide()\n"
                    "Public Function Leak(): Wide => Wide()") ==
          errors_t{ "Leak: 'Leak' is Public, but it gives Wide, which is Package in test.fb" });

    // A Protected member is seen as far as its Class, and a Public one of a Package type as far
    // as the package; the error names the visibility each is declared with.
    CHECK(errors_of(std::format("{}"
                                "Package Struct Wide\nEnd Struct\n"
                                "Public Class Enemy\n"
                                "    Protected Field hp: Hidden?\n"
                                "    Protected Method Range(): Wide? => Null\n"
                                "End Class\n"
                                "Package Class Holder\n"
                                "    Public Field item: Hidden?\n"
                                "    Public Field wide: Wide?\n"
                                "    Protected Field kept: Wide?\n"
                                "End Class",
                                hidden)) ==
          errors_t{
              "hp: 'hp' is Protected in Enemy, which is Public, but it holds Hidden, which is "
              "private to test.fb",
              "Range: 'Range' is Protected in Enemy, which is Public, but it gives Wide, which is "
              "Package in test.fb",
              "item: 'item' is Public, but it holds Hidden, which is private to test.fb",
          });
}

TEST_CASE("an Override can't be less visible than what it overrides", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/enemy.fb",
            .text = "Public Class Enemy\n"
                    "    Public Method Think(): String => \"enemy\"\n"
                    "    Protected Method Plan(): String => \"plan\"\n"
                    "    Package Method Route(): String => \"route\"\n"
                    "    Method Secret(): String => \"secret\"\n"
                    "    Public Property Health: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "    Protected Property Armor: Int\n"
                    "        Get => 2\n"
                    "    End Property\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/enemy\"\n";
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Public Override Method Think(): String => \"boss\"\n"
                                  "    Public Override Method Plan(): String => \"boss plan\"\n"
                                  "End Class",
                                  import),
                      modules)
              .empty());
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Private Override Method Think(): String => \"boss\"\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{ "Think: 'Think' is Private, so it can't override Enemy's, which is Public" });

    // Protected and Package reach different code, so neither can override the other.
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Package Override Method Plan(): String => \"boss plan\"\n"
                                  "    Protected Override Method Route(): String => \"boss\"\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{
              "Plan: 'Plan' is Package, so it can't override Enemy's, which is Protected",
              "Route: 'Route' is Protected, so it can't override Enemy's, which is Package",
          });

    // One that doesn't say its visibility has what it overrides'.
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Override Method Think(): String => \"boss\"\n"
                                  "    Override Method Plan(): String => \"boss plan\"\n"
                                  "End Class\n"
                                  "Local b = Boss()\n"
                                  "Local t = b.Think()",
                                  import),
                      modules)
              .empty());

    // One its code can't use isn't one it can override.
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Method Secret(): String => \"mine\"\n"
                                  "End Class",
                                  import),
                      modules) == errors_t{ "Secret: 'Secret' can't override Enemy's: 'Secret' is "
                                            "private to lib/enemy.fb" });

    // A Property's Override is the same.
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Public Override Property Health: Int\n"
                                  "        Get => 10\n"
                                  "    End Property\n"
                                  "    Protected Override Property Armor: Int\n"
                                  "        Get => 20\n"
                                  "    End Property\n"
                                  "End Class",
                                  import),
                      modules)
              .empty());
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Private Override Property Health: Int\n"
                                  "        Get => 10\n"
                                  "    End Property\n"
                                  "    Private Override Property Armor: Int\n"
                                  "        Get => 20\n"
                                  "    End Property\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{
              "Health: 'Health' is Private, so it can't override Enemy's, which is Public",
              "Armor: 'Armor' is Private, so it can't override Enemy's, which is Protected",
          });
    CHECK(errors_with(std::format("{}"
                                  "Class Boss Extends Enemy\n"
                                  "    Override Property Health: Int\n"
                                  "        Get => 10\n"
                                  "    End Property\n"
                                  "End Class\n"
                                  "Local h = Boss().Health",
                                  import),
                      modules)
              .empty());
}

TEST_CASE("what code can't use clashes with nothing", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/base.fb",
            .text = "Public Class Base\n"
                    "    Field cache: Int = 7\n"
                    "    Field size: Int = 0\n"
                    "    Method Grow(): Int => 1\n"
                    "    Property Mood: Int\n"
                    "        Get => 1\n"
                    "    End Property\n"
                    "    Public Field hp: Int = 1\n"
                    "End Class\n"
                    "Public Struct Point\n"
                    "    Field secret: Int = 0\n"
                    "    Method Scale(): Int => 1\n"
                    "End Struct\n"
                    "Public Function Origin(): Point => Point()\n"
                    "Public Class Kept Extends Base\n"
                    "End Class\n"
                    "Public Class Held<T> Extends Base\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/base\"\n";

    // A Class extending another file's can have Fields and Methods named like its private ones,
    // whichever they are, and its code uses its own.
    CHECK(errors_with(std::format("{}"
                                  "Class Mine Extends Base\n"
                                  "    Field cache: String = \"mine\"\n"
                                  "    Field Grow: String = \"field\"\n"
                                  "    Method size(): String => cache + Grow\n"
                                  "    Method Mood(x: Int): Int => x\n"
                                  "End Class\n"
                                  "Local text: String = Mine().size()",
                                  import),
                      modules)
              .empty());

    // So can an Extend, of another file's type or of std's.
    CHECK(errors_with(std::format("{}"
                                  "Extend Base\n"
                                  "    Method cache(): String => \"extended\"\n"
                                  "    Method Grow(): String => \"grown\"\n"
                                  "    Property size: String\n"
                                  "        Get => \"size\"\n"
                                  "    End Property\n"
                                  "End Extend\n"
                                  "Extend Point\n"
                                  "    Property secret: String\n"
                                  "        Get => \"secret\"\n"
                                  "    End Property\n"
                                  "    Method Scale(): String => \"scaled\"\n"
                                  "End Extend\n"
                                  "Local b = Base()\n"
                                  "Local p = Origin()\n"
                                  "Local text: String = b.cache() + b.Grow() + b.size + p.secret + "
                                  "p.Scale()",
                                  import),
                      modules)
              .empty());

    // And so can an Extend of a Class extending it, or of one instance of a generic one, whose
    // Method takes a hidden Method's parameters: an Extend's members aren't in the Method table,
    // so they override nothing.
    CHECK(errors_with(std::format("{}"
                                  "Extend Kept\n"
                                  "    Method Grow(): Int => 2\n"
                                  "    Property Mood: Int\n"
                                  "        Get => 3\n"
                                  "    End Property\n"
                                  "End Extend\n"
                                  "Extend Held<Int>\n"
                                  "    Method Grow(): Int => 4\n"
                                  "    Property Mood: Int\n"
                                  "        Get => 5\n"
                                  "    End Property\n"
                                  "End Extend\n"
                                  "Local n: Int = Kept().Grow() + Kept().Mood + "
                                  "Held<Int>().Grow() + Held<Int>().Mood",
                                  import),
                      modules)
              .empty());
    CHECK(errors_of("Import std.collections\n"
                    "Extend Map<K, V>\n"
                    "    Method Grow(): Int => 1\n"
                    "    Method Entries(): Int => 2\n"
                    "End Extend\n"
                    "Extend Stack<T>\n"
                    "    Property Items: Int\n"
                    "        Get => 3\n"
                    "    End Property\n"
                    "End Extend\n"
                    "Class Named Extends Queue<String>\n"
                    "    Field Head: String = \"first\"\n"
                    "End Class\n"
                    "Local m = Map<Int, Int>()\n"
                    "Local n: Int = m.Grow() + m.Entries() + Stack<Int>().Items\n"
                    "Local head: String = Named().Head")
              .empty());
    CHECK(errors_of("Import std.collections\n"
                    "Class Names Extends Map<String, Int>\n"
                    "End Class\n"
                    "Extend Names\n"
                    "    Method Grow(): Int => 4\n"
                    "End Extend\n"
                    "Class Counts<T> Extends Map<String, T>\n"
                    "End Class\n"
                    "Extend Counts<Int>\n"
                    "    Method Grow(): Int => 5\n"
                    "End Extend\n"
                    "Local n: Int = Names().Grow() + Counts<Int>().Grow()")
              .empty());

    // What its code can use still clashes, and a Method that takes a hidden one's parameters is
    // one it can't override.
    CHECK(errors_with(std::format("{}"
                                  "Class Mine Extends Base\n"
                                  "    Field hp: Int = 2\n"
                                  "    Method Grow(): Int => 2\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{
              "hp: 'hp' is already declared in Base",
              "Grow: 'Grow' can't override Base's: 'Grow' is private to lib/base.fb",
          });
    CHECK(errors_with(std::format("{}Extend Base\n    Method hp(): Int => 2\nEnd Extend", import),
                      modules) == errors_t{ "hp: 'hp' is already declared" });

    // So does a Property named like a hidden Property, which it would override, though an
    // Extend's, which overrides nothing, doesn't.
    CHECK(errors_with(std::format("{}"
                                  "Class Mine Extends Base\n"
                                  "    Property Mood: Int\n"
                                  "        Get => 2\n"
                                  "    End Property\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{ "Mood: 'Mood' can't override Base's: 'Mood' is private to lib/base.fb" });
    CHECK(errors_with(std::format("{}"
                                  "Extend Base\n"
                                  "    Property Mood: Int\n"
                                  "        Get => 2\n"
                                  "    End Property\n"
                                  "End Extend\n"
                                  "Local mood: Int = Base().Mood",
                                  import),
                      modules)
              .empty());

    // But it's a Property that an Extend of a Class extending Base can't add again, as with a
    // Method, though the hidden one comes first.
    CHECK(errors_with(std::format("{}"
                                  "Extend Base\n"
                                  "    Property Mood: Int\n"
                                  "        Get => 2\n"
                                  "    End Property\n"
                                  "End Extend\n"
                                  "Extend Kept\n"
                                  "    Property Mood: Int\n"
                                  "        Get => 3\n"
                                  "    End Property\n"
                                  "End Extend",
                                  import),
                      modules) ==
          errors_t{ "Mood: 'Mood' is already a member of Base, which an Extend can't override" });

    // Where code can use both, a Class's member hides what its base has of the name, and a
    // type's own hides what an Extend adds in its place.
    const std::vector<fb::library_file_t> both{
        {
            .path = "lib/a.fb",
            .text = "Import \"./b\"\n"
                    "Import \"./m\"\n"
                    "Public Class Base\n"
                    "    Field cache: Int = 7\n"
                    "    Method Grow(): Int => 1\n"
                    "End Class\n"
                    "Public Function Peek(m: Mine): String => m.cache\n"
                    "Public Function Poke(b: Base): Int => b.cache + b.Grow()\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./a\"\n"
                    "Public Class Mine Extends Base\n"
                    "    Public Field cache: String = \"mine\"\n"
                    "End Class\n",
        },
        {
            .path = "lib/m.fb",
            .text = "Import \"./a\"\n"
                    "Extend Base\n"
                    "    Public Method Grow(): String => \"grown\"\n"
                    "    Public Property cache: String\n"
                    "        Get => \"extended\"\n"
                    "    End Property\n"
                    "End Extend\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Import \"./lib/b\"\n"
                      "Import \"./lib/m\"\n"
                      "Local b = Base()\n"
                      "Local text: String = b.cache + b.Grow() + Mine().cache + Peek(Mine())\n"
                      "Local n: Int = Poke(b)",
                      both)
              .empty());
}

TEST_CASE("Enum members, Interface members, and Operators have their type's visibility", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/kinds.fb",
            .text = "Public Enum Kind\n"
                    "    Small, Large\n"
                    "End Enum\n"
                    "Public Struct Money\n"
                    "    Public Field Cents: Int\n"
                    "    Operator +(a: Money, b: Money): Money => Money(a.Cents + b.Cents)\n"
                    "End Struct\n"
                    "Public Interface Sized\n"
                    "    Method Size(): Int\n"
                    "End Interface\n",
        },
    };
    CHECK(errors_with("Import \"./lib/kinds\"\n"
                      "Local k = Kind.Large\n"
                      "Local m = Money(1) + Money(2)\n"
                      "Function Total(s: Sized): Int => s.Size()",
                      modules)
              .empty());

    // A modifier on one is an error.
    CHECK(errors_of("Enum Kind\n    Public Small\nEnd Enum") ==
          errors_t{ "Public: an Enum's members have its visibility, so they take no Public; put "
                    "it before the Enum" });
    CHECK(errors_of("Interface Sized\n    Public Method Size(): Int\nEnd Interface") ==
          errors_t{ "Public: an Interface's members have its visibility, so they take no Public; "
                    "put it before the Interface" });
    CHECK(errors_of("Struct Money\n"
                    "    Field Cents: Int\n"
                    "    Public Operator +(a: Money, b: Money): Money => Money(a.Cents + b.Cents)\n"
                    "End Struct") ==
          errors_t{ "Public: an Operator has its type's visibility, so it takes no Public" });
}

TEST_CASE("a default an Interface gives has the Interface's visibility", "[sema]") {
    // An Extend can give a Public type an Interface that other files can't name, and its
    // defaults are no more theirs to call than the Interface is.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Interface Secret\n"
                    "    Method Hidden(): Int => 42\n"
                    "End Interface\n"
                    "Public Interface Shown\n"
                    "    Method Visible(): Int => 7\n"
                    "End Interface\n"
                    "Public Class C\n"
                    "End Class\n"
                    "Extend C Implements Secret, Shown\n"
                    "End Extend\n"
                    "Public Function Ask(c: C): Int => c.Hidden()\n",
        },
        {
            .path = "std/extra/kept.fb",
            .text = "Package Interface Kept\n"
                    "    Method Inside(): Int => 1\n"
                    "End Interface\n"
                    "Public Class Box\n"
                    "End Class\n"
                    "Extend Box Implements Kept\n"
                    "End Extend\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\nLocal n = C().Visible() + Ask(C())", modules).empty());
    CHECK(errors_with("Import \"./lib/a\"\nLocal n = C().Hidden()", modules) ==
          errors_t{ "Hidden: 'Hidden' is private to lib/a.fb" });
    CHECK(errors_with("Import std.extra\nLocal n = Box().Inside()", modules) ==
          errors_t{ "Inside: 'Inside' is Package in std/extra/kept.fb, so only the package std can "
                    "use it" });

    // A Class extending C can't use it, so it can't override it either.
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Class Mine Extends C\n"
                      "    Method Hidden(): Int => 1\n"
                      "End Class",
                      modules) ==
          errors_t{ "Hidden: 'Hidden' can't override C's: 'Hidden' is private to lib/a.fb" });
}

TEST_CASE("each Interface's default is its own, and two of one name are ambiguous", "[sema]") {
    // A type that two Interfaces give defaults of one name and types has both, and each
    // Interface calls its own, whichever is listed first.
    const std::string both = "Interface Sized\n"
                             "    Method Size(): Int\n"
                             "    Method Twice(): Int => Size() * 2\n"
                             "End Interface\n"
                             "Interface Doubled\n"
                             "    Method Size(): Int\n"
                             "    Method Twice(): Int => Size() + 100\n"
                             "End Interface\n"
                             "Class Box Implements Sized, Doubled\n"
                             "    Method Size(): Int => 1\n"
                             "End Class\n";
    CHECK(
        errors_of(both + "Function F(s: Sized, d: Doubled): Int => s.Twice() + d.Twice()").empty());

    // So a use that can reach both is ambiguous: a call, an Override, or an Interface that
    // would take either one.
    const std::string_view ambiguous =
        "'Twice' is ambiguous here: Sized and Doubled each give Box a default of it";
    CHECK(errors_of(both + "Local n = Box().Twice()") ==
          errors_t{ "Box().Twice: the call to 'Twice' is ambiguous" });
    CHECK(errors_of(both + "Class Sub Extends Box\n"
                           "    Override Method Twice(): Int => 7\n"
                           "End Class") == errors_t{ std::format("Twice: {}", ambiguous) });
    CHECK(errors_of(both + "Interface Wants\n"
                           "    Method Twice(): Int\n"
                           "End Interface\n"
                           "Extend Box Implements Wants\n"
                           "End Extend") == errors_t{ std::format("Wants: {}", ambiguous) });

    // A type's own implements both.
    CHECK(errors_of("Interface Sized\n"
                    "    Method Twice(): Int => 2\n"
                    "End Interface\n"
                    "Interface Doubled\n"
                    "    Method Twice(): Int => 100\n"
                    "End Interface\n"
                    "Class Box Implements Sized, Doubled\n"
                    "    Method Twice(): Int => 1\n"
                    "End Class\n"
                    "Local n = Box().Twice()")
              .empty());

    // Where code can use only one, a call finds that one, and a private Interface's default
    // listed first doesn't implement a Public one's in its place.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Interface Secret\n"
                    "    Method Twice(): Int => 42\n"
                    "End Interface\n"
                    "Public Interface Shown\n"
                    "    Method Twice(): Int => 107\n"
                    "End Interface\n"
                    "Public Class C\n"
                    "End Class\n"
                    "Extend C Implements Secret, Shown\n"
                    "End Extend\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Local s: Shown = C()\n"
                      "Local n = C().Twice() + s.Twice()",
                      modules)
              .empty());
    // Beside the default code can use, the private one says nothing: the usable one says why.
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Class Mine Extends C\n"
                      "    Override Method Twice(): Int => 1\n"
                      "End Class",
                      modules) ==
          errors_t{ "Twice: 'Twice' was added to C by an Extend, so it can't be overridden" });
}

TEST_CASE("a default a type inherits is a Method it has, which implements what it fits", "[sema]") {
    // "<owner> <Interface>" of what implements `interface`'s Twice for `type`.
    const auto implementer = [](const std::string& program, const std::string_view type,
                                const std::string_view interface) {
        const fb::compilation_t compilation = fb::compile("test.fb", program);
        const fb::analysis_t& analysis = compilation.analysis;
        INFO(program);
        CHECK(compilation.diagnostics.diagnostics().empty());
        for (const fb::conformance_t& conformance : analysis.conformances) {
            if (analysis.types.name(conformance.type) == type &&
                analysis.types.name(conformance.interface) == interface) {
                const fb::symbol_t& twice = analysis.symbol(conformance.members.at(1));
                return std::format("{} {}", analysis.types.name(twice.owner),
                                   analysis.types.name(twice.default_interface));
            }
        }

        return std::string();
    };
    const std::string interfaces = "Interface Sized\n"
                                   "    Method Size(): Int\n"
                                   "    Method Twice(): Int => Size() * 2\n"
                                   "End Interface\n"
                                   "Interface Doubled\n"
                                   "    Method Size(): Int\n"
                                   "    Method Twice(): Int => Size() + 100\n"
                                   "End Interface\n";

    // What a Class has from the Class it extends, or an instance from its generic type, beats
    // the Interface's default, so an Override overrides it for both Interfaces.
    const std::string base = interfaces + "Class Base Implements Sized\n"
                                          "    Method Size(): Int => 1\n"
                                          "End Class\n";
    CHECK(implementer(base + "Class Sub Extends Base Implements Doubled\nEnd Class", "Sub",
                      "Doubled") == "Base Sized");
    CHECK(implementer(base + "Class Sub Extends Base\n"
                             "End Class\n"
                             "Extend Sub Implements Doubled\n"
                             "End Extend",
                      "Sub", "Doubled") == "Base Sized");
    CHECK(implementer(interfaces + "Class Box<T> Implements Sized\n"
                                   "    Method Size(): Int => 1\n"
                                   "End Class\n"
                                   "Extend Box<Int> Implements Doubled\n"
                                   "End Extend",
                      "Box<Int32>", "Doubled") == "Box<T> Sized");
    CHECK(errors_of(base + "Class Sub Extends Base Implements Doubled\n"
                           "End Class\n"
                           "Class Third Extends Sub\n"
                           "    Override Method Twice(): Int => 7\n"
                           "End Class\n"
                           "Local n = Sub().Twice() + Third().Twice()")
              .empty());

    // Of two the Class it extends has, the Interface's own is its.
    CHECK(implementer(interfaces + "Class Base Implements Sized, Doubled\n"
                                   "    Method Size(): Int => 1\n"
                                   "End Class\n"
                                   "Class Sub Extends Base Implements Doubled\n"
                                   "End Class",
                      "Sub", "Doubled") == "Base Doubled");

    // A Method the type has, or inherits, with a default's parameters and another result holds
    // its place, so it doesn't match.
    const auto mismatch = [](const std::string_view type) {
        return std::format(
            "Sized: {}'s 'Twice' doesn't match Sized's, which takes () and returns Int32", type);
    };
    CHECK(errors_of("Interface Sized\n"
                    "    Method Twice(): Int => 2\n"
                    "End Interface\n"
                    "Class Box Implements Sized\n"
                    "    Method Twice(): String => \"own\"\n"
                    "End Class") == errors_t{ mismatch("Box") });
    CHECK(errors_of("Interface Sized\n"
                    "    Method Twice(): Int => 2\n"
                    "End Interface\n"
                    "Class Base\n"
                    "    Method Twice(): String => \"base\"\n"
                    "End Class\n"
                    "Class Sub Extends Base Implements Sized\n"
                    "End Class") == errors_t{ mismatch("Sub") });

    // Overloads that an instance gives the same types aren't two defaults: an Override
    // overrides the first.
    CHECK(errors_of("Class Base<T>\n"
                    "    Method Put(x: T): String => \"generic\"\n"
                    "    Method Put(x: Int): String => \"int\"\n"
                    "End Class\n"
                    "Class Sub Extends Base<Int>\n"
                    "    Override Method Put(x: Int): String => \"sub\"\n"
                    "End Class\n"
                    "Local s: String = Sub().Put(1)")
              .empty());
}

TEST_CASE("an Interface's member like one of an Interface it extends takes its place", "[sema]") {
    // Doubled's Twice redeclares Sized's, so its default is what a type gets for both, whichever
    // is listed first, and a call through either, or on a type parameter, means one member.
    const std::string refined = "Interface Sized\n"
                                "    Method Size(): Int\n"
                                "    Method Twice(): Int => Size() * 2\n"
                                "End Interface\n"
                                "Interface Doubled Extends Sized\n"
                                "    Method Twice(): Int => Size() + 100\n"
                                "End Interface\n";
    const auto implementer = [](const std::string& program) {
        const fb::compilation_t compilation = fb::compile("test.fb", program);
        const fb::analysis_t& analysis = compilation.analysis;
        INFO(program);
        CHECK(compilation.diagnostics.diagnostics().empty());
        for (const fb::conformance_t& conformance : analysis.conformances) {
            if (analysis.types.name(conformance.type) == "Box" &&
                analysis.types.name(conformance.interface) == "Sized") {
                const fb::symbol_t& twice = analysis.symbol(conformance.members.at(1));
                return std::string(analysis.types.name(twice.default_interface));
            }
        }

        return std::string();
    };
    const std::string_view box = "    Method Size(): Int => 1\n"
                                 "End Class\n";
    CHECK(implementer(std::format("{}Class Box Implements Doubled\n{}", refined, box)) ==
          "Doubled");
    CHECK(implementer(std::format("{}Class Box Implements Sized, Doubled\n{}", refined, box)) ==
          "Doubled");
    CHECK(errors_of(std::format("{}Class Box Implements Doubled\n{}"
                                "Function D(d: Doubled): Int => d.Twice()\n"
                                "Function G<T: Doubled>(t: T): Int => t.Twice()\n"
                                "Function H<T: Sized & Doubled>(t: T): Int => t.Twice()\n"
                                "Local n = Box().Twice() + D(Box()) + G(Box()) + H(Box())",
                                refined, box))
              .empty());

    // An Operator too, like an Interface extending Comparable that gives its own >.
    const std::string_view ordered = "Import std.core\n"
                                     "Interface Ordered Extends Comparable\n"
                                     "    Operator >(a: Self, b: Self): Bool => True\n"
                                     "End Interface\n";
    const std::string_view point = "    Field n: Int\n"
                                   "    Operator <(a: P, b: P): Bool => a.n < b.n\n"
                                   "End Struct\n";
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Struct P Implements Comparable, Ordered\n{}"
                                "Function Big<T: Ordered>(a: T, b: T): Bool => a > b\n"
                                "Local b = P(2) > P(1) && Big(P(1), P(2))",
                                ordered, point))
              .empty());

    // Two Interfaces that each redeclare it give a type two defaults for the one they extend.
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}"
                                "Interface Reversed Extends Comparable\n"
                                "    Operator >(a: Self, b: Self): Bool => False\n"
                                "End Interface\n"
                                "Struct P Implements Ordered, Reversed\n{}",
                                ordered, point)) ==
          errors_t{ "Ordered: '>' is ambiguous here: Ordered and Reversed each give P a default of "
                    "it" });

    // In a default, a member of an Interface it extends used by name has this one's Self, with
    // the type arguments it extends that one with.
    CHECK(errors_of("Interface Shown<T>\n"
                    "    ReadOnly Property Item: T\n"
                    "    Method Get(): T\n"
                    "End Interface\n"
                    "Interface Damageable Extends Shown<String>\n"
                    "    Method Describe(): String => \"hurt {Item} {Get()}\"\n"
                    "    Method Size(): Int => Get().Chars.Count + Item.Chars.Count\n"
                    "End Interface")
              .empty());
}

TEST_CASE("a default Operator an Interface gives has the Interface's visibility, except ==",
          "[sema]") {
    // Code that can't name Ordered can't use its >, on a Public P either. Its == is what == means
    // for P everywhere, as inside the Structs and arrays that hold one.
    const std::string ordered = "Interface Ordered\n"
                                "    Method Key(): Int\n"
                                "    Operator >(a: Self, b: Self): Bool => a.Key() > b.Key()\n"
                                "    Operator ==(a: Self, b: Self): Bool => a.Key() == b.Key()\n"
                                "End Interface\n"
                                "Public Struct P\n"
                                "    Public Field n: Int\n"
                                "End Struct\n"
                                "Extend P Implements Ordered\n"
                                "    Method Key(): Int => n\n"
                                "End Extend\n";
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = ordered + "Public Function Bigger(a: P, b: P): Bool => a > b\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Local x = P(1) == P(2) || P(1) != P(2) || [P(1)].Contains(P(1))\n"
                      "Local y = Bigger(P(1), P(2))",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/a\"\nLocal x = P(5) > P(2)", modules) ==
          errors_t{ "P(5) > P(2): Operator > is private to lib/a.fb" });

    // Nor does the > it can't use implement Comparable's for an Extend in another file, which
    // takes Comparable's own.
    const std::vector<fb::library_file_t> alone{ { .path = "lib/a.fb", .text = ordered } };
    CHECK(errors_with("Import std.core\n"
                      "Import \"./lib/a\"\n"
                      "Extend P Implements Comparable\n"
                      "    Operator <(a: P, b: P): Bool => a.n < b.n\n"
                      "End Extend\n"
                      "Local x = P(5) > P(2)",
                      alone)
              .empty());
}

TEST_CASE("what implements an Interface's member is called through the Interface anywhere",
          "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/quiet.fb",
            .text = "Public Interface Shown\n"
                    "    Method Show(): String\n"
                    "End Interface\n"
                    "Public Class Quiet Implements Shown\n"
                    "    Method Show(): String => \"quiet\"\n"
                    "End Class\n"
                    "Public Function Make(): Quiet => Quiet()\n",
        },
    };
    const std::string_view import = "Import \"./lib/quiet\"\n";
    CHECK(errors_with(std::format("{}"
                                  "Local s: Shown = Make()\n"
                                  "Local text = s.Show()\n"
                                  "Function Twice<T: Shown>(value: T): String => value.Show()\n"
                                  "Local again = Twice(Make())",
                                  import),
                      modules)
              .empty());
    CHECK(errors_with(std::format("{}Local text = Make().Show()", import), modules) ==
          errors_t{ "Show: 'Show' is private to lib/quiet.fb" });
}

TEST_CASE("std's helpers are its own", "[sema]") {
    const std::string_view import = "Import std.collections\n";
    CHECK(
        errors_of(std::format("Import std.core\n{}"
                              "Local xs = [3, 1, 2]\n"
                              "xs.Sort()\n"
                              "MergeSort(xs, xs.Clone(), 0, 3, Function(a: Int, b: Int) => a < b)",
                              import)) ==
        errors_t{ "MergeSort: 'MergeSort' is Package in std/core/iterator.fb, so only the package "
                  "std can use it" });
    CHECK(errors_of(std::format("{}"
                                "Local m = Map<Int, Int>()\n"
                                "m.Add(1, 2)\n"
                                "m.Grow()\n"
                                "Local n = m.Entries.Count + m.IndexOf(1, 0)",
                                import)) ==
          errors_t{
              "Grow: 'Grow' is private to std/collections/map.fb",
              "Entries: 'Entries' is private to std/collections/map.fb",
              "IndexOf: 'IndexOf' is private to std/collections/map.fb",
          });
    CHECK(errors_of(std::format("{}Local s = Stack<Int>()\nLocal n = s.Items.Count", import)) ==
          errors_t{ "Items: 'Items' is private to std/collections/stack.fb" });
}

TEST_CASE("visibility is checked where code is written", "[sema]") {
    // A Public generic Function's code uses its module's own helpers for each type it's called
    // with, and a loop over a type's Fields goes over its private ones too.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/sum.fb",
            .text = "Import std.core\n"
                    "Public Function Sum<T: Numeric>(items: T[]): T\n"
                    "    Local total = T.Zero()\n"
                    "    For item In items\n"
                    "        total = Add(total, item)\n"
                    "    End For\n"
                    "    Return total\n"
                    "End Function\n"
                    "Function Add<T: Numeric>(a: T, b: T): T => a + b\n"
                    "Public Struct Pair\n"
                    "    Public Field A: Int = 1\n"
                    "    Field B: Int = 2\n"
                    "End Struct\n",
        },
    };
    CHECK(errors_with("Import \"./lib/sum\"\n"
                      "Local n = Sum([1, 2, 3])\n"
                      "Local f = Sum([1.5, 2.5])\n"
                      "Function Names<T>(): String\n"
                      "    Local text = \"\"\n"
                      "    For Field f In T\n"
                      "        text = text + f.Name\n"
                      "    End For\n"
                      "    Return text\n"
                      "End Function\n"
                      "Local names = Names<Pair>()",
                      modules)
              .empty());
}

TEST_CASE("an attribute's qualifier is an Import's", "[sema]") {
    // A type's name isn't a module's, however the file imports it.
    CHECK(errors_of("Import std.collections\n@Stack.Label(\"x\")\nFunction A()\nEnd Function") ==
          errors_t{ "Stack: 'Stack' isn't an imported module, so it can't come before '.'" });
}

TEST_CASE("an attribute is used where its Attribute is visible", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/tags.fb",
            .text = "Public Attribute Shown(Label: String = \"\")\nAttribute Hidden()\n",
        },
    };
    CHECK(errors_with("Import \"./lib/tags\"\n"
                      "Struct S\n"
                      "    @Shown(\"x\") Field a: Int\n"
                      "End Struct\n"
                      "Function Label<T>(): String\n"
                      "    For Field f In T\n"
                      "        Local shown = f.Attribute(Shown)\n"
                      "        If shown != Null Then Return shown.Label\n"
                      "    End For\n"
                      "    Return \"\"\n"
                      "End Function\n"
                      "Local label = Label<S>()",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/tags\"\n@Hidden\nFunction F()\nEnd Function", modules) ==
          errors_t{ "Hidden: 'Hidden' is private to lib/tags.fb" });

    // Through an Import's As, it's after the qualifier, in an attribute and in reflection.
    CHECK(errors_with("Import \"./lib/tags\" As T\n"
                      "Struct S\n"
                      "    @T.Shown(\"x\") Field a: Int\n"
                      "End Struct\n"
                      "Function Label<U>(): String\n"
                      "    For Field f In U\n"
                      "        Local shown = f.Attribute(T.Shown)\n"
                      "        If shown != Null && f.Has(T.Shown) Then Return shown.Label\n"
                      "    End For\n"
                      "    Return \"\"\n"
                      "End Function\n"
                      "Local label = Label<S>() + \"{S.Has(T.Shown)}\"",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./lib/tags\" As T\n"
                      "Struct S\n"
                      "    @T.Gone Field a: Int\n"
                      "    @T.Hidden Field b: Int\n"
                      "    @Q.Shown Field c: Int\n"
                      "    @S.Shown Field d: Int\n"
                      "    @Shown Field e: Int\n"
                      "End Struct\n"
                      "Function Has<U>(): Bool\n"
                      "    For Field f In U\n"
                      "        Return f.Has(T.Missing)\n"
                      "    End For\n"
                      "    Return False\n"
                      "End Function",
                      modules) ==
          errors_t{
              "Gone: 'Gone' isn't declared in \"./lib/tags\"",
              "Hidden: 'Hidden' is private to lib/tags.fb",
              "Q: 'Q' isn't declared",
              "S: 'S' is declared in this file, so it isn't an Import's qualifier",
              "Shown: 'Shown' is in lib.tags, which this file imports as T, so it's T.Shown",
              "Missing: 'Missing' isn't declared in \"./lib/tags\"",
          });
}

TEST_CASE("a Local or a block's Const takes no visibility", "[sema]") {
    CHECK(errors_of("Function F()\n    Public Local x = 1\nEnd Function") ==
          errors_t{ "Public: a Local is only its own code's, so it takes no Public; a top-level "
                    "Global or Const can" });
    CHECK(errors_of("Function F(): Int\n"
                    "    Public Const X = 1\n"
                    "    Protected Const Y = 2\n"
                    "    @Deprecated Const Z = 3\n"
                    "    Return X + Y + Z\n"
                    "End Function") ==
          errors_t{
              "Public: a Const in a block is only its own code's, so it takes no Public; a "
              "top-level Global or Const can",
              "Protected: Protected only goes before a Class's members, which the Classes "
              "extending it can use",
              "Z: a Const in a block can't be @Deprecated, since only its own code uses it",
          });
    CHECK(errors_of("Protected Local x = 1") ==
          errors_t{ "Protected: Protected only goes before a Class's members, which the Classes "
                    "extending it can use" });

    // A top-level one can.
    CHECK(errors_of("Public Const X = 1\nPackage Global y = 2\nLocal z = X + y").empty());
}

TEST_CASE("a conditional Method isn't in the Method table, so it can't be overridden", "[sema]") {
    const std::string_view base = "Import std.core\n"
                                  "Class Base<T>\n"
                                  "    Method Seal(): Int\n"
                                  "        Where T: Hashable\n"
                                  "        Return 3\n"
                                  "    End Method\n"
                                  "End Class\n";
    const std::string_view message =
        "'Seal' is a conditional Method of Base<T>, which only some of its instances have, so it "
        "isn't in the Method table and can't be overridden; give this one another name";
    CHECK(errors_of(std::format("{}Class IntSub Extends Base<Int>\n"
                                "    Override Method Seal(): Int => 7\n"
                                "End Class",
                                base)) == errors_t{ std::format("Seal: {}", message) });
    CHECK(errors_of(std::format("{}Class IntSub Extends Base<Int>\n"
                                "    Method Seal(): Int => 7\n"
                                "End Class",
                                base)) == errors_t{ std::format("Seal: {}", message) });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Class Sub<T> Extends Base<T>\n"
                                "    Override Method Seal(): Int\n"
                                "        Where T: Hashable\n"
                                "        Return 7\n"
                                "    End Method\n"
                                "End Class",
                                base)) == errors_t{ std::format("Seal: {}", message) });

    // An instance that never has it, like Base<Rock>, leaves its name free.
    CHECK(errors_of(std::format("{}Struct Rock\nEnd Struct\n"
                                "Class RockSub Extends Base<Rock>\n"
                                "    Method Seal(): Int => 7\n"
                                "End Class",
                                base))
              .empty());
}

TEST_CASE("a Const Function whose body didn't parse doesn't run in the compiler", "[sema]") {
    CHECK(errors_of("Const Function F(n: Int): Int\n"
                    "    Public\n"
                    "    Return n\n"
                    "End Function\n"
                    "Const k = F(2)") ==
          errors_t{ "Public: Public only goes before a declaration, like a Function or a Field" });
    CHECK(errors_of("Const Function F(n: Int): Int\n"
                    "    Return n +\n"
                    "End Function\n"
                    "Const k = F(2)") ==
          errors_t{ "End: expected an expression, found 'End Function'" });
}

TEST_CASE("an Abstract member has its Class's visibility, and an Override can't narrow a default",
          "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/shape.fb",
            .text = "Public Abstract Class Shape\n"
                    "    Abstract Method Area(): Int\n"
                    "    Abstract Property Sides: Int\n"
                    "    Protected Abstract Method Plan(): Int\n"
                    "End Class\n"
                    "Public Abstract Class Sealed\n"
                    "    Private Abstract Method Inside(): Int\n"
                    "End Class\n"
                    "Public Interface Doubled\n"
                    "    Method Twice(): Int => 2\n"
                    "End Interface\n"
                    "Public Class Ruler Implements Doubled\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/shape\"\n";

    // Another file completes a Public Abstract Class without saying the members' visibility.
    CHECK(errors_with(std::format("{}"
                                  "Class Square Extends Shape\n"
                                  "    Override Method Area(): Int => 4\n"
                                  "    Override Property Sides: Int\n"
                                  "        Get => 4\n"
                                  "        Set(value)\n"
                                  "        End Set\n"
                                  "    End Property\n"
                                  "    Override Method Plan(): Int => 1\n"
                                  "End Class\n"
                                  "Local s: Shape = Square()\n"
                                  "Local a = s.Area() + s.Sides",
                                  import),
                      modules)
              .empty());

    // A Private Abstract member keeps the Classes that complete it in its file.
    CHECK(errors_with(std::format("{}Class Mine Extends Sealed\nEnd Class", import), modules) ==
          errors_t{ "Mine: Mine has to be Abstract: it can't override Sealed's Abstract Method "
                    "'Inside', since 'Inside' is private to lib/shape.fb" });

    // A default an Interface gives is no exception to the narrowing rule.
    CHECK(errors_with(std::format("{}"
                                  "Class Mine Extends Ruler\n"
                                  "    Private Override Method Twice(): Int => 99\n"
                                  "End Class",
                                  import),
                      modules) ==
          errors_t{ "Twice: 'Twice' is Private, so it can't override Ruler's, which is Public" });
}

TEST_CASE("the signature rule covers every part a Public declaration shows", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/shapes.fb",
            .text = "Import std.core\n"
                    "Class Hidden\n"
                    "End Class\n"
                    "Interface Face\n"
                    "End Interface\n"
                    "Class Oops Extends Error\n"
                    "End Class\n"
                    "Public Function Risky() Throws Oops\n"
                    "End Function\n"
                    "Public Function Pick<T>(x: T)\n"
                    "    Where T: Face\n"
                    "End Function\n"
                    "Public Interface Wide Extends Face\n"
                    "End Interface\n"
                    "Public Struct P\n"
                    "    Public Field n: Int\n"
                    "End Struct\n"
                    "Extend P\n"
                    "    Public Method Make(): Hidden? => Null\n"
                    "End Extend\n"
                    "Public Class Saver\n"
                    "    Public Static Field Saved: Hidden? = Null\n"
                    "    Public Property Item: Hidden?\n"
                    "        Get => Null\n"
                    "    End Property\n"
                    "End Class\n",
        },
    };
    const std::string_view lib = "lib/shapes.fb";
    CHECK(errors_with("Import \"./lib/shapes\"", modules) ==
          errors_t{
              std::format("{0}: Risky: 'Risky' is Public, but it throws Oops, which is private to "
                          "{0}",
                          lib),
              std::format("{0}: Pick: 'Pick' is Public, but it needs Face, which is private to {0}",
                          lib),
              std::format("{0}: Wide: 'Wide' is Public, but it extends Face, which is private to "
                          "{0}",
                          lib),
              std::format("{0}: Make: 'Make' is Public, but it gives Hidden, which is private to "
                          "{0}",
                          lib),
              std::format("{0}: Saved: 'Saved' is Public, but it holds Hidden, which is private to "
                          "{0}",
                          lib),
              std::format("{0}: Item: 'Item' is Public, but it holds Hidden, which is private to "
                          "{0}",
                          lib),
          });

    // What an Extend adds is seen no further than the type it's added to: an instance or an
    // array of a Private type is its file's.
    const std::vector<fb::library_file_t> boxes{
        {
            .path = "lib/box.fb",
            .text = "Class Secret\n"
                    "End Class\n"
                    "Public Class Box<T>\n"
                    "End Class\n"
                    "Extend Box<Secret>\n"
                    "    Public Method M(): Secret? => Null\n"
                    "End Extend\n"
                    "Extend Secret[]\n"
                    "    Public Method N(): Secret? => Null\n"
                    "End Extend\n",
        },
    };
    CHECK(errors_with("Import \"./lib/box\"", boxes).empty());
}

TEST_CASE("an Extend's members have visibility across modules", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Struct P\n"
                    "    Public Field n: Int\n"
                    "End Struct\n"
                    "Extend P\n"
                    "    Method Hidden(): Int => 1\n"
                    "    Static Method Make(): P => P(1)\n"
                    "    Property Twice: Int\n"
                    "        Get => n * 2\n"
                    "    End Property\n"
                    "End Extend\n"
                    "Extend Int\n"
                    "    Method Triple(): Int => Self * 3\n"
                    "End Extend\n"
                    "Public Class Base\n"
                    "    Public New()\n"
                    "    End New\n"
                    "    Method Size(): Int => 1\n"
                    "End Class\n",
        },
    };
    const std::string_view import = "Import \"./lib/a\"\n";
    CHECK(errors_with(std::format("{}Local p = P(1)\nLocal a = p.Hidden()", import), modules) ==
          errors_t{ "Hidden: 'Hidden' is private to lib/a.fb" });
    CHECK(errors_with(std::format("{}Local p = P.Make()", import), modules) ==
          errors_t{ "Make: 'Make' is private to lib/a.fb" });
    CHECK(errors_with(std::format("{}Local p = P(1)\nLocal a = p.Twice", import), modules) ==
          errors_t{ "Twice: 'Twice' is private to lib/a.fb" });
    CHECK(errors_with(std::format("{}Local a = 5.Triple()", import), modules) ==
          errors_t{ "Triple: 'Triple' is private to lib/a.fb" });

    // The code that lists an Interface has to be able to use what implements it.
    CHECK(errors_with(std::format("{}Interface Sized\n"
                                  "    Method Size(): Int\n"
                                  "End Interface\n"
                                  "Extend Base Implements Sized\n"
                                  "End Extend",
                                  import),
                      modules) ==
          errors_t{ "Sized: Base implements Sized, so it needs a Method 'Size'; 'Size' is private "
                    "to lib/a.fb" });

    // Importing a module helps only where its member is one the code could use.
    const std::vector<fb::library_file_t> hinted{
        {
            .path = "lib/b.fb",
            .text = "Extend Int\n"
                    "    Method Twice(): Int => Self * 2\n"
                    "End Extend\n",
        },
        {
            .path = "lib/c.fb",
            .text = "Import \"./b\"\n",
        },
    };
    CHECK(errors_with("Import \"./lib/c\"\nLocal n = 5\nLocal a = n.Twice()", hinted) ==
          errors_t{ "Twice: Int32 has no member named 'Twice'" });
    CHECK(errors_with("Import \"./lib/c\"\nLocal a = 5.Twice()", hinted) ==
          errors_t{ "Twice: neither Int32 nor Int64 has a member named 'Twice'" });
}

TEST_CASE("a Const type argument that the code can't use says so", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib.fb",
            .text = "Const N: Int = 3\n"
                    "Public Class Sized<Const S: Int>\n"
                    "    Public New()\n"
                    "    End New\n"
                    "End Class\n",
        },
    };
    CHECK(errors_with("Import \"./lib\"\nLocal s = Sized<N>()", modules) ==
          errors_t{ "N: 'N' is private to lib.fb" });
    CHECK(errors_with("Import \"./lib\"\nLocal s: Sized<lib.N>? = Null", modules) ==
          errors_t{ "lib.N: 'N' is private to lib.fb" });
}

TEST_CASE("the News a Class takes are no more visible than its base's", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib.fb",
            .text = "Class Secret\n"
                    "End Class\n"
                    "Public Class Base\n"
                    "    New(s: Secret?)\n"
                    "    End New\n"
                    "    Public New(n: Int)\n"
                    "    End New\n"
                    "End Class\n"
                    "Public Class Sub Extends Base\n"
                    "End Class\n",
        },
    };
    CHECK(errors_with("Import \"./lib\"\nLocal s = Sub(Null)", modules) ==
          errors_t{
              "Sub: Sub's New is private to lib.fb",
              "Null: Null needs a type from where it's used, like 'Local e: Enemy? = Null'",
          });
    CHECK(errors_with("Import \"./lib\"\nLocal s = Sub(3)", modules).empty());
}

TEST_CASE("a Method the code can't use hides nothing above it", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Class Base\n"
                    "    Public New()\n"
                    "    End New\n"
                    "End Class\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./a\"\n"
                    "Public Class Sub Extends Base\n"
                    "    Method Foo(): String => \"sub\"\n"
                    "End Class\n",
        },
        {
            .path = "lib/c.fb",
            .text = "Import \"./a\"\n"
                    "Extend Base\n"
                    "    Public Method Foo(): Int => 42\n"
                    "End Extend\n",
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Import \"./lib/b\"\n"
                      "Import \"./lib/c\"\n"
                      "Local n: Int = Sub().Foo()",
                      modules)
              .empty());
}

TEST_CASE("what an Extend's listing gives is the Extend's, and a default is checked like a "
          "Method written there",
          "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Interface Doubled\n"
                    "    Method Twice(): Int => 200\n"
                    "End Interface\n"
                    "Public Class Base\n"
                    "    Public New()\n"
                    "    End New\n"
                    "    Protected Method Twice(): Int => 2\n"
                    "    Public Method UseTwice(): Int => Twice()\n"
                    "End Class\n",
        },
    };
    // A default another file's Extend gives doesn't clash with the type's own code.
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Extend Base Implements Doubled\n"
                      "End Extend\n"
                      "Local b = Base()\n"
                      "Local d: Doubled = b\n"
                      "Local n = b.UseTwice() + d.Twice()",
                      modules)
              .empty());

    // A default named like an inherited Field, or beside a Method the Class can't override.
    CHECK(errors_of("Interface Doubled\n"
                    "    Method Twice(): Int => 200\n"
                    "End Interface\n"
                    "Class Base\n"
                    "    Field Twice: Int = 2\n"
                    "End Class\n"
                    "Class Sub Extends Base Implements Doubled\n"
                    "End Class") ==
          errors_t{ "Doubled: Sub takes Doubled's default 'Twice', but 'Twice' is already "
                    "declared in Base" });
    // Or like the type's own Field or Static Method, or an inherited Static Method.
    CHECK(errors_of("Interface Doubled\n"
                    "    Method Twice(): Int => 200\n"
                    "End Interface\n"
                    "Struct Own Implements Doubled\n"
                    "    Field Twice: Int\n"
                    "End Struct\n"
                    "Struct Made Implements Doubled\n"
                    "    Static Method Twice(): Int => 2\n"
                    "    Method Twice(n: Int): Int => n\n"
                    "End Struct\n"
                    "Class Base\n"
                    "    Static Method Twice(): Int => 3\n"
                    "End Class\n"
                    "Class Sub Extends Base Implements Doubled\n"
                    "End Class") ==
          errors_t{ "Doubled: Own takes Doubled's default 'Twice', but 'Twice' is already "
                    "declared in Own",
                    "Doubled: Made takes Doubled's default 'Twice', but 'Twice' is already "
                    "declared in Made",
                    "Doubled: Sub takes Doubled's default 'Twice', but 'Twice' is already "
                    "declared in Base" });
    CHECK(errors_with("Import \"./lib/b\"\n"
                      "Interface Doubled\n"
                      "    Method Twice(): Int => 200\n"
                      "End Interface\n"
                      "Class Sub Extends Base Implements Doubled\n"
                      "End Class",
                      { {
                          .path = "lib/b.fb",
                          .text = "Public Class Base\n"
                                  "    Method Twice(): Int => 2\n"
                                  "End Class\n",
                      } }) ==
          errors_t{ "Doubled: Sub takes Doubled's default 'Twice', which would be a second Method "
                    "of its name and parameters beside Base's, which it can't override, since "
                    "'Twice' is private to lib/b.fb" });
}

TEST_CASE("two Interfaces' default Operator ==s make == on a type ambiguous everywhere", "[sema]") {
    const std::string_view interfaces =
        "Interface Keyed\n"
        "    Method Key(): Int\n"
        "    Operator ==(a: Self, b: Self): Bool => a.Key() == b.Key()\n"
        "End Interface\n"
        "Interface Alike\n"
        "    Operator ==(a: Self, b: Self): Bool => False\n"
        "End Interface\n";
    const std::string_view message = "Box takes Operator == from both Alike and Keyed, so == on it "
                                     "is ambiguous; give it an Operator == of its own";
    CHECK(errors_of(std::format("{}Struct Box Implements Alike, Keyed\n"
                                "    Field n: Int\n"
                                "    Method Key(): Int => n\n"
                                "End Struct\n"
                                "Local a = Box(1) == Box(1)\n"
                                "Local b = [Box(1)].Contains(Box(1))\n"
                                "Local c = (Box(1), 0) == (Box(1), 0)",
                                interfaces)) == errors_t{ std::format("Box: {}", message) });

    // Listing Equatable or Hashable too doesn't report it again.
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Struct Box Implements Alike, Keyed, Equatable\n"
                                "    Field n: Int\n"
                                "    Method Key(): Int => n\n"
                                "End Struct\n"
                                "Function Same<T: Equatable>(a: T, b: T): Bool => a == b\n"
                                "Local a = Same(Box(1), Box(1))",
                                interfaces)) == errors_t{ std::format("Box: {}", message) });
    CHECK(errors_of(std::format("Import std.core\n"
                                "{}Struct Box Implements Alike, Keyed, Hashable\n"
                                "    Field n: Int\n"
                                "    Method Key(): Int => n\n"
                                "    Method Hash(): UInt64 => 0\n"
                                "End Struct",
                                interfaces)) == errors_t{ std::format("Box: {}", message) });

    // Its own Operator == settles it.
    CHECK(errors_of(std::format("{}Struct Box Implements Alike, Keyed\n"
                                "    Field n: Int\n"
                                "    Method Key(): Int => n\n"
                                "    Operator ==(a: Box, b: Box): Bool => a.n == b.n\n"
                                "End Struct\n"
                                "Local a = [Box(1)].Contains(Box(1))",
                                interfaces))
              .empty());
}

TEST_CASE("a hint names only std modules whose names the code could use", "[sema]") {
    CHECK(errors_of("Local s = Stack<Int>()") ==
          errors_t{ "Stack<Int>: 'Stack' is in std.collections, which this file doesn't import" });
    CHECK(errors_of("Local a = MergeSort([3, 1])") ==
          errors_t{ "MergeSort: 'MergeSort' isn't declared" });
}

TEST_CASE("a visibility error labels the declaration, and a constructor's least visible Field",
          "[sema]") {
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    files.push_back({
        .path = "lib.fb",
        .text = "Public Struct Handle\n"
                "    Public Field Index: Int\n"
                "    Field Generation: Int\n"
                "End Struct\n"
                "Public Function Visible()\n"
                "End Function\n"
                "Function Hidden()\n"
                "End Function\n",
    });
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Import \"./lib\"\nLocal h = Handle(1, 2)\nHidden()", files);
    const std::vector<fb::diagnostic_t>& diagnostics = compilation.diagnostics.diagnostics();
    REQUIRE(diagnostics.size() == 2);
    const auto label_text = [&](const fb::diagnostic_t& diagnostic) {
        REQUIRE(diagnostic.labels.size() == 1);
        const fb::span_t span = diagnostic.labels.front().span;
        return std::string(compilation.sources.get(span.file).slice(span));
    };
    CHECK(diagnostics[0].message ==
          "Handle's constructor takes its Fields, and 'Generation' is private to lib.fb");
    CHECK(label_text(diagnostics[0]) == "Generation");
    CHECK(diagnostics[1].message == "'Hidden' is private to lib.fb");
    CHECK(label_text(diagnostics[1]) == "Hidden");
}

TEST_CASE("a Callback is a Function of exactly its types, or a lambda that captures nothing",
          "[sema]") {
    // A function value is a closure, which C can't call, whatever it holds.
    CHECK(errors_of("Function Twice(n: Int32): Int32 => n * 2\n"
                    "Local held: Func(Int32): Int32 = Twice\n"
                    "Local c: Callback(Int32): Int32 = held") ==
          errors_t{ "held: expected Callback(Int32): Int32, found Func(Int32): Int32, a function "
                    "value, which C can't call; a Callback is a Function's name or a lambda that "
                    "captures nothing, written where it's expected" });
    CHECK(errors_of("Import std.core\n"
                    "Function Twice(n: Int32): Int32 => n * 2\n"
                    "Local a: Callback(Int32): Int32 = Twice\n"
                    "Local b: Callback(Int32): Int32 = Function(n: Int32): Int32 => n + 1\n"
                    "Local c: Callback(Int32) = Null\n"
                    "Unsafe\n"
                    "    Local d = a(1) + b(2)\n"
                    "End Unsafe\n"
                    "Print(\"{a == b} {c == Null}\")\n")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Function Wide(n: Int64): Int64 => n\n"
                    "Function Pick<T>(n: Int32): Int32 => n\n"
                    "Async Function Later(n: Int32): Int32 => n\n"
                    "Function Fail(n: Int32): Int32 Throws Error\n"
                    "    Return n\n"
                    "End Function\n"
                    "Unsafe Function Risky(n: Int32): Int32 => n\n"
                    "Local base = 3\n"
                    "Local a: Callback(Int32): Int32 = Wide\n"
                    "Local b: Callback(Int32): Int32 = Pick\n"
                    "Local c: Callback(Int32): Int32 = Later\n"
                    "Local d: Callback(Int32): Int32 = Fail\n"
                    "Local e: Callback(Int32): Int32 = Risky\n"
                    "Local f: Callback(Int32): Int32 = Function(n: Int32): Int32 => n + base\n"
                    "Local g: Callback(Int32): Int32 = Function(n: Int64): Int32 => 1\n") ==
          errors_t{
              std::string("Wide: 'Wide' is Func(Int64): Int64, not Callback(Int32): Int32; a "
                          "Callback takes a Function of exactly its types"),
              std::string("Pick: 'Pick' is generic, so it can't be a Callback, which C calls with "
                          "one set of types"),
              std::string("Later: 'Later' is Async, so it can't be a Callback"),
              std::string("Fail: 'Fail' can throw, so it can't be a Callback, since C has no "
                          "errors to catch"),
              std::string("Risky: 'Risky' is Unsafe, so it can't be a Callback, since C calls it "
                          "without Unsafe code"),
              std::string("Function(n: Int32): Int32 => n + base: a Callback's lambda can't use "
                          "'base' from around it, since C passes it only its arguments; a "
                          "Ptr<Void> C passes along can carry what it needs"),
              std::string("Function(n: Int64): Int32 => 1: this lambda is Func(Int64): Int32, not "
                          "Callback(Int32): Int32; a Callback takes a lambda of exactly its types"),
          });

    // It takes and gives what C can, is Null already, and only Unsafe code calls it.
    CHECK(errors_of("Import std.core\n"
                    "Local g: Callback(String): Int32 = Null\n"
                    "Local h: Callback(Int32)? = Null\n"
                    "Function Twice(n: Int32): Int32 => n * 2\n"
                    "Local i: Callback(Int32): Int32 = Twice\n"
                    "Print(\"{i(2)}\")\n") ==
          errors_t{
              std::string("String: a Callback takes and gives what C can: numbers, Bools, Enums, "
                          "CStrings, Ptrs, and Callbacks, and a Struct through a Ptr, not String"),
              std::string("Callback(Int32)?: Callback(Int32) can be Null already, as C's NULL"),
              std::string("i: calling a Callback needs Unsafe code: an Unsafe block, Function, or "
                          "Method"),
          });

    // C can call one on any thread.
    CHECK(errors_of("@MainThread Function Frame(): Int32 => 1\n"
                    "Function Uses(n: Int32): Int32 => Frame() + n\n"
                    "Local cb: Callback(Int32): Int32 = Uses\n") ==
          errors_t{ "Uses: C code can call a Callback on any thread, but it calls 'Frame', which "
                    "is @MainThread" });

    // It's C's pointer, which isn't a T? or shown in a string.
    CHECK(errors_of("Import std.core\n"
                    "Local cb: Callback(Int32) = Null\n"
                    "Local other = cb ?? cb\n"
                    "Print(\"{cb}\")\n") ==
          errors_t{
              std::string("cb: Callback(Int32) is C's pointer, not a T?, so '\?\?' doesn't take "
                          "it; compare it with Null instead"),
              "cb: Callback(Int32) can't be shown in a string",
          });
}

TEST_CASE("a Callback's Function takes values, and is the one Function that fits", "[sema]") {
    CHECK(errors_of("Function Bump(ByRef n: Int32): Int32\n"
                    "    n += 1\n"
                    "    Return n\n"
                    "End Function\n"
                    "Local cb: Callback(Int32): Int32 = Bump\n") ==
          errors_t{ "Bump: 'Bump' takes a parameter ByRef, so it can't be a Callback, whose "
                    "parameters C passes as values" });

    // Two imports' Functions that fit are ambiguous, and one after its qualifier is a Callback.
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Public Function Pick(n: Int32): Int32 => n + 100\n" },
        { .path = "lib/b.fb", .text = "Public Function Pick(n: Int32): Int32 => n + 200\n" },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Import \"./lib/b\"\n"
                      "Local cb: Callback(Int32): Int32 = Pick\n",
                      modules) == errors_t{ "Pick: the Callback 'Pick' is ambiguous" });
    CHECK(errors_with("Import std.core\n"
                      "Import \"./lib/a\" As A\n"
                      "Local cb: Callback(Int32): Int32 = A.Pick\n"
                      "Print(\"{cb == A.Pick}\")\n",
                      modules)
              .empty());

    // An overload takes a Function's name or a lambda for its Callback parameter.
    CHECK(errors_of("Function Twice(n: Int32): Int32 => n * 2\n"
                    "Function Apply(f: Callback(Int32): Int32, n: Int32): Int32\n"
                    "    Unsafe\n"
                    "        Return f(n)\n"
                    "    End Unsafe\n"
                    "End Function\n"
                    "Function Apply(f: Callback(Float64): Float64, n: Float64): Float64\n"
                    "    Unsafe\n"
                    "        Return f(n)\n"
                    "    End Unsafe\n"
                    "End Function\n"
                    "Local a = Apply(Twice, 3)\n"
                    "Local b = Apply(Function(n: Float64): Float64 => n / 2, 3.0)\n")
              .empty());

    // A lambda tried against a generic's Callback reports what its code reaches once.
    CHECK(errors_of("@MainThread Function Frame(): Int32 => 1\n"
                    "Function Run<T>(cb: Callback(Int32): Int32, x: T): T => x\n"
                    "Local a = Run(Function(n: Int32): Int32 => Frame() + n, 5)\n")
              .size() == 1);
}

TEST_CASE("a Callback's call takes its values in order, and a Callback is a pointer's size",
          "[sema]") {
    CHECK(errors_of("Function Twice(n: Int32): Int32 => n * 2\n"
                    "Local cb: Callback(Int32): Int32 = Twice\n"
                    "Local p: Ptr<Callback(Int32): Int32> = Null\n"
                    "Unsafe\n"
                    "    Local a = cb(n: 1)\n"
                    "    Local b = p.Value(5)\n"
                    "End Unsafe\n"
                    "Local c: Int32 = [cb]\n") ==
          errors_t{
              "n: a Callback's parameters have no names",
              "[cb]: expected Int32, found (Callback(Int32): Int32)[]",
          });
    CHECK(probe("Local size = SizeOf<Callback(Int32): Int32>()") == "Int64 8");
}

TEST_CASE("a @CLayout Struct holds what C can, and is the only Struct C sees", "[sema]") {
    CHECK(errors_of("@CLayout Struct Good\n"
                    "    Field x: Int32\n"
                    "    Field next: Ptr<Good>\n"
                    "    Field inner: Inner\n"
                    "    Field bytes: UInt8[4]\n"
                    "    Field name: CString\n"
                    "End Struct\n"
                    "@CLayout Struct Inner\n"
                    "    Field f: Float\n"
                    "End Struct\n"
                    "Extern \"C\"\n"
                    "    Function Take(good: Good, ByRef inner: Inner, p: Ptr<Good>): Good\n"
                    "End Extern\n")
              .empty());
    CHECK(errors_of("@CLayout Struct Bad\n"
                    "    Field s: String\n"
                    "    Field plain: Plain\n"
                    "    Field far: Ptr<Plain>\n"
                    "End Struct\n"
                    "Struct Plain\n"
                    "    Field x: Int32\n"
                    "End Struct\n"
                    "@CLayout Struct Empty\n"
                    "End Struct\n"
                    "@CLayout Struct Generic<T>\n"
                    "    Field x: Int32\n"
                    "End Struct\n") ==
          errors_t{
              std::string("s: a @CLayout Struct's Field holds what C can: a number, Bool, Enum, "
                          "CString, Ptr, Callback, or a fixed array or @CLayout Struct of these; "
                          "String isn't one"),
              std::string("plain: a @CLayout Struct's Field holds other Structs only when "
                          "they're @CLayout too, and Plain isn't"),
              std::string("far: a @CLayout Struct's Field holds other Structs only when they're "
                          "@CLayout too, and Plain isn't"),
              std::string("Empty: a @CLayout Struct needs a Field, since C has no empty structs"),
              std::string("Generic: a @CLayout Struct can't be generic, since C lays out each "
                          "type it has once"),
          });

    // A Struct that C code sees, by value, ByRef, or through a Ptr, is @CLayout.
    const std::string_view sees = "C code sees Plain here, so it has to be @CLayout, which lays it "
                                  "out as C does";
    CHECK(errors_of("Struct Plain\n"
                    "    Field x: Int32\n"
                    "End Struct\n"
                    "Extern \"C\"\n"
                    "    Function Take(a: Plain, ByRef b: Plain, c: Ptr<Ptr<Plain>>): Plain[2]\n"
                    "End Extern\n") ==
          errors_t{
              std::format("Plain: {}", sees),
              std::format("Plain: {}", sees),
              std::format("Ptr<Ptr<Plain>>: {}", sees),
              std::string("Plain[2]: Plain[2] can't come from C: an Extern Function gives a "
                          "number, Bool, an Enum, a Ptr, a Callback, a CString, a @CLayout Struct, "
                          "or a String or String?, which is a copy of the text"),
          });

    // So is one C code sees through what a Callback takes or gives.
    CHECK(errors_of("Struct Plain\n"
                    "    Field x: Int32\n"
                    "End Struct\n"
                    "@CLayout Struct Hooks\n"
                    "    Field each: Callback(Ptr<Plain>)\n"
                    "End Struct\n"
                    "Extern \"C\"\n"
                    "    Function Visit(each: Callback(Ptr<Plain>))\n"
                    "    Function Maker(): Callback(): Ptr<Plain>\n"
                    "End Extern\n") ==
          errors_t{
              std::string("each: a @CLayout Struct's Field holds other Structs only when they're "
                          "@CLayout too, and Plain isn't"),
              std::format("Callback(Ptr<Plain>): {}", sees),
              std::format("Callback(): Ptr<Plain>: {}", sees),
          });
}

TEST_CASE("an Extern Function takes and gives what C has", "[sema]") {
    // Numbers, Bool, Enums, and CString go as they are, a String becomes a CString for the call,
    // a String C gives is a copy of its text, and ByRef passes a variable's address.
    CHECK(errors_of("Enum Mode: UInt8\n"
                    "    On\n"
                    "End Enum\n"
                    "Extern \"C\"\n"
                    "    Function F(a: Int8, b: UInt64, c: Float32, d: Bool, e: Mode, f: CString,\n"
                    "               ByRef g: Int32, ByRef h: CString): Bool\n"
                    "    Function Look(name: CString): String?\n"
                    "    Function Name(n: Int32): String = \"strerror\"\n"
                    "End Extern\n"
                    "Global h: CString\n"
                    "Local g: Int32 = 0\n"
                    "Local ok = F(1, 2, 3.0, True, Mode.On, h, ByRef g, ByRef h)\n"
                    "Local found: String? = Look(\"PATH\")\n"
                    "Local named: String = Name(2)\n"
                    "Local f: Func(CString): String? = Look")
              .empty());
    CHECK(errors_of("Extern\n"
                    "    Function F(s: String): Int[]\n"
                    "    Function G(a: Int[], ByRef s: String)\n"
                    "End Extern") ==
          errors_t{
              "String: C takes a CString, not a String; declare it CString, which a String "
              "argument becomes for the call",
              "Int[]: Int32[] can't come from C: an Extern Function gives a number, Bool, an "
              "Enum, a Ptr, a Callback, a CString, a @CLayout Struct, or a String or String?, "
              "which is a copy of the text",
              "Int[]: Int32[] can't go to C: an Extern Function takes numbers, Bool, Enums, Ptrs, "
              "Callbacks, CString, and @CLayout Structs",
              "String: C takes text ByRef as a CString, which C can set; a String can't go ByRef",
          });

    // C could give back a pointer into a String's copy, which goes right after the call: a
    // CString, a Ptr to its bytes, or what holds one. A Ptr<Void> is a handle, which can't.
    const std::string escapes =
        "this Extern Function gives, or sets through ByRef or a Ptr, what could point into text, "
        "a CString or a Ptr to Int8s or UInt8s, and a String's copy goes right after the call; it "
        "takes only CStrings";
    CHECK(errors_of("Extern\n"
                    "    Function strstr(text: CString, part: CString): CString\n"
                    "    Function Split(text: CString, ByRef rest: CString): Int32\n"
                    "End Extern\n"
                    "Local found = strstr(\"abc\", \"b\")\n"
                    "Local rest = found\n"
                    "Local n = Split(\"a,b\", ByRef rest)") == errors_t{
                                                                   "\"abc\": " + escapes,
                                                                   "\"b\": " + escapes,
                                                                   "\"a,b\": " + escapes,
                                                               });
    CHECK(
        errors_of("@CLayout Struct Found\n"
                  "    Field at: Ptr<UInt8>\n"
                  "End Struct\n"
                  "Extern\n"
                  "    Function strchr(text: CString, c: Int32): Ptr<UInt8>\n"
                  "    Function strtol(text: CString, ByRef rest: Ptr<Int8>, base: Int32): Int64\n"
                  "    Function Find(text: CString): Ptr<Found>\n"
                  "    Function Open(name: CString): Ptr<Void>\n"
                  "End Extern\n"
                  "Local a = strchr(\"abc\", 98)\n"
                  "Local rest: Ptr<Int8> = Null\n"
                  "Local b = strtol(\"12\", ByRef rest, 10)\n"
                  "Local c = Find(\"x\")\n"
                  "Local d = Open(\"y\")") == errors_t{
                                                  "\"abc\": " + escapes,
                                                  "\"12\": " + escapes,
                                                  "\"x\": " + escapes,
                                              });

    // A Struct declared after the Function counts as one before it, and C writes text through
    // a Ptr<CString> as through a ByRef CString.
    CHECK(errors_of("Extern\n"
                    "    Function First(text: CString): Ptr<Later>\n"
                    "    Function strtol(text: CString, rest: Ptr<CString>, base: Int32): Int64\n"
                    "    Function Count(text: CString, counts: Ptr<Int32>): Int32\n"
                    "End Extern\n"
                    "@CLayout Struct Later\n"
                    "    Field name: CString\n"
                    "End Struct\n"
                    "Local a = First(\"a\")\n"
                    "Local b = strtol(\"12\", Null, 10)\n"
                    "Local c = Count(\"c\", Null)\n") == errors_t{
                                                             "\"a\": " + escapes,
                                                             "\"12\": " + escapes,
                                                         });

    // A String? that may be Null is checked first.
    CHECK(errors_of("Extern\n"
                    "    Function strlen(text: CString): UInt64\n"
                    "End Extern\n"
                    "Function Size(text: String?): UInt64 => strlen(text)") ==
          errors_t{ "text: expected String, found String?, which may be Null; check it first, or "
                    "give a value for Null with '?"
                    "?'" });

    // Only an Extern Function's CString parameter takes a String.
    CHECK(errors_of("Function Keep(c: CString)\nEnd Function\nKeep(\"a\")") ==
          errors_t{ "\"a\": expected CString, found String" });
}

TEST_CASE("an Extern Function has only what C code has", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Class Oops Extends Error\n"
                    "End Class\n"
                    "Extern\n"
                    "    Function A<T>(x: Int)\n"
                    "    Function B() Throws Oops\n"
                    "    Async Function C()\n"
                    "    Const Function D(): Int\n"
                    "    @Inline Function E()\n"
                    "    Function F(x: Int = 1)\n"
                    "End Extern") ==
          errors_t{
              "A: an Extern Function can't be generic, since C has one function for each symbol",
              "Oops: an Extern Function can't throw, since C's functions have no errors",
              "C: an Extern Function can't be Async, since C's functions can't Await",
              "D: an Extern Function can't be Const, since the compiler can't run C's code",
              "E: an Extern Function can't be @Inline, since its code is C's",
              "1: an Extern Function's parameter has no default, since C's have none",
          });

    // A Const Function can't call one, since the compiler can't run it.
    CHECK(errors_of("Extern\n"
                    "    Function abs(n: Int32): Int32\n"
                    "End Extern\n"
                    "Const Function Twice(n: Int32): Int32 => abs(n) * 2") ==
          errors_t{ "abs: a Const Function can only call Const Functions, and 'abs' isn't one" });

    // Its parameters' names are one each, as a Function's with a body are.
    CHECK(errors_of("Extern\n    Function div(a: Int32, a: Int32): Int32\nEnd Extern") ==
          errors_t{ "a: 'a' is already declared in this scope" });
}

TEST_CASE("Extern blocks' conventions, libraries, and symbols are checked", "[sema]") {
    const std::string intrinsic = "\"llvm.sqrt.f64\": 'llvm.sqrt.f64' is one of LLVM's own "
                                  "functions, which C code doesn't define";
    CHECK(errors_of("Extern \"runtime\"\n    Function fb_print(text: String)\nEnd Extern") ==
          errors_t{ "\"runtime\": only the standard library calls the runtime; C code's "
                    "convention is \"C\"" });
    CHECK(errors_of("Extern Lib \"vecmath\"\nEnd Extern") ==
          errors_t{ "\"vecmath\": 'Lib' names one of a package's [native] libraries, and this "
                    "program has no project.toml" });
    CHECK(errors_of("Extern\n"
                    "    Function fb_panic()\n"
                    "    Function Start() = \"main\"\n"
                    "    Function Twice(n: Int32): Int32 = \"fb.Twice(Int32)\"\n"
                    "    Function Root(x: Int32): Int32 = \"llvm.sqrt.f64\"\n"
                    "    Function Nothing() = \"\"\n"
                    "    Function fb_print(text: CString)\n"
                    "    Function Cut() = \"abs\\x00wide\"\n"
                    "    Function Spaced() = \"f x\"\n"
                    "    Function Decorated() = \"_f@8\"\n"
                    "End Extern") ==
          errors_t{
              "fb_panic: 'fb_panic' is a symbol the program defines itself",
              "\"main\": 'main' is a symbol the program defines itself",
              "\"fb.Twice(Int32)\": 'fb.Twice(Int32)' is a symbol the program defines itself",
              intrinsic,
              "\"\": a C symbol has a name",
              "fb_print: 'fb_print' is a symbol the program defines itself",
              "\"abs\\x00wide\": a C symbol can't hold a NUL",
              "\"f x\": a C symbol can't hold a space or a control character",
          });

    // One C symbol has one signature, where an Enum is its integer type.
    CHECK(errors_of("Enum Offset: Int32\n"
                    "    Back = -3\n"
                    "End Enum\n"
                    "Extern\n"
                    "    Function abs(n: Int32): Int32\n"
                    "    Function Distance(n: Offset): Int32 = \"abs\"\n"
                    "    Function Wide(n: Int64): Int64 = \"abs\"\n"
                    "End Extern") ==
          errors_t{ "Wide: 'abs' is a C symbol with one signature, but this declaration "
                    "gives Int64 where the first gives Int32" });
}

TEST_CASE("a Spawn's work gives nothing that points at memory", "[sema]") {
    CHECK(
        errors_of("Struct Holder\n"
                  "    Field where: Ptr<Int32>\n"
                  "End Struct\n"
                  "Local a = Spawn(Function(): Ptr<Int32> => Null)\n"
                  "Local b = Spawn(Function() => Holder(Null))\n") ==
        errors_t{
            std::string("Function(): Ptr<Int32> => Null: the work Spawn runs can't give "
                        "Ptr<Int32>, since a CString or a Ptr points at memory, which nothing "
                        "keeps for another thread"),
            std::string("Function() => Holder(Null): the work Spawn runs can't give Holder, since "
                        "a CString or a Ptr points at memory, which nothing keeps for another "
                        "thread"),
        });
}

TEST_CASE("a CString is C's, so it isn't Printable, Equatable, or Sendable", "[sema]") {
    CHECK(errors_of("Function Show(c: CString): String => \"{c}\"") ==
          errors_t{ "c: a CString can't be shown in a string; show the String its ToString() "
                    "gives, in Unsafe code" });
    CHECK(errors_of("Function Same(a: CString, b: CString): Bool => a == b") ==
          errors_t{ "a == b: '==' can't be used on CString; compare the Strings ToString() "
                    "gives, in Unsafe code" });
    CHECK(errors_of("Function Has(a: CString, b: CString): Bool => [a].Contains(b)") ==
          errors_t{ "[a].Contains: Contains compares with ==, which CString doesn't have" });
    CHECK(errors_of("Function Make(): CString[] => []\n"
                    "Local t = Spawn(Function() => Make())") ==
          errors_t{ "Function() => Make(): the work Spawn runs can't give CString[], since a "
                    "CString or a Ptr points at memory, which nothing keeps for another thread" });
    // An object it makes holds one too, however deep, even in a Class that holds its own kind.
    CHECK(errors_of("Class Link\n"
                    "    Field next: Link?\n"
                    "    Field at: Ptr<Int32>\n"
                    "End Class\n"
                    "Class Chain\n    Field first: Link?\nEnd Class\n"
                    "Local t = Spawn(Function() => Chain())") ==
          errors_t{ "Function() => Chain(): the work Spawn runs can't give Chain, since a "
                    "CString or a Ptr points at memory, which nothing keeps for another thread" });
    CHECK(errors_of("Function Use(c: CString): Int => 1\n"
                    "Function Send(c: CString)\n"
                    "    Local t = Spawn(Function() => Use(c))\n"
                    "End Function") ==
          errors_t{ "Function() => Use(c): 'c' is CString, which another thread can't have: a "
                    "CString points at C code's memory, which nothing keeps for another thread" });
}

TEST_CASE("an Extern block's visibility is its Functions'", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib.fb",
            .text = "Enum Secret: Int32\n"
                    "    A\n"
                    "End Enum\n"
                    "Public Extern\n"
                    "    Function abs(n: Int32): Int32\n"
                    "    Private Function labs(n: Int32): Int32\n"
                    "    Function Hidden(s: Secret): Int32 = \"abs\"\n"
                    "End Extern\n",
        },
    };
    CHECK(errors_with("Import \"./lib\"\nLocal a = abs(-1) + labs(-2)", modules) ==
          errors_t{
              "labs: 'labs' is private to lib.fb",
              "lib.fb: Hidden: 'Hidden' is Public, but it takes Secret, which is private to lib.fb",
          });

    // Protected goes only before a Class's members; the block's Functions are then the file's.
    const std::vector<fb::library_file_t> guarded{
        {
            .path = "lib.fb",
            .text = "Protected Extern\n    Function abs(n: Int32): Int32\nEnd Extern\n",
        },
    };
    CHECK(errors_with("Import \"./lib\"\nLocal a = abs(-1)", guarded) ==
          errors_t{
              "abs: 'abs' is private to lib.fb",
              "lib.fb: Protected: Protected only goes before a Class's members, which the Classes "
              "extending it can use",
          });
}

TEST_CASE("a Function's own types beat a built-in's that take anything", "[sema]") {
    // RefEquals takes any Classes, so one of a module's own that names its types is better.
    CHECK(errors_of("Class Node\n"
                    "End Class\n"
                    "Function RefEquals(a: Int32, b: Int32): Bool => a == b\n"
                    "Function RefEquals(a: Node, b: Node): Bool => False\n"
                    "Local x = RefEquals(1, 2)\n"
                    "Local y = RefEquals(Node(), Node())")
              .empty());

    // Even where the argument converts to the type it names, as a Class extending it does.
    CHECK(errors_of("Class Base\n"
                    "End Class\n"
                    "Class Derived Extends Base\n"
                    "End Class\n"
                    "Function RefEquals(a: Base, b: Base): Bool => False\n"
                    "Local x = RefEquals(Derived(), Derived())\n"
                    "Local y: Base? = Null\n"
                    "Local z = RefEquals(Base(), Derived())")
              .empty());

    // An overload of a built-in that a call fits as well as the built-in says so.
    CHECK(errors_of("Function Assert(condition: Bool)\n"
                    "End Function\n"
                    "Assert(True)") ==
          errors_t{ "Assert: the call to 'Assert' is ambiguous; the built-in one fits it as well "
                    "as this one" });
}

TEST_CASE("a module's own Function hides only Functions, so a type name finds an import's type",
          "[sema]") {
    CHECK(errors_of("Import std.collections\n"
                    "Function Map(x: Int): Int => x * 2\n"
                    "Local m: Map<String, Int> = {\"a\": 1}\n"
                    "Local n = Map(2)\n")
              .empty());
}

TEST_CASE("Null fits an overload's Callback, Ptr, or CString parameter, and code its Callback",
          "[sema]") {
    CHECK(errors_of("Function Take(cb: Callback(Int32): Int32): Int32 => 1\n"
                    "Function Take(s: String): Int32 => 2\n"
                    "Function Point(p: Ptr<Int32>): Int32 => 1\n"
                    "Function Point(s: String): Int32 => 2\n"
                    "Function Text(c: CString): Int32 => 1\n"
                    "Function Text(n: Int32): Int32 => 2\n"
                    "Local a = Take(Null) + Point(Null) + Text(Null)\n")
              .empty());

    // A ternary of two Functions is code, which an overload's Callback parameter takes.
    CHECK(errors_of("Function Twice(n: Int32): Int32 => n * 2\n"
                    "Function Thrice(n: Int32): Int32 => n * 3\n"
                    "Function Take(cb: Callback(Int32): Int32): Int32 => 1\n"
                    "Function Take(s: String): Int32 => 2\n"
                    "Local flag = True\n"
                    "Local a = Take(flag ? Twice : Thrice)\n")
              .empty());
}

TEST_CASE("a Callback read through '?.' may be Null, and a Ptr's Value is called in Unsafe code",
          "[sema]") {
    CHECK(errors_of("Class H\n"
                    "    Field cb: Callback(Int32): Int32\n"
                    "End Class\n"
                    "Function Call(h: H?): Int32\n"
                    "    Local f = h?.cb\n"
                    "    Unsafe\n"
                    "        Return f(1)\n"
                    "    End Unsafe\n"
                    "End Function\n") ==
          errors_t{ "f: 'f' may be Null; check that it isn't before calling it" });

    // Outside it, the Value is what needs it, once.
    CHECK(
        errors_of("Local p: Ptr<Callback(Int32): Int32> = Null\n"
                  "Local a = p.Value(5)\n") ==
        errors_t{ "Value: a Ptr's Value needs Unsafe code: an Unsafe block, Function, or Method" });
}

TEST_CASE("a Const's Callback first needed in a quiet try is still checked for its thread",
          "[sema]") {
    // Take's overloads are tried quietly, and the try that needs the Const keeps what it found.
    const errors_t errors = errors_of("@MainThread Function Frame(): Int32 => 1\n"
                                      "Function Uses(n: Int32): Int32 => Frame() + n\n"
                                      "Function Take(cb: Callback(Int32): Int32): Int32 => 1\n"
                                      "Function Take(s: String): Int32 => 2\n"
                                      "Global g = Take(handler)\n"
                                      "Const handler: Callback(Int32): Int32 = Uses\n");
    CHECK(std::ranges::contains(errors, "Uses: C code can call a Callback on any thread, but it "
                                        "calls 'Frame', which is @MainThread"));
}

TEST_CASE("std.ffi's CString.From is safe code, and what reads or frees C's memory is Unsafe",
          "[sema]") {
    CHECK(errors_of("Import std.ffi\n"
                    "Local c = CString.From(\"x\")\n"
                    "Local n = c.Length()\n"
                    "c.Free()\n"
                    "Local m = Mem.Resize(Null, 4)\n") ==
          errors_t{
              "Length: calling 'Length', which is Unsafe, needs Unsafe code: an Unsafe block, "
              "Function, or Method",
              "Free: calling 'Free', which is Unsafe, needs Unsafe code: an Unsafe block, "
              "Function, or Method",
              "Resize: calling 'Resize', which is Unsafe, needs Unsafe code: an Unsafe block, "
              "Function, or Method",
          });
}

TEST_CASE("a name two imports give is reported once, and nothing follows from either", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Public Global Spawned = 1\nPublic Global Thing = 2\n" },
        { .path = "lib/b.fb", .text = "Public Global Spawned = 3\nPublic Global Thing = 4\n" },
    };
    CHECK(errors_with("Import \"./lib/a\"\n"
                      "Import \"./lib/b\"\n"
                      "Local t = Spawned(5)\n"
                      "Local m = Thing.Start()\n",
                      modules) ==
          errors_t{
              "Spawned: 'Spawned' is ambiguous here: lib.a and lib.b each declare it",
              "Thing: 'Thing' is ambiguous here: lib.a and lib.b each declare it",
          });
}

TEST_CASE("an Import's name for its module isn't a built-in's", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "a/String.fb", .text = "Public Function F(): Int => 1\n" },
    };
    CHECK(errors_with("Import std.collections As Panic\n"
                      "Import \"./a/String\"\n"
                      "Local s: String = \"a\"\n",
                      modules) ==
          errors_t{
              "Panic: 'Panic' is built in, so it can't name an imported module; give the module "
              "another name with As",
              "String: 'String' is built in, so it can't name an imported module; give the module "
              "another name with As",
          });
}

TEST_CASE("a hint says how a file imports another of the program's by its path", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/x.fb", .text = "Public Function X(): Int => 2\n" },
        { .path = "sub/a.fb", .text = "Public Function A(): Int => X() + M()\n" },
    };
    CHECK(errors_with("Import \"./sub/a\"\nImport \"./lib/x\"\nPublic Function M(): Int => 1\n",
                      modules) ==
          errors_t{
              "sub/a.fb: X: 'X' is in lib.x, which this file doesn't import: Import "
              "\"../lib/x\"",
              "sub/a.fb: M: 'M' is in test, which this file doesn't import: Import \"../test\"",
          });
}

TEST_CASE("modifiers go between the words of Const Function and ByRef Method too", "[sema]") {
    CHECK(errors_of("Struct S\n"
                    "    Field x: Int\n"
                    "    ByRef Public Method B()\n"
                    "        x = 1\n"
                    "    End Method\n"
                    "    Public ByRef Method C()\n"
                    "        x = 2\n"
                    "    End Method\n"
                    "End Struct\n"
                    "Const Public Function F(): Int => 4\n"
                    "Const Unsafe Function G(): Int => 5\n"
                    "Local s = S(0)\n"
                    "s.B()\n"
                    "s.C()\n"
                    "Local n = F()\n") ==
          errors_t{ "G: a Const Function runs in the compiler, so it can't be Unsafe" });
}

TEST_CASE("attributes come before modifiers, and a Get or Set has its Property's visibility",
          "[sema]") {
    CHECK(errors_of("Public @Inline Function F(): Int => 1\n"
                    "Static Public @Inline Function G(): Int => 1\n"
                    "Class D\n"
                    "    Public Property P: Int\n"
                    "        Get => 1\n"
                    "        Private Set(value)\n"
                    "        End Set\n"
                    "    End Property\n"
                    "End Class\n"
                    "Interface I\n"
                    "    Protected Method M(): Int\n"
                    "End Interface\n"
                    "Enum E\n"
                    "    Protected A\n"
                    "End Enum\n") ==
          errors_t{
              "Public: Public goes after a declaration's attributes, as in @Inline Public Function",
              "@: attributes go before a declaration's modifiers, as in @Inline Static Function",
              "Private: a Property's Get and Set have its visibility, so they take no Private",
              "Protected: an Interface's members have its visibility, so they take no Protected",
              "Protected: an Enum's members have its visibility, so they take no Protected",
          });
}

TEST_CASE(
    "a variable's visibility goes before its keyword, and one without a name declares nothing",
    "[sema]") {
    CHECK(errors_of("Const Public Limit = 4\nGlobal Package g = Limit\n") ==
          errors_t{
              "Public: Public goes before Const, as in Public Const x",
              "Package: Package goes before Global, as in Package Global x",
          });
    CHECK(errors_of("Enum Mood\n"
                    "    Calm\n"
                    "End Enum\n"
                    "Public Const Default = Mood.Calm\n"
                    "Const Local = 1\n") == errors_t{
                                                "Default: expected identifier, found 'Default'",
                                                "Local: expected identifier, found 'Local'",
                                            });
}

TEST_CASE("a visibility error labels the declaration it's about", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Public Class Bag\n"
                    "    Method Iterate(): Int[] => [1]\n"
                    "End Class\n"
                    "Public Interface Shown\n"
                    "    Method Show(): String\n"
                    "End Interface\n"
                    "Public Class Thing\n"
                    "    Method Show(): String => \"thing\"\n"
                    "End Class\n"
                    "Function Helper(): Int => 1\n"
                    "Public Struct Half<A>\n"
                    "    Public Field First: A\n"
                    "    Field Second: A\n"
                    "End Struct\n",
        },
    };
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    files.insert(files.end(), modules.begin(), modules.end());
    const fb::compilation_t compilation = fb::compile("test.fb",
                                                      "Import \"./lib/a\"\n"
                                                      "For x In Bag()\nEnd For\n"
                                                      "Extend Thing Implements Shown\nEnd Extend\n"
                                                      "Local n = Helper()\n"
                                                      "Local h = Half<Int>(1, 2)\n",
                                                      files);

    // Each one's message, and the line of lib/a.fb its "declared here" label is at.
    std::vector<std::string> labeled;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        std::string line = diagnostic.message;
        for (const fb::label_t& label : diagnostic.labels) {
            const fb::source_file_t& file = compilation.sources.get(label.span.file);
            line +=
                std::format(" [{} at {}:{}]", label.message, file.path(),
                            file.location_of(label.span.begin, fb::column_unit_t::UTF8).line + 1);
        }

        labeled.push_back(line);
    }

    CHECK(labeled ==
          std::vector<std::string>{
              "'For ... In' goes over arrays, Channels, Iterators, and values whose Iterate() "
              "gives an Iterator, not Bag; 'Iterate' is private to lib/a.fb [declared here at "
              "lib/a.fb:2]",
              "Thing implements Shown, so it needs a Method 'Show'; 'Show' is private to lib/a.fb "
              "[declared here at lib/a.fb:8]",
              "'Helper' is private to lib/a.fb [declared here at lib/a.fb:10]",
              "Half<Int32>'s constructor takes its Fields, and 'Second' is private to lib/a.fb "
              "[declared here at lib/a.fb:13]",
          });
}

TEST_CASE("a Public type that lists an instance of a hidden type names the instance", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Struct Hidden\n"
                    "    Field X: Int\n"
                    "End Struct\n"
                    "Public Class Box<T>\n"
                    "End Class\n"
                    "Public Struct It Implements Iterator<Hidden>\n"
                    "    Public ByRef Method Next(): Hidden? => Null\n"
                    "End Struct\n"
                    "Public Class Sub Extends Box<Hidden>\n"
                    "End Class\n") ==
          errors_t{
              "It: 'It' is Public, but it implements Iterator<Hidden>, and Hidden is private to "
              "test.fb",
              "Next: 'Next' is Public, but it gives Hidden, which is private to test.fb",
              "Sub: 'Sub' is Public, but it extends Box<Hidden>, and Hidden is private to test.fb",
          });
}

TEST_CASE("a hidden Method an Extend adds sits beside a Class's own of its parameters", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Public Class Base\nEnd Class\n" },
        {
            .path = "lib/m.fb",
            .text = "Import \"./a\"\n"
                    "Extend Base\n"
                    "    Method Mx(): Int => 1\n"
                    "End Extend\n"
                    "Public Function Use(b: Base): Int => b.Mx()\n",
        },
    };
    for (const std::string_view imports :
         { "Import \"./lib/a\"\nImport \"./lib/m\"\n", "Import \"./lib/a\"\n" }) {
        CHECK(errors_with(std::format("{}"
                                      "Class Mine Extends Base\n"
                                      "    Method Mx(): Int => 10\n"
                                      "End Class\n"
                                      "Local n = Mine().Mx()\n",
                                      imports),
                          modules)
                  .empty());
    }
}

TEST_CASE("two modules' @CLayout Structs of one shape are one C type", "[sema]") {
    // As two packages' bindings of one header have them, each Field's name and C type alike.
    const std::string_view div = "@CLayout Public Struct Div\n"
                                 "    Public Field quot: Int32\n"
                                 "    Public Field rem: Int32\n"
                                 "End Struct\n";
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = std::format("{}Public Extern \"C\"\n"
                                "    Function div(a: Int32, b: Int32): Div\n"
                                "End Extern\n",
                                div),
        },
        {
            .path = "lib/b.fb",
            .text = std::format("{}@CLayout Public Struct Other\n"
                                "    Public Field quotient: Int32\n"
                                "    Public Field rem: Int32\n"
                                "End Struct\n"
                                "Public Extern \"C\"\n"
                                "    Function div(a: Int32, b: Int32): Div\n"
                                "    Function Divide(a: Int32, b: Int32): Other = \"div\"\n"
                                "End Extern\n",
                                div),
        },
    };
    CHECK(errors_with("Import \"./lib/a\"\nImport \"./lib/b\" As B\n", modules) ==
          errors_t{ "lib/b.fb: Divide: 'div' is a C symbol with one signature, but this "
                    "declaration gives Other where the first gives Div" });
}

TEST_CASE("an Import that loads nothing is reported once, and its names say nothing more",
          "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/a.fb", .text = "Public Function F(): Int => 1\n" },
    };
    CHECK(errors_with("Import \"./nothing\"\n"
                      "Import \"./none\" As N\n"
                      "From \"./gone\" Import G\n"
                      "Import \"./lib/a.fb\"\n"
                      "Local a = Missing() + N.F() + G()\n"
                      "Local b = Typo()\n",
                      modules) ==
          errors_t{
              "\"./nothing\": there's no module \"./nothing\": no nothing.fb",
              "\"./none\": there's no module \"./none\": no none.fb",
              "\"./gone\": there's no module \"./gone\": no gone.fb",
              "\"./lib/a.fb\": there's no module \"./lib/a.fb\": an Import leaves out .fb, as in "
              "\"./lib/a\"",
          });
    // Without one, a name that isn't declared is reported.
    CHECK(errors_with("Import \"./lib/a\"\nLocal b = Typo()\n", modules) ==
          errors_t{ "Typo: 'Typo' isn't declared" });
}

TEST_CASE("std's Methods of a built-in type say which module to import", "[sema]") {
    CHECK(errors_of("Local c = CString.From(\"x\")\n"
                    "Local xs = [3, 1, 2]\n"
                    "xs.Sort()\n"
                    "Local f: Int[3]\n"
                    "f.Sort()\n") ==
          errors_t{
              "From: 'From' is in std.ffi, which this file doesn't import",
              "Sort: 'Sort' is in std.collections, which this file doesn't import",
              "Sort: Int32[3] has no member named 'Sort'",
          });
}

TEST_CASE("a Static Field read through its type before it initializes is reported", "[sema]") {
    // b initializes before a, which imports it, so Counter.count isn't set when b3 reads it.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/a.fb",
            .text = "Import \"./b\"\n"
                    "Public Class Counter\n"
                    "    Public Static Field count: Int = 10\n"
                    "End Class\n",
        },
        { .path = "lib/b.fb", .text = "Import \"./a\"\nPublic Global b3 = Counter.count\n" },
    };
    CHECK(errors_with("Import \"./lib/a\"\nImport \"./lib/b\"\n", modules) ==
          errors_t{ "lib/b.fb: Counter.count: 'count' isn't initialized yet; lib.a's Globals and "
                    "Consts initialize after this module's, since a module's imports initialize "
                    "first, depth-first from the main module" });
}

TEST_CASE("misleading messages about qualifiers, Super.New, and hidden modules", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "lib/s.fb", .text = "Public Enum Kind\n    Circle\nEnd Enum\n" },
        { .path = "lib/t.fb", .text = "Public Function Make(): Int => 1\n" },
    };
    CHECK(errors_with("Import \"./lib/s\" As S\n"
                      "Import \"./lib/t\"\n"
                      "S.Kind.Circle = 2\n"
                      "Local t = 1\n"
                      "Local n = t.Make()\n"
                      "Class Base\nEnd Class\n"
                      "Class Box<T>\n    Public Field x: Int\nEnd Class\n"
                      "Class Typed Extends Box<Int>\n"
                      "    New()\n"
                      "        Super.New<String>()\n"
                      "    End New\n"
                      "End Class\n"
                      "Local b = Base<Int>()\n",
                      modules) ==
          errors_t{
              "S.Kind.Circle: only a variable or one of its fields can be assigned to",
              "Make: Int32 has no member named 'Make'; the Local 't' here hides the imported "
              "module t",
              "Super.New<String>: Super.New takes no type arguments, since the Class this one "
              "extends has them",
              "Base<Int>: 'Base<Int>' isn't generic",
          });
}

TEST_CASE("a Method of T[] implements an Interface for one array type", "[sema]") {
    CHECK(probe("Interface Headed\n"
                "    Method Head(): Int\n"
                "End Interface\n"
                "Extend T[]\n"
                "    Method Head(): T => Self[0]\n"
                "End Extend\n"
                "Extend Int[] Implements Headed\n"
                "End Extend\n"
                "Function First<T: Headed>(h: T): Int => h.Head()\n"
                "Local n = First([3, 1, 2])") == "Int32");
}

TEST_CASE("what the code can't use is reported once, and says why where it's needed", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib.fb",
            .text = "Class Box\n"
                    "    Public Field v: Int = 4\n"
                    "End Class\n"
                    "Interface Shown\n"
                    "    Method Show(): String\n"
                    "End Interface\n"
                    "Public Function F(): String => \"F()\"\n"
                    "Function F(x: Int): String => \"F(Int)\"\n"
                    "Public Struct P\n"
                    "    Public Field n: Int\n"
                    "    Operator ==(a: P, b: P): Bool => a.n == b.n\n"
                    "    Method Hash(): UInt64 => n As UInt64\n"
                    "End Struct\n",
        },
    };
    const std::string unhashed = "Hashable: P has an Operator == of its own, so it needs a Hash "
                                 "that agrees with it; 'Hash' is private to lib.fb";
    CHECK(errors_with("Import std.core\n"
                      "Import \"./lib\"\n"
                      "Import std.collections\n"
                      "Function G<T: Shown>(t: T): String => t.Show()\n"
                      "Class C Extends Box\n"
                      "End Class\n"
                      "Local xs: Box[] = []\n"
                      "Local n = C().v + xs.Count\n"
                      "Local f: Func(Int): String = F\n"
                      "Extend P Implements Hashable\n"
                      "End Extend\n"
                      "Class D\n"
                      "    Public Method Destroy()\n"
                      "    End Method\n"
                      "End Class\n",
                      modules) ==
          errors_t{
              "Shown: 'Shown' is private to lib.fb",
              "Box: 'Box' is private to lib.fb",
              "Box: 'Box' is private to lib.fb",
              "F: 'F' is private to lib.fb",
              unhashed,
              "Public: Destroy runs by itself, so nothing calls it, and it takes no Public",
          });
}

TEST_CASE("an Extern block's mistakes are reported once", "[sema]") {
    CHECK(errors_of("Function F(): Int32\n"
                    "    Extern \"C\"\n"
                    "        Function abs(n: Int32): Int32\n"
                    "    End Extern\n"
                    "    Return abs(-3)\n"
                    "End Function\n"
                    "@Deprecated(\"old\")\n"
                    "Extern \"C\"\n"
                    "    Function labs(n: Int32): Int32\n"
                    "End Extern\n"
                    "Extend CString\n"
                    "    Public Method ToString(): String => \"x\"\n"
                    "    Public Method Hash(): UInt64 => 1\n"
                    "End Extend\n") ==
          errors_t{
              "Extern: an Extern block goes at the top level of a file",
              "Extern: an Extern block takes no attributes; put them before its Functions",
              "ToString: CString already has a member named 'ToString'",
          });
}

TEST_CASE("an Extend can't add the Method a default gives the type", "[sema]") {
    // Where the type's declaration lists the Interface, the default is the type's own, whether
    // or not the declaration sees the Extend.
    const std::vector<fb::library_file_t> modules{
        {
            .path = "lib/p.fb",
            .text = "Public Interface Shown\n"
                    "    Method Show(): String => \"default\"\n"
                    "End Interface\n"
                    "Public Struct P Implements Shown\n"
                    "    Public Field X: Int\n"
                    "End Struct\n",
        },
        {
            .path = "lib/y.fb",
            .text = "Import \"./p\"\n"
                    "Extend P\n"
                    "    Public Method Show(): String => \"y's\"\n"
                    "End Extend\n"
                    "Public Function YShow(p: P): String => p.Show()\n",
        },
    };
    CHECK(errors_with("Import \"./lib/p\"\n"
                      "Import \"./lib/y\"\n"
                      "Local a = P(1).Show()\n"
                      "Local b = YShow(P(1))\n",
                      modules) ==
          errors_t{ "lib/y.fb: Show: 'Show' is already declared with the same parameter types" });
}

TEST_CASE("a quoted path whose file name isn't a name gives no qualifier", "[sema]") {
    const std::vector<fb::library_file_t> modules{
        { .path = "hud-2.fb", .text = "Public Function Two(): Int => 2\n" },
        { .path = "End.fb", .text = "Public Function Ending(): Int => 3\n" },
    };
    CHECK(errors_with("Import \"./hud-2\"\n"
                      "Import \"./End\"\n"
                      "Local n = Two() + Ending()\n",
                      modules)
              .empty());
    CHECK(errors_with("Import \"./hud-2\"\n"
                      "Local n = hud.Two()\n",
                      modules) == errors_t{ "hud: 'hud' isn't declared" });
}

TEST_CASE("an Interface's member like one it extends gives what that one does", "[sema]") {
    // One with another result is reported once, at the Interface, and a type listing it
    // implements the other.
    CHECK(errors_of("Interface Source<T>\n"
                    "    Method Get(): T\n"
                    "    Method Both(): T[] => [Get(), Get()]\n"
                    "End Interface\n"
                    "Interface Wrapped<U> Extends Source<U[]>\n"
                    "    Method Both(): U[] => Get()\n"
                    "End Interface\n"
                    "Class Box Implements Wrapped<Int>\n"
                    "    Method Get(): Int[] => [1, 2, 3]\n"
                    "End Class\n"
                    "Class Crate Implements Wrapped<String>\n"
                    "    Method Get(): String[] => []\n"
                    "End Class\n"
                    "Local n = Box().Both().Count") ==
          errors_t{ "Both: 'Both' takes Source<U[]>'s parameters, so it gives what that one does, "
                    "U[][], not U[]" });
}

TEST_CASE("a default seen less far can't take the place of one a type gives everyone", "[sema]") {
    // A private Interface refining Comparable's > would make P's > private, though code with a
    // T: Comparable would run it, and another one's default would do the same for Shown's.
    CHECK(errors_of("Import std.core\n"
                    "Interface Ordered Extends Comparable\n"
                    "    Operator >(a: Self, b: Self): Bool => True\n"
                    "End Interface\n"
                    "Public Struct P Implements Comparable\n"
                    "    Public Field n: Int\n"
                    "    Operator <(a: P, b: P): Bool => a.n < b.n\n"
                    "End Struct\n"
                    "Extend P Implements Ordered\n"
                    "End Extend\n"
                    "Interface Secret\n"
                    "    Method Twice(): Int => 42\n"
                    "End Interface\n"
                    "Public Interface Shown\n"
                    "    Method Twice(): Int => 107\n"
                    "End Interface\n"
                    "Public Class Base\n"
                    "End Class\n"
                    "Public Class Sub Extends Base Implements Shown\n"
                    "End Class\n"
                    "Extend Base Implements Secret\n"
                    "End Extend\n") ==
          errors_t{
              "Ordered: Ordered's default '>' would take the place of Comparable's for P, which "
              "is Public, but Ordered is private to test.fb",
              "Secret: Secret's default 'Twice' would take the place of Shown's for Sub, which is "
              "Public, but Secret is private to test.fb",
          });

    // As visible as the type, it does.
    CHECK(errors_of("Import std.core\n"
                    "Public Interface Ordered Extends Comparable\n"
                    "    Operator >(a: Self, b: Self): Bool => True\n"
                    "End Interface\n"
                    "Public Struct P Implements Comparable\n"
                    "    Public Field n: Int\n"
                    "    Operator <(a: P, b: P): Bool => a.n < b.n\n"
                    "End Struct\n"
                    "Extend P Implements Ordered\n"
                    "End Extend\n")
              .empty());
}

TEST_CASE("a default's place, its inference, and a Const Function that can't lower say one thing",
          "[sema]") {
    constexpr std::string_view source = "Import std.core\n"
                                        "Interface Source<T>\n"
                                        "    Method Take(): T\n"
                                        "    Method With<U>(u: U): (T, U) => (Take(), u)\n"
                                        "End Interface\n"
                                        "Interface Doubler\n"
                                        "    Method With(u: Bool): (Int, Bool) => (2, u)\n"
                                        "End Interface\n"
                                        "Class Counter<T> Implements Source<T>\n"
                                        "    Field v: T\n"
                                        "    New(v: T)\n"
                                        "        Self.v = v\n"
                                        "    End New\n"
                                        "    Method Take(): T => v\n"
                                        "End Class\n";
    // The copy reported goes, so a use finds the outside default alone, with no second error.
    CHECK(errors_of(std::string(source) + "Extend Counter<T> Implements Doubler\n"
                                          "    Where T: Equatable\n"
                                          "End Extend\n"
                                          "Local w = Counter<Int>(1).With(True)") ==
          errors_t{ "Doubler: Counter<T> gets 'With' from Doubler's default, which would take the "
                    "place of Source<T>'s generic default, which is what a call through Source<T> "
                    "runs" });

    // An Extend's Method in a generic default's place says so, from another module too.
    CHECK(errors_with("Import std.collections\n"
                      "Import \"./lib/rows\"\n"
                      "Extend IntRows\n"
                      "    Method Flatten(): Int => 5\n"
                      "End Extend",
                      { { .path = "lib/rows.fb",
                          .text = "Import std.core\n"
                                  "Import std.collections\n"
                                  "Public Class IntRows Implements Iterator<Stack<Int>>\n"
                                  "    Public Method Next(): Stack<Int>? => Null\n"
                                  "End Class\n" } }) ==
          errors_t{ "Flatten: 'Flatten' would take the place of Iterator<Stack<Int32>>'s generic "
                    "default, which is what a call through Iterator<Stack<Int32>> runs; give it "
                    "another name" });

    // A constraint naming nothing says nothing more.
    CHECK(errors_of("Import std.core\n"
                    "Class Bag<T> Implements Iterator<T>\n"
                    "    Method Next(): T? => Null\n"
                    "End Class\n"
                    "Function F<X: Nope>(b: Bag<X>): Int => b.Flatten().Count()") ==
          errors_t{ "Nope: 'Nope' isn't a type" });

    // A Const Function whose instance can't be made says why at the call.
    CHECK(errors_of("Const Function Show<Const M: UInt8>(): Int => M\n"
                    "Const Function Twice<Const N: UInt8>(): Int => Show<N * 2>()\n"
                    "Const X = Twice<200>()\n"
                    "Local y = X") ==
          errors_t{ "Twice<200>(): 'Twice' can't run at compile time: a Const type argument comes "
                    "out as 400 here, which doesn't fit in UInt8" });
    // Nor is a use of a Const that an error left without a value.
    CHECK(errors_of("Const X = 1 / 0\nLocal a: Int[X]\nLocal b: Int[X * 2]") ==
          errors_t{ "1 / 0: division by zero" });
}

TEST_CASE("a fixed array's sizes are checked before they're multiplied", "[sema]") {
    CHECK(errors_of("Local a: Int[4, 4611686018427387904]") ==
          errors_t{ "Int[4, 4611686018427387904]: a fixed array can hold at most 2147483647 "
                    "elements" });
    CHECK(errors_of("Local a: Int[65536, 65536]") ==
          errors_t{ "Int[65536, 65536]: a fixed array can hold at most 2147483647 elements" });
    CHECK(errors_of("Local a: Int[2, 1073741823]").empty());
}

TEST_CASE("only a Static member named Has or Attribute hides T.Has(A) and T.Attribute(A)",
          "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Attribute Tag()\n"
                    "@Tag\n"
                    "Class Bag\n"
                    "    Method Has(n: Int): Bool => n > 0\n"
                    "End Class\n"
                    "Local a = Bag.Has(Tag)\n"
                    "Local b = Bag().Has(3)")
              .empty());
    CHECK(errors_of("Import std.core\n"
                    "Attribute Tag()\n"
                    "Class Bag\n"
                    "    Static Method Has(n: Int): Bool => n > 0\n"
                    "End Class\n"
                    "Local a = Bag.Has(1)")
              .empty());
}

TEST_CASE("a Return in a Try runs its Finally first, which can assign what New needs", "[sema]") {
    constexpr std::string_view classes = "Import std.core\n"
                                         "Class E1 Extends Error\n"
                                         "End Class\n"
                                         "Class Node\n"
                                         "End Class\n"
                                         "Function Risky() Throws E1\n"
                                         "End Function\n";
    CHECK(errors_of(std::format("{}Class A\n"
                                "    Field m: Node\n"
                                "    New()\n"
                                "        Try\n"
                                "            Try Risky()\n"
                                "            Return\n"
                                "        Catch e As E1\n"
                                "            Return\n"
                                "        Finally\n"
                                "            Try\n"
                                "            Finally\n"
                                "                m = Node()\n"
                                "            End Try\n"
                                "        End Try\n"
                                "    End New\n"
                                "End Class",
                                classes))
              .empty());
    CHECK(errors_of(std::format("{}Class A\n"
                                "    Field m: Node\n"
                                "    New()\n"
                                "        Try\n"
                                "            Try\n"
                                "                Return\n"
                                "            Finally\n"
                                "                Print(\"inner\")\n"
                                "            End Try\n"
                                "        Finally\n"
                                "            Print(\"outer\")\n"
                                "        End Try\n"
                                "        m = Node()\n"
                                "    End New\n"
                                "End Class",
                                classes)) ==
          errors_t{ "Return: New must assign 'm' before this Return, since Node has no zero "
                    "value" });
}

TEST_CASE("literals alone in a For loop take one type, a Float with a decimal among them",
          "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "For f = 0 To 1.5 Step 0.5\n"
                    "    Local x: Float = f\n"
                    "End For\n"
                    "For i = 0 To 10 Step 2\n"
                    "    Local y: Int = i\n"
                    "End For")
              .empty());
    CHECK(errors_of("For f = 0 To 1.5 Step 0.5\n    Local x: Int = f\nEnd For") ==
          errors_t{ "f: expected Int32, found Float32" });
}

TEST_CASE("a throwing call in a Defer, Finally, or lambda says it can't throw", "[sema]") {
    CHECK(errors_of("Import std.core\n"
                    "Class E1 Extends Error\n"
                    "End Class\n"
                    "Function F() Throws E1\n"
                    "End Function\n"
                    "Function G() Throws E1\n"
                    "    Try\n"
                    "        Defer F()\n"
                    "        Try F()\n"
                    "    Catch e As E1\n"
                    "    End Try\n"
                    "    Try\n"
                    "        Try F()\n"
                    "    Finally\n"
                    "        F()\n"
                    "    End Try\n"
                    "End Function") ==
          errors_t{
              "F(): a Defer can't throw, so calling 'F', which can throw E1, needs Try?, or a Try "
              "block inside it",
              "F(): a Finally can't throw, so calling 'F', which can throw E1, needs Try?, or a "
              "Try block inside it",
          });
}
