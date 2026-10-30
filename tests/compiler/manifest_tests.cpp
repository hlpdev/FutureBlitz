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

TEST_CASE("a project.toml's type says whether it's an executable or a library", "[manifest]") {
    const std::string_view project = "[project]\nname = \"physics\"\nversion = \"0.1.0\"\n";
    CHECK_FALSE(read(project).manifest.value_or(fb::manifest_t{}).type);

    const read_t library = read(std::string(project) + "type = \"library\"\n");
    CHECK(library.diagnostics.empty());
    CHECK(library.manifest.value_or(fb::manifest_t{}).type == fb::package_type_t::LIBRARY);
    CHECK(read(std::string(project) + "type = \"executable\"\n")
              .manifest.value_or(fb::manifest_t{})
              .type == fb::package_type_t::EXECUTABLE);

    CHECK(read(std::string(project) + "type = \"plugin\"\n").diagnostics ==
          diagnostics_t{ "error: \"plugin\": a package's type is \"executable\" or \"library\", "
                         "not \"plugin\"" });
    CHECK(read(std::string(project) + "type = 1\n").diagnostics ==
          diagnostics_t{ "error: 1: 'type' is text, like \"library\"" });
    CHECK(read(std::string(project) + "type = \"library\"\nmain = \"main.fb\"\n").diagnostics ==
          diagnostics_t{ "error: \"main.fb\": a library has no main file; its files are its "
                         "modules" });
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
              "error: \"1.0\": a package's version is three numbers, like \"0.1.0\", and maybe a "
              "pre-release, like \"1.0.0-beta.1\", not \"1.0\"",
              "error: \"../x.fb\": 'main' is a .fb file in the package's folder, like \"main.fb\" "
              "or \"src/game.fb\", not \"../x.fb\"",
          });
    CHECK(read("[project]\nname = \"std\"\nversion = \"1.0.0\"\n").diagnostics ==
          diagnostics_t{ "error: \"std\": std is the standard library's name" });
    CHECK(read("[project]\nname = 5\nversion = \"1.0.0\"\n").diagnostics ==
          diagnostics_t{ "error: 5: 'name' is text, like \"game\"" });
}

TEST_CASE("a dependency is a range of a registry's versions, or a local package's folder",
          "[manifest]") {
    const std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    const read_t versioned =
        read(std::format("{}[dependencies]\nphysics = \"1.2\"\nshapes = {{ version = \"~0.3\", "
                         "link = \"beside\" }}\ntools = {{ version = \"2\", registry = \"company\" "
                         "}}\n\n[registries]\ncompany = {{ index = "
                         "\"https://packages.example.com/index\" }}\nlocal = {{ index = "
                         "\"..\\\\registry\\\\index\" }}\n",
                         project));
    CHECK(versioned.diagnostics.empty());
    const fb::manifest_t manifest = versioned.manifest.value_or(fb::manifest_t{});
    REQUIRE(manifest.dependencies.size() == 3);
    CHECK(manifest.dependencies[0].path.empty());
    REQUIRE(manifest.dependencies[0].range);
    CHECK(manifest.dependencies[0].range->text == "1.2");
    CHECK_FALSE(manifest.dependencies[0].registry);
    CHECK(manifest.dependencies[1].range->text == "~0.3");
    CHECK(manifest.dependencies[1].beside);
    CHECK(manifest.dependencies[2].registry == "company");
    REQUIRE(manifest.registries.size() == 2);
    CHECK(manifest.registries[0].name == "company");
    CHECK(manifest.registries[0].index == "https://packages.example.com/index");
    CHECK(manifest.registries[1].index == "../registry/index");

    CHECK(read(std::format("{}[dependencies]\nphysics = \"1.2.x\"\n", project)).diagnostics ==
          diagnostics_t{ "error: \"1.2.x\": \"1.2.x\" isn't a version, which is up to three "
                         "numbers, like 1.2 or 1.2.3" });
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ link = \"beside\" }}\n", project))
              .diagnostics ==
          diagnostics_t{ "error: { link = \"beside\" }: a dependency needs a range of versions, "
                         "like physics = \"1.2\", or a local package's folder, like physics = { "
                         "path = \"../physics\" }" });
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ path = \"../p\", version = \"1\" }}\n",
                           project))
              .diagnostics ==
          diagnostics_t{ "error: \"../p\": a dependency is from a registry, by its version, or a "
                         "local package, by its path, not both" });
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ path = \"../p\", registry = \"x\" }}\n",
                           project))
              .diagnostics.front()
              .contains("takes no 'registry'"));
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ version = \"1\", registry = \"nope\" "
                           "}}\n",
                           project))
              .diagnostics ==
          diagnostics_t{ "error: \"nope\": there's no registry 'nope' in [registries], which "
                         "names each by its index, like nope = { index = "
                         "\"https://packages.example.com/index/\" }" });
    // futureblitz.dev's is there without [registries].
    CHECK(read(std::format("{}[dependencies]\nphysics = {{ version = \"1\", registry = "
                           "\"futureblitz\" }}\n",
                           project))
              .diagnostics.empty());
    CHECK(read(std::format("{}[dependencies]\nphysics = 5\n", project)).diagnostics ==
          diagnostics_t{ "error: 5: a dependency is a range of a package's versions, like "
                         "physics = \"1.2\", or a local package's folder, like physics = { path = "
                         "\"../physics\" }" });
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
    CHECK(
        read(std::format("{}[dependencies]\nphysics = {{ version = 1 }}\n", project)).diagnostics ==
        diagnostics_t{ "error: 1: 'version' is text, like \"1.2\"" });
}

TEST_CASE("[registries] names registries by their indexes", "[manifest]") {
    const std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    const read_t named =
        read(std::format("{}[registries]\nfutureblitz = {{ index = \"http://127.0.0.1:8080/\" }}\n"
                         "dev = {{ index = \"http://index.localhost:8787/\" }}\n"
                         "disk = {{ index = \"file:///C:/registry/index\" }}\n",
                         project));
    CHECK(named.diagnostics.empty());
    REQUIRE(named.manifest);
    CHECK(named.manifest.value_or(fb::manifest_t{}).registries.size() == 3);

    CHECK(read(std::format("{}[registries]\nnear = {{ index = \"http://example.com/index/\" }}\n",
                           project))
              .diagnostics.front()
              .contains("is https://, or http:// only on this machine"));
    CHECK(read(std::format("{}[registries]\nnear = {{ index = \"http://localhost.example.com/\" "
                           "}}\n",
                           project))
              .diagnostics.front()
              .contains("is https://"));
    CHECK(read(std::format("{}[registries]\nnear = {{ index = \"\" }}\n", project)).diagnostics ==
          diagnostics_t{ "error: \"\": a registry's index can't be empty" });
    CHECK(read(std::format("{}[registries]\nnear = {{ url = \"x\" }}\n", project)).diagnostics ==
          diagnostics_t{
              "warning: url: a registry doesn't use 'url'",
              "error: { url = \"x\" }: a registry needs its index, like company = { index = "
              "\"https://packages.example.com/index/\" }",
          });
    CHECK(read(std::format("{}[registries]\n\"my-co\" = {{ index = \"x\" }}\n", project))
              .diagnostics ==
          diagnostics_t{ "error: \"my-co\": a registry's name is an identifier, like company, not "
                         "\"my-co\"" });
    CHECK(read(std::format("{}registries = 3\n", project)).diagnostics.size() == 1);
}

TEST_CASE("[project] says what a registry shows of the package", "[manifest]") {
    const read_t shown =
        read("[project]\nname = \"physics\"\nversion = \"1.0.0-beta.2\"\n"
             "description = \"A 2D physics engine\"\n"
             "license = \"MIT OR (Apache-2.0 WITH LLVM-exception)\"\n"
             "license_file = \"legal\\\\LICENSE-MIT\"\nreadme = \"docs/README.md\"\n"
             "repository = \"https://github.com/me/physics\"\n"
             "homepage = \"https://physics.example.com\"\n"
             "documentation = \"https://docs.example.com\"\n"
             "keywords = [\"physics\", \"2d\", \"game-dev\"]\nfb = \">=0.1\"\n");
    CHECK(shown.diagnostics == diagnostics_t{});
    const fb::manifest_t manifest = shown.manifest.value_or(fb::manifest_t{});
    CHECK(manifest.version == "1.0.0-beta.2");
    CHECK(manifest.description == "A 2D physics engine");
    CHECK(manifest.license == "MIT OR (Apache-2.0 WITH LLVM-exception)");
    CHECK(manifest.license_file == "legal/LICENSE-MIT");
    CHECK(manifest.readme == "docs/README.md");
    CHECK(manifest.repository == "https://github.com/me/physics");
    CHECK(manifest.homepage == "https://physics.example.com");
    CHECK(manifest.documentation == "https://docs.example.com");
    CHECK(manifest.keywords == std::vector<std::string>{ "physics", "2d", "game-dev" });
    REQUIRE(manifest.fb);
    CHECK(manifest.fb->text == ">=0.1");

    const std::string_view project = "[project]\nname = \"physics\"\nversion = \"1.0.0\"\n";
    CHECK(read(std::format("{}license = \"MIT OR\"\n", project)).diagnostics ==
          diagnostics_t{ "error: \"MIT OR\": a license is an SPDX expression, like \"MIT\" or "
                         "\"MIT OR Apache-2.0\", not \"MIT OR\"" });
    CHECK(read(std::format("{}readme = \"../README.md\"\n", project)).diagnostics.size() == 1);
    CHECK(read(std::format("{}homepage = \"http://x.com\"\n", project)).diagnostics ==
          diagnostics_t{ "error: \"http://x.com\": 'homepage' is an https:// URL, not "
                         "\"http://x.com\"" });
    CHECK(read(std::format("{}keywords = [\"a\", \"b\", \"c\", \"d\", \"e\", \"f\"]\n", project))
              .diagnostics.front()
              .contains("at most 5 keywords"));
    CHECK(read(std::format("{}keywords = [\"Physics\"]\n", project))
              .diagnostics.front()
              .contains("a keyword is lowercase letters"));
    CHECK(read(std::format("{}fb = \"x\"\n", project))
              .diagnostics.front()
              .contains("isn't a version"));
}

TEST_CASE("a license is an SPDX expression", "[manifest]") {
    for (const std::string_view license :
         { "MIT", "Apache-2.0", "MIT OR Apache-2.0", "GPL-2.0+", "(MIT AND BSD-2-Clause) OR Zlib",
           "Apache-2.0 WITH LLVM-exception", "LicenseRef-Mine", "DocumentRef-x:LicenseRef-y",
           "((MIT))" }) {
        INFO(license);
        CHECK_FALSE(fb::license_error(license));
    }

    for (const std::string_view license :
         { "", "MIT OR", "OR MIT", "MIT Apache-2.0", "(MIT", "MIT)", "MIT WITH", "MIT/Apache-2.0",
           "AND", "MIT WITH (X)", "()" }) {
        INFO(license);
        CHECK(fb::license_error(license));
    }
}

TEST_CASE("a dependency's link says whether it's in the executable or beside it", "[manifest]") {
    const std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    const read_t linked =
        read(std::format("{}[dependencies]\nphysics = {{ path = \"../physics\" }}\n"
                         "engine = {{ path = \"../engine\", link = \"beside\" }}\n"
                         "math = {{ path = \"../math\", link = \"embed\" }}\n",
                         project));
    CHECK(linked.diagnostics.empty());
    const fb::manifest_t manifest = linked.manifest.value_or(fb::manifest_t{});
    REQUIRE(manifest.dependencies.size() == 3);
    CHECK_FALSE(manifest.dependencies[0].beside);
    CHECK_FALSE(manifest.dependencies[0].link_span);
    CHECK(manifest.dependencies[1].beside);
    CHECK(manifest.dependencies[1].link_span);
    CHECK_FALSE(manifest.dependencies[2].beside);
    CHECK(manifest.dependencies[2].link_span);

    CHECK(read(std::format("{}[dependencies]\nengine = {{ path = \"../engine\", link = "
                           "\"dynamic\" }}\n",
                           project))
              .diagnostics ==
          diagnostics_t{ "error: \"dynamic\": a dependency's link is \"embed\", into the "
                         "executable, or \"beside\", a file of its own beside it, not "
                         "\"dynamic\"" });
    CHECK(read(std::format("{}[dependencies]\nengine = {{ path = \"../engine\", link = true }}\n",
                           project))
              .diagnostics == diagnostics_t{ "error: true: 'link' is text, like \"beside\"" });
}

TEST_CASE("[native] names static archives, system libraries, and dynamic libraries", "[manifest]") {
    const std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    const read_t game = read(std::format("{}[native]\n"
                                         "joltc = \"libs/joltc\"\n"
                                         "sdl = {{ static = \"vendor/SDL3\", notes = 1 }}\n"
                                         "m = {{ system = \"m\" }}\n"
                                         "engine = {{ dynamic = 'native\\{{target}}\\engine' }}\n"
                                         "per = \"libs/{{target}}/{{target}}x\"\n",
                                         project));
    CHECK(game.diagnostics ==
          diagnostics_t{ "warning: notes: a native library doesn't use 'notes'" });
    REQUIRE(game.manifest);
    const std::vector<fb::native_t> natives = game.manifest.value_or(fb::manifest_t{}).natives;
    REQUIRE(natives.size() == 5);
    // A dynamic library's path takes '\' as '/', and {target} stays for the build to fill.
    CHECK(natives[3].name == "engine");
    CHECK(natives[3].kind == fb::native_kind_t::DYNAMIC);
    CHECK(natives[3].path == "native/{target}/engine");
    CHECK(natives[4].path == "libs/{target}/{target}x");
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
    CHECK(errors("x = { static = \"libs/x\", system = \"x\" }\n") ==
          diagnostics_t{ "error: system: a native library is static or system, not both" });
    CHECK(errors("x = { dynamic = \"x\", static = \"x\" }\n") ==
          diagnostics_t{ "error: static: a native library is dynamic or static, not both" });
    CHECK(errors("x = { dynamic = 5 }\n") ==
          diagnostics_t{ "error: 5: 'dynamic' is text, like \"native/{target}/x\"" });
    // {target} is the only {...} a path takes, in a static or dynamic library's.
    const std::string braces = "a native library's path takes {target} for the build's target, "
                               "as in \"native/{target}/x\", and no other {...}";
    CHECK(errors("x = { dynamic = \"native/{os}/x\" }\ny = \"libs/{target\"\n"
                 "z = \"libs/target}\"\n") ==
          diagnostics_t{
              "error: \"native/{os}/x\": " + braces,
              "error: \"libs/{target\": " + braces.substr(0, braces.find("\"native")) +
                  "\"native/{target}/y\", and no other {...}",
              "error: \"libs/target}\": " + braces.substr(0, braces.find("\"native")) +
                  "\"native/{target}/z\", and no other {...}",
          });
    const std::string like = R"(a native library is like x = "libs/x" for a static archive, )"
                             R"(x = { system = "x" }, or x = { dynamic = "native/{target}/x" })";
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
              "error: \"\": a native library's path or name can't be empty",
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

TEST_CASE("[format] gives fb fmt's line width", "[manifest]") {
    constexpr std::string_view project = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";
    CHECK_FALSE(read(project).manifest.value_or(fb::manifest_t{}).format_width);
    const read_t wide = read(std::format("{}[format]\nwidth = 120\n", project));
    CHECK(wide.diagnostics.empty());
    CHECK(wide.manifest.value_or(fb::manifest_t{}).format_width == 120U);

    for (const std::string_view width : { "39", "1001", "\"100\"", "100.5" }) {
        INFO(width);
        const read_t bad = read(std::format("{}[format]\nwidth = {}\n", project, width));
        CHECK_FALSE(bad.manifest);
        CHECK(bad.diagnostics ==
              diagnostics_t{ std::format("error: {}: [format]'s width is a whole number of "
                                         "columns from 40 to 1000, like 100",
                                         width) });
    }

    CHECK(read(std::format("{}[format]\nindent = 2\n", project)).diagnostics ==
          diagnostics_t{ "warning: indent: [format] doesn't use 'indent'" });
    CHECK(read(std::format("format = 5\n{}", project)).diagnostics ==
          diagnostics_t{ "error: format: 'format' is a table, [format], with the line width" });
}

TEST_CASE("text in a project.toml that isn't UTF-8 is reported at its first bad byte",
          "[manifest]") {
    CHECK(read("[project]\nname = \"ab\xff\xfe\"\n").diagnostics ==
          diagnostics_t{ "error: \xff: project.toml isn't valid TOML: Encountered invalid utf-8 "
                         "sequence" });
}
