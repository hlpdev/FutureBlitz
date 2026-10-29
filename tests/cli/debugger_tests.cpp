#include "fb/build_config.hpp"
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
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// A debug build in lldb, with tools/lldb/futureblitz.py, which the VS Code extension loads: it
// stops at a line, shows the Locals there as their values, and steps over a line and into a
// Function. It runs where lldb is, and where it can run its Python. On macOS, a build's debug
// info is in the .dSYM beside it.

namespace {

/// The program the debugger stops in. Circle has no Method, and no variable has it as its type,
/// so only its object's header says what it is.
constexpr std::string_view source = R"(Import std.core
Import std.collections

Class Shape
    Field Name: String
End Class

Class Circle Extends Shape
    Field Radius: Float64 = 2.5
End Class

Struct Point
    Field X: Int32
    Field Y: Int32
End Struct

Enum Color
    RED, GREEN
End Enum

Function Describe(s: Shape): String
    Return "shape {s.Name}"
End Function

Local name = "Orc \"big\""
Local nothing: String? = Null
Local count: Int32? = 4
Local missing: Int32? = Null
Local items = [1, 2, 3]
Local ages = Map<String, Int32>()
ages["ann"] = 31
Local shape: Shape = Circle()
shape.Name = "wheel"
Local point = Point()
point.Y = 7
Local color = Color.GREEN
Print("{name} {nothing ?? ""} {count ?? 0} {missing ?? 0} {items.Count} {ages.Count}")
Print(Describe(shape))
Print("{point.Y} {color}")
)";

/// The line of `text` in the source, from 1.
std::size_t line_of(const std::string_view text) {
    const std::size_t at = source.find(text);
    REQUIRE(at != std::string_view::npos);
    return static_cast<std::size_t>(std::ranges::count(source.substr(0, at), '\n')) + 1;
}

/// Builds the program in `mode`, as main in the test's folder, and gives the executable.
std::filesystem::path build(const std::string_view name, const bool release) {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "debugger";
    std::filesystem::create_directories(folder);
    const std::filesystem::path main = folder / "main.fb";
    std::ofstream(main, std::ios::binary) << source;
    std::filesystem::path executable = folder / name;
    executable += fb::executable_extension;
    std::vector<std::string> arguments{
        "build",
        fb::to_utf8(main),
        "-o",
        fb::to_utf8(executable),
    };
    if (release) {
        arguments.emplace_back("--release");
    }

    const std::expected<fb::process_result_t, std::string> built =
        fb::run_process(FB_CLI, arguments);
    REQUIRE(built);
    INFO(built->output);
    REQUIRE(built->exit_code == 0);
    return executable;
}

#ifdef FB_TEST_LLDB
/// Runs lldb with `arguments`, giving what it printed and its exit code, or -1 when it can't
/// start.
fb::process_result_t run_lldb(const std::vector<std::string>& arguments) {
    const std::expected<fb::process_result_t, std::string> ran =
        fb::run_process(FB_TEST_LLDB, arguments);
    return ran.value_or(fb::process_result_t{ .exit_code = -1 });
}
#endif

} // namespace

TEST_CASE("lldb steps through a debug build and shows its Locals as FutureBlitz values",
          "[cli][debugger]") {
#ifndef FB_TEST_LLDB
    SKIP("there's no lldb here");
#else
    // Where lldb has no Python, it can crash, and print anything.
    const fb::process_result_t probe =
        run_lldb({ "--batch", "-o", "script print('fb-python-' + str(6 * 7))" });
    if (probe.exit_code != 0 || !probe.output.contains("fb-python-42")) {
        SKIP("lldb here can't run its Python");
    }

    const std::filesystem::path executable = build("main", false);
    const std::size_t first = line_of("Print(\"{name}");
    const std::filesystem::path commands = executable.parent_path() / "commands.lldb";
    std::ofstream(commands, std::ios::binary)
        << std::format("command script import \"{}\"\n", FB_LLDB_SCRIPT)
        << std::format("breakpoint set --file main.fb --line {}\n", first) << "run\n"
        << "frame variable\n"
        << "frame variable shape\n"
        << "next\n"
        << "step\n"
        << "kill\n";
    const fb::process_result_t shown =
        run_lldb({ "--batch", "-s", fb::to_utf8(commands), fb::to_utf8(executable) });
    INFO(shown.output);
    const std::string& text = shown.output;
    CHECK(text.contains(R"(name = "Orc \"big\"")"));
    CHECK(text.contains("nothing = Null"));
    CHECK(text.contains("count = 4"));
    CHECK(text.contains("missing = Null"));
    CHECK(text.contains("items = [1, 2, 3]"));
    CHECK(text.contains(R"(ages = {"ann": 31})"));
    CHECK(text.contains("Y = 7"));
    CHECK(text.contains("color = GREEN"));
    // The Shape is a Circle, whose Fields it shows on its own.
    const std::size_t alone = text.find("(lldb) frame variable shape");
    REQUIRE(alone != std::string::npos);
    const std::string shape = text.substr(alone, text.find("(lldb) next", alone) - alone);
    CHECK(shape.contains("shape = Circle"));
    CHECK(shape.contains(R"(Name = "wheel")"));
    CHECK(shape.contains("Radius = 2.5"));
    // A step over the line stops at the next, and a step into it in Describe.
    const std::size_t stepped = text.find("(lldb) next");
    REQUIRE(stepped != std::string::npos);
    const std::string after = text.substr(stepped);
    CHECK(after.contains(std::format("main.fb:{}", first + 1)));
    CHECK(after.contains(std::format("main.fb:{}", line_of("    Return \"shape"))));
    CHECK(after.contains("Describe"));
#endif
}

TEST_CASE("a library beside the executable built from its source has its files' folders",
          "[cli][debugger]") {
    // Its debug info names its files by their paths in the program, "tools/tools.fb", from
    // the folder they're in here, which a debugger finds them by.
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "debug_beside";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "tools");
    std::filesystem::create_directories(folder / "app");
    std::ofstream(folder / "tools" / "project.toml", std::ios::binary)
        << "[project]\nname = \"tools\"\nversion = \"0.1.0\"\ntype = \"library\"\n";
    std::ofstream(folder / "tools" / "tools.fb", std::ios::binary)
        << "Public Function Triple(n: Int): Int => n * 3\n";
    std::ofstream(folder / "app" / "project.toml", std::ios::binary)
        << "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
           "tools = { path = \"../tools\", link = \"beside\" }\n";
    std::ofstream(folder / "app" / "main.fb", std::ios::binary)
        << "Import std.core\nImport tools\nPrint(\"{Triple(2)}\")\n";
    const std::array arguments{ std::string("build"), fb::to_utf8(folder / "app") };
    const std::expected<fb::process_result_t, std::string> built =
        fb::run_process(FB_CLI, arguments);
    REQUIRE(built);
    INFO(built->output);
    REQUIRE(built->exit_code == 0);
#ifdef _WIN32
    const std::filesystem::path binary = folder / "app" / "build" / "tools.dll";
#elifdef __APPLE__
    // Its debug info is in its .dSYM.
    const std::filesystem::path binary =
        folder / "app" / "build" / "libtools.dylib.dSYM/Contents/Resources/DWARF/libtools.dylib";
#else
    const std::filesystem::path binary = folder / "app" / "build" / "libtools.so";
#endif

    std::ifstream file(binary, std::ios::binary);
    REQUIRE(file);
    const std::string bytes{ std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>() };
    std::string where = fb::to_utf8(folder);
    std::ranges::replace(where, '\\', '/');
    CHECK(bytes.contains(where));
}

TEST_CASE("on macOS a build has a .dSYM beside it, a release build's line tables too",
          "[cli][debugger]") {
    if (!fb::build_config::target_triple.contains("apple")) {
        SKIP("only macOS keeps debug info apart from the executable");
    }

    for (const bool release : { false, true }) {
        INFO(release);
        std::filesystem::path symbols = build(release ? "release" : "debug", release);
        symbols += ".dSYM";
        CHECK(std::filesystem::is_directory(symbols));
    }
}
