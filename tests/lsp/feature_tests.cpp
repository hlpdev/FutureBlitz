#include "client.hpp"
#include "fb/build_config.hpp"
#include "fb/format.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/source.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;
using fb::lsp::test::place_of;
using fb::lsp::test::place_t;

namespace {

constexpr std::string_view manifest = R"([project]
name = "geo"
version = "0.1.0"
)";

constexpr std::string_view main_text = R"(Import std.core
Import "./shapes"

Local p = Point(3, 4)
Print("{Length(p)}")
)";

constexpr std::string_view shapes_text = R"(Import std.core

/// A point on the grid.
Public Struct Point
    Public Field X: Int
    Public Field Y: Int

    /// Its length, squared.
    Public Method Squared(): Int
        Return X * X + Y * Y
    End Method
End Struct

/// How long `p` is, squared.
Public Function Length(p: Point): Int
    Local total = p.Squared()
    Return total
End Function

Public Function Scale(p: Point, by: Int): Point
    Return Point(p.X * by, p.Y * by)
End Function
)";

constexpr std::string_view report_text = R"(Import std.core
Import "./shapes"

Public Function Report(p: Point): String
    Return "{Length(p)} at {p.X}"
End Function
)";

/// The geo package: a main file, and two modules, one of which imports the other.
std::vector<std::pair<std::string, std::string>> geo() {
    return {
        { "project.toml", std::string(manifest) },
        { "main.fb", std::string(main_text) },
        { "shapes.fb", std::string(shapes_text) },
        { "report.fb", std::string(report_text) },
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

/// A semantic token, as its line, column, length, and type's name.
struct semantic_token_t {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t length = 0;
    std::string type = "none";
};

/// A document's semantic tokens, from their relative positions.
std::vector<semantic_token_t> semantic_tokens(client_t& client, const std::string& uri) {
    const json_t legend = client.initialized()["capabilities"]["semanticTokensProvider"]["legend"];
    const json_t data = client.request("textDocument/semanticTokens/full",
                                       { { "textDocument", { { "uri", uri } } } })["data"];
    REQUIRE(data.size() % 5 == 0);
    std::vector<semantic_token_t> tokens;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    for (std::size_t i = 0; i < data.size(); i += 5) {
        const auto delta = data[i].get<std::uint32_t>();
        line += delta;
        column = delta == 0 ? column + data[i + 1].get<std::uint32_t>()
                            : data[i + 1].get<std::uint32_t>();
        tokens.push_back({
            .line = line,
            .column = column,
            .length = data[i + 2].get<std::uint32_t>(),
            .type = legend["tokenTypes"][data[i + 3].get<std::size_t>()].get<std::string>(),
        });
    }

    return tokens;
}

/// The token at a line and column, or one of type "none" when none starts there.
semantic_token_t token_at(const std::vector<semantic_token_t>& tokens, const std::uint32_t line,
                          const std::uint32_t column) {
    const auto found = std::ranges::find_if(tokens, [&](const semantic_token_t& token) {
        return token.line == line && token.column == column;
    });
    return found != tokens.end() ? *found : semantic_token_t{};
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
    json_t diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0]["severity"] == 1);
    CHECK(diagnostics[0]["source"] == "fb");
    CHECK(diagnostics[0]["range"]["start"]["line"] == 1);

    client.change(uri, "Import std.core\nLocal n: Int = 5\nPrint(n)\n");
    client.settle();
    diagnostics = client.diagnostics(uri);
    CHECK(diagnostics.empty());

    // A line that doesn't parse, as one does while it's typed.
    client.change(uri, "Import std.core\nLocal n: Int = 5)\nPrint(n)\n");
    client.settle();
    diagnostics = client.diagnostics(uri);
    INFO(diagnostics.dump());
    REQUIRE(messages(diagnostics).size() == 1);
    CHECK(messages(diagnostics).front().starts_with("expected"));
    CHECK(diagnostics[0]["range"]["start"] == client_t::position(1, 16));

    // Its diagnostics go when a file outside the workspace closes.
    client.close(uri);
    client.settle();
    diagnostics = client.diagnostics(uri);
    CHECK(diagnostics.empty());
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
    CHECK(text.contains("Function Length(p: Point): Int32"));
    CHECK(text.contains("How long `p` is, squared."));
    CHECK(length["range"]["start"] == client_t::position(4, 8));

    const json_t point = client.request("textDocument/hover", client.at(main, "Point"));
    CHECK(point["contents"]["value"].get<std::string>().contains("Struct Point"));
    CHECK(point["contents"]["value"].get<std::string>().contains("A point on the grid."));

    const json_t total = client.request("textDocument/hover", client.at(shapes, "total", 2));
    CHECK(total["contents"]["value"].get<std::string>().contains("Local total: Int32"));

    const json_t method = client.request("textDocument/hover", client.at(shapes, "Squared()\n", 1));
    CHECK(method["contents"]["value"].get<std::string>().contains("Method Point.Squared(): Int32"));
    CHECK(method["contents"]["value"].get<std::string>().contains("Its length, squared."));

    // An expression with no name, as a literal, shows its type.
    const json_t literal = client.request("textDocument/hover", client.at(main, "3, 4"));
    CHECK(literal["contents"]["value"].get<std::string>().contains("Int32"));

    // Nothing, between lines.
    CHECK(client.request("textDocument/hover", client.at(main, "\n\nLocal", 1)).is_null());
}

TEST_CASE("hover shows a /// doc comment, an Enum member's too, and no plain comment", "[lsp]") {
    constexpr std::string_view text = R"(Import std.core

/// What a light shows.
///
/// # Examples
///
/// ```
/// # Import std.core
/// Expect(Next(Light.RED) == Light.GREEN)
/// ```
Public Enum Light
    /// Stop.
    RED,
    /// Go.
    GREEN
End Enum

// Not a doc comment.
Public Function Next(light: Light): Light
    Return light == Light.RED ? Light.GREEN : Light.RED
End Function

Local shown = Next(Light.GREEN)
)";
    const folder_t folder({ { "lights.fb", std::string(text) } });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("lights.fb");
    client.open(uri, text);

    // A type's doc, its example without the line it hides.
    const json_t light =
        client.request("textDocument/hover", client.at(uri, "Next(Light.GREEN)", 5));
    REQUIRE(light.is_object());
    const std::string shown = light["contents"]["value"].get<std::string>();
    CHECK(shown.contains("Enum Light"));
    CHECK(shown.contains("What a light shows.\n\n# Examples"));
    CHECK(shown.contains("Expect(Next(Light.RED) == Light.GREEN)"));
    CHECK_FALSE(shown.contains("# Import"));

    // An Enum member's, on a line after a comma.
    const json_t green =
        client.request("textDocument/hover", client.at(uri, "Next(Light.GREEN)", 11));
    REQUIRE(green.is_object());
    CHECK(green["contents"]["value"].get<std::string>().contains("Go."));
    const json_t red = client.request("textDocument/hover", client.at(uri, "RED ?"));
    REQUIRE(red.is_object());
    CHECK(red["contents"]["value"].get<std::string>().contains("Stop."));

    // A // comment isn't a doc.
    const json_t next = client.request("textDocument/hover", client.at(uri, "Next(Light.GREEN)"));
    REQUIRE(next.is_object());
    CHECK(next["contents"]["value"].get<std::string>().contains("Function Next(light: Light)"));
    CHECK_FALSE(next["contents"]["value"].get<std::string>().contains("Not a doc comment"));
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
    CHECK(print["uri"].get<std::string>().contains("std/core"));
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

TEST_CASE("references and renames reach the code of doc comments' examples", "[lsp]") {
    const std::string grow = "/// Grows `n`.\n"
                             "///\n"
                             "/// # Examples\n"
                             "///\n"
                             "/// ```\n"
                             "/// Local x = Grow(1, 2)\n"
                             "/// Expect(x == 3)\n"
                             "/// ```\n"
                             "Public Function Grow(n: Int, by: Int): Int => n + by\n";
    const std::string uses = "Import std.test\nImport geo.grow\n"
                             "@Test Function Grows()\n"
                             "    Expect(Grow(1, 1) == 2)\n"
                             "End Function\n";
    const folder_t folder({
        { "project.toml", std::string(manifest) },
        { "grow.fb", grow },
        { "tests/grows.fb", uses },
    });
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("grow.fb");
    client.open(uri, grow);
    client.settle();

    json_t params = client.at(uri, "Grow(n", 0);
    params["context"] = { { "includeDeclaration", true } };
    const json_t found = client.request("textDocument/references", params);
    std::set<std::uint32_t> lines;
    for (const json_t& location : found) {
        if (location["uri"] == uri) {
            lines.insert(location["range"]["start"]["line"].get<std::uint32_t>());
        }
    }

    CHECK(lines == std::set<std::uint32_t>{ 5, 8 });
    CHECK(found.size() == 3);

    params = client.at(uri, "Grow(n", 0);
    params["newName"] = "Enlarge";
    const json_t changes = client.request("textDocument/rename", params)["changes"];
    REQUIRE(changes.contains(uri));
    REQUIRE(changes[uri].size() == 2);
    CHECK(changes[folder.uri("tests/grows.fb")].size() == 1);
    for (const json_t& edit : changes[uri]) {
        if (edit["range"]["start"]["line"] == 5) {
            CHECK(edit["range"]["start"] == client_t::position(5, 14));
            CHECK(edit["range"]["end"] == client_t::position(5, 18));
        }
    }
}

TEST_CASE("rename changes the names a From line brings in", "[lsp]") {
    const folder_t folder({
        { "project.toml", std::string(manifest) },
        { "lib.fb", "Public Function Area(): Int => 1\n" },
        { "user.fb", "From \"./lib\" Import Area\nPublic Function Twice(): Int => Area() * 2\n" },
    });
    client_t client({ .folders = { folder.root() } });
    const std::string user = folder.uri("user.fb");
    client.open(user, "From \"./lib\" Import Area\nPublic Function Twice(): Int => Area() * 2\n");
    client.settle();

    json_t params = client.at(user, "Area()");
    params["newName"] = "Size";
    const json_t changes = client.request("textDocument/rename", params)["changes"];
    REQUIRE(changes.contains(user));
    CHECK(changes[user].size() == 2);
    CHECK(changes[folder.uri("lib.fb")].size() == 1);
}

TEST_CASE("rename changes an open file outside the workspace, but not std's", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("alone.fb");
    const std::string text = "Import std.core\nLocal count = 1\nPrint(count + 1)\n";
    client_t client({});
    client.open(uri, text);

    json_t params = client.at(uri, "count + 1");
    params["newName"] = "total";
    const json_t edit = client.request("textDocument/rename", params);
    REQUIRE(edit["changes"].contains(uri));
    CHECK(edit["changes"][uri].size() == 2);

    // std's files are std's, even open.
    const std::filesystem::path print =
        fb::path_from_utf8(fb::build_config::std_directory) / "core" / "print.fb";
    std::ifstream const in(print, std::ios::binary);
    std::ostringstream read;
    read << in.rdbuf();
    const std::string print_uri = fb::lsp::uri_of(print);
    client.open(print_uri, read.str());
    CHECK(client.request("textDocument/prepareRename", client.at(uri, "Print")).is_null());
    params = client.at(uri, "Print");
    params["newName"] = "Show";
    CHECK(client.send("textDocument/rename", params)["error"]["code"] == -32803);
}

namespace {

/// Makes the toolchain's std another folder while it lives, as a release's is beside its bin/.
class chosen_std_t {
public:
    explicit chosen_std_t(const std::filesystem::path& folder) {
        fb::toolchain_t chosen = m_before;
        chosen.std = folder;
        fb::use_toolchain(std::move(chosen));
    }

    chosen_std_t(const chosen_std_t&) = delete;
    chosen_std_t& operator=(const chosen_std_t&) = delete;
    chosen_std_t(chosen_std_t&&) = delete;
    chosen_std_t& operator=(chosen_std_t&&) = delete;

    // The toolchain's paths move back, which allocates nothing, though clang-tidy can't see it.
    ~chosen_std_t() { // NOLINT(bugprone-exception-escape)
        fb::use_toolchain(std::move(m_before));
    }

private:
    fb::toolchain_t m_before = fb::toolchain();
};

} // namespace

TEST_CASE("the LSP's std is the toolchain's as a release's is and not the build's", "[lsp]") {
    // A std of its own, with a module the build's hasn't.
    const folder_t release(
        { { "std/zz/probe.fb", "//! A probe.\n\n/// One.\nPublic Function Probe(): Int => 1\n" } });
    std::filesystem::copy(fb::path_from_utf8(fb::build_config::std_directory), release.path("std"),
                          std::filesystem::copy_options::recursive);
    const chosen_std_t chosen(release.path("std"));
    const folder_t folder({});
    const std::string uri = folder.uri("main.fb");
    client_t client({});

    // Its modules are the ones an Import completes.
    client.open(uri, "Import std.core\nImport std.z\nPrint(1)\n");
    const std::set<std::string> modules =
        labels(client.request("textDocument/completion", client.at(uri, "std.z", 5)));
    CHECK(modules.contains("std.zz.probe"));
    CHECK(modules.contains("std.collections"));

    // Its files are std's, which check as std, and whose names aren't renamed.
    client.change(uri, "Import std.core\nPrint(1)\n");
    const std::filesystem::path print = release.path("std/core/print.fb");
    std::ifstream const in(print, std::ios::binary);
    std::ostringstream read;
    read << in.rdbuf();
    const std::string print_uri = fb::lsp::uri_of(print);
    client.open(print_uri, read.str());
    client.settle();
    CHECK(client.diagnostics(print_uri).empty());
    CHECK(client.request("textDocument/prepareRename", client.at(uri, "Print")).is_null());
    json_t params = client.at(print_uri, "Function Print", 9);
    params["newName"] = "Show";
    CHECK(client.send("textDocument/rename", params)["error"]["code"] == -32803);
}

TEST_CASE("completion gives members, names in scope, modules, and keywords", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string shapes = folder.uri("shapes.fb");
    client.open(shapes, shapes_text);

    // After a '.', with the rest of the line not typed yet.
    std::string typing(shapes_text);
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
    CHECK(names.contains("Object"));
    CHECK(!names.contains("by"));

    // In a Method, Self's members.
    const std::set<std::string> own =
        labels(client.request("textDocument/completion", client.at(shapes, "Return X", 7)));
    CHECK(own.contains("X"));
    CHECK(own.contains("Squared"));

    // An Import's modules.
    std::string importing = "Import std.co\n" + std::string(shapes_text);
    client.change(shapes, importing);
    const json_t modules =
        client.request("textDocument/completion", client.at(shapes, "std.co", 6));
    CHECK(labels(modules).contains("std.core"));
    CHECK(labels(modules).contains("std.collections"));
    CHECK(!labels(modules).contains("If"));

    // A module's names, after its qualifier.
    importing = "Import std.collections\n" + std::string(shapes_text);
    importing.replace(importing.find("Local total = p.Squared()"), 25,
                      "Local total = p.Squared()\n    collections.");
    client.change(shapes, importing);
    CHECK(labels(client.request("textDocument/completion", client.at(shapes, "collections.\n", 12)))
              .contains("Stack"));
}

TEST_CASE("an Import in a package's own file doesn't complete its tests/ folder's modules",
          "[lsp]") {
    std::vector<std::pair<std::string, std::string>> files = geo();
    files.emplace_back("tests/outside.fb", "Import geo.shapes\n");
    const folder_t folder(files);
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string importing = "Import geo.\n" + std::string(shapes_text);
    const std::string shapes = folder.uri("shapes.fb");
    client.open(shapes, importing);
    const std::set<std::string> own =
        labels(client.request("textDocument/completion", client.at(shapes, "geo.\n", 4)));
    CHECK(own.contains("geo.report"));
    CHECK(!own.contains("geo.tests"));
    CHECK(!own.contains("geo.tests.outside"));

    // The tests/ folder's own files see each other.
    const std::string outside = folder.uri("tests/outside.fb");
    client.open(outside, "Import geo.\nImport geo.shapes\n");
    CHECK(labels(client.request("textDocument/completion", client.at(outside, "geo.\n", 4)))
              .contains("geo.tests.outside"));
}

TEST_CASE("completion in a package's tests/ folder offers its Package names", "[lsp]") {
    std::vector<std::pair<std::string, std::string>> files = geo();
    files.emplace_back("inside.fb", "Package Function Inner(): Int => 1\n"
                                    "Function Hidden(): Int => 2\n");
    files.emplace_back("tests/outside.fb", "Import geo.inside\n");
    const folder_t folder(files);
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string outside = folder.uri("tests/outside.fb");
    client.open(outside, "Import geo.inside\nLocal x = In\n");
    const std::set<std::string> names =
        labels(client.request("textDocument/completion", client.at(outside, "= In", 4)));
    CHECK(names.contains("Inner"));
    CHECK(!names.contains("Hidden"));
}

TEST_CASE("signature help shows the parameters of the call being typed", "[lsp]") {
    const folder_t folder(geo());
    client_t client({ .folders = { folder.root() }, .load_folders = false });
    const std::string report = folder.uri("report.fb");
    std::string typing(report_text);
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

    const std::vector<semantic_token_t> tokens = semantic_tokens(client, main);
    const auto type_at = [&](const std::uint32_t line, const std::uint32_t column) {
        return token_at(tokens, line, column).type;
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

TEST_CASE("a string's semantic tokens leave its escapes and interpolations' braces to the grammar",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("strings.fb");
    client_t client({});
    client.open(uri, R"(Import std.core
Local name = "x"
Print("a\tb{{c}}d{name}e\u{41}")
Print(r"raw\t")
Local c = '\n'
)");
    const std::vector<semantic_token_t> tokens = semantic_tokens(client, uri);
    const auto type_at = [&](const std::uint32_t line, const std::uint32_t column) {
        return token_at(tokens, line, column).type;
    };
    const auto length_at = [&](const std::uint32_t line, const std::uint32_t column) {
        return token_at(tokens, line, column).length;
    };

    // `"a`, then `\t`, `b`, `{{`, `c`, `}}`, `d`, the interpolation's `{`, and after its `}`,
    // `e`, `\u{41}`, and `"`.
    CHECK(type_at(2, 6) == "string");
    CHECK(length_at(2, 6) == 2);
    CHECK(type_at(2, 8) == "none");
    CHECK(type_at(2, 10) == "string");
    CHECK(length_at(2, 10) == 1);
    CHECK(type_at(2, 11) == "none");
    CHECK(type_at(2, 13) == "string");
    CHECK(type_at(2, 14) == "none");
    CHECK(type_at(2, 16) == "string");
    CHECK(length_at(2, 16) == 1);
    CHECK(type_at(2, 17) == "none");
    CHECK(type_at(2, 18) == "variable");
    CHECK(type_at(2, 22) == "none");
    CHECK(type_at(2, 23) == "string");
    CHECK(length_at(2, 23) == 1);
    CHECK(type_at(2, 24) == "none");
    CHECK(type_at(2, 30) == "string");
    CHECK(length_at(2, 30) == 1);

    // A raw string has no escapes.
    CHECK(type_at(3, 6) == "string");
    CHECK(length_at(3, 6) == 8);

    // A character's escape is left out too.
    CHECK(type_at(4, 10) == "string");
    CHECK(length_at(4, 10) == 1);
    CHECK(type_at(4, 11) == "none");
    CHECK(type_at(4, 13) == "string");
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
    const json_t diagnostics = client.diagnostics(uri);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0]["severity"] == 2);

    const json_t actions = client.request("textDocument/codeAction",
                                          {
                                              { "textDocument", { { "uri", uri } } },
                                              { "range", diagnostics[0]["range"] },
                                              { "context", { { "diagnostics", diagnostics } } },
                                          });
    REQUIRE(actions.size() == 1);
    CHECK(actions[0]["kind"] == "quickfix");
    CHECK(actions[0]["title"] == "Move the constraint inline");
    const json_t& edits = actions[0]["edit"]["changes"][uri];
    REQUIRE(edits.size() == 2);
    CHECK(edits[0]["newText"] == ": Comparable");
    CHECK(edits[0]["range"]["start"] == client_t::position(1, 18));
    CHECK(edits[1]["newText"].get<std::string>().empty());

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

TEST_CASE("a miscased keyword's code action says what it writes", "[lsp]") {
    const folder_t folder({});
    const auto actions_for = [&](const std::string& name, const std::string& text) {
        const std::string uri = folder.uri(name);
        client_t client({});
        client.open(uri, text);
        client.settle();
        const json_t diagnostics = client.diagnostics(uri);
        REQUIRE_FALSE(diagnostics.empty());
        return client.request("textDocument/codeAction",
                              {
                                  { "textDocument", { { "uri", uri } } },
                                  {
                                      "range",
                                      {
                                          { "start", client_t::position(0, 0) },
                                          { "end", client_t::position(9, 0) },
                                      },
                                  },
                                  { "context", { { "diagnostics", diagnostics } } },
                              });
    };

    const json_t actions = actions_for("if.fb", "Import std.core\n"
                                                "Local x = 1\n"
                                                "if x == 1 Then\n"
                                                "    Print(\"one\")\n"
                                                "End If\n");
    REQUIRE(actions.size() == 1);
    CHECK(actions[0]["title"] == "Write 'if' as 'If'");
    const json_t& edits = actions[0]["edit"]["changes"][folder.uri("if.fb")];
    REQUIRE(edits.size() == 1);
    CHECK(edits[0]["newText"] == "If");

    // Variables named like keywords in lowercase are names, which no action rewrites.
    CHECK(actions_for("names.fb", "Import std.core\n"
                                  "Local from = 1\n"
                                  "Local to = 5\n"
                                  "Local d = to - from)\n"
                                  "Print(d)\n")
              .empty());
}

TEST_CASE("formatting gives the document in fb's style, and on save when asked", "[lsp]") {
    const folder_t folder(
        { { "project.toml", std::string(manifest) + "\n[format]\nwidth = 40\n" } });
    const std::string uri = folder.uri("main.fb");
    const std::string text = "Import std.core\nLocal   a=1+2\nPrint(  a )\n";
    client_t client({
        .folders = { folder.root() },
        .load_folders = false,
        .initialization_options = { { "formatOnSave", true } },
    });
    CHECK(client.initialized()["capabilities"]["textDocumentSync"]["willSaveWaitUntil"] == true);
    client.open(uri, text);

    const json_t params{
        { "textDocument", { { "uri", uri } } },
        { "options", { { "tabSize", 4 }, { "insertSpaces", true } } },
    };
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
    CHECK(broken[0]["newText"] == fb::format_source(file, 40).value_or(""));
    CHECK(broken[0]["newText"] != longer);

    // Formatted already, or a file that doesn't parse: nothing to change.
    client.change(uri, "Import std.core\nLocal a = 1\n");
    CHECK(client.request("textDocument/formatting", params).empty());
    client.change(uri, "Import std.core\nLocal a = (\n");
    CHECK(client.request("textDocument/formatting", params).empty());
}
