#include "fb/driver.hpp"
#include "fb/sema.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// A program with every kind of use the language has: of a Function, a Local, a parameter, a
/// Field, a Static Field, a Method, a Static Method, an Override and the Method it overrides, an
/// Interface and its Method, a Property, a New by its type's name, an Enum and its values, in
/// code and in a Case, a Class after Extends and after Extend, an Interface after Implements, an
/// Alias as a type and as a constructor, a module's qualifier in code and in a type, a type
/// before its Static member, a From line's names, named arguments, and an assignment's target,
/// plain and compound.
constexpr std::string_view lib = "Public Function Area(): Int => 1\n"
                                 "Public Class Counter\n"
                                 "    Public Static Method Zero(): Int => 0\n"
                                 "End Class\n";

constexpr std::string_view program =
    "Import std.core\n"
    "Import \"./lib\" As L\n"
    "From \"./lib\" Import Area\n"
    "Enum Mood\n"
    "    Happy\n"
    "    Sad\n"
    "End Enum\n"
    "Interface Shown\n"
    "    Method Show(): Int\n"
    "End Interface\n"
    "Class Base Implements Shown\n"
    "    Field size: Int\n"
    "    Static Field made: Int\n"
    "    Method Show(): Int => size\n"
    "    Static Method Make(): Base => Base()\n"
    "    Property Twice: Int\n"
    "        Get => size * 2\n"
    "    End Property\n"
    "End Class\n"
    "Class Child Extends Base\n"
    "    Override Method Show(): Int => Super.Show() + 1\n"
    "End Class\n"
    "Extend Base\n"
    "    Method Grow(by: Int)\n"
    "        size = size + by\n"
    "    End Method\n"
    "End Extend\n"
    "Alias Thing = Base\n"
    "Function Pick(first: Int, second: Int): Int => first\n"
    "Function Count(c: L.Counter?): Int => 0\n"
    "Local m = Mood.Happy\n"
    "Select m\n"
    "    Case Mood.Sad\n"
    "        Print(\"sad\")\n"
    "    Default\n"
    "End Select\n"
    "Base.made += 1\n"
    "Local b: Thing = Thing()\n"
    "b.Grow(by: 2)\n"
    "Local c = Child()\n"
    "Print(c.Show(), b.Twice, Base.Make().size, L.Counter.Zero())\n"
    "Print(Area(), Pick(second: 1, first: 2), Count(Null))\n"
    "Local n = 1\n"
    "n = 2\n"
    "n += 3\n";

} // namespace

TEST_CASE("analysis records every kind of use the language has", "[sema]") {
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    files.push_back({ .path = "lib.fb", .text = std::string(lib) });
    const fb::compilation_t compilation = fb::compile("test.fb", std::string(program), files);
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        INFO(diagnostic.message);
        CHECK(diagnostic.severity != fb::severity_t::ERROR);
    }

    // Each as "<line> <text>: <symbol>", with lines from 1.
    const fb::source_file_t& file = compilation.source();
    std::set<std::string> uses;
    for (const fb::symbol_use_t& use : compilation.analysis.uses) {
        if (use.span.file == compilation.file) {
            uses.insert(std::format(
                "{} {}: {}", file.location_of(use.span.begin, fb::column_unit_t::UTF8).line + 1,
                file.slice(use.span), compilation.analysis.symbol(use.symbol).name));
        }
    }

    // One a line.
    constexpr std::string_view expected = "3 Area: Area\n"
                                          "11 Shown: Shown\n"
                                          "14 size: size\n"
                                          "15 Base: Base\n"
                                          "15 Base: New\n"
                                          "17 size: size\n"
                                          "20 Base: Base\n"
                                          "21 Show: Show\n"
                                          "23 Base: Base\n"
                                          "25 size: size\n"
                                          "25 by: by\n"
                                          "28 Base: Base\n"
                                          "29 first: first\n"
                                          "30 L: L\n"
                                          "30 L.Counter: Counter\n"
                                          "31 Mood: Mood\n"
                                          "31 Happy: Happy\n"
                                          "33 Mood: Mood\n"
                                          "33 Sad: Sad\n"
                                          "37 Base: Base\n"
                                          "37 made: made\n"
                                          "38 Thing: Thing\n"
                                          "39 b: b\n"
                                          "39 Grow: Grow\n"
                                          "39 by: by\n"
                                          "41 Show: Show\n"
                                          "41 Twice: Twice\n"
                                          "41 Base: Base\n"
                                          "41 Make: Make\n"
                                          "41 L: L\n"
                                          "41 L.Counter: Counter\n"
                                          "41 Zero: Zero\n"
                                          "42 Area: Area\n"
                                          "42 Pick: Pick\n"
                                          "42 second: second\n"
                                          "42 first: first\n"
                                          "44 n: n\n"
                                          "45 n: n\n";
    for (std::size_t begin = 0; begin < expected.size();) {
        const std::size_t end = expected.find('\n', begin);
        const std::string use(expected.substr(begin, end - begin));
        INFO(use);
        CHECK(uses.contains(use));
        begin = end + 1;
    }
}
