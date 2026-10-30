#include "fb/driver.hpp"
#include "fb/interface.hpp"
#include "fb/lower.hpp"
#include "fb/package_archive.hpp"
#include "fb/packages.hpp"
#include "fb/process.hpp"
#include "package_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// physics' dependency on shapes, from the registry on disk.
constexpr std::string_view shapes_dependency =
    "shapes = { version = \"0.3\", registry = \"local\" }\n";

/// shapes' code, whose Version says which it is.
std::string shapes_code(const std::string_view version) {
    return std::format(
        "Public Function Version(): String\n    Return \"shapes {}\"\nEnd Function\n", version);
}

/// physics' code, which says which it is, and which shapes it uses.
std::string physics_code(const std::string_view version) {
    return std::format("Import shapes\nPublic Function Describe(): String\n    Return \"physics {} "
                       "with \" + Version()\nEnd Function\n",
                       version);
}

/// The compilation's errors' messages, in order.
std::vector<std::string> errors_of(const fb::compilation_t& compilation) {
    std::vector<std::string> errors;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (diagnostic.severity == fb::severity_t::ERROR) {
            errors.push_back(diagnostic.message);
        }
    }

    return errors;
}

/// What sync_packages() said as it worked, as "Verb message" lines.
struct said_t {
    std::vector<std::string> lines;

    [[nodiscard]] fb::status_t status() {
        return [this](const std::string_view verb, const std::string_view message) {
            lines.push_back(std::format("{} {}", verb, message));
        };
    }
};

/// A registry on disk with shapes 0.3.0 and 0.3.4, and physics 1.0.0 and 1.2.0, which use
/// shapes, and a game that depends on physics from it.
struct world_t {
    fb::testing::sandbox_t sandbox;
    std::string index = sandbox.registry();

    world_t() {
        for (const std::string_view version : { "0.3.0", "0.3.4" }) {
            sandbox.library("shapes", "shapes", version, "",
                            { { "area.fb", shapes_code(version) } });
            sandbox.publish("shapes", index);
        }

        for (const std::string_view version : { "1.0.0", "1.2.0" }) {
            sandbox.library("physics", "physics", version, shapes_dependency,
                            { { "world.fb", physics_code(version) } });
            sandbox.publish("physics", index);
        }

        game("physics = { version = \"1\", registry = \"local\" }\n");
    }

    /// The game's project.toml, with `dependencies`.
    void game(const std::string_view dependencies) const {
        sandbox.write("game/project.toml",
                      std::format("[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n"
                                  "[registries]\nlocal = {{ index = \"../registry/index\" }}\n\n"
                                  "[dependencies]\n{}",
                                  dependencies));
        sandbox.write("game/main.fb", "Import std.core\nImport physics\nPrint(Describe())\n");
    }

    [[nodiscard]] std::expected<fb::sync_result_t, std::string>
    sync(said_t& said, fb::sync_options_t options = {}) const {
        options.status = said.status();
        options.http = fb::testing::offline;
        return fb::sync_packages(sandbox / "game", options);
    }

    /// Builds the game and runs it, giving what it printed, or its errors.
    [[nodiscard]] std::string run() const {
        const std::expected<fb::compilation_t, std::string> game =
            fb::compile_file(sandbox / "game");
        if (!game) {
            return game.error();
        }

        if (const std::vector<std::string> errors = errors_of(*game); !errors.empty()) {
            return errors.front();
        }

        std::filesystem::path program = sandbox / "game/build/game";
        program += fb::executable_extension;
        const fb::mir_module_t module = fb::lower(game->ast, game->analysis);
        const std::expected<fb::process_result_t, std::string> ran =
            fb::build_program(module, *game, program, fb::build_mode_t::DEBUG).and_then([&] {
                return fb::run_process(program, {});
            });
        return ran ? ran->output : ran.error();
    }
};

} // namespace

TEST_CASE("a package with versioned dependencies resolves, locks, and builds from a registry on "
          "disk",
          "[packages]") {
    const world_t world;
    said_t said;
    const std::expected<fb::sync_result_t, std::string> synced = world.sync(said);
    INFO((synced ? std::string() : synced.error()));
    REQUIRE(synced);
    CHECK(said.lines ==
          std::vector<std::string>{ "Adding physics v1.2.0", "Adding shapes v0.3.4" });
    REQUIRE(synced->lockfile.packages.size() == 2);
    const fb::locked_package_t& physics = synced->lockfile.packages[0];
    CHECK(physics.name == "physics");
    CHECK(physics.version == "1.2.0");
    CHECK(physics.source == world.index);
    CHECK(physics.dependencies == std::vector<std::string>{ "shapes" });
    CHECK(physics.checksum ==
          fb::sha256_hex(world.sandbox.read("registry/dl/physics/1.2.0/physics-1.2.0.fbpkg")));
    CHECK(world.sandbox.read("game/fb.lock") == fb::write_lockfile(synced->lockfile));
    CHECK(fb::is_unpacked(physics, fb::build_target()));
    CHECK(world.run() == "physics 1.2.0 with shapes 0.3.4\n");

    // Again, nothing changes, and nothing's said.
    said.lines.clear();
    REQUIRE(world.sync(said));
    CHECK(said.lines.empty());
}

TEST_CASE("fb.lock keeps its versions until fb update, which takes the newest", "[packages]") {
    const world_t world;
    said_t said;
    REQUIRE(world.sync(said));

    // A version published since doesn't change a locked build.
    world.sandbox.library("physics", "physics", "1.3.0", shapes_dependency,
                          { { "world.fb", physics_code("1.3.0") } });
    world.sandbox.publish("physics", world.index);
    said.lines.clear();
    REQUIRE(world.sync(said));
    CHECK(said.lines.empty());
    CHECK(world.run() == "physics 1.2.0 with shapes 0.3.4\n");

    said.lines.clear();
    REQUIRE(world.sync(said, { .update = true, .update_names = { "physics" } }));
    CHECK(said.lines == std::vector<std::string>{ "Updating physics v1.2.0 -> v1.3.0" });
    CHECK(world.run() == "physics 1.3.0 with shapes 0.3.4\n");

    const std::expected<fb::sync_result_t, std::string> unknown =
        world.sync(said, { .update = true, .update_names = { "nope" } });
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error() == "fb.lock has no package nope to update");
}

TEST_CASE("a range fb.lock is outside resolves again, and what's no longer used goes",
          "[packages]") {
    const world_t world;
    said_t said;
    REQUIRE(world.sync(said));
    world.game("physics = { version = \"=1.0.0\", registry = \"local\" }\n");
    said.lines.clear();
    REQUIRE(world.sync(said));
    CHECK(said.lines == std::vector<std::string>{ "Updating physics v1.2.0 -> v1.0.0" });
    CHECK(world.run() == "physics 1.0.0 with shapes 0.3.4\n");

    // Without physics, its shapes isn't in fb.lock either.
    world.game("");
    world.sandbox.write("game/main.fb", "Import std.core\nPrint(\"alone\")\n");
    said.lines.clear();
    const std::expected<fb::sync_result_t, std::string> alone = world.sync(said);
    REQUIRE(alone);
    CHECK(alone->lockfile.packages.empty());
    CHECK(said.lines ==
          std::vector<std::string>{ "Removing physics v1.0.0", "Removing shapes v0.3.4" });
}

TEST_CASE("an archive that isn't what fb.lock says stops the build", "[packages]") {
    const world_t world;
    said_t said;
    REQUIRE(world.sync(said));
    // The cache is cleared, and the registry's archive changed under fb.lock.
    std::filesystem::remove_all(world.sandbox / "home");
    world.sandbox.write("registry/dl/shapes/0.3.4/shapes-0.3.4.fbpkg", "tampered");
    const std::expected<fb::sync_result_t, std::string> synced = world.sync(said);
    REQUIRE_FALSE(synced);
    CHECK(synced.error().starts_with("shapes-0.3.4.fbpkg isn't what fb.lock says it is: its "
                                     "SHA-256 is " +
                                     fb::sha256_hex("tampered")));
    CHECK(world.run() == "shapes v0.3.4 isn't in the cache yet; fb build or fb check fetches it");
}

TEST_CASE("the driver finds registry packages through fb.lock, and says what's missing",
          "[packages]") {
    const world_t world;
    CHECK(world.run() == "physics = \"1\" is from a registry, but there's no fb.lock yet; fb "
                         "build, fb check, or fb update makes it and fetches the package");
    said_t said;
    REQUIRE(world.sync(said));
    world.game("physics = { version = \"2\", registry = \"local\" }\n");
    CHECK(world.run() == "fb.lock has physics v1.2.0, which isn't in \"2\"; fb build, fb check, or "
                         "fb update locks another");
    world.game("physics = \"1\"\n");
    CHECK(world.run() == "fb.lock doesn't have physics = \"1\" yet; fb build, fb check, or fb "
                         "update locks it and fetches it");
    world.game("physics = { version = \"1\", registry = \"local\" }\n");
    world.sandbox.write("game/fb.lock", "version = 9\n");
    CHECK(world.run().starts_with("fb.lock can't be read: fb.lock is of another format"));
}

TEST_CASE("a registry package's dependencies are from its registry, not paths", "[packages]") {
    const world_t world;
    world.sandbox.library("bad", "bad", "1.0.0", "", { { "bad.fb", "// bad\n" } });
    world.sandbox.publish("bad", world.index);
    // As if its archive had a path dependency, which fb publish refuses.
    said_t said;
    world.game("bad = { version = \"1\", registry = \"local\" }\n");
    world.sandbox.write("game/main.fb", "Import std.core\nPrint(\"bad\")\n");
    const std::expected<fb::sync_result_t, std::string> synced = world.sync(said);
    REQUIRE(synced);
    const std::filesystem::path folder = fb::package_cache_folder(world.index, "bad", "1.0.0");
    std::ofstream(folder / "project.toml", std::ios::app) << "other = { path = \"../other\" }\n";
    CHECK(world.run() == "bad v1.0.0 is from a registry, so its dependencies are too, but other "
                         "is a path");
}

TEST_CASE("an unresolvable range says why, before anything's written", "[packages]") {
    const world_t world;
    world.game("physics = { version = \"7\", registry = \"local\" }\n");
    said_t said;
    const std::expected<fb::sync_result_t, std::string> synced = world.sync(said);
    REQUIRE_FALSE(synced);
    CHECK(synced.error().starts_with("no version of physics fits 7, which game asks for"));
    CHECK_FALSE(std::filesystem::exists(world.sandbox / "game/fb.lock"));

    // A project.toml with mistakes is left to the compile, which reports them.
    world.sandbox.write("game/project.toml", "[project\n");
    const std::expected<fb::sync_result_t, std::string> broken = world.sync(said);
    REQUIRE(broken);
    CHECK(broken->broken);
}

TEST_CASE("a path dependency's own registry dependencies are in the program's fb.lock",
          "[packages]") {
    const world_t world;
    world.sandbox.write("engine/project.toml",
                        "[project]\nname = \"engine\"\nversion = \"0.1.0\"\ntype = \"library\"\n\n"
                        "[registries]\nlocal = { index = \"../registry/index\" }\n\n"
                        "[dependencies]\nphysics = { version = \"1.0\", registry = \"local\" }\n");
    world.sandbox.write("engine/engine.fb", "Import physics\nPublic Function Run(): String\n"
                                            "    Return \"engine: \" + Describe()\n"
                                            "End Function\n");
    world.game("engine = { path = \"../engine\" }\n");
    world.sandbox.write("game/main.fb", "Import std.core\nImport engine\nPrint(Run())\n");
    said_t said;
    const std::expected<fb::sync_result_t, std::string> synced = world.sync(said);
    INFO((synced ? std::string() : synced.error()));
    REQUIRE(synced);
    CHECK(synced->lockfile.find("physics") != nullptr);
    CHECK(world.run() == "engine: physics 1.2.0 with shapes 0.3.4\n");
}

TEST_CASE("fb add's line goes in [dependencies], keeping the file's other lines", "[packages]") {
    const auto added = [](const std::string_view text) {
        const std::expected<std::string, std::string> out =
            fb::with_dependency(text, "physics", "physics = \"1.2.0\"");
        return out ? *out : "error: " + out.error();
    };
    CHECK(added("[project]\nname = \"game\"\n") ==
          "[project]\nname = \"game\"\n\n[dependencies]\nphysics = \"1.2.0\"\n");
    CHECK(added("[project]\nname = \"game\"") ==
          "[project]\nname = \"game\"\n\n[dependencies]\nphysics = \"1.2.0\"\n");
    CHECK(
        added(
            "[project]\n\n[dependencies] # mine\nui = { path = \"ui\" }\n\n# later\n[native]\n") ==
        "[project]\n\n[dependencies] # mine\nui = { path = \"ui\" }\nphysics = \"1.2.0\"\n\n# "
        "later\n[native]\n");
    CHECK(added("[dependencies]\nphysics = \"1.0\"  # old\nui = \"2\"\n") ==
          "[dependencies]\nphysics = \"1.2.0\"\nui = \"2\"\n");
    CHECK(added("[dependencies]\r\n\"physics\" = { version = \"1\" }\r\n") ==
          "[dependencies]\r\nphysics = \"1.2.0\"\r\n");
    CHECK(added("[dependencies]\r\nui = \"1\"\r\n") ==
          "[dependencies]\r\nui = \"1\"\r\nphysics = \"1.2.0\"\r\n");
    CHECK(added("[dependencies]\nphysics = {\n  version = \"1\" }\n").starts_with("error: "));
    CHECK(added("[dependencies.physics]\nversion = \"1\"\n").starts_with("error: "));
    CHECK(added("[dependencies]\nphysics.version = \"1\"\n").starts_with("error: "));
    // A key that only starts with the name is another's.
    CHECK(added("[dependencies]\nphysics2 = \"1\"\n") ==
          "[dependencies]\nphysics2 = \"1\"\nphysics = \"1.2.0\"\n");
}
