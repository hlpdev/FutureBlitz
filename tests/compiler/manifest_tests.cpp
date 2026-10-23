#include "fb/manifest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct read_t {
    std::optional<fb::manifest_t> manifest;
    /// "error: <text the span covers>: message", or "warning: ...", in order.
    std::vector<std::string> diagnostics;
};

/// Reads `text` as a project.toml.
read_t read(const std::string_view text) {
    const fb::source_file_t file(fb::file_id_t{}, "project.toml", std::string(text));
    fb::diagnostic_bag_t bag;
    read_t result{ .manifest = fb::read_manifest(file, bag) };
    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        result.diagnostics.push_back(std::format(
            "{}: {}: {}", diagnostic.severity == fb::severity_t::ERROR ? "error" : "warning",
            file.slice(diagnostic.span), diagnostic.message));
    }

    return result;
}

using diagnostics_t = std::vector<std::string>;

} // namespace

TEST_CASE("a project.toml names the package, its version, and its dependencies", "[manifest]") {
    const read_t game = read("[project]\n"
                             "name = \"game\"\n"
                             "version = \"0.1.0\"\n"
                             "\n"
                             "[dependencies]\n"
                             "physics = { path = \"../physics\" }\n"
                             "ui = { path = \"libs/ui\" }\n");
    CHECK(game.diagnostics.empty());
    REQUIRE(game.manifest);
    const fb::manifest_t manifest = game.manifest.value_or(fb::manifest_t{});
    CHECK(manifest.name == "game");
    CHECK(manifest.version == "0.1.0");
    CHECK(manifest.main == "main.fb");
    REQUIRE(manifest.dependencies.size() == 2);
    CHECK(manifest.dependencies[0].name == "physics");
    CHECK(manifest.dependencies[0].path == "../physics");
    CHECK(manifest.dependencies[1].name == "ui");

    // main names another main file, and a key it doesn't know is only a warning.
    const read_t tool = read("[project]\n"
                             "name = \"tool\"\n"
                             "version = \"2.10.3\"\n"
                             "main = \"src/tool.fb\"\n"
                             "authors = [\"me\"]\n"
                             "\n"
                             "[badges]\n");
    REQUIRE(tool.manifest);
    CHECK(tool.manifest.value_or(fb::manifest_t{}).main == "src/tool.fb");
    CHECK(tool.diagnostics ==
          diagnostics_t{ "warning: authors: [project] doesn't use 'authors'",
                         "warning: badges: project.toml doesn't use 'badges'" });
}

TEST_CASE("a project.toml's mistakes are reported at their lines", "[manifest]") {
    CHECK(read("[project\n").diagnostics.size() == 1);
    CHECK(read("[project\n").diagnostics.front().starts_with("error: "));
    CHECK(read("[project\n").diagnostics.front().contains("project.toml isn't valid TOML: "));
    CHECK_FALSE(read("[project\n").manifest);

    CHECK(read("").diagnostics ==
          diagnostics_t{ "error: : project.toml needs a [project] table, with the package's name "
                         "and version" });
    CHECK(read("[project]\nversion = \"1.0.0\"\n").diagnostics ==
          diagnostics_t{ "error: project: [project] needs the package's name, like name = "
                         "\"game\"" });
    CHECK(read("[project]\nname = \"game\"\n").diagnostics ==
          diagnostics_t{ "error: project: [project] needs a version, like version = \"0.1.0\"" });
    CHECK(read("[project]\nname = \"my game\"\nversion = \"1.0\"\nmain = \"../x.fb\"\n")
              .diagnostics ==
          diagnostics_t{
              "error: \"my game\": a package's name is an identifier, like game or my_physics, "
              "not \"my game\"",
              "error: \"1.0\": a package's version is three numbers, like \"0.1.0\", not \"1.0\"",
              "error: \"../x.fb\": 'main' is a .fb file in the package's folder, like \"main.fb\" "
              "or \"src/game.fb\", not \"../x.fb\"",
          });
    CHECK(read("[project]\nname = \"std\"\nversion = \"1.0.0\"\n").diagnostics ==
          diagnostics_t{ "error: \"std\": std is the standard library's name" });
    CHECK(read("[project]\nname = 5\nversion = \"1.0.0\"\n").diagnostics ==
          diagnostics_t{ "error: 5: 'name' is text, like \"game\"" });
}

TEST_CASE("a dependency is a local package's folder for now", "[manifest]") {
    const std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    CHECK(read(std::format("{}[dependencies]\nphysics = \"1.2\"\n", project)).diagnostics ==
          diagnostics_t{ "error: \"1.2\": versioned dependencies come with the package manager "
                         "in M8; give the package's folder, like physics = { path = "
                         "\"../physics\" }" });
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ version = \"1.2\" }}\n", project))
              .diagnostics ==
          diagnostics_t{
              "warning: version: a dependency doesn't use 'version'",
              "error: { version = \"1.2\" }: a dependency needs its package's folder, like "
              "physics = { path = \"../physics\" }",
          });
    CHECK(read(std::format("{}[dependencies]\n\"my-lib\" = {{ path = \"../x\" }}\nstd = {{ path "
                           "= \"../std\" }}\n",
                           project))
              .diagnostics ==
          diagnostics_t{
              "error: \"my-lib\": a dependency's name is an identifier, like physics, not "
              "\"my-lib\"",
              "error: std: std is the standard library's name",
          });

    // A path that isn't text is one mistake.
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ path = 5 }}\n", project)).diagnostics ==
          diagnostics_t{ "error: 5: 'path' is text, like \"../physics\"" });
}

TEST_CASE("[native] names static archives and system libraries", "[manifest]") {
    const std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    const read_t game = read(std::format("{}[native]\n"
                                         "joltc = \"libs/joltc\"\n"
                                         "sdl = {{ static = \"vendor/SDL3\", notes = 1 }}\n"
                                         "m = {{ system = \"m\" }}\n",
                                         project));
    CHECK(game.diagnostics ==
          diagnostics_t{ "warning: notes: a native library doesn't use 'notes'" });
    REQUIRE(game.manifest);
    const std::vector<fb::native_t> natives = game.manifest.value_or(fb::manifest_t{}).natives;
    REQUIRE(natives.size() == 3);
    CHECK(natives[0].name == "joltc");
    CHECK(natives[0].kind == fb::native_kind_t::STATIC);
    CHECK(natives[0].path == "libs/joltc");
    CHECK(natives[1].name == "sdl");
    CHECK(natives[1].path == "vendor/SDL3");
    CHECK(natives[2].name == "m");
    CHECK(natives[2].kind == fb::native_kind_t::SYSTEM);
    CHECK(natives[2].path == "m");

    const auto errors = [&](const std::string_view natives_text) {
        return read(std::format("{}[native]\n{}", project, natives_text)).diagnostics;
    };
    CHECK(errors("sdl = { dynamic = \"SDL3\" }\n") ==
          diagnostics_t{ "error: dynamic: dynamic libraries come in M7; link it statically, like "
                         "sdl = \"libs/sdl\"" });
    CHECK(errors("x = { static = \"libs/x\", system = \"x\" }\n") ==
          diagnostics_t{ "error: system: a native library is static or system, not both" });
    const std::string like = R"(a native library is like x = "libs/x" for a static archive, or )"
                             R"(x = { system = "x" })";
    CHECK(errors("x = 5\n") == diagnostics_t{ "error: 5: " + like });
    CHECK(errors("x = {}\n") == diagnostics_t{ "error: {}: " + like });
    CHECK(errors("\"my lib\" = \"libs/x\"\n") ==
          diagnostics_t{ "error: \"my lib\": a native library's name is letters, digits, and "
                         "'_', '-', '.', or '+', not starting with '-', like joltc, not \"my "
                         "lib\"" });
    CHECK(errors("x = { system = \"-static\" }\ny = { system = \"a b\" }\nz = \"\"\n") ==
          diagnostics_t{
              "error: \"-static\": a system library is the name the linker takes, like system = "
              "\"m\" for -lm, not \"-static\"",
              "error: \"a b\": a system library is the name the linker takes, like system = "
              "\"m\" for -lm, not \"a b\"",
              "error: \"\": a native library's archive or name can't be empty",
          });
    CHECK(errors("x = { static = 5 }\n") ==
          diagnostics_t{ "error: 5: 'static' is text, like \"libs/x\"" });
    CHECK(read(std::format("native = 5\n{}", project)).diagnostics ==
          diagnostics_t{ "error: native: 'native' is a table, [native], of C libraries by name" });
}

TEST_CASE("a project.toml's paths take '\\' as '/', as an Import's path does", "[manifest]") {
    // So a project.toml written on Windows means the same on Linux and macOS.
    const read_t game = read("[project]\n"
                             "name = \"game\"\n"
                             "version = \"0.1.0\"\n"
                             "main = 'src\\main.fb'\n"
                             "\n"
                             "[dependencies]\n"
                             "physics = { path = '..\\physics' }\n"
                             "\n"
                             "[native]\n"
                             "joltc = 'libs\\joltc'\n"
                             "sdl = { static = 'vendor\\SDL3' }\n");
    CHECK(game.diagnostics.empty());
    REQUIRE(game.manifest);
    const fb::manifest_t manifest = game.manifest.value_or(fb::manifest_t{});
    CHECK(manifest.main == "src/main.fb");
    REQUIRE(manifest.dependencies.size() == 1);
    CHECK(manifest.dependencies[0].path == "../physics");
    REQUIRE(manifest.natives.size() == 2);
    CHECK(manifest.natives[0].path == "libs/joltc");
    CHECK(manifest.natives[1].path == "vendor/SDL3");
}

TEST_CASE("text in a project.toml that isn't UTF-8 is reported at its first bad byte",
          "[manifest]") {
    CHECK(read("[project]\nname = \"ab\xff\xfe\"\n").diagnostics ==
          diagnostics_t{ "error: \xff: project.toml isn't valid TOML: Encountered invalid utf-8 "
                         "sequence" });
}
