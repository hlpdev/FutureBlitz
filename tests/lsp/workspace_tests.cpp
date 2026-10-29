#include "client.hpp"
#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

/// A library whose modules import each other in a line, base, middle, and top, and one that
/// imports none.
std::vector<std::pair<std::string, std::string>> layers() {
    return {
        { "project.toml", "[project]\nname = \"layers\"\nversion = \"0.1.0\"\n" },
        { "base.fb", "Import std.core\nPublic Function Base(): Int => 1\n" },
        {
            "middle.fb",
            "Import std.core\nImport \"./base\"\nPublic Function Middle(): Int => Base() + 1\n",
        },
        {
            "top.fb",
            "Import std.core\nImport \"./middle\"\nPublic Function Top(): Int => Middle() + 1\n",
        },
        { "alone.fb", "Import std.core\nPublic Function Alone(): Int => 5\n" },
    };
}

/// The files each check since the `from`th checked, by their names, in order.
std::vector<std::set<std::string>> roots_since(client_t& client, const std::size_t from) {
    std::vector<std::set<std::string>> found;
    const std::vector<fb::lsp::check_record_t> history = client.server().workspace().history();
    for (std::size_t i = from; i < history.size(); ++i) {
        std::set<std::string> names;
        for (const std::filesystem::path& root : history[i].roots) {
            names.insert(fb::to_utf8(root.filename()));
        }

        found.push_back(std::move(names));
    }

    return found;
}

std::size_t checks(client_t& client) {
    return client.server().workspace().history().size();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream const in(path, std::ios::binary);
    std::ostringstream read;
    read << in.rdbuf();
    return read.str();
}

} // namespace

TEST_CASE("the workspace's packages are checked whole once, with every file's diagnostics",
          "[lsp]") {
    std::vector<std::pair<std::string, std::string>> files = layers();
    files.emplace_back("broken.fb", "Import std.core\nPublic Function Broken(): Int => \"no\"\n");
    const folder_t folder(files);
    client_t client({ .folders = { folder.root() } });
    client.settle();

    const std::vector<std::set<std::string>> roots = roots_since(client, 0);
    REQUIRE(!roots.empty());
    CHECK(roots.front() ==
          std::set<std::string>{ "alone.fb", "base.fb", "broken.fb", "middle.fb", "top.fb" });
    for (const std::string file : { "alone.fb", "base.fb", "middle.fb", "top.fb" }) {
        const json_t diagnostics = client.diagnostics(folder.uri(file));
        CHECK(diagnostics.empty());
    }

    const json_t broken = client.diagnostics(folder.uri("broken.fb"));
    CHECK(broken.size() == 1);
}

TEST_CASE("an edit checks its file, and then only the files that import it", "[lsp]") {
    const folder_t folder(layers());
    client_t client({ .folders = { folder.root() } });
    client.settle();

    // Nothing imports alone.fb, so its edit checks it alone, with what it imports.
    std::size_t before = checks(client);
    const std::string alone = folder.uri("alone.fb");
    client.open(alone, "Import std.core\nPublic Function Alone(): Int => 5\n");
    client.change(alone, "Import std.core\nPublic Function Alone(): Int => 6\n");
    client.settle();
    for (const std::set<std::string>& roots : roots_since(client, before)) {
        CHECK(roots == std::set<std::string>{ "alone.fb" });
    }

    const std::vector<fb::lsp::check_record_t> history = client.server().workspace().history();
    CHECK(history.back().modules < history.front().modules);

    // middle.fb and top.fb import base.fb, one through the other, but alone.fb doesn't.
    const std::string base = folder.uri("base.fb");
    client.open(base, "Import std.core\nPublic Function Base(): Int => 1\n");
    client.settle();
    before = checks(client);
    client.change(base, "Import std.core\nPublic Function Base(): Int => 2\n");
    client.settle();
    CHECK(roots_since(client, before) ==
          std::vector<std::set<std::string>>{ { "base.fb" }, { "middle.fb", "top.fb" } });

    // top.fb has nothing that imports it.
    before = checks(client);
    const std::string top = folder.uri("top.fb");
    client.open(top, "Import std.core\nImport \"./middle\"\nPublic Function Top(): Int => 3\n");
    client.settle();
    CHECK(roots_since(client, before) == std::vector<std::set<std::string>>{ { "top.fb" } });
}

TEST_CASE("breaking a module shows in the modules that import it, and fixing it clears them",
          "[lsp]") {
    const folder_t folder(layers());
    client_t client({ .folders = { folder.root() } });
    const std::string base = folder.uri("base.fb");
    const std::string middle = folder.uri("middle.fb");
    client.open(base, "Import std.core\nPublic Function Base(): Int => 1\n");
    client.settle();

    // middle.fb isn't open, but it's checked again, as it imports base.fb.
    client.change(base, "Import std.core\nPublic Function Bottom(): Int => 1\n");
    client.settle();
    json_t diagnostics = client.diagnostics(middle);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0]["message"].get<std::string>().contains("Base"));
    CHECK(diagnostics[0]["range"]["start"] == client_t::position(2, 33));
    // top.fb uses Middle, which is still there.
    CHECK(client.diagnostics(folder.uri("top.fb")).empty());

    client.change(base, "Import std.core\nPublic Function Base(): Int => 1\n");
    client.settle();
    diagnostics = client.diagnostics(middle);
    CHECK(diagnostics.empty());

    // Closing a document without saving it checks its text on disk again.
    client.change(base, "Import std.core\nPublic Function Bottom(): Int => 1\n");
    client.settle();
    CHECK(client.diagnostics(middle).size() == 1);
    client.close(base);
    client.settle();
    CHECK(client.diagnostics(middle).empty());
}

TEST_CASE("files that change on disk are checked again, with what imports them", "[lsp]") {
    const folder_t folder(layers());
    client_t client({ .folders = { folder.root() } });
    client.settle();
    const std::string middle = folder.uri("middle.fb");

    folder.write("base.fb", "Import std.core\nPublic Function Bottom(): Int => 1\n");
    client.notify(
        "workspace/didChangeWatchedFiles",
        { { "changes", json_t::array({ { { "uri", folder.uri("base.fb") }, { "type", 2 } } }) } });
    client.settle();
    CHECK(client.diagnostics(middle).size() == 1);

    // A file made, and one deleted.
    folder.write("extra.fb", "Import std.core\nPublic Function Extra(): Int => \"no\"\n");
    std::filesystem::remove(folder.path("base.fb"));
    client.notify("workspace/didChangeWatchedFiles",
                  {
                      {
                          "changes",
                          json_t::array({
                              { { "uri", folder.uri("extra.fb") }, { "type", 1 } },
                              { { "uri", folder.uri("base.fb") }, { "type", 3 } },
                          }),
                      },
                  });
    client.settle();
    CHECK(client.diagnostics(folder.uri("extra.fb")).size() == 1);
    CHECK(client.diagnostics(folder.uri("base.fb")).empty());
    const json_t missing = client.diagnostics(middle);
    REQUIRE(!missing.empty());
    CHECK(missing[0]["range"]["start"]["line"] == 1);
}

TEST_CASE("a package that depends on another is checked again when the other changes", "[lsp]") {
    const folder_t folder({
        { "lib/project.toml", "[project]\nname = \"lib\"\nversion = \"0.1.0\"\n" },
        { "lib/shapes.fb", "Import std.core\nPublic Function Area(): Int => 4\n" },
        {
            "app/project.toml",
            "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
            "[dependencies]\nlib = { path = \"../lib\" }\n",
        },
        { "app/main.fb", "Import std.core\nImport lib.shapes\nPrint(Area())\n" },
    });
    client_t client({ .folders = { folder.root() } });
    const std::string shapes = folder.uri("lib/shapes.fb");
    const std::string main = folder.uri("app/main.fb");
    client.open(shapes, "Import std.core\nPublic Function Area(): Int => 4\n");
    client.settle();
    CHECK(client.diagnostics(main).empty());

    client.change(shapes, "Import std.core\nPublic Function Size(): Int => 4\n");
    client.settle();
    REQUIRE(client.diagnostics(main).size() == 1);

    // Its definition is in the other package's file.
    client.change(shapes, "Import std.core\nPublic Function Area(): Int => 4\n");
    client.open(main, "Import std.core\nImport lib.shapes\nPrint(Area())\n");
    const json_t area = client.request("textDocument/definition", client.at(main, "Area"));
    CHECK(fb::lsp::test::place_of(area) ==
          fb::lsp::test::place_t{ .uri = shapes, .line = 1, .character = 16 });
    client.settle();
    CHECK(client.diagnostics(main).empty());
}

TEST_CASE("a package's main file, another main file, and its tests' files are checked as fb test "
          "has them",
          "[lsp]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"tool\"\nversion = \"0.1.0\"\n" },
        { "main.fb", "Import std.core\nImport \"./shared\"\nLocal n: Int = Common()\nPrint(n)\n" },
        { "other.fb", "Import std.core\nLocal s: String = 5\nPrint(s)\n" },
        { "shared.fb", "Import std.core\nPublic Function Common(): Int => 2\n" },
        {
            "tests/shared_tests.fb",
            "Import std.test\nImport tool.shared\n"
            "@Test Function Twos()\n    Expect(Common() == \"2\")\nEnd Function\n",
        },
    });
    client_t client({ .folders = { folder.root() } });
    client.settle();
    INFO(client.diagnostics(folder.uri("main.fb")).dump());
    INFO(client.diagnostics(folder.uri("shared.fb")).dump());
    CHECK(client.diagnostics(folder.uri("main.fb")).empty());
    CHECK(client.diagnostics(folder.uri("shared.fb")).empty());
    // Another main file's top-level code is checked on its own, as a main file.
    CHECK(client.diagnostics(folder.uri("other.fb")).size() == 1);
    CHECK(client.diagnostics(folder.uri("tests/shared_tests.fb")).size() == 1);
}

TEST_CASE("std's files are checked quickly, with only what they import", "[lsp][std]") {
    const std::filesystem::path fuzzy =
        fb::path_from_utf8(fb::build_config::std_directory) / "text" / "fuzzy.fb";
    const std::string uri = fb::lsp::uri_of(fuzzy);
    const std::string text = read_file(fuzzy);
    REQUIRE(!text.empty());
    client_t client({});
    const auto start = std::chrono::steady_clock::now();
    client.open(uri, text);
    client.settle();
    const auto checked = std::chrono::steady_clock::now() - start;
    CHECK(client.diagnostics(uri).empty());

    // A fraction of std's modules.
    const std::vector<fb::lsp::check_record_t> history = client.server().workspace().history();
    REQUIRE(!history.empty());
    const std::size_t std_files = fb::lsp::package_files(fuzzy.parent_path().parent_path()).size();
    INFO("modules " << history.back().modules << " of " << std_files << ", checked in "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(checked).count()
                    << " ms");
    CHECK(history.back().modules * 4 < std_files);

    // Each request after the check answers from it, without another.
    const std::size_t before = history.size();
    const auto timed = [&](const std::string& method, const json_t& params) {
        const auto asked = std::chrono::steady_clock::now();
        const json_t result = client.request(method, params);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - asked);
        INFO(method << " took " << took.count() << " ms");
        CHECK(took < std::chrono::milliseconds(2000));
        return result;
    };
    CHECK(timed("textDocument/hover", client.at(uri, "Const SCORE_MATCH", 6)).is_object());
    CHECK(!timed("textDocument/completion", client.at(uri, "Const GAP_START"))["items"].empty());
    CHECK(timed("textDocument/semanticTokens/full", { { "textDocument", { { "uri", uri } } } })
              .contains("data"));
    CHECK(
        !timed("textDocument/documentSymbol", { { "textDocument", { { "uri", uri } } } }).empty());
    CHECK(client.server().workspace().history().size() == before);

    // An edit's check, which takes about as long as the first.
    std::string edited = text;
    edited.replace(edited.find("Const SCORE_MATCH = 16"), 22, "Const SCORE_MATCH = 17");
    const auto edit = std::chrono::steady_clock::now();
    client.change(uri, edited);
    CHECK(timed("textDocument/hover", client.at(uri, "Const SCORE_MATCH", 6))["contents"]["value"]
              .get<std::string>()
              .contains("17"));
    const auto rechecked = std::chrono::steady_clock::now() - edit;
    INFO("rechecked in " << std::chrono::duration_cast<std::chrono::milliseconds>(rechecked).count()
                         << " ms");
    CHECK(rechecked < 3 * checked + std::chrono::seconds(1));
}

TEST_CASE("std as a workspace folder: an edit checks only the std files that import it",
          "[lsp][std]") {
    const std::filesystem::path folder = fb::path_from_utf8(fb::build_config::std_directory);
    client_t client({ .folders = { folder } });
    client.settle();
    const std::vector<fb::lsp::check_record_t> loaded = client.server().workspace().history();
    REQUIRE(!loaded.empty());
    const std::size_t std_files = fb::lsp::package_files(folder).size();
    CHECK(loaded.front().roots.size() == std_files);
    // std checks without errors.
    for (const std::string file : { "core/error.fb", "text/fuzzy.fb", "net/http/server.fb" }) {
        const json_t diagnostics =
            client.diagnostics(fb::lsp::uri_of(folder / fb::path_from_utf8(file)));
        CHECK(diagnostics.empty());
    }

    // Nothing in std imports std.uuid.
    const std::filesystem::path uuid = folder / "uuid" / "uuid.fb";
    std::size_t before = checks(client);
    client.open(fb::lsp::uri_of(uuid), read_file(uuid));
    client.change(fb::lsp::uri_of(uuid), read_file(uuid) + "\n");
    client.settle();
    for (const std::set<std::string>& roots : roots_since(client, before)) {
        CHECK(roots == std::set<std::string>{ "uuid.fb" });
    }

    // What imports std.text, the folder fuzzy.fb is in, is checked again after it, but not the
    // rest of std.
    const std::filesystem::path fuzzy = folder / "text" / "fuzzy.fb";
    client.open(fb::lsp::uri_of(fuzzy), read_file(fuzzy));
    client.settle();
    before = checks(client);
    client.change(fb::lsp::uri_of(fuzzy), read_file(fuzzy) + "\n");
    client.settle();
    const std::vector<std::set<std::string>> roots = roots_since(client, before);
    REQUIRE(roots.size() == 2);
    CHECK(roots[0] == std::set<std::string>{ "fuzzy.fb" });
    CHECK(roots[1].contains("args.fb"));
    CHECK(!roots[1].contains("uuid.fb"));
    CHECK(roots[1].size() * 2 < std_files);
}

TEST_CASE("fb-lsp talks JSON-RPC over its stdin and stdout", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("piped.fb");
    std::string input;
    const auto message = [&](const json_t& body) {
        const std::string text = body.dump();
        input += "Content-Length: " + std::to_string(text.size()) + "\r\n\r\n" + text;
    };
    message({
        { "jsonrpc", "2.0" },
        { "id", 1 },
        { "method", "initialize" },
        { "params", { { "capabilities", json_t::object() } } },
    });
    message({ { "jsonrpc", "2.0" }, { "method", "initialized" }, { "params", json_t::object() } });
    message({
        { "jsonrpc", "2.0" },
        { "method", "textDocument/didOpen" },
        {
            "params",
            {
                {
                    "textDocument",
                    {
                        { "uri", uri },
                        { "languageId", "futureblitz" },
                        { "version", 1 },
                        { "text", "Import std.core\nLocal n: Int = \"x\"\nPrint(n)\n" },
                    },
                },
            },
        },
    });
    message({
        { "jsonrpc", "2.0" },
        { "id", 2 },
        { "method", "textDocument/hover" },
        {
            "params",
            {
                { "textDocument", { { "uri", uri } } },
                { "position", client_t::position(2, 6) },
            },
        },
    });
    input += "Content-Length: 5\r\n\r\n{nope";
    message({ { "jsonrpc", "2.0" }, { "id", 3 }, { "method", "shutdown" } });
    message({ { "jsonrpc", "2.0" }, { "method", "exit" } });

    const std::expected<fb::process_result_t, std::string> run = fb::run_process(FB_LSP, {}, input);
    REQUIRE(run);
    CHECK(run->exit_code == 0);
    std::istringstream output(run->output);
    std::vector<json_t> received;
    while (const std::optional<std::string> body = fb::lsp::read_message(output)) {
        received.push_back(json_t::parse(*body));
    }

    const auto response = [&](const int id) -> json_t {
        for (const json_t& each : received) {
            if (each.contains("id") && each["id"] == id) {
                return each;
            }
        }

        return nullptr;
    };
    CHECK(response(1)["result"]["capabilities"]["hoverProvider"] == true);
    CHECK(response(2)["result"]["contents"]["value"].get<std::string>().contains("Local n: Int32"));
    CHECK(response(3).contains("result"));
    const bool unparsed = std::ranges::any_of(received, [](const json_t& each) {
        return each.contains("error") && each["error"]["code"] == -32700;
    });
    CHECK(unparsed);
    const bool published = std::ranges::any_of(received, [](const json_t& each) {
        return each.value("method", "") == "textDocument/publishDiagnostics" &&
               each["params"]["diagnostics"].size() == 1;
    });
    CHECK(published);
}
