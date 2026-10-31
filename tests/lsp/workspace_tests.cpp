#include "client.hpp"
#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/interface.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
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

/// The messages of a file's diagnostics.
std::vector<std::string> messages_of(const json_t& diagnostics) {
    std::vector<std::string> found;
    for (const json_t& diagnostic : diagnostics) {
        found.push_back(diagnostic["message"].get<std::string>());
    }

    return found;
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
    // Whether or not the checks share what they know of the disk, which the client's watching
    // lets them.
    const bool watches = GENERATE(false, true);
    const folder_t folder(layers());
    client_t client({ .folders = { folder.root() }, .watches = watches });
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

TEST_CASE("after edits, each file's diagnostics are those a check from nothing gives", "[lsp]") {
    const folder_t folder(layers());
    client_t client({ .folders = { folder.root() }, .watches = true });
    client.settle();
    const std::vector<std::string> files{ "base.fb", "middle.fb", "top.fb", "alone.fb" };
    std::map<std::string, std::string> open;
    // Each file's diagnostics, and those of a client that starts now with the same documents.
    const auto same_as_new = [&] {
        client.settle();
        client_t fresh({ .folders = { folder.root() }, .watches = true });
        for (const auto& [file, text] : open) {
            fresh.open(folder.uri(file), text);
        }

        fresh.settle();
        for (const std::string& file : files) {
            INFO(file);
            CHECK(messages_of(client.diagnostics(folder.uri(file))) ==
                  messages_of(fresh.diagnostics(folder.uri(file))));
        }
    };
    const auto edit = [&](const std::string& file, const std::string& text) {
        if (!open.contains(file)) {
            client.open(folder.uri(file), read_file(folder.path(file)));
        }

        open[file] = text;
        client.change(folder.uri(file), text);
    };

    // A body, a declaration that what imports it sees, and an Import.
    edit("base.fb",
         "Import std.core\nPublic Function Base(): Int\n    Return \"no\"\nEnd Function\n");
    same_as_new();
    edit("base.fb", "Import std.core\nPublic Function Base(): String => \"1\"\n");
    same_as_new();
    REQUIRE(!client.diagnostics(folder.uri("middle.fb")).empty());
    edit("top.fb", "Import std.core\nPublic Function Top(): Int => Middle() + 1\n");
    same_as_new();
    REQUIRE(!client.diagnostics(folder.uri("top.fb")).empty());
}

TEST_CASE("a folder deleted on disk takes its files, and what imports them sees it", "[lsp]") {
    std::vector<std::pair<std::string, std::string>> files = layers();
    files.emplace_back("parts/wheel.fb", "Import std.core\nPublic Function Wheel(): Int => 4\n");
    files.emplace_back("car.fb", "Import std.core\nImport \"./parts/wheel\"\n"
                                 "Public Function Car(): Int => Wheel()\n");
    const folder_t folder(files);
    client_t client({ .folders = { folder.root() }, .watches = true });
    client.settle();
    REQUIRE(client.diagnostics(folder.uri("car.fb")).empty());

    // The client says only that the folder went.
    std::filesystem::remove_all(folder.path("parts"));
    client.notify(
        "workspace/didChangeWatchedFiles",
        { { "changes", json_t::array({ { { "uri", folder.uri("parts") }, { "type", 3 } } }) } });
    client.settle();
    const json_t missing = client.diagnostics(folder.uri("car.fb"));
    REQUIRE(missing.size() == 1);
    CHECK(missing[0]["range"]["start"]["line"] == 1);
    CHECK(client.diagnostics(folder.uri("parts/wheel.fb")).empty());
}

TEST_CASE("the checks share what they know of the disk only while the client watches it", "[lsp]") {
    const bool watches = GENERATE(false, true);
    const folder_t folder({
        { "project.toml", "[project]\nname = \"shared\"\nversion = \"0.1.0\"\n" },
        { "main.fb", "Import std.core\nImport \"./extra\"\nPrint(Extra())\n" },
    });
    client_t client({ .folders = { folder.root() }, .watches = watches });
    const std::string main = folder.uri("main.fb");
    client.open(main, read_file(folder.path("main.fb")));
    client.settle();
    REQUIRE(!client.diagnostics(main).empty());

    // A file made without a word from the client: a check that asks the disk itself finds it,
    // and one that shares what the checks before it found doesn't, until the client says.
    folder.write("extra.fb", "Import std.core\nPublic Function Extra(): Int => 1\n");
    client.change(main, "Import std.core\nImport \"./extra\"\nPrint(Extra())\n\n");
    client.settle();
    CHECK(client.diagnostics(main).empty() == !watches);

    client.notify(
        "workspace/didChangeWatchedFiles",
        { { "changes", json_t::array({ { { "uri", folder.uri("extra.fb") }, { "type", 1 } } }) } });
    client.change(main, "Import std.core\nImport \"./extra\"\nPrint(Extra())\n");
    client.settle();
    CHECK(client.diagnostics(main).empty());
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

namespace {

/// The messages of the diagnostics published last for `uri`, or none when none were.
std::vector<std::string> last_messages(const client_t& client, const std::string& uri) {
    std::vector<std::string> found;
    for (const json_t& message : client.notifications()) {
        if (message.value("method", "") == "textDocument/publishDiagnostics" &&
            message["params"]["uri"] == uri) {
            found = messages_of(message["params"]["diagnostics"]);
        }
    }

    return found;
}

/// Whether each of `uris` has the diagnostics in `client` that a client starting now gives, with
/// `folders` and `main` open with `text`.
bool as_fresh(const client_t& client, const std::vector<std::filesystem::path>& folders,
              const std::string& main, const std::string& text,
              const std::vector<std::string>& uris) {
    client_t fresh({ .folders = folders, .watches = true });
    fresh.open(main, text);
    fresh.settle();
    return std::ranges::all_of(uris, [&](const std::string& uri) {
        return last_messages(client, uri) == last_messages(fresh, uri);
    });
}

/// Says to the server that the client saw `uri` made (1), changed (2), or deleted (3).
void saw(client_t& client, const std::string& uri, const int type) {
    client.notify("workspace/didChangeWatchedFiles",
                  { { "changes", json_t::array({ { { "uri", uri }, { "type", type } } }) } });
}

/// The watchers of the registrations the server asked the client for.
std::vector<json_t> watchers_asked(const client_t& client) {
    std::vector<json_t> found;
    for (const json_t& message : client.notifications()) {
        if (message.value("method", "") == "client/registerCapability") {
            found.push_back(message["params"]["registrations"][0]["registerOptions"]["watchers"]);
        }
    }

    return found;
}

} // namespace

TEST_CASE("the client is asked to watch every file, and trusted to once it says it does", "[lsp]") {
    const bool answers = GENERATE(false, true);
    const folder_t folder({
        { "project.toml", "[project]\nname = \"asked\"\nversion = \"0.1.0\"\n" },
        { "main.fb", "Import std.core\nImport \"./extra\"\nPrint(Extra())\n" },
    });
    client_t client({ .folders = { folder.root() }, .watches = true, .answers = answers });
    const std::string main = folder.uri("main.fb");
    const std::string text = read_file(folder.path("main.fb"));
    client.open(main, text);
    client.settle();
    CHECK(watchers_asked(client) ==
          std::vector<json_t>{ json_t::array({ { { "globPattern", "**/*" } } }) });

    // Until the client takes the registration, the checks don't share what they know.
    folder.write("extra.fb", "Import std.core\nPublic Function Extra(): Int => 1\n");
    client.change(main, text + "\n");
    client.settle();
    CHECK(client.diagnostics(main).empty() == !answers);
}

TEST_CASE("files no Import names, made or changed, are read again once the client says so",
          "[lsp]") {
    // A file an Embed names, made after a check, and the config a Const Function reads, fixed.
    const folder_t folder({
        { "project.toml", "[project]\nname = \"embeds\"\nversion = \"0.1.0\"\n" },
        { "config.txt", "bad" },
        {
            "main.fb",
            "Import std.core\nConst Greeting = EmbedText(\"hello.txt\")\n"
            "Const Function Checked(text: String): Int\n"
            "    If text != \"ok\" Then Panic(\"the config isn't ok\")\n    Return 1\n"
            "End Function\nConst C = Checked(EmbedText(\"config.txt\"))\nPrint(Greeting)\n"
            "Print(C)\n",
        },
    });
    client_t client({ .folders = { folder.root() }, .watches = true });
    const std::string main = folder.uri("main.fb");
    const std::string text = read_file(folder.path("main.fb"));
    client.open(main, text);
    client.settle();
    REQUIRE(client.diagnostics(main).size() == 2);

    folder.write("hello.txt", "hi");
    saw(client, folder.uri("hello.txt"), 1);
    folder.write("config.txt", "ok");
    saw(client, folder.uri("config.txt"), 2);
    client.change(main, text + "\n");
    client.settle();
    CHECK(client.diagnostics(main).empty());
    CHECK(as_fresh(client, { folder.root() }, main, text + "\n", { main }));
}

TEST_CASE("a folder of modules moved in is read once the client says it was made", "[lsp]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"moved\"\nversion = \"0.1.0\"\n" },
        { "main.fb", "Import std.core\nImport \"./parts/wheel\"\nPrint(Wheel())\n" },
    });
    client_t client({ .folders = { folder.root() }, .watches = true });
    const std::string main = folder.uri("main.fb");
    const std::string text = read_file(folder.path("main.fb"));
    client.open(main, text);
    client.settle();
    REQUIRE(!client.diagnostics(main).empty());

    // The client says the folder was made, not each file in it.
    folder.write("parts/wheel.fb", "Import std.core\nPublic Function Wheel(): Int => 4\n");
    saw(client, folder.uri("parts"), 1);
    client.change(main, text + "\n");
    client.settle();
    CHECK(client.diagnostics(main).empty());
    CHECK(as_fresh(client, { folder.root() }, main, text + "\n", { main }));
}

TEST_CASE("a package outside the client's folders is watched with a folder of its own, or not "
          "kept",
          "[lsp]") {
    const bool relative = GENERATE(false, true);
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
    client_t client({
        .folders = { folder.path("app") },
        .watches = true,
        .relative_patterns = relative,
    });
    const std::string main = folder.uri("app/main.fb");
    const std::string text = read_file(folder.path("app/main.fb"));
    client.open(main, text);
    client.settle();
    REQUIRE(client.diagnostics(main).empty());

    // A client whose watchers can have folders of their own is asked to watch lib's too.
    const std::vector<json_t> asked = watchers_asked(client);
    const bool lib_watched = std::ranges::any_of(asked, [&](const json_t& watchers) {
        return watchers[0]["globPattern"].is_object() &&
               fb::lsp::path_of(watchers[0]["globPattern"]["baseUri"].get<std::string>()) ==
                   fb::lsp::path_of(folder.uri("lib"));
    });
    CHECK(lib_watched == relative);
    folder.write("lib/shapes.fb", "Import std.core\nPublic Function Size(): Int => 4\n");
    if (relative) {
        saw(client, folder.uri("lib/shapes.fb"), 2);
    }

    client.change(main, text + "\n");
    client.settle();
    CHECK(!client.diagnostics(main).empty());
    CHECK(as_fresh(client, { folder.path("app") }, main, text + "\n", { main }));
}

TEST_CASE("a .proto changed on disk checks again what imports it", "[lsp]") {
    // The .proto main.fb imports, or one that one imports.
    const bool inner = GENERATE(false, true);
    const std::string login = "syntax = \"proto3\";\n\nimport \"name.proto\";\n\n"
                              "message Login {\n  string user = 1;\n  Name name = 2;\n}\n";
    const folder_t folder({
        { "project.toml", "[project]\nname = \"protos\"\nversion = \"0.1.0\"\n" },
        { "name.proto", "syntax = \"proto3\";\n\nmessage Name {\n  string first = 1;\n}\n" },
        { "login.proto", login },
        {
            "main.fb",
            "Import std.core\nImport \"./login.proto\"\nLocal l = Login(user: \"a\")\n"
            "Print(l.user)\n",
        },
    });
    client_t client({ .folders = { folder.root() }, .watches = true });
    const std::string main = folder.uri("main.fb");
    const std::string text = read_file(folder.path("main.fb"));
    client.open(main, text);
    client.settle();
    REQUIRE(client.diagnostics(main).empty());

    if (inner) {
        folder.write("name.proto",
                     "syntax = \"proto3\";\n\nmessage Name {\n  strin first = 1;\n}\n");
        saw(client, folder.uri("name.proto"), 2);
    } else {
        std::string renamed = login;
        renamed.replace(renamed.find("user"), 4, "person");
        folder.write("login.proto", renamed);
        saw(client, folder.uri("login.proto"), 2);
    }

    client.settle();
    CHECK(!client.diagnostics(main).empty());
    CHECK(as_fresh(client, { folder.root() }, main, text, { main }));
}

TEST_CASE("a package's library or fb.lock changed on disk reads its package again", "[lsp]") {
    const std::string target(fb::build_target());
#ifdef _WIN32
    const std::string library = "dyncalc.dll";
#elifdef __APPLE__
    const std::string library = "libdyncalc.dylib";
#else
    const std::string library = "libdyncalc.so";
#endif
    const folder_t folder({
        {
            "calc/project.toml",
            "[project]\nname = \"calc\"\nversion = \"0.1.0\"\n\n"
            "[native]\ndyncalc = { dynamic = \"native/{target}/dyncalc\" }\n",
        },
        {
            "calc/calc.fb",
            "Package Extern \"C\" Lib \"dyncalc\"\n    Function dc_add(a: Int32, b: Int32): Int32\n"
            "End Extern\nPublic Function Add(a: Int32, b: Int32): Int32 => dc_add(a, b)\n",
        },
        {
            "app/project.toml",
            "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
            "[dependencies]\ncalc = { path = \"../calc\" }\n",
        },
        { "app/main.fb", "Import std.core\nImport calc\nPrint(Add(2, 3))\n" },
    });
    client_t client({ .folders = { folder.root() }, .watches = true });
    const std::string main = folder.uri("app/main.fb");
    const std::string text = read_file(folder.path("app/main.fb"));
    client.open(main, text);
    client.settle();
    const std::vector<std::string> uris{
        main,
        folder.uri("calc/project.toml"),
        folder.uri("app/project.toml"),
    };
    const std::vector<std::string> before = last_messages(client, uris[1]);

    // A build of the C library writes it.
    const std::string built = "calc/native/" + target + "/" + library;
    folder.write(built, "not a library");
    saw(client, folder.uri(built), 1);
    client.settle();
    CHECK(last_messages(client, uris[1]) != before);
    CHECK(as_fresh(client, { folder.root() }, main, text, uris));

    // And fb.lock, which says which packages a build has, is read with its package.
    const std::size_t checked = checks(client);
    folder.write("app/fb.lock", "# fb.lock\n");
    saw(client, folder.uri("app/fb.lock"), 1);
    client.settle();
    CHECK(checks(client) > checked);
    CHECK(as_fresh(client, { folder.root() }, main, text, uris));
}
