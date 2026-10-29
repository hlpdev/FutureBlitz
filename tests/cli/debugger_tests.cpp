#include "fb/driver.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

// A debug build in lldb, with tools/lldb/futureblitz.py, which the VS Code extension loads: it
// stops at a line and shows the Locals there as their values. It runs where lldb is, and where
// it can run its Python.

namespace {

#ifdef FB_TEST_LLDB
/// The program the debugger stops in, at its last line.
constexpr std::string_view source = R"(Import std.core
Import std.collections

Class Shape
    Field Name: String
End Class

Class Circle Extends Shape
    Field Radius: Float64
End Class

Struct Point
    Field X: Int32
    Field Y: Int32
End Struct

Enum Color
    RED, GREEN
End Enum

Local name = "Orc \"big\""
Local nothing: String? = Null
Local count: Int32? = 4
Local missing: Int32? = Null
Local items = [1, 2, 3]
Local ages = Map<String, Int32>()
ages["ann"] = 31
Local round = Circle()
round.Name = "wheel"
round.Radius = 2.5
Local shape: Shape = round
Local point = Point()
point.Y = 7
Local color = Color.GREEN
Print("{name} {nothing ?? ""} {count ?? 0} {missing ?? 0} {items.Count} {ages.Count}")
Print("{shape.Name} {point.Y} {color}")
)";

/// Runs lldb with `arguments`, giving what it printed, or nothing when it can't start.
std::string run_lldb(const std::vector<std::string>& arguments) {
    const std::expected<fb::process_result_t, std::string> ran =
        fb::run_process(FB_TEST_LLDB, arguments);
    return ran ? ran->output : std::string();
}
#endif

} // namespace

TEST_CASE("lldb shows a debug build's Locals as FutureBlitz values", "[cli][debugger]") {
#ifndef FB_TEST_LLDB
    SKIP("there's no lldb here");
#else
    if (!run_lldb({ "--batch", "-o", "script print(6 * 7)" }).contains("42")) {
        SKIP("lldb here can't run its Python");
    }

    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "debugger";
    std::filesystem::create_directories(folder);
    const std::filesystem::path main = folder / "main.fb";
    std::ofstream(main, std::ios::binary) << source;
    std::filesystem::path executable = folder / "main";
    executable += fb::executable_extension;
    const std::array build{
        std::string("build"),
        fb::to_utf8(main),
        std::string("-o"),
        fb::to_utf8(executable),
    };
    const std::expected<fb::process_result_t, std::string> built = fb::run_process(FB_CLI, build);
    REQUIRE(built);
    INFO(built->output);
    REQUIRE(built->exit_code == 0);

    // It stops at the first Print, the line before the last.
    const std::size_t lines = static_cast<std::size_t>(std::ranges::count(source, '\n'));
    const std::filesystem::path commands = folder / "commands.lldb";
    std::ofstream(commands, std::ios::binary)
        << std::format("command script import \"{}\"\n", FB_LLDB_SCRIPT)
        << std::format("breakpoint set --file main.fb --line {}\n", lines - 1) << "run\n"
        << "frame variable\n"
        << "kill\n";
    const std::string shown =
        run_lldb({ "--batch", "-s", fb::to_utf8(commands), fb::to_utf8(executable) });
    INFO(shown);
    CHECK(shown.contains(R"(name = "Orc \"big\"")"));
    CHECK(shown.contains("nothing = Null"));
    CHECK(shown.contains("count = 4"));
    CHECK(shown.contains("missing = Null"));
    CHECK(shown.contains("items = [1, 2, 3]"));
    CHECK(shown.contains(R"(ages = {"ann": 31})"));
    // The Shape is a Circle, whose Fields it shows.
    CHECK(shown.contains("shape = Circle"));
    CHECK(shown.contains(R"(Name = "wheel")"));
    CHECK(shown.contains("Radius = 2.5"));
    CHECK(shown.contains("Y = 7"));
    CHECK(shown.contains("color = GREEN"));
#endif
}
