#include "client.hpp"
#include "fb/format.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;
using fb::lsp::test::place_of;
using fb::lsp::test::place_t;

namespace {

const std::string manifest = R"([project]
name = "geo"
version = "0.1.0"
)";

const std::string main_text = R"(Import std.core
Import "./shapes"

Local p = Point(3, 4)
Print("{Length(p)}")
)";

const std::string shapes_text = R"(Import std.core

// A point on the grid.
Public Struct Point
    Public Field X: Int
    Public Field Y: Int

    // Its length, squared.
    Public Method Squared(): Int
        Return X * X + Y * Y
    End Method
End Struct

// How long `p` is, squared.
Public Function Length(p: Point): Int
    Local total = p.Squared()
    Return total
End Function

Public Function Scale(p: Point, by: Int): Point
    Return Point(p.X * by, p.Y * by)
End Function
)";

const std::string report_text = R"(Import std.core
Import "./shapes"

Public Function Report(p: Point): String
    Return "{Length(p)} at {p.X}"
End Function
)";

/// The geo package: a main file, and two modules, one of which imports the other.
std::vector<std::pair<std::string, std::string>> geo() {
    return {
        { "project.toml", manifest },
        { "main.fb", main_text },
        { "shapes.fb", shapes_text },
        { "report.fb", report_text },
    };
}

/// The labels of a completion's items.
std::set<std::string> labels(const json_t& completion) {
    std::set<std::string> found;
    for (const json_t& item : completion["items"]) {
        found.insert(item["label"].get<std::string>());
    }

    return found;
}

/// The places of a list of Locations, sorted.
std::vector<place_t> places(const json_t& locations) {
    std::vector<place_t> found;
    for (const json_t& location : locations) {
        found.push_back(place_of(location));
    }

    std::ranges::sort(found);
    return found;
}

/// The messages of a file's diagnostics.
std::vector<std::string> messages(const json_t& diagnostics) {
    std::vector<std::string> found;
    for (const json_t& diagnostic : diagnostics) {
        found.push_back(diagnostic["message"].get<std::string>());
    }

    return found;
}

} // namespace

TEST_CASE("diagnostics come as a file is typed, broken, and fixed", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("typed.fb");
    client_t client({});
    client.open(uri, "Import std.core\nLocal n: Int = \"text\"\nPrint(n)\n");
    client.settle();
    std::optional<json_t> diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics);
    REQUIRE(diagnostics->size() == 1);
    CHECK((*diagnostics)[0]["severity"] == 1);
    CHECK((*diagnostics)[0]["source"] == "fb");
    CHECK((*diagnostics)[0]["range"]["start"]["line"] == 1);

    client.change(uri, "Import std.core\nLocal n: Int = 5\nPrint(n)\n");
    client.settle();
    diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics);
    CHECK(diagnostics->empty());

    // A line that doesn't parse, as one does while it's typed.
    client.change(uri, "Import std.core\nLocal n: Int = 5)\nPrint(n)\n");
    client.settle();
    diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics);
    INFO(diagnostics->dump());
    REQUIRE(messages(*diagnostics).size() == 1);
    CHECK(messages(*diagnostics).front().starts_with("expected"));
    CHECK((*diagnostics)[0]["range"]["start"] == client_t::position(1, 16));

    // Its diagnostics go when a file outside the workspace closes.
    client.close(uri);
    client.settle();
    diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics);
    CHECK(diagnostics->empty());
}

TEST_CASE("hover shows a name's declaration and its docs, and an expression's type", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() } });
    const std::string main = folder.uri("main.fb");
    const std::string shapes = folder.uri("shapes.fb");
    client.open(main, main_text);
    client.open(shapes, shapes_text);

    const json_t length = client.request("textDocument/hover", client.at(main, "Length"));
    REQUIRE(length.is_object());
    CHECK(length["contents"]["kind"] == "markdown");
    const std::string text = length["contents"]["value"].get<std::string>();
    CHECK(text.find("Function Length(p: Point): Int32") != std::string::npos);
    CHECK(text.find("How long `p` is, squared.") != std::string::npos);
    CHECK(length["range"]["start"] == client_t::position(4, 8));

    const json_t point = client.request("textDocument/hover", client.at(main, "Point"));
    CHECK(point["contents"]["value"].get<std::string>().find("Struct Point") != std::string::npos);
    CHECK(point["contents"]["value"].get<std::string>().find("A point on the grid.") !=
          std::string::npos);

    const json_t total = client.request("textDocument/hover", client.at(shapes, "total", 2));
    CHECK(total["contents"]["value"].get<std::string>().find("Local total: Int32") !=
          std::string::npos);

    const json_t method = client.request("textDocument/hover", client.at(shapes, "Squared()\n", 1));
    CHECK(method["contents"]["value"].get<std::string>().find("Method Point.Squared(): Int32") !=
          std::string::npos);
    CHECK(method["contents"]["value"].get<std::string>().find("Its length, squared.") !=
          std::string::npos);

    // An expression with no name, as a literal, shows its type.
    const json_t literal = client.request("textDocument/hover", client.at(main, "3, 4"));
    CHECK(literal["contents"]["value"].get<std::string>().find("Int32") != std::string::npos);

    // Nothing, between lines.
    CHECK(client.request("textDocument/hover", client.at(main, "\n\nLocal", 1)).is_null());
}

TEST_CASE("go to definition finds declarations in other modules, and Imports' files", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() } });
    const std::string main = folder.uri("main.fb");
    const std::string report = folder.uri("report.fb");
    client.open(main, main_text);
    client.open(report, report_text);

    const json_t length = client.request("textDocument/definition", client.at(main, "Length"));
    CHECK(place_of(length) ==
          place_t{ .uri = folder.uri("shapes.fb"), .line = 14, .character = 16 });

    // A Struct's name, as its constructor's call writes it, is the Struct's.
    const json_t point = client.request("textDocument/definition", client.at(main, "Point"));
    CHECK(place_of(point) == place_t{ .uri = folder.uri("shapes.fb"), .line = 3, .character = 14 });

    const json_t field = client.request("textDocument/definition", client.at(report, "X}"));
    CHECK(place_of(field) == place_t{ .uri = folder.uri("shapes.fb"), .line = 4, .character = 17 });

    const json_t imported = client.request("textDocument/definition", client.at(main, "shapes"));
    CHECK(place_of(imported) == place_t{ .uri = folder.uri("shapes.fb") });

    // std's own declarations are in std's files.
    const json_t print = client.request("textDocument/definition", client.at(main, "Print"));
    REQUIRE(print.is_object());
    CHECK(print["uri"].get<std::string>().find("std/core") != std::string::npos);
}

TEST_CASE("find references finds a name's uses in each module that imports it", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() } });
    const std::string shapes = folder.uri("shapes.fb");
    client.open(shapes, shapes_text);
    client.settle();

    json_t params = client.at(shapes, "Length");
    params["context"] = { { "includeDeclaration", true } };
    const std::vector<place_t> found = places(client.request("textDocument/references", params));
    const std::vector<place_t> expected{
        { .uri = folder.uri("main.fb"), .line = 4, .character = 8 },
        { .uri = folder.uri("report.fb"), .line = 4, .character = 13 },
        { .uri = shapes, .line = 14, .character = 16 },
    };
    CHECK(found == expected);

    params["context"] = { { "includeDeclaration", false } };
    CHECK(places(client.request("textDocument/references", params)).size() == 2);

    // From a use, in a file that isn't open, as from the declaration.
    const std::string report = folder.uri("report.fb");
    client.open(report, report_text);
    json_t from_use = client.at(report, "Length");
    from_use["context"] = { { "includeDeclaration", true } };
    CHECK(places(client.request("textDocument/references", from_use)) == expected);
}

TEST_CASE("rename changes a name everywhere the workspace uses it", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() } });
    const std::string shapes = folder.uri("shapes.fb");
    const std::string main = folder.uri("main.fb");
    client.open(shapes, shapes_text);
    client.open(main, main_text);
    client.settle();

    const json_t prepared = client.request("textDocument/prepareRename", client.at(main, "Length"));
    CHECK(prepared["placeholder"] == "Length");

    json_t params = client.at(main, "Length");
    params["newName"] = "Size";
    const json_t edit = client.request("textDocument/rename", params);
    const json_t& changes = edit["changes"];
    std::size_t edits = 0;
    for (const auto& [uri, list] : changes.items()) {
        for (const json_t& each : list) {
            CHECK(each["newText"] == "Size");
            ++edits;
        }
    }

    CHECK(edits == 3);
    CHECK(changes.size() == 3);

    // A keyword isn't a name, and std's declarations aren't the workspace's.
    params["newName"] = "If";
    CHECK(client.send("textDocument/rename", params)["error"]["code"] == -32602);
    CHECK(client.request("textDocument/prepareRename", client.at(main, "Print")).is_null());
}

TEST_CASE("completion gives members, names in scope, modules, and keywords", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string shapes = folder.uri("shapes.fb");
    client.open(shapes, shapes_text);

    // After a '.', with the rest of the line not typed yet.
    std::string typing = shapes_text;
    typing.replace(typing.find("p.Squared()"), 11, "p.");
    client.change(shapes, typing);
    const std::set<std::string> members =
        labels(client.request("textDocument/completion", client.at(shapes, "p.\n", 2)));
    CHECK(members.contains("X"));
    CHECK(members.contains("Y"));
    CHECK(members.contains("Squared"));
    CHECK(!members.contains("Length"));

    // And with some of the member typed.
    typing.replace(typing.find("p.\n"), 3, "p.Sq\n");
    client.change(shapes, typing);
    CHECK(labels(client.request("textDocument/completion", client.at(shapes, "p.Sq", 4)))
              .contains("Squared"));

    // The names a function's code can use: its parameters and Locals before it, the file's own
    // declarations, its Imports', and the keywords.
    client.change(shapes, shapes_text);
    const std::set<std::string> names =
        labels(client.request("textDocument/completion", client.at(shapes, "Return total", 7)));
    CHECK(names.contains("p"));
    CHECK(names.contains("total"));
    CHECK(names.contains("Length"));
    CHECK(names.contains("Point"));
    CHECK(names.contains("Print"));
    CHECK(names.contains("Return"));
    CHECK(!names.contains("by"));

    // In a Method, Self's members.
    const std::set<std::string> own =
        labels(client.request("textDocument/completion", client.at(shapes, "Return X", 7)));
    CHECK(own.contains("X"));
    CHECK(own.contains("Squared"));

    // An Import's modules.
    std::string importing = "Import std.co\n" + shapes_text;
    client.change(shapes, importing);
    const json_t modules =
        client.request("textDocument/completion", client.at(shapes, "std.co", 6));
    CHECK(labels(modules).contains("std.core"));
    CHECK(labels(modules).contains("std.collections"));
    CHECK(!labels(modules).contains("If"));

    // A module's names, after its qualifier.
    importing = "Import std.collections\n" + shapes_text;
    importing.replace(importing.find("Local total = p.Squared()"), 25,
                      "Local total = p.Squared()\n    collections.");
    client.change(shapes, importing);
    CHECK(labels(client.request("textDocument/completion", client.at(shapes, "collections.\n", 12)))
              .contains("Stack"));
}

TEST_CASE("signature help shows the parameters of the call being typed", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string report = folder.uri("report.fb");
    std::string typing = report_text;
    typing.replace(typing.find("    Return"), 0, "    Local q = Scale(p, \n");
    client.open(report, typing);

    const json_t help =
        client.request("textDocument/signatureHelp", client.at(report, "Scale(p, ", 9));
    REQUIRE(help.is_object());
    CHECK(help["signatures"][0]["label"] == "Function Scale(p: Point, by: Int32): Point");
    CHECK(help["activeParameter"] == 1);
    const json_t& parameter = help["signatures"][0]["parameters"][1]["label"];
    CHECK(parameter == json_t::array({ 25, 34 }));

    const json_t first =
        client.request("textDocument/signatureHelp", client.at(report, "Scale(p", 6));
    CHECK(first["activeParameter"] == 0);
}

TEST_CASE("document symbols outline a file, and workspace symbols find them", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string shapes = folder.uri("shapes.fb");
    client.open(shapes, shapes_text);

    const json_t symbols = client.request("textDocument/documentSymbol",
                                          { { "textDocument", { { "uri", shapes } } } });
    REQUIRE(symbols.size() == 3);
    CHECK(symbols[0]["name"] == "Point");
    CHECK(symbols[0]["kind"] == 23);
    REQUIRE(symbols[0]["children"].size() == 3);
    CHECK(symbols[0]["children"][0]["name"] == "X");
    CHECK(symbols[0]["children"][0]["kind"] == 8);
    CHECK(symbols[0]["children"][2]["name"] == "Squared");
    CHECK(symbols[0]["children"][2]["kind"] == 6);
    CHECK(symbols[1]["name"] == "Length");
    CHECK(symbols[1]["kind"] == 12);
    CHECK(symbols[1]["detail"] == "Public Function Length(p: Point): Int");
    CHECK(symbols[1]["selectionRange"]["start"] == client_t::position(14, 16));

    const json_t found = client.request("workspace/symbol", { { "query", "rprt" } });
    REQUIRE(found.size() == 1);
    CHECK(found[0]["name"] == "Report");
    CHECK(place_of(found[0]["location"]) ==
          place_t{ .uri = folder.uri("report.fb"), .line = 3, .character = 16 });
    CHECK(client.request("workspace/symbol", { { "query", "" } }).size() == 8);
}

TEST_CASE("semantic tokens say what each name is", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string main = folder.uri("main.fb");
    client.open(main, main_text);

    const json_t legend = client.initialized()["capabilities"]["semanticTokensProvider"]["legend"];
    const json_t data = client.request("textDocument/semanticTokens/full",
                                       { { "textDocument", { { "uri", main } } } })["data"];
    REQUIRE(data.size() % 5 == 0);
    // Each token, as its line, column, length, and type's name.
    struct token_t {
        std::uint32_t line;
        std::uint32_t column;
        std::uint32_t length;
        std::string type;
    };
    std::vector<token_t> tokens;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    for (std::size_t i = 0; i < data.size(); i += 5) {
        const auto delta = data[i].get<std::uint32_t>();
        line += delta;
        column = delta == 0 ? column + data[i + 1].get<std::uint32_t>()
                            : data[i + 1].get<std::uint32_t>();
        tokens.push_back(
            { line, column, data[i + 2].get<std::uint32_t>(),
              legend["tokenTypes"][data[i + 3].get<std::size_t>()].get<std::string>() });
    }

    const auto type_at = [&](const std::uint32_t at_line, const std::uint32_t at_column) {
        for (const token_t& token : tokens) {
            if (token.line == at_line && token.column == at_column) {
                return token.type;
            }
        }

        return std::string("none");
    };
    CHECK(type_at(0, 0) == "keyword");
    CHECK(type_at(0, 7) == "namespace");
    CHECK(type_at(0, 11) == "namespace");
    CHECK(type_at(1, 7) == "string");
    CHECK(type_at(3, 6) == "variable");
    CHECK(type_at(3, 10) == "struct");
    CHECK(type_at(3, 16) == "number");
    CHECK(type_at(4, 0) == "function");
    CHECK(type_at(4, 6) == "string");
    CHECK(type_at(4, 8) == "function");
    CHECK(type_at(4, 15) == "variable");
}

TEST_CASE("code actions make the fix-its fb has", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("fixed.fb");
    const std::string text = "Import std.core\n"
                             "Function Biggest<T>(a: T, b: T): T\n"
                             "    Where T: Comparable\n"
                             "    Return a\n"
                             "End Function\n"
                             "Print(Biggest(1, 2))\n";
    client_t client({});
    client.open(uri, text);
    client.settle();
    const std::optional<json_t> diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics);
    REQUIRE(diagnostics->size() == 1);
    CHECK((*diagnostics)[0]["severity"] == 2);

    const json_t actions = client.request("textDocument/codeAction",
                                          {
                                              { "textDocument", { { "uri", uri } } },
                                              { "range", (*diagnostics)[0]["range"] },
                                              { "context", { { "diagnostics", *diagnostics } } },
                                          });
    REQUIRE(actions.size() == 1);
    CHECK(actions[0]["kind"] == "quickfix");
    const json_t& edits = actions[0]["edit"]["changes"][uri];
    REQUIRE(edits.size() == 2);
    CHECK(edits[0]["newText"] == ": Comparable");
    CHECK(edits[0]["range"]["start"] == client_t::position(1, 18));
    CHECK(edits[1]["newText"] == "");

    // None where no diagnostic has a fix.
    CHECK(client
              .request("textDocument/codeAction",
                       { { "textDocument", { { "uri", uri } } },
                         { "range",
                           { { "start", client_t::position(5, 0) },
                             { "end", client_t::position(5, 1) } } },
                         { "context", { { "diagnostics", json_t::array() } } } })
              .empty());
}

TEST_CASE("formatting gives the document in fb's style, and on save when asked", "[lsp]") {
    const folder_t folder({ { "project.toml", manifest + "\n[format]\nwidth = 40\n" } });
    const std::string uri = folder.uri("main.fb");
    const std::string text = "Import std.core\nLocal   a=1+2\nPrint(  a )\n";
    client_t client({ .folders = { folder.root() },
                      .load_folders = false,
                      .initialization_options = { { "formatOnSave", true } } });
    CHECK(client.initialized()["capabilities"]["textDocumentSync"]["willSaveWaitUntil"] == true);
    client.open(uri, text);

    const json_t params{ { "textDocument", { { "uri", uri } } },
                         { "options", { { "tabSize", 4 }, { "insertSpaces", true } } } };
    const json_t edits = client.request("textDocument/formatting", params);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0]["newText"] == "Import std.core\nLocal a = 1 + 2\nPrint(a)\n");
    CHECK(edits[0]["range"]["start"] == client_t::position(0, 0));
    CHECK(edits[0]["range"]["end"] == client_t::position(3, 0));

    const json_t saved =
        client.request("textDocument/willSaveWaitUntil",
                       { { "textDocument", { { "uri", uri } } }, { "reason", 1 } });
    CHECK(saved == edits);

    // The package's width breaks a long line.
    const std::string longer =
        "Import std.core\nLocal total = 1000000 + 2000000 + 3000000 + 4000000\n";
    client.change(uri, longer);
    const json_t broken = client.request("textDocument/formatting", params);
    REQUIRE(broken.size() == 1);
    const fb::source_file_t file(fb::file_id_t{}, "main.fb", longer);
    CHECK(broken[0]["newText"] == *fb::format_source(file, 40));

    // Formatted already, or a file that doesn't parse: nothing to change.
    client.change(uri, "Import std.core\nLocal a = 1\n");
    CHECK(client.request("textDocument/formatting", params).empty());
    client.change(uri, "Import std.core\nLocal a = (\n");
    CHECK(client.request("textDocument/formatting", params).empty());
}
