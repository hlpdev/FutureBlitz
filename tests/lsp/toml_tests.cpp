#include "../compiler/package_helpers.hpp"
#include "../compiler/registry_server.hpp"
#include "client.hpp"
#include "fb/driver.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/registry.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

constexpr std::string_view game_manifest = "[project]\nname = \"game\"\nversion = \"0.1.0\"\n";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

/// A completion's items: a list's, or the array they are.
json_t items_of(const json_t& completion) {
    return completion.is_array() ? completion : completion.at("items");
}

/// The labels of a completion's items, in order.
std::vector<std::string> labels(const json_t& items) {
    std::vector<std::string> out;
    for (const json_t& item : items_of(items)) {
        out.push_back(item.at("label").get<std::string>());
    }

    return out;
}

/// What a completion's item named `label` puts in the document.
std::string inserted(const json_t& items, const std::string& label) {
    for (const json_t& item : items_of(items)) {
        if (item.at("label") == label) {
            return item.at("textEdit").at("newText").get<std::string>();
        }
    }

    FAIL("no item " << label);
    return "";
}

/// What `ask` gives once it gives something, as a request that waits on a registry fb-lsp is
/// asking does, trying for ten seconds at most.
template <typename Ask>
std::string eventually(const Ask& ask) {
    for (int tries = 0; tries < 500; ++tries) {
        if (std::string answer = ask(); !answer.empty()) {
            return answer;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return {};
}

/// The messages of the diagnostics published, in order.
std::vector<std::string> messages(const json_t& diagnostics) {
    std::vector<std::string> out;
    for (const json_t& diagnostic : diagnostics) {
        out.push_back(diagnostic.at("message").get<std::string>());
    }

    return out;
}

/// `text` with a code action's edits to it made, last first.
std::string applied(std::string text, const json_t& action, const std::string& uri) {
    const fb::source_file_t file(fb::file_id_t{}, "project.toml", text);
    const fb::lsp::positions_t positions;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
    std::vector<std::string> replacements;
    for (const json_t& edit : action.at("edit").at("changes").at(uri)) {
        spans.emplace_back(positions.offset(file, edit.at("range").at("start")),
                           positions.offset(file, edit.at("range").at("end")));
        replacements.push_back(edit.at("newText").get<std::string>());
    }

    for (std::size_t i = spans.size(); i-- > 0;) {
        text.replace(spans[i].first, spans[i].second - spans[i].first, replacements[i]);
    }

    return text;
}

/// A version of a package for the test registry's index.
fb::index_entry_t version_of(const std::string& name, const std::string& version,
                             const bool yanked = false) {
    return {
        .name = name,
        .version = version,
        .source = { .sha256 = std::string(64, 'a'), .size = 1 },
        .yanked = yanked,
    };
}

} // namespace

TEST_CASE("fb-lsp checks a project.toml as it's typed", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, game_manifest);
    CHECK(client.published(uri).empty());

    // TOML that isn't, a version that isn't semver, and a path dependency that isn't there.
    client.change(uri, "[project\n");
    CHECK(messages(client.published(uri)).at(0).starts_with("project.toml isn't valid TOML"));
    client.change(uri, "[project]\nname = \"game\"\nversion = \"one\"\n");
    const json_t version = client.published(uri);
    REQUIRE(version.size() == 1);
    CHECK(version[0].at("range").at("start").at("line") == 2);
    client.change(uri, std::string(game_manifest) +
                           "\n[dependencies]\nshapes = { path = \"../shapes\" }\n");
    const json_t missing = client.published(uri);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].at("message").get<std::string>().contains("which shapes names"));
    CHECK(missing[0].at("range").at("start").at("line") == 5);

    // The file on disk is as it was, and closing the document gives its diagnostics again.
    CHECK(read_file(folder.path("project.toml")) == game_manifest);
    client.close(uri);
    client.settle();
    INFO(client.diagnostics(uri).dump());
    CHECK(client.diagnostics(uri).empty());

    // A file of another kind, opened and closed, isn't checked as FutureBlitz code, as the
    // workspace once checked a closed file in its folder.
    folder.write("notes.md", "# Notes\n");
    client.open(folder.uri("notes.md"), "# Notes\n");
    client.close(folder.uri("notes.md"));
    client.settle();
    CHECK(client.publications(folder.uri("notes.md")) == 0);
}

// The package's checks read project.toml from disk, so while it's open, what's published for it
// is its text's, as the editor has it.
TEST_CASE("an open project.toml's diagnostics are its text's, not its package's checks'",
          "[lsp][toml]") {
    const folder_t folder({
        { "project.toml", std::string(game_manifest) + "colour = \"red\"\n" },
        { "main.fb", "Import std.core\n\nPrint(\"hi\")\n" },
    });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    client.settle();
    CHECK(messages(client.diagnostics(uri)) ==
          std::vector<std::string>{ "[project] doesn't use 'colour'" });

    client.open(uri, game_manifest);
    CHECK(client.published(uri).empty());
    client.open(folder.uri("main.fb"), "Import std.core\n\nPrint(\"there\")\n");
    CHECK(client.published(folder.uri("main.fb")).empty());
    client.settle();
    CHECK(client.diagnostics(uri).empty());
}

// The step's done-when: every error fb gives for a project.toml shows in the editor, as it's
// typed, the same as fb check gives once it's saved.
TEST_CASE("every project.toml error fb has shows as it's typed", "[lsp][toml]") {
    const std::string project = std::string(game_manifest);
    const std::vector<std::pair<std::string, std::string>> cases{
        { "not TOML", "[project\nname = \"game\"\n" },
        { "no [project]", "[format]\nwidth = 100\n" },
        { "no name", "[project]\nversion = \"0.1.0\"\n" },
        { "a name that isn't one", "[project]\nname = \"std\"\nversion = \"0.1.0\"\n" },
        { "a version that isn't semver", "[project]\nname = \"game\"\nversion = \"1.0\"\n" },
        { "a type there isn't", project + "type = \"app\"\n" },
        { "a main file that isn't there", project + "main = \"nope.fb\"\n" },
        { "a license that isn't SPDX", project + "license = \"MIT OR\"\n" },
        { "a repository that isn't https", project + "repository = \"github.com/me/game\"\n" },
        {
            "too many keywords",
            project + "keywords = [\"a\", \"b\", \"c\", \"d\", \"e\", \"f\"]\n",
        },
        { "a range of fb that isn't one", project + "fb = \"newest\"\n" },
        { "a key it doesn't use", project + "colour = \"red\"\n" },
        { "a table it doesn't use", project + "\n[lints]\nunused = \"warn\"\n" },
        { "a width out of range", project + "\n[format]\nwidth = 5\n" },
        { "a build step that isn't a .fb file", project + "\n[build]\nprogram = \"gen.txt\"\n" },
        {
            "a build step of no dependency",
            project + "\n[build]\nprogram = \"gen.fb\"\npackage = \"tools\"\n",
        },
        {
            "a dependency's folder that isn't there",
            project + "\n[dependencies]\nshapes = { path = \"../nowhere\" }\n",
        },
        {
            "a dependency named otherwise",
            project + "\n[dependencies]\nshapes = { path = \"shapes\" }\n",
        },
        {
            "a local version out of its range",
            project + "\n[dependencies]\nlines = { path = \"lines\", version = \"2.0\" }\n",
        },
        {
            "a registry's package without fb.lock",
            project + "\n[dependencies]\nphysics = \"1.2\"\n",
        },
        {
            "a registry it doesn't name",
            project + "\n[dependencies]\nphysics = { version = \"1.2\", registry = \"company\" }\n",
        },
        {
            "a registry's index that isn't one",
            project + "\n[registries]\ncompany = { index = \"ftp://x/\" }\n",
        },
        { "a dependency on itself", project + "\n[dependencies]\ngame = { path = \".\" }\n" },
        { "a native library that isn't there", project + "\n[native]\nzstd = \"libs/zstd\"\n" },
        {
            "a native library's kind there isn't",
            project + "\n[native]\nzstd = { shared = \"libs/zstd\" }\n",
        },
    };
    const folder_t folder({
        { "project.toml", std::string(game_manifest) },
        { "main.fb", "Import std.core\n" },
        { "shapes/project.toml", "[project]\nname = \"geometry\"\nversion = \"0.1.0\"\n" },
        { "lines/project.toml", "[project]\nname = \"lines\"\nversion = \"1.0.0\"\n" },
    });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, game_manifest);
    CHECK(client.published(uri).empty());
    for (const auto& [name, text] : cases) {
        INFO(name);
        client.change(uri, text);
        const std::vector<std::string> shown = messages(client.published(uri));
        CHECK_FALSE(shown.empty());

        // fb check's, of the same text saved.
        folder.write("project.toml", text);
        std::vector<std::string> checked;
        const std::expected<fb::compilation_t, std::string> compiled =
            fb::compile_file(folder.root());
        REQUIRE(compiled);
        for (const fb::diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
            if (compiled->disk_path(diagnostic.span.file).filename() == "project.toml" &&
                fb::lsp::file_key(compiled->disk_path(diagnostic.span.file)) ==
                    fb::lsp::file_key(folder.path("project.toml"))) {
                checked.push_back(diagnostic.message);
            }
        }

        REQUIRE(checked.size() == shown.size());
        for (std::size_t i = 0; i < checked.size(); ++i) {
            CHECK(shown[i].starts_with(checked[i]));
        }

        folder.write("project.toml", std::string(game_manifest));
    }
}

TEST_CASE("fb-lsp completes project.toml's tables, keys, and values", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    const auto complete = [&](const std::string& text, const std::string& at,
                              const std::uint32_t offset) {
        client.change(uri, text);
        return client.request("textDocument/completion", client.at(uri, at, offset));
    };
    client.open(uri, game_manifest);

    // A header's tables, with the `]` when it isn't typed yet.
    const json_t tables = complete(std::string(game_manifest) + "\n[d", "[d", 2);
    CHECK(labels(tables) == std::vector<std::string>{ "project", "dependencies", "native", "format",
                                                      "build", "registries" });
    CHECK(inserted(tables, "dependencies") == "dependencies]");
    CHECK(inserted(complete(std::string(game_manifest) + "\n[d]", "[d]", 2), "dependencies") ==
          "dependencies");

    // [project]'s keys but those it has, each with what it is.
    const json_t keys = complete("[project]\nname = \"game\"\nver\n", "ver", 3);
    const std::vector<std::string> project = labels(keys);
    CHECK_FALSE(std::ranges::contains(project, "name"));
    CHECK(std::ranges::contains(project, "version"));
    CHECK(std::ranges::contains(project, "license"));
    CHECK(inserted(keys, "version") == "version = ");
    CHECK(items_of(keys)[0].at("documentation").at("value").get<std::string>().starts_with('`'));

    // A key's values, when they're few, quoted or in the string typed.
    CHECK(labels(complete(std::string(game_manifest) + "type = ", "type = ", 7)) ==
          std::vector<std::string>{ "executable", "library" });
    CHECK(inserted(complete(std::string(game_manifest) + "type = ", "type = ", 7), "library") ==
          "\"library\"");
    CHECK(inserted(complete(std::string(game_manifest) + "type = \"li\"", "\"li", 3), "library") ==
          "library");

    // A dependency's own table's keys, and its link's and registry's values.
    const std::string dependencies =
        std::string(game_manifest) +
        "\n[registries]\ncompany = { index = \"https://x.example/\" }\n\n[dependencies]\n";
    CHECK(labels(complete(dependencies + "physics = { ve }", "{ ve", 4)) ==
          std::vector<std::string>{ "version", "path", "registry", "link" });
    CHECK(labels(complete(dependencies + "physics = { version = \"1.2\", li }", ", li", 4)) ==
          std::vector<std::string>{ "path", "registry", "link" });
    CHECK(labels(complete(dependencies + "physics = { link = \"\" }", "link = \"", 8)) ==
          std::vector<std::string>{ "embed", "beside" });
    CHECK(labels(complete(dependencies + "physics = { registry = \"\" }", "registry = \"", 12)) ==
          std::vector<std::string>{ "company", "futureblitz" });
    CHECK(labels(complete(dependencies + "\n[dependencies.physics]\nre", "\nre", 3)) ==
          std::vector<std::string>{ "version", "path", "registry", "link" });
    CHECK(labels(complete(std::string(game_manifest) + "\n[native]\nzstd = { s }", "{ s", 3)) ==
          std::vector<std::string>{ "static", "system", "dynamic" });

    // Nothing in a comment, and [build]'s package is one of the dependencies.
    CHECK(labels(complete(std::string(game_manifest) + "# ver", "# ver", 5)).empty());
    CHECK(labels(complete(
              dependencies +
                  "tools = { path = \"tools\" }\n\n[build]\nprogram = \"g.fb\"\npackage = \"\"\n",
              "package = \"", 11)) == std::vector<std::string>{ "tools" });
}

// The step's done-when: completing a dependency's name and version against a test registry.
TEST_CASE("fb-lsp completes a dependency's name and version from its registry", "[lsp][toml]") {
    const fb::testing::sandbox_t sandbox;
    fb::testing::registry_server_t registry;
    registry.add(version_of("physics", "1.2.0"));
    registry.add(version_of("physics", "1.4.0", true));
    registry.add(version_of("physics", "1.3.0"));
    registry.add(version_of("physics2d", "0.1.0"));
    registry.add(version_of("audio", "2.0.0"));
    const std::string head =
        std::format("{}\n[registries]\nfutureblitz = {{ index = \"{}\" }}\n\n[dependencies]\n",
                    game_manifest, registry.index());
    const folder_t folder({ { "project.toml", head }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, head);
    // The registry's asked on a thread of fb-lsp's own, and a completion that waits for it is
    // incomplete, so the client asks again until it isn't.
    const auto complete = [&](const std::string& text, const std::string& at,
                              const std::uint32_t offset) {
        client.change(uri, text);
        json_t answer;
        for (int tries = 0; tries < 500; ++tries) {
            answer = client.request("textDocument/completion", client.at(uri, at, offset));
            if (!answer.at("isIncomplete").get<bool>()) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        CHECK_FALSE(answer.at("isIncomplete").get<bool>());
        return answer.at("items");
    };

    // A name, from the registry's search, with its newest version that isn't yanked.
    const json_t names = complete(head + "phys", "\nphys", 5);
    CHECK(labels(names) == std::vector<std::string>{ "physics", "physics2d" });
    CHECK(inserted(names, "physics") == "physics = \"1.3.0\"");
    CHECK(names[0].at("documentation") == "A test package");
    CHECK(inserted(complete(head + "\"phys\"", "\"phys", 5), "physics") == "physics");

    // A version, from its index, newest first, without the yanked one.
    const json_t versions = complete(head + "physics = \"\"", "physics = \"", 11);
    CHECK(labels(versions) == std::vector<std::string>{ "1.3.0", "1.2.0" });
    CHECK(versions[0].at("preselect") == true);
    CHECK(labels(complete(head + "physics = { version = \"1\" }", "version = \"1", 12)) ==
          std::vector<std::string>{ "1.3.0", "1.2.0" });

    // A name with "registry" in it isn't the key, and a dependency's own table's registry is
    // the one its versions are from.
    registry.add(version_of("registry-tools", "0.5.0"));
    CHECK(labels(complete(head + "registry-tools = \"\"", "tools = \"", 9)) ==
          std::vector<std::string>{ "0.5.0" });
    fb::testing::registry_server_t company;
    company.add(version_of("physics", "9.0.0"));
    const std::string two =
        std::format("{}\n[registries]\nfutureblitz = {{ index = \"{}\" }}\ncompany = {{ index = "
                    "\"{}\" }}\n\n[dependencies.physics]\nregistry = \"company\"\nversion = \"\"\n",
                    game_manifest, registry.index(), company.index());
    CHECK(labels(complete(two, "version = \"\"", 11)) == std::vector<std::string>{ "9.0.0" });

    // Hover has its description and newest version.
    client.change(uri, head + "physics = \"1.2\"\n");
    const std::string hovered = eventually([&] {
        const json_t found = client.request("textDocument/hover", client.at(uri, "physics =", 2));
        // Its newest version can come before its description, which a search gives.
        const std::string shown =
            found.is_null() ? std::string() : found.at("contents").at("value").get<std::string>();
        return shown.contains("A test package") ? shown : std::string();
    });
    CHECK(hovered.starts_with("**physics** 1.3.0\n\nA test package"));

    // A range that doesn't take the newest version updates to it, once the registry says.
    registry.add(version_of("physics", "2.0.0"));
    client_t fresh({ .folders = { folder.root() } });
    fresh.open(uri, head + "physics = \"1.2\"\n");
    json_t actions;
    (void)eventually([&] {
        actions = fresh.request(
            "textDocument/codeAction",
            {
                { "textDocument", { { "uri", uri } } },
                {
                    "range",
                    { { "start", client_t::position(8, 2) }, { "end", client_t::position(8, 2) } },
                },
                { "context", { { "diagnostics", json_t::array() } } },
            });
        return actions.empty() ? std::string() : actions.dump();
    });
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].at("title") == "Update physics to 2.0.0");
    CHECK(applied(head + "physics = \"1.2\"\n", actions[0], uri) == head + "physics = \"2.0.0\"\n");

    // Offline, the names and versions are the cache's, which the registry's answers filled.
    client_t offline({ .folders = { folder.root() }, .offline = true });
    offline.open(uri, head + "phys");
    CHECK(labels(offline.request("textDocument/completion", offline.at(uri, "\nphys", 5))) ==
          std::vector<std::string>{ "physics" });
    offline.change(uri, head + "physics = \"\"");
    CHECK(labels(offline.request("textDocument/completion", offline.at(uri, "physics = \"", 11))) ==
          std::vector<std::string>{ "2.0.0", "1.3.0", "1.2.0" });
}

TEST_CASE("fb-lsp's quick fix adds what fb publish needs to [project]", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    const auto actions = [&](const std::string& text, const std::uint32_t line) {
        client.change(uri, text);
        return client.request("textDocument/codeAction",
                              {
                                  { "textDocument", { { "uri", uri } } },
                                  {
                                      "range",
                                      {
                                          { "start", client_t::position(line, 0) },
                                          { "end", client_t::position(line, 0) },
                                      },
                                  },
                                  { "context", { { "diagnostics", json_t::array() } } },
                              });
    };
    client.open(uri, game_manifest);

    // An executable, as it has a main file, without a description or a license.
    const json_t added = actions(std::string(game_manifest), 1);
    REQUIRE(added.size() == 1);
    CHECK(added[0].at("title") == "Add what fb publish needs: type, description and license");
    const std::string publishable = applied(std::string(game_manifest), added[0], uri);
    CHECK(publishable == std::string(game_manifest).substr(0, game_manifest.size() - 1) +
                             "\ntype = \"library\"\ndescription = \"The game package\"\nlicense = "
                             "\"MIT\"\n");

    // From the review: what it adds is what fb check and fb publish take, which empty strings
    // weren't.
    folder.write("project.toml", publishable);
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_file(folder.root());
    REQUIRE(compiled);
    // Errors only, as inspections can warn of the package's code.
    CHECK(
        std::ranges::none_of(compiled->diagnostics.diagnostics(), [](const fb::diagnostic_t& each) {
            return each.severity == fb::severity_t::ERROR;
        }));
    folder.write("project.toml", std::string(game_manifest));

    // A multi-line value's last line is where the table's last key ends, and a document's line
    // breaks are what's put in it.
    const std::string multiline = "[project]\nname = \"game\"\nversion = \"0.1.0\"\ndescription = "
                                  "\"\"\"\nA game\nof squares\n\"\"\"\n\n[format]\nwidth = 90\n";
    const json_t after_string = actions(multiline, 0);
    REQUIRE(after_string.size() == 1);
    CHECK(applied(multiline, after_string[0], uri) ==
          "[project]\nname = \"game\"\nversion = \"0.1.0\"\ndescription = \"\"\"\nA game\nof "
          "squares\n\"\"\"\ntype = \"library\"\nlicense = \"MIT\"\n\n[format]\nwidth = 90\n");
    const std::string crlf = "[project]\r\nname = \"game\"\r\nversion = \"0.1.0\"\r\n";
    const json_t crlf_actions = actions(crlf, 0);
    REQUIRE(crlf_actions.size() == 1);
    CHECK(applied(crlf, crlf_actions[0], uri) ==
          "[project]\r\nname = \"game\"\r\nversion = \"0.1.0\"\r\ntype = \"library\"\r\n"
          "description = \"The game package\"\r\nlicense = \"MIT\"\r\n");

    // A written type is changed, and what's there stays.
    const std::string typed = "[project]\nname = \"game\"\ntype = \"executable\"\nversion = "
                              "\"0.1.0\"\nlicense = \"MIT\"\n\n[format]\nwidth = 90\n";
    const json_t changed = actions(typed, 0);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].at("title") == "Add what fb publish needs: type and description");
    CHECK(applied(typed, changed[0], uri) ==
          "[project]\nname = \"game\"\ntype = \"library\"\nversion = \"0.1.0\"\nlicense = "
          "\"MIT\"\ndescription = \"The game package\"\n\n[format]\nwidth = 90\n");

    // Nothing outside [project], or for a library that has them.
    CHECK(actions(typed, 7).empty());
    CHECK(actions("[project]\nname = \"game\"\nversion = \"0.1.0\"\ntype = "
                  "\"library\"\ndescription = \"A game\"\nlicense = \"MIT\"\n",
                  1)
              .empty());
}

TEST_CASE("fb-lsp's hover explains project.toml's tables and keys", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    const std::string text =
        std::string(game_manifest) +
        "\n[format]\nwidth = 90\n\n[native]\nzstd = { static = \"libs/zstd\" }\n";
    client.open(uri, text);
    const auto hovered = [&](const std::string& at, const std::uint32_t offset) {
        const json_t found = client.request("textDocument/hover", client.at(uri, at, offset));
        return found.is_null() ? std::string()
                               : found.at("contents").at("value").get<std::string>();
    };
    CHECK(hovered("[project]", 2).starts_with("`[project]`: The package"));
    CHECK(hovered("version", 1).starts_with("`version`: Its version, semver"));
    CHECK(hovered("width", 1).starts_with("`width`: The line width"));
    CHECK(hovered("static", 1).starts_with("`static`: A static archive"));
    // A native library's name and a value are the package's own.
    CHECK(hovered("zstd", 1).empty());
    CHECK(hovered("\"game\"", 2).empty());
}

// Its other requests, which an editor sends a project.toml as it does a .fb file, get nothing,
// not answers that read it as FutureBlitz code.
TEST_CASE("fb-lsp doesn't read a project.toml as FutureBlitz code", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    const json_t document{ { "textDocument", { { "uri", uri } } } };
    client.open(uri, game_manifest);
    CHECK(client.request("textDocument/semanticTokens/full", document).is_null());
    CHECK(client.request("textDocument/documentSymbol", document).is_null());
    CHECK(client
              .request("textDocument/formatting",
                       { { "textDocument", { { "uri", uri } } },
                         { "options", { { "tabSize", 4 }, { "insertSpaces", true } } } })
              .is_null());
    CHECK(client.request("textDocument/definition", client.at(uri, "name", 1)).is_null());
    client.close(uri);
    CHECK(client
              .request("textDocument/hover", { { "textDocument", { { "uri", uri } } },
                                               { "position", client_t::position(1, 1) } })
              .is_null());
}

// From the review: the quick fix's values are placeholders the user types over where the client
// takes snippet edits.
TEST_CASE("fb-lsp's publish fix is a snippet where the client takes one", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() }, .snippet_edits = true });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, game_manifest);
    const json_t actions = client.request(
        "textDocument/codeAction",
        {
            { "textDocument", { { "uri", uri } } },
            {
                "range",
                { { "start", client_t::position(0, 0) }, { "end", client_t::position(0, 0) } },
            },
            { "context", { { "diagnostics", json_t::array() } } },
        });
    REQUIRE(actions.size() == 1);
    const json_t& changes = actions[0].at("edit").at("documentChanges");
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].at("textDocument").at("uri") == uri);
    const json_t& edits = changes[0].at("edits");
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].at("snippet").at("kind") == "snippet");
    CHECK(edits[0].at("snippet").at("value") == "\ntype = \"library\"\ndescription = \"${1:What "
                                                "the package does}\"\nlicense = \"${2:MIT}\"");
}

// From the review: a comment's words and a string's searched the registry and showed keys' docs.
TEST_CASE("fb-lsp's hover in a project.toml's comment or string is nothing", "[lsp][toml]") {
    const folder_t folder(
        { { "project.toml", std::string(game_manifest) }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() }, .offline = true });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, std::string(game_manifest) +
                         "# add the name later\n\n[dependencies]\n# add physics later\nshapes = { "
                         "path = \"../version\" }\n");
    for (const auto& [at, offset] : {
             std::pair{ "# add the name", 6U },
             std::pair{ "# add physics", 2U },
             std::pair{ "# add physics", 6U },
             std::pair{ "../version", 4U },
         }) {
        INFO(at);
        CHECK(client.request("textDocument/hover", client.at(uri, at, offset)).is_null());
    }
}

// From the review: a registry that's slow, or never answers, held up fb-lsp's every request.
TEST_CASE("fb-lsp answers while a registry it asks never answers", "[lsp][toml]") {
    const fb::testing::sandbox_t sandbox;
    std::mutex mutex;
    std::condition_variable released;
    bool done = false;
    const fb::testing::http_server_t stalled([&](const fb::http_request_t&) {
        std::unique_lock lock(mutex);
        released.wait(lock, [&] { return done; });
        return fb::http_response_t{ .status = 503 };
    });
    const std::string head = std::format(
        "{}\n[registries]\nfutureblitz = {{ index = \"{}index/\" }}\n\n[dependencies]\n",
        game_manifest, stalled.url());
    const folder_t folder({ { "project.toml", head }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    // After fb-lsp, so it goes first and frees fb-lsp's fetching thread, which the client's end
    // waits for.
    const std::unique_ptr<void, std::function<void(void*)>> release(&done, [&](void*) {
        {
            const std::scoped_lock lock(mutex);
            done = true;
        }

        released.notify_all();
    });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, head + "physics = \"\"\nphys");
    const auto start = std::chrono::steady_clock::now();
    const json_t versions =
        client.request("textDocument/completion", client.at(uri, "physics = \"", 11));
    CHECK(versions.at("isIncomplete") == true);
    CHECK(versions.at("items").empty());
    CHECK(
        client.request("textDocument/completion", client.at(uri, "\nphys", 5)).at("isIncomplete") ==
        true);
    CHECK(client.request("textDocument/hover", client.at(uri, "physics", 2)).is_null());
    CHECK(client.request("textDocument/hover", client.at(uri, "version", 1))
              .at("contents")
              .at("value")
              .get<std::string>()
              .starts_with("`version`"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

// From the recheck: a quoted key is a dependency's name, which hover shows, and three quotes in a
// comment open no string.
TEST_CASE("fb-lsp's hover on a quoted name, and a comment's quotes", "[lsp][toml]") {
    const fb::testing::sandbox_t sandbox;
    fb::testing::registry_server_t registry;
    registry.add(version_of("physics", "1.0.0"));
    const std::string head = std::format(
        "{}# write \"\"\" for long text\n\n[registries]\nfutureblitz = {{ index = \"{}\" }}\n\n"
        "[dependencies]\n\"physics\" = \"1.0\"\n\n[format]\nwid\n",
        game_manifest, registry.index());
    const folder_t folder({ { "project.toml", head }, { "main.fb", "Import std.core\n" } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, head);
    const std::string hovered = eventually([&] {
        const json_t found = client.request("textDocument/hover", client.at(uri, "\"physics\"", 2));
        return found.is_null() ? std::string()
                               : found.at("contents").at("value").get<std::string>();
    });
    CHECK(hovered.starts_with("**physics** 1.0.0"));
    CHECK(labels(client.request("textDocument/completion", client.at(uri, "\nwid", 4))) ==
          std::vector<std::string>{ "width" });
}

// From the recheck: a registry that couldn't be reached once was never asked again for versions.
TEST_CASE("fb-lsp asks a registry again that couldn't be reached", "[lsp][toml]") {
    const fb::testing::sandbox_t sandbox;
    fb::testing::registry_server_t registry;
    registry.add(version_of("physics", "1.0.0"));
    std::atomic<bool> down = true;
    const std::string head = std::format(
        "{}\n[registries]\nfutureblitz = {{ index = \"{}\" }}\n\n[dependencies]\nphysics = \"\"\n",
        game_manifest, registry.index());
    const folder_t folder({ { "project.toml", head }, { "main.fb", "Import std.core\n" } });
    client_t client({
        .folders = { folder.root() },
        .http = [&](const fb::http_request_t& request)
            -> std::expected<fb::http_response_t, std::string> {
            if (down) {
                return std::unexpected("the registry is down");
            }

            return fb::send_http(request);
        },
        .registry_life = std::chrono::seconds(1),
    });
    const std::string uri = folder.uri("project.toml");
    client.open(uri, head);
    // Once the registry's answer, or its failure, is in.
    const auto versions = [&] {
        json_t answer;
        for (int tries = 0; tries < 500; ++tries) {
            answer = client.request("textDocument/completion", client.at(uri, "physics = \"", 11));
            if (!answer.at("isIncomplete").get<bool>()) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        return labels(answer);
    };
    CHECK(versions().empty());
    down = false;
    const std::string asked_again = eventually([&] {
        return versions() == std::vector<std::string>{ "1.0.0" } ? std::string("1.0.0")
                                                                 : std::string();
    });
    CHECK(asked_again == "1.0.0");
}
