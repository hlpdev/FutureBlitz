#include "client.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"
#include "fb/lsp/workspace.hpp"
#include "fb/source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

/// The labels of a completion's items.
std::set<std::string> labels(const json_t& completion) {
    std::set<std::string> found;
    for (const json_t& item : completion["items"]) {
        found.insert(item["label"].get<std::string>());
    }

    return found;
}

/// A ranged contentChange.
json_t change(const std::uint32_t line, const std::uint32_t from, const std::uint32_t end_line,
              const std::uint32_t to, const std::string& text) {
    return {
        {
            "range",
            {
                { "start", client_t::position(line, from) },
                { "end", client_t::position(end_line, to) },
            },
        },
        { "text", text },
    };
}

/// The names of a document's symbols.
std::vector<std::string> symbol_names(client_t& client, const std::string& uri) {
    std::vector<std::string> found;
    for (const json_t& symbol : client.request("textDocument/documentSymbol",
                                               { { "textDocument", { { "uri", uri } } } })) {
        found.push_back(symbol["name"].get<std::string>());
    }

    return found;
}

} // namespace

TEST_CASE("completion gives nothing in a string's text, a comment, or a quoted Import", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("text.fb");
    client_t client({});
    client.open(uri, "Import std.core\n"
                     "Import \"./other\"\n"
                     "Local s = \"hello p. wor\"\n"
                     "// a comment p. here\n"
                     "Local xs = [1]\n"
                     "Local t = \"{xs.Count} te\"\n");
    CHECK(client.request("textDocument/completion", client.at(uri, " wor", 4))["items"].empty());
    CHECK(client.request("textDocument/completion", client.at(uri, " here", 5))["items"].empty());
    CHECK(client.request("textDocument/completion", client.at(uri, "p. wor", 2))["items"].empty());
    CHECK(client.request("textDocument/completion", client.at(uri, "} te", 4))["items"].empty());
    CHECK(client.request("textDocument/completion", client.at(uri, "/other", 1))["items"].empty());
    // An interpolation's code is code.
    CHECK(labels(client.request("textDocument/completion", client.at(uri, "xs.Count", 3)))
              .contains("Count"));
}

TEST_CASE("completion where a type goes gives types, built-in ones and type parameters too",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("types.fb");
    client_t client({});
    client.open(uri, "Import std.core\n"
                     "Struct Box<TVal>\n"
                     "    Field v: TV\n"
                     "    Method Get(): TVal\n"
                     "        Local y: TV\n"
                     "        Return v\n"
                     "    End Method\n"
                     "End Struct\n"
                     "Function F<TItem>(a: TItem): TItem\n"
                     "    Local x: TI\n"
                     "    Return a\n"
                     "End Function\n"
                     "Local n: In\n");
    const std::set<std::string> local =
        labels(client.request("textDocument/completion", client.at(uri, "n: In", 5)));
    CHECK(local.contains("Int"));
    CHECK(local.contains("Int32"));
    CHECK(local.contains("String"));
    CHECK(local.contains("Box"));
    CHECK(!local.contains("Print"));
    CHECK(!local.contains("Return"));
    CHECK(!local.contains("n"));
    CHECK(labels(client.request("textDocument/completion", client.at(uri, "x: TI", 5)))
              .contains("TItem"));
    CHECK(labels(client.request("textDocument/completion", client.at(uri, "y: TV", 5)))
              .contains("TVal"));
    CHECK(labels(client.request("textDocument/completion", client.at(uri, "v: TV", 5)))
              .contains("TVal"));
    // Anywhere else, the built-in types are among the names.
    CHECK(labels(client.request("textDocument/completion", client.at(uri, "Return a", 7)))
              .contains("Int"));
}

TEST_CASE("completion gives Super's members, a Static Method's Statics, and a constraint's members",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("members.fb");
    client_t client({});
    client.open(uri, "Import std.core\n"
                     "Interface Sized\n"
                     "    Method Size(): Int\n"
                     "End Interface\n"
                     "Class Animal\n"
                     "    Field name: String\n"
                     "    Method Speak(): Int => 1\n"
                     "End Class\n"
                     "Class Dog Extends Animal\n"
                     "    Field hp: Int\n"
                     "    Static Field count: Int\n"
                     "    Override Method Speak(): Int\n"
                     "        Super.\n"
                     "        Return 2\n"
                     "    End Method\n"
                     "    Static Method Spawn(): Int\n"
                     "        Local z = 1\n"
                     "        Return z\n"
                     "    End Method\n"
                     "End Class\n"
                     "Function Measure<T: Sized>(x: T): Int\n"
                     "    x.\n"
                     "    Return 0\n"
                     "End Function\n");
    const std::set<std::string> super =
        labels(client.request("textDocument/completion", client.at(uri, "Super.", 6)));
    CHECK(super.contains("Speak"));
    CHECK(super.contains("name"));

    const std::set<std::string> statics =
        labels(client.request("textDocument/completion", client.at(uri, "Return z", 7)));
    CHECK(statics.contains("count"));
    CHECK(statics.contains("Spawn"));
    CHECK(!statics.contains("hp"));

    CHECK(labels(client.request("textDocument/completion", client.at(uri, "x.\n", 2)))
              .contains("Size"));
}

TEST_CASE("signature help lists a call's overloads, and skips a character's comma", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("calls.fb");
    client_t client({});
    client.open(uri, "Import std.core\n"
                     "Function Add(a: Int, b: Int): Int => a + b\n"
                     "Function Add(a: Float, b: Float, c: Float): Float => a + b + c\n"
                     "Function Join(sep: Char, a: String, b: String): String => a + b\n"
                     "Print(Join(',', \"x\", \"y\"))\n"
                     "Print(Join('(', \"x\", \"y\"))\n"
                     "Local y = Add(1.0, 2.0, \n");
    const json_t typing =
        client.request("textDocument/signatureHelp", client.at(uri, "2.0, \n", 5));
    REQUIRE(typing.is_object());
    CHECK(typing["signatures"].size() == 2);
    CHECK(typing["activeParameter"] == 2);
    CHECK(typing["signatures"][typing["activeSignature"].get<std::size_t>()]["label"] ==
          "Function Add(a: Float32, b: Float32, c: Float32): Float32");

    CHECK(client.request("textDocument/signatureHelp",
                         client.at(uri, "\"x\", \"y\"))", 1))["activeParameter"] == 1);
    CHECK(client.request("textDocument/signatureHelp",
                         client.at(uri, "'(', \"x\"", 6))["activeParameter"] == 1);
}

TEST_CASE("semantic tokens mark built-in types' names", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("tokens.fb");
    client_t client({});
    client.open(uri, "Function A(): Int => 1\n");
    const json_t legend = client.initialized()["capabilities"]["semanticTokensProvider"]["legend"];
    const json_t data = client.request("textDocument/semanticTokens/full",
                                       { { "textDocument", { { "uri", uri } } } })["data"];
    bool found = false;
    std::uint32_t column = 0;
    for (std::size_t i = 0; i + 4 < data.size(); i += 5) {
        column = data[i].get<std::uint32_t>() == 0 ? column + data[i + 1].get<std::uint32_t>()
                                                   : data[i + 1].get<std::uint32_t>();
        if (column == 14) {
            CHECK(legend["tokenTypes"][data[i + 3].get<std::size_t>()] == "type");
            CHECK(legend["tokenModifiers"][3] == "defaultLibrary");
            CHECK((data[i + 4].get<std::uint32_t>() & 8U) != 0);
            found = true;
        }
    }

    CHECK(found);
}

TEST_CASE("ranged changes edit the text in UTF-16, UTF-8, across CRLF lines, and after a BOM",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("ranged.fb");
    {
        client_t client({});
        // "😀" is two UTF-16 units and four UTF-8 bytes.
        client.open(uri,
                    "Import std.core\r\nLocal s = \"\xF0\x9F\x98\x80\" + \"x\"\r\nLocal a = 1\r\n");
        client.edit(uri, json_t::array({
                             change(1, 17, 1, 20, "\"y\""),
                             change(2, 6, 2, 7, "b"),
                         }));
        CHECK(symbol_names(client, uri) == std::vector<std::string>{ "s", "b" });
        client.settle();
        CHECK(client.diagnostics(uri).empty());
        // Several changes in one, each on the text the one before left, over a line break.
        client.edit(uri, json_t::array({
                             change(2, 0, 3, 0, ""),
                             change(1, 0, 1, 0, "Local c = 2\r\n"),
                         }));
        CHECK(symbol_names(client, uri) == std::vector<std::string>{ "c", "s" });
    }

    {
        client_t client({ .encodings = { "utf-8" } });
        client.open(uri, "Import std.core\nLocal s = \"\xF0\x9F\x98\x80\" + \"x\"\nLocal a = 1\n");
        client.edit(uri, json_t::array({ change(1, 19, 1, 22, "\"y\"") }), fb::column_unit_t::UTF8);
        client.settle();
        CHECK(client.diagnostics(uri).empty());
        const json_t hover = client.request("textDocument/hover", client.at(uri, "+ \"y\"", 3));
        CHECK(hover["range"]["start"] == client_t::position(1, 19));
        CHECK(client.text(uri).contains("+ \"y\""));
    }

    {
        // A BOM isn't in the editor's positions, which a source file leaves it out of too.
        client_t client({});
        client.open(uri, "\xEF\xBB\xBFImport std.core\nLocal a = 1\nPrint(a)\n");
        client.edit(uri, json_t::array({ change(1, 10, 1, 11, "2") }));
        CHECK(symbol_names(client, uri) == std::vector<std::string>{ "a" });
        client.settle();
        CHECK(client.diagnostics(uri).empty());
        CHECK(client.request("textDocument/hover", client.at(uri, "2\n"))["contents"]["value"]
                  .get<std::string>()
                  .contains("Int32"));
    }
}

TEST_CASE("initialize fails without initializing, and comes once", "[lsp]") {
    std::vector<json_t> sent;
    fb::lsp::server_t server([&](const json_t& message) { sent.push_back(message); });
    server.handle({
        { "jsonrpc", "2.0" },
        { "id", 1 },
        { "method", "initialize" },
        {
            "params",
            { { "capabilities", nullptr }, { "workspaceFolders", json_t::array({ 5 }) } },
        },
    });
    REQUIRE(sent.size() == 1);
    CHECK(sent[0]["error"]["code"] == -32602);
    server.handle({
        { "jsonrpc", "2.0" },
        { "id", 2 },
        { "method", "workspace/symbol" },
        { "params", json_t::object() },
    });
    CHECK(sent[1]["error"]["code"] == -32002);

    // A client's capabilities of null say nothing, which is no mistake.
    server.handle({
        { "jsonrpc", "2.0" },
        { "id", 3 },
        { "method", "initialize" },
        { "params", { { "capabilities", nullptr } } },
    });
    CHECK(sent[2].contains("result"));
    server.handle({
        { "jsonrpc", "2.0" },
        { "id", 4 },
        { "method", "initialize" },
        {
            "params",
            {
                {
                    "capabilities",
                    { { "general", { { "positionEncodings", { "utf-8" } } } } },
                },
            },
        },
    });
    CHECK(sent[3]["error"]["code"] == -32600);
}

TEST_CASE("an open project.toml isn't checked as code, and its changes on disk are seen", "[lsp]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"pkg\"\nversion = \"0.1.0\"\n" },
        { "a.fb", "Import std.core\nPublic Function A(): Int => 1\n" },
    });
    client_t client({ .folders = { folder.root() } });
    client.open(folder.uri("project.toml"), "[project]\nname = \"pkg\"\nversion = \"0.1.0\"\n");
    client.settle();
    CHECK(client.diagnostics(folder.uri("project.toml")).empty());

    folder.write("project.toml", "[project]\nname = 5\nversion = \"0.1.0\"\n");
    client.notify("workspace/didChangeWatchedFiles",
                  {
                      {
                          "changes",
                          json_t::array({
                              {
                                  { "uri", folder.uri("project.toml") },
                                  { "type", 2 },
                              },
                          }),
                      },
                  });
    client.settle();
    const json_t diagnostics = client.diagnostics(folder.uri("project.toml"));
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0]["message"].get<std::string>().contains("name"));
}

TEST_CASE("a project.toml made in the workspace makes its files a package", "[lsp]") {
    const folder_t folder({
        { "two.fb", "Import std.core\nPublic Function Two(): Int => 2\n" },
        { "main.fb", "Import std.core\nImport pkg2.two\nPrint(Two())\n" },
    });
    client_t client({ .folders = { folder.root() } });
    const std::string main = folder.uri("main.fb");
    client.open(main, "Import std.core\nImport pkg2.two\nPrint(Two())\n");
    client.settle();
    CHECK(!client.diagnostics(main).empty());

    folder.write("project.toml", "[project]\nname = \"pkg2\"\nversion = \"0.1.0\"\n");
    client.notify("workspace/didChangeWatchedFiles",
                  {
                      {
                          "changes",
                          json_t::array({
                              {
                                  { "uri", folder.uri("project.toml") },
                                  { "type", 1 },
                              },
                          }),
                      },
                  });
    client.settle();
    CHECK(client.diagnostics(main).empty());
}

TEST_CASE("a folder the client removes isn't the workspace's", "[lsp]") {
    const folder_t first({
        { "project.toml", "[project]\nname = \"first\"\nversion = \"0.1.0\"\n" },
        { "base.fb", "Public Function Base(): Int => \"no\"\n" },
    });
    const folder_t second({
        { "project.toml", "[project]\nname = \"second\"\nversion = \"0.1.0\"\n" },
        { "n.fb", "Public Function N(): Int => 1\n" },
    });
    client_t client({ .folders = { first.root(), second.root() } });
    client.settle();
    CHECK(client.diagnostics(first.uri("base.fb")).size() == 1);

    client.notify("workspace/didChangeWorkspaceFolders",
                  {
                      {
                          "event",
                          {
                              { "added", json_t::array() },
                              {
                                  "removed",
                                  json_t::array({
                                      {
                                          { "uri", fb::lsp::uri_of(first.root()) },
                                          { "name", "first" },
                                      },
                                  }),
                              },
                          },
                      },
                  });
    client.settle();
    CHECK(client.diagnostics(first.uri("base.fb")).empty());
    std::set<std::string> names;
    for (const json_t& symbol : client.request("workspace/symbol", { { "query", "" } })) {
        names.insert(symbol["name"].get<std::string>());
    }

    CHECK(names == std::set<std::string>{ "N" });
}

TEST_CASE("a package outside the workspace keeps no diagnostics once none of its files is open",
          "[lsp]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"outside\"\nversion = \"0.1.0\"\n" },
        { "a.fb", "Public Function A(): Int => 1\n" },
        { "broken.fb", "Public Function Broken(): Int => \"no\"\n" },
    });
    client_t client({});
    client.open(folder.uri("a.fb"), "Public Function A(): Int => 1\n");
    client.settle();
    CHECK(client.diagnostics(folder.uri("broken.fb")).size() == 1);
    client.close(folder.uri("a.fb"));
    client.settle();
    CHECK(client.diagnostics(folder.uri("broken.fb")).empty());
}

TEST_CASE("an Import added while a package is checked whole keeps its place in the graph",
          "[lsp]") {
    // Files that make the whole check take a while, so the edit's check is done before it.
    std::vector<std::pair<std::string, std::string>> files{
        { "project.toml", "[project]\nname = \"slow\"\nversion = \"0.1.0\"\n" },
        { "a.fb", "Public Function A(): Int => 1\n" },
        { "b.fb", "Public Function B(): Int => 1\n" },
    };
    for (int i = 0; i < 6; ++i) {
        files.emplace_back(std::format("f{}.fb", i), "Import std.net.http\nImport std.data.json\n");
    }

    const folder_t folder(files);
    client_t client({ .folders = { folder.root() } });
    const std::string a = folder.uri("a.fb");
    client.open(a, "Public Function A(): Int => 1\n");
    client.change(a, "Import \"./b\"\nPublic Function A(): Int => B()\n");
    CHECK(client.request("textDocument/hover", client.at(a, "B()"))["contents"]["value"]
              .get<std::string>()
              .contains("Function B"));
    client.settle();

    const std::string b = folder.uri("b.fb");
    client.open(b, "Public Function B(): Int => 1\n");
    client.change(b, "Public Function Bx(): Int => 1\n");
    client.settle();
    CHECK(client.diagnostics(a).size() == 1);
}
