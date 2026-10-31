// fb's package commands, run as fb itself, against a registry on disk and one the test serves on
// 127.0.0.1, never the internet.

#include "../compiler/package_helpers.hpp"
#include "../compiler/registry_server.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
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
    // commands-9: a package's name goes into the API's paths, so it's checked first.
    check_usage({ "yank", "@1.0.0" }, "a package's name is an identifier");
    check_usage({ "yank", "a/b@1.0.0" }, "not \"a/b\"");
    check_usage({ "owner", "x/../y" }, "not \"x/../y\"");
    check_usage({ "owner", "physics", "--add", "a/b" }, "'a/b' isn't a username");
    check_usage({ "owner", "physics", "--remove", "team:" }, "'team:' isn't a username");
    check_usage({ "owner", "physics", "--add", "-alice" }, "isn't a username");
    // commands-11: publishing is the network's, so --offline is only for a dry run.
    check_usage({ "publish", "--offline" }, "fb publish sends the package to the registry");
    // commands-6's scheme checks: --registry's URL is one an index takes, in any letter case.
    check_usage({ "search", "x", "--registry", "ftp://example.com/index/" }, "ftp");
    check_usage({ "search", "x", "--registry", "HTTP://example.com/index/" },
                "http:// only on this machine");

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
    // m10-journey-2 and m10-journey-12: each command's options say what it takes.
    const std::string add = fb_run({ "help", "add" }).output;
    CHECK(add.contains("A registry that [registries] names (futureblitz.dev)"));
    CHECK_FALSE(add.contains("index's URL"));
    CHECK(fb_run({ "help", "publish" })
              .output.contains("With --dry-run, use only what's in the cache"));
    CHECK(fb_run({ "help", "search" }).output.contains("have every word"));
    CHECK(fb_run({ "help", "owner" }).output.contains("needs no token"));
}

TEST_CASE("a --registry folder has to be a registry's, and a host's name needs its https://",
          "[cli-packages]") {
    // m10-edges-7: every command says a folder without config.json is no registry, and a name
    // like a host's that's no folder is most likely a URL that lost its scheme.
    const fb::testing::sandbox_t sandbox;
    const working_folder_t in_sandbox(sandbox.folder());
    const fb::testing::environment_t token("FB_REGISTRY_TOKEN", "x");
    const std::string no_registry = "/nope: it has no config.json";
    const std::string hint =
        "; an index's URL starts with https://, like https://staging-index.futureblitz.dev/";
    for (const std::vector<std::string>& command : std::vector<std::vector<std::string>>{
             { "search", "json" },
             { "yank", "mathx@1.0.0" },
             { "owner", "mathx" },
             { "login" },
         }) {
        std::vector<std::string> missing = command;
        missing.insert(missing.end(), { "--registry", "./nope" });
        const fb::process_result_t folder = fb_run(missing);
        INFO(folder.output);
        CHECK(folder.exit_code == 2);
        CHECK(folder.output.contains("there's no registry in "));
        CHECK(folder.output.contains(no_registry));
        CHECK_FALSE(folder.output.contains("https://"));

        std::vector<std::string> host = command;
        host.insert(host.end(), { "--registry", "staging-index.futureblitz.dev" });
        const fb::process_result_t bare = fb_run(host);
        INFO(bare.output);
        CHECK(bare.exit_code == 2);
        CHECK(bare.output.contains("it has no config.json" + hint));
    }

    // A folder that's there, without config.json, isn't a host's name.
    std::filesystem::create_directories(sandbox / "index.example.com");
    const fb::process_result_t there =
        fb_run({ "search", "json", "--registry", "index.example.com" });
    CHECK(there.exit_code == 2);
    CHECK(there.output.contains("it has no config.json"));
    CHECK_FALSE(there.output.contains("https://"));
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
    // m10-journey-10: it's said to be added once it's locked.
    CHECK(added.output.contains("Adding shapes v0.3.0"));
    CHECK(added.output.find("Adding shapes v0.3.0") <
          added.output.find("Added physics v1.2.0 to dependencies"));
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
    CHECK_FALSE(missing.output.contains("to dependencies"));
    CHECK(sandbox.read("game/project.toml") == before);
    const fb::process_result_t unfit = fb_run({ "add", "physics@2", "--registry", "local" });
    CHECK(unfit.exit_code == 1);
    CHECK(unfit.output.contains("no version of physics fits 2, which game asks for; its only "
                                "version is 1.2.0"));
    CHECK_FALSE(unfit.output.contains("this fb is"));
    CHECK_FALSE(unfit.output.contains("to dependencies"));
    CHECK(sandbox.read("game/project.toml") == before);

    const fb::process_result_t found = fb_run({ "search", "phys", "--registry", "local" });
    INFO(found.output);
    CHECK(found.exit_code == 0);
    CHECK(found.output.contains("physics = \"1.2.0\""));
    CHECK(fb_run({ "search", "zzz", "--registry", "local" }).output.contains("no packages in"));
    // commands-12 and m10-journey-1: every one of the words, in any letter case, as the API's
    // search needs every word.
    CHECK(fb_run({ "search", "PHYSICS", "--registry", "local" })
              .output.contains("physics = \"1.2.0\""));
    const std::string both = fb_run({ "search", "SHAPE", "pes", "--registry", "local" }).output;
    CHECK(both.contains("shapes = \"0.3.0\""));
    CHECK_FALSE(both.contains("physics"));
    const fb::process_result_t neither =
        fb_run({ "search", "shape", "phys", "--registry", "local" });
    CHECK(neither.exit_code == 0);
    CHECK(neither.output.contains("no packages in"));
    CHECK_FALSE(neither.output.contains(" = \""));

    // A dry run can be offline; a publish can't.
    CHECK(fb_run({ "publish", fb::to_utf8(sandbox / "shapes"), "--registry", index, "--dry-run",
                   "--offline" })
              .exit_code == 0);
    // A registry on disk has no accounts.
    const fb::process_result_t login = fb_run({ "login", "--token", "--registry", index }, "x\n");
    CHECK(login.exit_code == 2);
    CHECK(login.output.contains("a registry on disk has no accounts"));

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
TEST_CASE("fb publish checks a path dependency's version is the registry's, and builds the "
          "archive from the registry's packages",
          "[cli-packages]") {
    const fb::testing::sandbox_t sandbox;
    const std::string index = fb::to_utf8(sandbox / "registry/index");
    (void)sandbox.registry();
    const std::string version_code =
        "Public Function Version(): String\n    Return \"shapes\"\nEnd Function\n";
    sandbox.library("shapes", "shapes", "0.3.0", "", { { "area.fb", version_code } });
    REQUIRE(fb_run({ "publish", fb::to_utf8(sandbox / "shapes"), "--registry", index }).exit_code ==
            0);

    // The folder physics builds with has a version the registry doesn't, with what it doesn't.
    sandbox.library("shapes_local", "shapes", "0.3.9", "",
                    { { "area.fb", version_code + "Public Function Extra(): Int => 1\n" } });
    sandbox.library("physics", "physics", "1.2.0",
                    "shapes = { path = \"../shapes_local\", version = \"0.3\", registry = "
                    "\"local\" }\n",
                    { { "world.fb", "Import shapes\nPublic Function Uses(): Int => Extra()\n" } });
    const fb::process_result_t unpublished =
        fb_run({ "publish", fb::to_utf8(sandbox / "physics"), "--registry", index });
    INFO(unpublished.output);
    CHECK(unpublished.exit_code == 1);
    CHECK(unpublished.output.contains(
        "physics builds with shapes v0.3.9 from its folder, '../shapes_local', which the "
        "registry doesn't have, so no one could build the published package; publish shapes "
        "v0.3.9 first"));

    // At the registry's version, the archive builds from the registry's shapes, which has no
    // Extra, in a dry run too.
    sandbox.library("shapes_local", "shapes", "0.3.0", "",
                    { { "area.fb", version_code + "Public Function Extra(): Int => 1\n" } });
    for (const bool dry : { true, false }) {
        std::vector<std::string> arguments{ "publish", fb::to_utf8(sandbox / "physics"),
                                            "--registry", index };
        if (dry) {
            arguments.emplace_back("--dry-run");
        }

        const fb::process_result_t unbuilt = fb_run(arguments);
        INFO(unbuilt.output);
        CHECK(unbuilt.exit_code == 1);
        CHECK(unbuilt.output.contains("Verifying physics v1.2.0 as the registry would have it"));
        CHECK(unbuilt.output.contains("'Extra' isn't declared"));
        CHECK(unbuilt.output.contains("physics v1.2.0 doesn't build as the registry would have "
                                      "it, from its packages, because of 1 error; --no-verify "
                                      "publishes it anyway"));
    }

    CHECK_FALSE(std::filesystem::exists(sandbox / "registry/index/ph/ys/physics"));
    const fb::process_result_t unverified = fb_run(
        { "publish", fb::to_utf8(sandbox / "physics"), "--registry", index, "--no-verify" });
    INFO(unverified.output);
    CHECK(unverified.exit_code == 0);
    CHECK_FALSE(unverified.output.contains("Verifying"));
    CHECK(unverified.output.contains("Published physics v1.2.0"));
}

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

    // A change needs a token.
    const fb::process_result_t without =
        fb_run({ "owner", "physics", "--registry", index, "--add", "alice" });
    CHECK(without.exit_code == 1);
    CHECK(without.output.contains("fb has no token for"));

    // The browser's sign-in, approved on the second poll.
    const fb::process_result_t signed_in = fb_run({ "login", "--registry", index });
    INFO(signed_in.output);
    REQUIRE(signed_in.exit_code == 0);
    CHECK(signed_in.output.contains("cli-login/login1 to sign in, in a browser"));
    CHECK(signed_in.output.contains("WDJB-MJHT"));
    CHECK(signed_in.output.contains("Logged in to " + index + " as tester"));
    CHECK(sandbox.read("home/credentials.json").contains(fb::testing::test_token));

    sandbox.library("physics", "physics", "1.0.0", "", { { "world.fb", "// physics\n" } });
    const fb::process_result_t published =
        fb_run({ "publish", fb::to_utf8(sandbox / "physics"), "--registry", index });
    INFO(published.output);
    CHECK(published.exit_code == 0);
    CHECK(published.output.contains("warning: the readme has no title"));
    CHECK(published.output.contains("Published physics v1.0.0"));
    // m10-edges-12: Uploading says the size Packaged did.
    const std::string packaged = "Packaged physics-1.0.0.fbpkg: ";
    const std::size_t files = published.output.find(packaged);
    REQUIRE(files != std::string::npos);
    const std::size_t size = published.output.find(", ", files) + 2;
    std::string shown = published.output.substr(size, published.output.find('\n', size) - size);
    if (shown.ends_with('\r')) {
        shown.pop_back();
    }

    CHECK(shown.ends_with(" B"));
    CHECK(published.output.contains(std::format("Uploading physics-1.0.0.fbpkg ({})", shown)));

    // m10-edges-11 and m10-journey-11: an invite is said as one, and a team as added.
    CHECK(fb_run({ "owner", "physics", "--registry", index }).output.contains("user tester"));
    CHECK(fb_run({ "owner", "physics", "--registry", index, "--add", "alice" })
              .output.contains("Invited alice to own physics, which they accept on the package "
                               "site"));
    CHECK(fb_run({ "owner", "physics", "--registry", index, "--add", "team:acme" })
              .output.contains("Added team acme as an owner of physics"));

    CHECK(fb_run({ "logout", "--registry", index }).output.contains("Logged out of " + index));
    CHECK(fb_run({ "logout", "--registry", index }).output.contains("fb kept no token for"));

    // m10-edges-5: the list needs no token, and shows the invites.
    const fb::process_result_t listed = fb_run({ "owner", "physics", "--registry", index });
    INFO(listed.output);
    CHECK(listed.exit_code == 0);
    CHECK(listed.output.starts_with("user tester\n"));
    CHECK(listed.output.contains("\nuser alice (invited, until 2026-11-13)\n"));

    // A token from stdin, as one made on the dashboard is given, without what's around it, kept
    // once the registry takes it, and saying whose it is.
    const fb::process_result_t pasted =
        fb_run({ "login", "--token", "--registry", index },
               std::format(" \t{}\t \r\n", fb::testing::test_token));
    INFO(pasted.output);
    CHECK(pasted.exit_code == 0);
    CHECK(pasted.output.contains("Logged in to " + index + " as tester"));
    const std::string kept = std::format("\"{}\"", fb::testing::test_token);
    CHECK(sandbox.read("home/credentials.json").contains(kept));
    // m10-edges-9 and m10-journey-3: one the registry doesn't take isn't kept over it.
    const fb::process_result_t refused =
        fb_run({ "login", "--token", "--registry", index }, "fbr_notarealtoken\n");
    CHECK(refused.exit_code == 1);
    CHECK(refused.output.contains("the registry didn't take the token: a token is needed, and "
                                  "that one isn't it"));
    CHECK_FALSE(refused.output.contains("Logged in"));
    CHECK(sandbox.read("home/credentials.json").contains(kept));
    CHECK_FALSE(sandbox.read("home/credentials.json").contains("fbr_notarealtoken"));
    CHECK(fb_run({ "login", "--token", "--registry", index }, "").output.contains("gave none"));
    // commands-7: a token is printable ASCII, and one that isn't is refused, leaving nothing.
    const fb::process_result_t binary =
        fb_run({ "login", "--token", "--registry", index }, "fbr_\xff\xfe\n");
    CHECK(binary.exit_code == 2);
    CHECK(binary.output.contains("that isn't a registry's token"));
    CHECK(sandbox.read("home/credentials.json").contains(kept));
    for (const auto& entry : std::filesystem::directory_iterator(sandbox / "home")) {
        CHECK_FALSE(fb::to_utf8(entry.path().filename()).contains(".partial"));
    }

    // FB_REGISTRY_TOKEN comes first, as in CI.
    {
        const fb::testing::environment_t ci("FB_REGISTRY_TOKEN",
                                            std::string(fb::testing::test_token));
        CHECK(fb_run({ "yank", "physics@1.0.0", "--registry", index }).exit_code == 0);
    }

    // m10-edges-6: a token without the owners scope, as fb login's page gives by default, says how
    // to get one that has it.
    const fb::testing::environment_t unscoped("FB_REGISTRY_TOKEN",
                                              std::string(fb::testing::publish_only_token));
    const fb::process_result_t scoped =
        fb_run({ "owner", "physics", "--registry", index, "--add", "bob" });
    INFO(scoped.output);
    CHECK(scoped.exit_code == 1);
    CHECK(scoped.output.contains("the token doesn't have the owners scope; a token that has it "
                                 "comes from fb login, with it ticked on the sign-in page"));
}

TEST_CASE("fb login checks it can keep a token before the sign-in, which it never loses",
          "[cli-packages]") {
    // commands-3: a store that can't keep a token fails fb login before anything's approved.
    const fb::testing::registry_server_t server;
    const fb::testing::sandbox_t sandbox;
    sandbox.write("home/credentials.json", "[1]");
    const fb::testing::environment_t credentials("FB_CREDENTIALS_FILE",
                                                 fb::to_utf8(sandbox / "home/credentials.json"));
    const fb::testing::environment_t no_browser("FB_BROWSER", "none");
    const fb::process_result_t broken = fb_run({ "login", "--registry", server.index() });
    INFO(broken.output);
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.contains("fb can't keep a token: "));
    CHECK(broken.output.contains("isn't a JSON object of tokens"));
    for (const std::string& request : server.requests()) {
        CHECK_FALSE(request.contains("cli-logins"));
    }

    // An empty file, made to be private first, is a store without tokens yet.
    sandbox.write("home/credentials.json", "");
    const fb::process_result_t kept = fb_run({ "login", "--registry", server.index() });
    INFO(kept.output);
    CHECK(kept.exit_code == 0);
    CHECK(sandbox.read("home/credentials.json").contains(fb::testing::test_token));
}

TEST_CASE("fb publish in a GitHub workflow uses trusted publishing, and revokes its token",
          "[cli-packages]") {
    // commands-14: fb publish itself, with ACTIONS_ID_TOKEN_REQUEST_URL, asks for the
    // workflow's token, publishes with it, and revokes it after, whether or not it published.
    const fb::testing::registry_server_t server;
    const fb::testing::sandbox_t sandbox;
    const fb::testing::environment_t credentials("FB_CREDENTIALS_FILE",
                                                 fb::to_utf8(sandbox / "home/credentials.json"));
    const fb::testing::environment_t no_token("FB_REGISTRY_TOKEN", std::nullopt);
    const std::string base = server.index().substr(0, server.index().size() - 6);
    const fb::testing::environment_t url("ACTIONS_ID_TOKEN_REQUEST_URL",
                                         base + "github/oidc?api-version=2.0");
    const fb::testing::environment_t request("ACTIONS_ID_TOKEN_REQUEST_TOKEN", "request-token");
    sandbox.library("physics", "physics", "1.0.0", "", { { "world.fb", "// physics\n" } });
    std::string toml = sandbox.read("physics/project.toml");
    toml.replace(toml.find("../registry/index"), std::string_view("../registry/index").size(),
                 server.index());
    sandbox.write("physics/project.toml", toml);

    const auto revokes = [&] {
        return std::ranges::count(server.requests(), std::string("DELETE /api/v1/tokens/current"));
    };
    const fb::process_result_t published =
        fb_run({ "publish", fb::to_utf8(sandbox / "physics"), "--registry", server.index() });
    INFO(published.output);
    CHECK(published.exit_code == 0);
    CHECK(published.output.contains("Published physics v1.0.0"));
    const std::vector<std::string> requests = server.requests();
    CHECK(std::ranges::contains(requests, std::string("POST /api/v1/trusted-publishing/token")));
    CHECK(requests.back() == "DELETE /api/v1/tokens/current");
    CHECK(revokes() == 1);
    // Nothing was kept.
    CHECK_FALSE(std::filesystem::exists(sandbox / "home/credentials.json"));

    // A publish the registry refuses revokes it too.
    const fb::process_result_t again =
        fb_run({ "publish", fb::to_utf8(sandbox / "physics"), "--registry", server.index() });
    CHECK(again.exit_code == 1);
    CHECK(again.output.contains("published already"));
    CHECK(server.requests().back() == "DELETE /api/v1/tokens/current");
    CHECK(revokes() == 2);

    // m10-journey-9: a package the token isn't for, as a new one isn't, uploads nothing, says
    // where its first version comes from, and revokes the token.
    sandbox.library("shapes", "shapes", "0.1.0", "", { { "area.fb", "// shapes\n" } });
    toml = sandbox.read("shapes/project.toml");
    toml.replace(toml.find("../registry/index"), std::string_view("../registry/index").size(),
                 server.index());
    sandbox.write("shapes/project.toml", toml);
    const fb::process_result_t fresh =
        fb_run({ "publish", fb::to_utf8(sandbox / "shapes"), "--registry", server.index() });
    INFO(fresh.output);
    CHECK(fresh.exit_code == 1);
    CHECK(fresh.output.contains("shapes isn't on the registry yet; its first version comes from "
                                "fb login or FB_REGISTRY_TOKEN"));
    CHECK_FALSE(fresh.output.contains("Uploading"));
    CHECK(revokes() == 3);
}

TEST_CASE("fb login doesn't wait for the browser it opens", "[cli-packages]") {
    // commands-1: the opener runs on its own, with none of fb's output, so a browser that stays
    // open, as one xdg-open starts does, doesn't hold fb up. This one stays 6 seconds.
    const fb::testing::sandbox_t sandbox;
    sandbox.write("browser/browser.fb", "Import std.core\nImport std.time\nSleep(6.Seconds)\n"
                                        "Print(\"the browser closed\")\n");
    std::filesystem::path browser = std::filesystem::path(FB_TEST_OUTPUT) / "slow_browser";
    browser += fb::executable_extension;
    const fb::process_result_t built = fb_run(
        { "build", fb::to_utf8(sandbox / "browser/browser.fb"), "-o", fb::to_utf8(browser) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const fb::testing::registry_server_t server;
    const fb::testing::environment_t credentials("FB_CREDENTIALS_FILE",
                                                 fb::to_utf8(sandbox / "home/credentials.json"));
    const fb::testing::environment_t opener("FB_BROWSER", fb::to_utf8(browser));
    const auto started = std::chrono::steady_clock::now();
    const fb::process_result_t signed_in = fb_run({ "login", "--registry", server.index() });
    const auto took = std::chrono::steady_clock::now() - started;
    INFO(signed_in.output);
    CHECK(signed_in.exit_code == 0);
    CHECK(signed_in.output.contains("Opening " +
                                    server.index().substr(0, server.index().size() - 6)));
    CHECK_FALSE(signed_in.output.contains("the browser closed"));
    CHECK(took < std::chrono::seconds(5));
    // The browser's done before its folder goes.
    std::this_thread::sleep_until(started + std::chrono::seconds(7));
}
