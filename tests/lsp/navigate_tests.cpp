#include "client.hpp"
#include "fb/lsp/protocol.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;
using fb::lsp::test::place_of;
using fb::lsp::test::place_t;

namespace {

constexpr std::string_view zoo_text = R"(Import std.core

/// What can say how big it is.
Public Interface Sized
    Method Size(): Int
End Interface

Public Class Animal Implements Sized
    Public Field Legs: Int = 4

    Public Method Size(): Int
        Return Legs
    End Method

    Public Method Speak(): String => "..."
End Class

Public Class Dog Extends Animal
    Public Override Method Speak(): String => "woof"
End Class

Public Struct Crate Implements Sized
    Public Field Width: Int

    Public Method Size(): Int
        Return Width
    End Method
End Struct

Public Function Total(things: Sized[]): Int
    Local sum = 0
    For thing In things
        sum += thing.Size()
    End For

    Return sum
End Function
)";

constexpr std::string_view main_text = R"(Import std.core
Import "./zoo"

Function Feed(animal: Animal, times: Int): Int
    Return animal.Size() * times
End Function

Local rex = Dog()
Local count = Feed(rex, 3)
Local pets: Sized[] = [rex, Animal()]
Local twice: Func(Int): Int = Function(n) => n * 2
Print(Total(pets) + twice(count) + Crate(2).Size())
Print(rex.Speak())
)";

/// The zoo package: an Interface, two Classes, one extending the other, a Struct, and a main
/// file that uses them.
std::vector<std::pair<std::string, std::string>> zoo() {
    return {
        { "project.toml", "[project]\nname = \"zoo\"\nversion = \"0.1.0\"\n" },
        { "zoo.fb", std::string(zoo_text) },
        { "main.fb", std::string(main_text) },
    };
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream const in(path, std::ios::binary);
    std::ostringstream read;
    read << in.rdbuf();
    return read.str();
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

/// Each range as its start's line and character, sorted.
std::vector<std::pair<std::uint32_t, std::uint32_t>> starts(const json_t& ranges) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const json_t& range : ranges) {
        found.emplace_back(range["start"]["line"].get<std::uint32_t>(),
                           range["start"]["character"].get<std::uint32_t>());
    }

    std::ranges::sort(found);
    return found;
}

/// The names of a hierarchy's items, sorted.
std::vector<std::string> names(const json_t& items) {
    std::vector<std::string> found;
    for (const json_t& item : items) {
        found.push_back(item["name"].get<std::string>());
    }

    std::ranges::sort(found);
    return found;
}

/// Each inlay hint as its line, character, and label.
std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> hints(const json_t& found) {
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> out;
    for (const json_t& hint : found) {
        out.emplace_back(hint["position"]["line"].get<std::uint32_t>(),
                         hint["position"]["character"].get<std::uint32_t>(),
                         hint["label"].get<std::string>());
    }

    return out;
}

/// The params of a request about all of a document, from its start to line 100.
json_t whole(const std::string& uri) {
    return {
        { "textDocument", { { "uri", uri } } },
        {
            "range",
            { { "start", client_t::position(0, 0) }, { "end", client_t::position(100, 0) } },
        },
    };
}

} // namespace

TEST_CASE("the server says it does the editor's basics", "[lsp]") {
    client_t client({});
    const json_t& capabilities = client.initialized()["capabilities"];
    for (const char* provider : {
             "foldingRangeProvider",
             "selectionRangeProvider",
             "documentHighlightProvider",
             "inlayHintProvider",
             "typeDefinitionProvider",
             "implementationProvider",
             "callHierarchyProvider",
             "typeHierarchyProvider",
         }) {
        INFO(provider);
        CHECK(capabilities[provider] == true);
    }

    CHECK(capabilities["codeLensProvider"]["resolveProvider"] == true);
}

TEST_CASE("folding ranges fold blocks, each branch, runs of comments, and Imports", "[lsp]") {
    constexpr std::string_view text = R"(Import std.core
Import std.math

// A comment
// over three
// lines.
Function Pick(n: Int): String
    If n < 0
        Return "negative"
    ElseIf n == 0
        Return "zero"
    Else
        Return "positive"
    End If
End Function

Function Name(n: Int): String
    Select n
        Case 1
            Return "one"
        Default
            Return "many"
    End Select
End Function

Local all = [
    1,
    2,
]
Try
    Print(Pick(1))
Catch e
    Print(e.Message)
End Try
)";
    const folder_t folder({ { "fold.fb", std::string(text) } });
    client_t client({});
    const std::string uri = folder.uri("fold.fb");
    client.open(uri, text);
    const json_t found =
        client.request("textDocument/foldingRange", { { "textDocument", { { "uri", uri } } } });
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::string>> ranges;
    for (const json_t& range : found) {
        ranges.emplace(range["startLine"].get<std::uint32_t>(),
                       range["endLine"].get<std::uint32_t>(), range.value("kind", ""));
    }

    const std::set<std::tuple<std::uint32_t, std::uint32_t, std::string>> expected{
        { 0, 1, "imports" },
        { 3, 5, "comment" },
        // Pick, its If, ElseIf, and Else.
        { 6, 13, "" },
        { 7, 8, "" },
        { 9, 10, "" },
        { 11, 12, "" },
        // Name, its Select, its Case, and its Default.
        { 16, 22, "" },
        { 17, 21, "" },
        { 18, 19, "" },
        { 20, 21, "" },
        // The array, and the Try and its Catch.
        { 25, 27, "" },
        { 29, 30, "" },
        { 31, 32, "" },
    };
    CHECK(ranges == expected);
}

TEST_CASE("a selection grows from a word through its expressions and statements to the file",
          "[lsp]") {
    const folder_t folder(zoo());
    client_t client({});
    const std::string uri = folder.uri("main.fb");
    client.open(uri, main_text);
    json_t params = client.at(uri, "times\n");
    params["positions"] = json_t::array({ params["position"] });
    const json_t found = client.request("textDocument/selectionRange", params);
    REQUIRE(found.size() == 1);
    // times, animal.Size() * times, the Return, the body, Feed, and the file.
    std::vector<std::pair<json_t, json_t>> chain;
    for (const json_t* range = &found[0]; range != nullptr;
         range = range->contains("parent") ? &(*range)["parent"] : nullptr) {
        chain.emplace_back((*range)["range"]["start"], (*range)["range"]["end"]);
    }

    const std::vector<std::pair<json_t, json_t>> expected{
        { client_t::position(4, 27), client_t::position(4, 32) },
        { client_t::position(4, 11), client_t::position(4, 32) },
        { client_t::position(4, 4), client_t::position(4, 32) },
        { client_t::position(3, 0), client_t::position(5, 12) },
        { client_t::position(0, 0), client_t::position(13, 0) },
    };
    CHECK(chain == expected);
}

TEST_CASE("document highlights mark a name's reads and writes in its file", "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("zoo.fb");
    client.open(uri, zoo_text);
    const json_t found = client.request("textDocument/documentHighlight", client.at(uri, "sum"));
    std::vector<std::tuple<std::uint32_t, std::uint32_t, int>> marks;
    for (const json_t& mark : found) {
        marks.emplace_back(mark["range"]["start"]["line"].get<std::uint32_t>(),
                           mark["range"]["start"]["character"].get<std::uint32_t>(),
                           mark["kind"].get<int>());
    }

    std::ranges::sort(marks);
    // Declared, added to, and read.
    const std::vector<std::tuple<std::uint32_t, std::uint32_t, int>> expected{
        { 30, 10, 3 },
        { 32, 8, 3 },
        { 35, 11, 2 },
    };
    CHECK(marks == expected);
}

TEST_CASE("inlay hints give types where none is written, and parameters' names, as asked",
          "[lsp]") {
    const folder_t folder(zoo());
    const bool types = GENERATE(false, true);
    const bool parameters = GENERATE(false, true);
    client_t client({
        .folders = { folder.root() },
        .initialization_options = {
            { "inlayHints", { { "types", types }, { "parameterNames", parameters } } },
        },
    });
    const std::string uri = folder.uri("main.fb");
    client.open(uri, main_text);
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> expected;
    if (parameters) {
        expected.emplace_back(8, 19, "animal:");
        expected.emplace_back(8, 24, "times:");
    }

    // Not for rex, whose Dog() says it, nor for Print's values.
    if (types) {
        expected.emplace_back(8, 11, ": Int32");
        expected.emplace_back(10, 40, ": Int32");
    }

    if (parameters) {
        expected.emplace_back(11, 12, "things:");
        expected.emplace_back(11, 41, "Width:");
    }

    std::ranges::sort(expected);
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> found =
        hints(client.request("textDocument/inlayHint", whole(uri)));
    std::ranges::sort(found);
    CHECK(found == expected);

    // A For's variable, in a range of its own.
    const std::string zoo_uri = folder.uri("zoo.fb");
    client.open(zoo_uri, zoo_text);
    json_t params = whole(zoo_uri);
    params["range"] = {
        { "start", client_t::position(30, 0) },
        { "end", client_t::position(33, 0) },
    };
    found = hints(client.request("textDocument/inlayHint", params));
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> loop;
    if (types) {
        loop = { { 30, 13, ": Int32" }, { 31, 13, ": Sized" } };
    }

    CHECK(found == loop);
}

TEST_CASE("the settings' inlay hints change as the client's settings do", "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("main.fb");
    client.open(uri, main_text);
    CHECK(!client.request("textDocument/inlayHint", whole(uri)).empty());
    const json_t hints_off{ { "types", false }, { "parameterNames", false } };

    client.notify("workspace/didChangeConfiguration",
                  {
                      {
                          "settings",
                          { { "futureblitz", { { "inlayHints", hints_off } } } },
                      },
                  });
    CHECK(client.request("textDocument/inlayHint", whole(uri)).empty());
}

TEST_CASE("go to type definition finds a value's type, through T? and T[]", "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("main.fb");
    client.open(uri, main_text);
    const std::string zoo_uri = folder.uri("zoo.fb");
    const json_t rex = client.request("textDocument/typeDefinition", client.at(uri, "rex,"));
    CHECK(place_of(rex) == place_t{ .uri = zoo_uri, .line = 17, .character = 13 });
    const json_t pets = client.request("textDocument/typeDefinition", client.at(uri, "pets)"));
    CHECK(place_of(pets) == place_t{ .uri = zoo_uri, .line = 3, .character = 17 });
    // A built-in type has no declaration.
    CHECK(client.request("textDocument/typeDefinition", client.at(uri, "count =")).is_null());
}

TEST_CASE("implementations find what implements an Interface, extends a Class, or overrides a "
          "member",
          "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("zoo.fb");
    client.open(uri, zoo_text);
    const auto implementations = [&](const std::string& at) {
        return places(client.request("textDocument/implementation", client.at(uri, at)));
    };

    CHECK(implementations("Sized\n") == std::vector<place_t>{
                                            { .uri = uri, .line = 7, .character = 13 },
                                            { .uri = uri, .line = 17, .character = 13 },
                                            { .uri = uri, .line = 21, .character = 14 },
                                        });
    CHECK(implementations("Animal Implements") ==
          std::vector<place_t>{ { .uri = uri, .line = 17, .character = 13 } });
    CHECK(implementations("Size(): Int\nEnd") == std::vector<place_t>{
                                                     { .uri = uri, .line = 10, .character = 18 },
                                                     { .uri = uri, .line = 24, .character = 18 },
                                                 });
    CHECK(implementations("Speak(): String => \".") ==
          std::vector<place_t>{ { .uri = uri, .line = 18, .character = 27 } });
}

TEST_CASE("the call hierarchy finds what calls a function, and what it calls", "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string main = folder.uri("main.fb");
    client.open(main, main_text);
    const json_t prepared =
        client.request("textDocument/prepareCallHierarchy", client.at(main, "Feed("));
    REQUIRE(prepared.size() == 1);
    const json_t& feed = prepared[0];
    CHECK(feed["name"] == "Feed");
    CHECK(feed["selectionRange"]["start"] == client_t::position(3, 9));

    // main.fb's top-level code calls it.
    const json_t incoming = client.request("callHierarchy/incomingCalls", { { "item", feed } });
    REQUIRE(incoming.size() == 1);
    CHECK(incoming[0]["from"]["name"] == "main.fb");
    CHECK(starts(incoming[0]["fromRanges"]) ==
          std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 8, 14 } });

    // It calls Animal's Size, which only it calls.
    const json_t outgoing = client.request("callHierarchy/outgoingCalls", { { "item", feed } });
    REQUIRE(outgoing.size() == 1);
    const json_t& size = outgoing[0]["to"];
    CHECK(size["name"] == "Size");
    CHECK(size["uri"] == folder.uri("zoo.fb"));
    CHECK(size["selectionRange"]["start"] == client_t::position(10, 18));
    CHECK(starts(outgoing[0]["fromRanges"]) ==
          std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 4, 18 } });
    const json_t callers = client.request("callHierarchy/incomingCalls", { { "item", size } });
    REQUIRE(callers.size() == 1);
    CHECK(callers[0]["from"]["name"] == "Feed");

    // The file's top-level code calls the rest, outside Feed, but for the News types get.
    json_t called = json_t::array();
    for (const json_t& call :
         client.request("callHierarchy/outgoingCalls", { { "item", incoming[0]["from"] } })) {
        called.push_back(call["to"]);
    }

    CHECK(names(called) == std::vector<std::string>{ "Feed", "Print", "Size", "Speak", "Total" });

    // Not a variable.
    CHECK(client.request("textDocument/prepareCallHierarchy", client.at(main, "rex =")).is_null());
}

TEST_CASE("the type hierarchy finds a type's supertypes and subtypes", "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string uri = folder.uri("zoo.fb");
    client.open(uri, zoo_text);
    const auto prepare = [&](const std::string& at) {
        const json_t prepared =
            client.request("textDocument/prepareTypeHierarchy", client.at(uri, at));
        REQUIRE(prepared.size() == 1);
        return prepared[0];
    };

    const json_t animal = prepare("Animal Implements");
    CHECK(animal["kind"] == 5);
    CHECK(names(client.request("typeHierarchy/supertypes", { { "item", animal } })) ==
          std::vector<std::string>{ "Sized" });
    CHECK(names(client.request("typeHierarchy/subtypes", { { "item", animal } })) ==
          std::vector<std::string>{ "Dog" });
    const json_t sized = prepare("Sized\n");
    CHECK(sized["kind"] == 11);
    CHECK(names(client.request("typeHierarchy/subtypes", { { "item", sized } })) ==
          std::vector<std::string>{ "Animal", "Crate" });
    CHECK(prepare("Crate Implements")["kind"] == 23);
    CHECK(names(client.request("typeHierarchy/supertypes", { { "item", prepare("Dog") } })) ==
          std::vector<std::string>{ "Animal" });
}

TEST_CASE("code lenses count each declaration's references, and run and debug a main file",
          "[lsp]") {
    const folder_t folder(zoo());
    client_t client({ .folders = { folder.root() } });
    const std::string zoo_uri = folder.uri("zoo.fb");
    const std::string main = folder.uri("main.fb");
    client.open(zoo_uri, zoo_text);
    client.open(main, main_text);
    client.settle();
    const json_t lenses =
        client.request("textDocument/codeLens", { { "textDocument", { { "uri", zoo_uri } } } });
    std::vector<std::pair<std::uint32_t, std::uint32_t>> at;
    for (const json_t& lens : lenses) {
        CHECK(!lens.contains("command"));
        at.emplace_back(lens["range"]["start"]["line"].get<std::uint32_t>(),
                        lens["range"]["start"]["character"].get<std::uint32_t>());
    }

    // Each type, Method, and Function, but no Field.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected{
        { 3, 17 },  { 4, 11 },  { 7, 13 },  { 10, 18 }, { 14, 18 },
        { 17, 13 }, { 18, 27 }, { 21, 14 }, { 24, 18 }, { 29, 16 },
    };
    CHECK(at == expected);

    const auto resolved = [&](const std::uint32_t line) {
        const auto lens = std::ranges::find_if(
            lenses, [&](const json_t& each) { return each["range"]["start"]["line"] == line; });
        REQUIRE(lens != lenses.end());
        return client.request("codeLens/resolve", *lens)["command"];
    };
    const json_t total = resolved(29);
    CHECK(total["title"] == "1 reference");
    CHECK(total["command"] == "futureblitz.showReferences");
    REQUIRE(total["arguments"].size() == 3);
    CHECK(places(total["arguments"][2]) ==
          std::vector<place_t>{ { .uri = main, .line = 11, .character = 6 } });
    CHECK(resolved(7)["title"] == "3 references");

    // The package's main file runs and debugs from its top, and its other files don't.
    const json_t own =
        client.request("textDocument/codeLens", { { "textDocument", { { "uri", main } } } });
    REQUIRE(own.size() >= 2);
    CHECK(own[0]["command"]["command"] == "futureblitz.run");
    CHECK(own[1]["command"]["command"] == "futureblitz.debug");
}

TEST_CASE("each file's @Test Functions are sent as the checks find them, with fb test's names",
          "[lsp]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"counted\"\nversion = \"0.1.0\"\n" },
        {
            "counting.fb",
            "Import std.core\nImport std.test\n\nPublic Function Twice(n: Int): Int => n * 2\n\n"
            "@Test\nFunction Doubles()\n    Expect(Twice(2) == 4)\nEnd Function\n",
        },
        {
            "tests/outside.fb",
            "Import std.core\nImport std.test\nImport counted.counting\n\n@Test\n"
            "Function FromOutside()\n    Expect(Twice(1) == 2)\nEnd Function\n",
        },
    });
    client_t client({
        .folders = { folder.root() },
        .initialization_options = { { "tests", true } },
    });
    client.settle();
    std::map<std::string, std::vector<std::string>> sent;
    for (const json_t& message : client.notifications()) {
        if (message.value("method", "") == "fb/tests") {
            std::vector<std::string>& tests = sent[message["params"]["uri"].get<std::string>()];
            tests.clear();
            for (const json_t& test : message["params"]["tests"]) {
                tests.push_back(test["name"].get<std::string>());
            }
        }
    }

    const std::map<std::string, std::vector<std::string>> expected{
        { folder.uri("counting.fb"), { "counted.counting.Doubles" } },
        { folder.uri("tests/outside.fb"), { "counted.tests.outside.FromOutside" } },
    };
    CHECK(sent == expected);

    // And all of them at once, each with its Function's name and where it is.
    const json_t listed = client.request("fb/tests", json_t::object());
    REQUIRE(listed.size() == 2);
    const auto counting = std::ranges::find_if(
        listed, [&](const json_t& file) { return file["uri"] == folder.uri("counting.fb"); });
    REQUIRE(counting != listed.end());
    REQUIRE((*counting)["tests"].size() == 1);
    CHECK((*counting)["tests"][0]["label"] == "Doubles");
    CHECK((*counting)["tests"][0]["range"]["start"] == client_t::position(5, 0));

    // A test added is sent again.
    const std::string uri = folder.uri("counting.fb");
    client.open(uri, read_file(folder.path("counting.fb")));
    client.change(uri, client.text(uri) + "\n@Test\nFunction Again()\nEnd Function\n");
    client.settle();
    std::size_t tests = 0;
    for (const json_t& message : client.notifications()) {
        if (message.value("method", "") == "fb/tests" && message["params"]["uri"] == uri) {
            tests = message["params"]["tests"].size();
        }
    }

    CHECK(tests == 2);
}
