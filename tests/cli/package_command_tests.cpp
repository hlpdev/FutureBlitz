// fb's package commands, run as fb itself, against a registry on disk and one the test serves on
// 127.0.0.1, never the internet.

#include "../compiler/package_helpers.hpp"
#include "../compiler/registry_server.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

/// Runs fb, with `input` as its stdin, and collects its exit code and its output.
fb::process_result_t fb_run(const std::vector<std::string>& arguments,
                            const std::optional<std::string_view> input = std::string_view()) {
    const std::expected<fb::process_result_t, std::string> result =
        fb::run_process(FB_CLI, arguments, input);
    REQUIRE(result);
    return *result;
}

/// The current folder, changed for as long as it lives, as fb's commands find their package from
/// it.
class working_folder_t {
public:
    explicit working_folder_t(const std::filesystem::path& folder)
        : m_old(std::filesystem::current_path()) {
        std::filesystem::current_path(folder);
    }

    working_folder_t(const working_folder_t&) = delete;
    working_folder_t& operator=(const working_folder_t&) = delete;
    working_folder_t(working_folder_t&&) = delete;
    working_folder_t& operator=(working_folder_t&&) = delete;

    ~working_folder_t() {
        std::error_code ignored;
        std::filesystem::current_path(m_old, ignored);
    }

private:
    std::filesystem::path m_old;
};

void check_usage(const std::vector<std::string>& arguments, const std::string_view message) {
    const fb::process_result_t result = fb_run(arguments);
    INFO(result.output);
    CHECK(result.exit_code == 2);
    CHECK(result.output.contains(message));
}

} // namespace

TEST_CASE("fb's package commands say what they take", "[cli-packages]") {
    check_usage({ "add" }, "'fb add' needs the packages it adds");
    check_usage({ "search" }, "'fb search' needs what it looks for");
    check_usage({ "yank", "physics" }, "'fb yank' takes one package's version");
    check_usage({ "owner" }, "'fb owner' takes one package");
    check_usage({ "owner", "physics", "--add", "a", "--remove", "b" }, "one --add or --remove");
    check_usage({ "login", "extra" }, "unexpected argument 'extra'; 'fb login' takes none");
    check_usage({ "update", "--registry", "x" }, "'--registry' doesn't work with 'fb update'");
    check_usage({ "add", "physics", "--limit", "3" }, "'--limit' doesn't work with 'fb add'");
    check_usage({ "search", "x", "--limit", "0" }, "'--limit' takes a number from 1 to 100");
    check_usage({ "publish", "a", "b" }, "unexpected argument 'b'; 'fb publish' takes one");
    check_usage({ "publish", "--dry-run=yes" }, "'--dry-run' takes no value");
    check_usage({ "add", "physics", "--nope" }, "unknown option '--nope'");
    check_usage({ "fmt", "--offline" }, "'--offline' doesn't work with 'fb fmt'");

    const fb::process_result_t help = fb_run({ "help", "publish" });
    CHECK(help.exit_code == 0);
    CHECK(help.output.contains("Usage: fb publish [options] [package folder]"));
    CHECK(help.output.contains("--dry-run"));
    CHECK(fb_run({ "add", "--help" }).output.contains("Usage: fb add [options] <package>"));
    const std::string all = fb_run({ "help" }).output;
    for (const std::string_view command :
         { "add", "update", "search", "publish", "yank", "owner", "login", "logout" }) {
        INFO(command);
        CHECK(all.contains(std::format("\n  {} ", command)));
    }

    CHECK(fb_run({ "help", "build" }).output.contains("--offline"));
}

TEST_CASE("fb publish, add, search, and yank work with a registry on disk", "[cli-packages]") {
    const fb::testing::sandbox_t sandbox;
    const std::string index = fb::to_utf8(sandbox / "registry/index");
    (void)sandbox.registry();
    const std::string shapes_code =
        "Public Function Version(): String\n    Return \"shapes 0.3.0\"\nEnd Function\n";
    const std::string physics_code = "Import shapes\nPublic Function Describe(): String\n    "
                                     "Return \"physics with \" + Version()\nEnd Function\n";
    sandbox.library("shapes", "shapes", "0.3.0", "", { { "area.fb", shapes_code } });
    sandbox.library("physics", "physics", "1.2.0",
                    "shapes = { version = \"0.3\", registry = \"local\" }\n",
                    { { "world.fb", physics_code } });

    // A dry run builds and lists the archive, and publishes nothing.
    const fb::process_result_t dry =
        fb_run({ "publish", fb::to_utf8(sandbox / "shapes"), "--registry", index, "--dry-run" });
    INFO(dry.output);
    REQUIRE(dry.exit_code == 0);
    CHECK(dry.output.contains("Packaged shapes-0.3.0.fbpkg: 4 files"));
    CHECK(dry.output.contains("shapes-0.3.0.fbpkg  area.fb"));
    CHECK(dry.output.contains("nothing was published"));
    CHECK(std::filesystem::exists(sandbox / "shapes/build/package/shapes-0.3.0.fbpkg"));
    CHECK_FALSE(std::filesystem::exists(sandbox / "registry/index/shapes"));

    for (const std::string_view package : { "shapes", "physics" }) {
        const fb::process_result_t published =
            fb_run({ "publish", fb::to_utf8(sandbox / package), "--registry", index });
        INFO(published.output);
        REQUIRE(published.exit_code == 0);
        CHECK(published.output.contains(std::format("Published {} v", package)));
    }

    const fb::process_result_t again =
        fb_run({ "publish", fb::to_utf8(sandbox / "shapes"), "--registry", index });
    CHECK(again.exit_code == 1);
    CHECK(again.output.contains("shapes v0.3.0 is published already"));

    // Another package adds physics by the registry's name in its [registries].
    sandbox.write("game/project.toml", "[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n"
                                       "[registries]\nlocal = { index = \"../registry/index\" }\n");
    sandbox.write("game/main.fb", "Import std.core\nImport physics\nPrint(Describe())\n");
    const working_folder_t in_game(sandbox / "game");
    const fb::process_result_t added = fb_run({ "add", "physics", "--registry", "local" });
    INFO(added.output);
    REQUIRE(added.exit_code == 0);
    CHECK(added.output.contains("Adding physics v1.2.0 to dependencies"));
    CHECK(added.output.contains("Adding shapes v0.3.0"));
    CHECK(sandbox.read("game/project.toml")
              .ends_with(
                  "\n[dependencies]\nphysics = { version = \"1.2.0\", registry = \"local\" }\n"));
    CHECK(std::filesystem::exists(sandbox / "game/fb.lock"));

    const fb::process_result_t ran = fb_run({ "run" });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("physics with shapes 0.3.0\n"));
    CHECK(fb_run({ "build", "--offline" }).exit_code == 0);

    // A package it doesn't have is taken out of project.toml again.
    const std::string before = sandbox.read("game/project.toml");
    const fb::process_result_t missing = fb_run({ "add", "nope@1", "--registry", "local" });
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.contains("there's no package nope in"));
    CHECK(sandbox.read("game/project.toml") == before);

    const fb::process_result_t found = fb_run({ "search", "phys", "--registry", "local" });
    INFO(found.output);
    CHECK(found.exit_code == 0);
    CHECK(found.output.contains("physics = \"1.2.0\""));
    CHECK(fb_run({ "search", "zzz", "--registry", "local" }).output.contains("no packages in"));

    const fb::process_result_t yanked = fb_run({ "yank", "physics@1.2.0", "--registry", "local" });
    INFO(yanked.output);
    CHECK(yanked.exit_code == 0);
    CHECK(yanked.output.contains("Yanked physics v1.2.0"));
    CHECK(sandbox.read("registry/index/physics").contains("\"yanked\":true"));
    // fb.lock still has it, so it still builds.
    CHECK(fb_run({ "check" }).exit_code == 0);
    CHECK(fb_run({ "yank", "physics@1.2.0", "--registry", "local", "--undo" })
              .output.contains("Unyanked physics v1.2.0"));

    const fb::process_result_t updated = fb_run({ "update" });
    CHECK(updated.exit_code == 0);
    CHECK(fb_run({ "update", "nope" }).output.contains("fb.lock has no package nope to update"));
}

// Against a registry's real API, which FB_TEST_REGISTRY names by its index's URL, like
// http://index.localhost:8787/ for futureblitz-web's workers/api under `wrangler dev`, and
// FB_TEST_REGISTRY_TOKEN gives a token of, with the publish scopes: one computer logs in with it,
// publishes two packages, and searches for them, and another, with a cache of its own, adds one
// and builds with it. Without FB_TEST_REGISTRY, it's skipped, so the tests stay offline. A
// registry's versions are forever, so each run's packages have names of their own.
TEST_CASE("fb publishes to a registry's API, and another computer adds and builds",
          "[cli-packages]") {
    const std::optional<std::string> index = fb::environment_variable("FB_TEST_REGISTRY");
    const std::optional<std::string> token = fb::environment_variable("FB_TEST_REGISTRY_TOKEN");
    if (!index || index->empty() || !token) {
        SKIP("FB_TEST_REGISTRY and FB_TEST_REGISTRY_TOKEN name no registry to test against");
    }

    const fb::testing::sandbox_t sandbox;
    const fb::testing::environment_t credentials("FB_CREDENTIALS_FILE",
                                                 fb::to_utf8(sandbox / "credentials.json"));
    const fb::testing::environment_t no_token("FB_REGISTRY_TOKEN", std::nullopt);
    const std::string run = fb::sha256_hex(fb::to_utf8(sandbox.folder())).substr(0, 10);
    const std::string shapes = "shapes_" + run;
    const std::string physics = "physics_" + run;
    // The registry stands in for futureblitz.dev, so the packages' dependencies name no registry,
    // as the registry takes them.
    const std::string registries =
        std::format("[registries]\nfutureblitz = {{ index = \"{}\" }}\n", *index);
    const auto library = [&](const std::string& name, const std::string& dependencies,
                             const std::string& code) {
        sandbox.write(name + "/project.toml",
                      std::format("[project]\nname = \"{}\"\nversion = \"1.0.0\"\ntype = "
                                  "\"library\"\ndescription = \"fb's test of the registry\"\n"
                                  "license = \"MIT\"\n\n{}\n[dependencies]\n{}",
                                  name, registries, dependencies));
        sandbox.write(name + "/LICENSE", "MIT License\n");
        sandbox.write(name + "/README.md", std::format("# {}\n\nA test.\n", name));
        sandbox.write(name + "/code.fb", code);
    };
    library(shapes, "", "Public Function Version(): String\n    Return \"shapes\"\nEnd Function\n");
    library(physics, std::format("{} = \"1\"\n", shapes),
            std::format("Import {}\nPublic Function Describe(): String\n    Return \"physics "
                        "with \" + Version()\nEnd Function\n",
                        shapes));

    {
        const fb::testing::environment_t home("FB_HOME", fb::to_utf8(sandbox / "home-one"));
        const fb::process_result_t logged_in =
            fb_run({ "login", "--token", "--registry", *index }, *token + "\n");
        INFO(logged_in.output);
        REQUIRE(logged_in.exit_code == 0);
        for (const std::string& name : { shapes, physics }) {
            const fb::process_result_t published =
                fb_run({ "publish", fb::to_utf8(sandbox / name), "--registry", *index });
            INFO(published.output);
            REQUIRE(published.exit_code == 0);
            CHECK(published.output.contains(std::format("Published {} v1.0.0", name)));
        }

        // Search follows a publish by the registry's queue, within a minute.
        const std::string listed = std::format("{} = \"1.0.0\"", physics);
        fb::process_result_t found = fb_run({ "search", physics, "--registry", *index });
        for (int tries = 0; tries < 60 && !found.output.contains(listed); ++tries) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            found = fb_run({ "search", physics, "--registry", *index });
        }

        INFO(found.output);
        CHECK(found.exit_code == 0);
        CHECK(found.output.contains(listed));

        // Its owner is the token's account, and a yank takes back.
        const fb::process_result_t owners = fb_run({ "owner", physics, "--registry", *index });
        INFO(owners.output);
        CHECK(owners.exit_code == 0);
        CHECK(owners.output.starts_with("user "));
        CHECK(fb_run({ "yank", shapes + "@1.0.0", "--registry", *index }).exit_code == 0);
        CHECK(fb_run({ "yank", shapes + "@1.0.0", "--registry", *index, "--undo" }).exit_code == 0);
    }

    // Another computer, with a cache of its own and no token.
    const fb::testing::environment_t home("FB_HOME", fb::to_utf8(sandbox / "home-two"));
    sandbox.write("game/project.toml", std::format("[project]\nname = \"game\"\nversion = "
                                                   "\"0.1.0\"\n\n{}",
                                                   registries));
    sandbox.write("game/main.fb",
                  std::format("Import std.core\nImport {}\nPrint(Describe())\n", physics));
    const working_folder_t in_game(sandbox / "game");
    const fb::process_result_t added = fb_run({ "add", physics });
    INFO(added.output);
    REQUIRE(added.exit_code == 0);
    CHECK(added.output.contains(std::format("Downloading {} v1.0.0", shapes)));
    CHECK(sandbox.read("game/project.toml").contains(std::format("{} = \"1.0.0\"", physics)));
    const fb::process_result_t ran = fb_run({ "run" });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("physics with shapes\n"));
}

TEST_CASE("fb login keeps a token that publish, owner, and logout use", "[cli-packages]") {
    const fb::testing::registry_server_t server;
    const fb::testing::sandbox_t sandbox;
    const fb::testing::environment_t credentials("FB_CREDENTIALS_FILE",
                                                 fb::to_utf8(sandbox / "home/credentials.json"));
    const fb::testing::environment_t no_token("FB_REGISTRY_TOKEN", std::nullopt);
    const fb::testing::environment_t no_browser("FB_BROWSER", "none");
    const std::string index = server.index();

    const fb::process_result_t without = fb_run({ "owner", "physics", "--registry", index });
    CHECK(without.exit_code == 1);
    CHECK(without.output.contains("fb has no token for"));

    // The browser's sign-in, approved on the second poll.
    const fb::process_result_t signed_in = fb_run({ "login", "--registry", index });
    INFO(signed_in.output);
    REQUIRE(signed_in.exit_code == 0);
    CHECK(signed_in.output.contains("cli-login/login1 to sign in, in a browser"));
    CHECK(signed_in.output.contains("WDJB-MJHT"));
    CHECK(signed_in.output.contains("Logged in to " + index));
    CHECK(sandbox.read("home/credentials.json").contains(fb::testing::test_token));

    sandbox.library("physics", "physics", "1.0.0", "", { { "world.fb", "// physics\n" } });
    const fb::process_result_t published =
        fb_run({ "publish", fb::to_utf8(sandbox / "physics"), "--registry", index });
    INFO(published.output);
    CHECK(published.exit_code == 0);
    CHECK(published.output.contains("Uploading physics-1.0.0.fbpkg"));
    CHECK(published.output.contains("warning: the readme has no title"));
    CHECK(published.output.contains("Published physics v1.0.0"));

    CHECK(fb_run({ "owner", "physics", "--registry", index }).output.contains("user tester"));
    CHECK(fb_run({ "owner", "physics", "--registry", index, "--add", "alice" })
              .output.contains("alice is invited to own physics"));

    CHECK(fb_run({ "logout", "--registry", index }).output.contains("Logged out of " + index));
    CHECK(fb_run({ "logout", "--registry", index }).output.contains("fb kept no token for"));

    // A token from stdin, as one made on the dashboard is given.
    const fb::process_result_t pasted =
        fb_run({ "login", "--token", "--registry", index }, "fbr_pasted\n");
    CHECK(pasted.exit_code == 0);
    CHECK(sandbox.read("home/credentials.json").contains("fbr_pasted"));
    CHECK(fb_run({ "login", "--token", "--registry", index }, "").output.contains("gave none"));

    // FB_REGISTRY_TOKEN comes first, as in CI.
    const fb::testing::environment_t ci("FB_REGISTRY_TOKEN", std::string(fb::testing::test_token));
    CHECK(fb_run({ "yank", "physics@1.0.0", "--registry", index }).exit_code == 0);
}
