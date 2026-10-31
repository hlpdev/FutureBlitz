#include "client.hpp"
#include "fb/lsp/protocol.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

/// A program with an Enum, a function that takes one, and Locals of several types, which a
/// test types the end of `Main` into.
constexpr std::string_view painting = R"(Import std.core

Enum Color
    RED, GREEN
End Enum

Function Paint(c: Color, times: Int): Int
    Return times
End Function

Function Main(): Int
    Local count = 3
    Local name = "x"
    Local shade: Color = Color.RED
    Local ok = True
)";

/// textDocument/completion's params at a position in `uri`.
json_t completion_at(const std::string& uri, const std::uint32_t line,
                     const std::uint32_t character) {
    return {
        { "textDocument", { { "uri", uri } } },
        { "position", client_t::position(line, character) },
    };
}

/// The completion items at the end of `typed`, written at the end of `painting`'s Main, with the
/// rest of Main after it, in the order the editor shows them: by their sortText.
std::vector<json_t> completed(client_t& client, const std::string& uri, const std::string& typed) {
    const std::string text = std::string(painting) + typed + "\n    Return 0\nEnd Function\n";
    client.change(uri, text);
    // At the end of what's typed, which may be written earlier in the program too.
    const std::string_view written =
        std::string_view(text).substr(0, painting.size() + typed.size());
    const auto line = static_cast<std::uint32_t>(std::ranges::count(written, '\n'));
    const auto character = static_cast<std::uint32_t>(written.size() - written.rfind('\n') - 1);
    const json_t found =
        client.request("textDocument/completion", completion_at(uri, line, character));
    std::vector<json_t> items(found["items"].begin(), found["items"].end());
    std::ranges::stable_sort(items, {}, [](const json_t& item) {
        return item.value("sortText", item["label"].get<std::string>());
    });
    return items;
}

/// Where an item is among `items`, nullopt when it isn't.
std::optional<std::size_t> place_of(const std::vector<json_t>& items, const std::string& label) {
    const auto found =
        std::ranges::find_if(items, [&](const json_t& item) { return item["label"] == label; });
    if (found == items.end()) {
        return std::nullopt;
    }

    return static_cast<std::size_t>(found - items.begin());
}

/// Whether `first` comes before `second` among `items`, both being there.
bool before(const std::vector<json_t>& items, const std::string& first, const std::string& second) {
    const std::optional<std::size_t> a = place_of(items, first);
    const std::optional<std::size_t> b = place_of(items, second);
    return a && b && *a < *b;
}

/// The labels of `items`.
std::set<std::string> labels_of(const std::vector<json_t>& items) {
    std::set<std::string> found;
    for (const json_t& item : items) {
        found.insert(item["label"].get<std::string>());
    }

    return found;
}

/// The item labeled `label`, which must be there.
json_t item_of(const std::vector<json_t>& items, const std::string& label) {
    const std::optional<std::size_t> at = place_of(items, label);
    REQUIRE(at);
    return at ? items[*at] : json_t();
}

} // namespace

TEST_CASE("completion ranks first what the code wants where the word is", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("paint.fb");
    client_t client({});
    client.open(uri, painting);

    // A parameter's type: an Int for Paint's times.
    std::vector<json_t> items = completed(client, uri, "    Local n = Paint(shade, ");
    CHECK(before(items, "count", "name"));
    CHECK(item_of(items, "count")["sortText"].get<std::string>().starts_with('0'));

    // An Enum's members where it wants one, and its Locals.
    items = completed(client, uri, "    Local other: Color = ");
    CHECK(place_of(items, "Color.RED") < std::optional<std::size_t>(2));
    CHECK(place_of(items, "Color.GREEN") < std::optional<std::size_t>(3));
    CHECK(before(items, "shade", "count"));

    // What the function returns.
    items = completed(client, uri, "    Return ");
    CHECK(before(items, "count", "name"));
    CHECK(before(items, "count", "Null"));

    // The other side of a comparison.
    items = completed(client, uri, "    If shade == ");
    CHECK(before(items, "Color.GREEN", "count"));

    // An assignment's target.
    items = completed(client, uri, "    count = ");
    CHECK(before(items, "count", "name"));

    // True and False where a Bool is wanted.
    items = completed(client, uri, "    Local yes: Bool = ");
    CHECK(before(items, "True", "count"));
    CHECK(before(items, "ok", "count"));
}

TEST_CASE("completion offers the names of the parameters a call doesn't give yet", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("paint.fb");
    client_t client({});
    client.open(uri, painting);

    std::vector<json_t> items = completed(client, uri, "    Local n = Paint(");
    CHECK(labels_of(items).contains("c:"));
    CHECK(labels_of(items).contains("times:"));
    CHECK(item_of(items, "times:")["insertText"] == "times: ");

    items = completed(client, uri, "    Local n = Paint(shade, ");
    CHECK(!labels_of(items).contains("c:"));
    CHECK(labels_of(items).contains("times:"));

    items = completed(client, uri, "    Local n = Paint(times: 1, ");
    CHECK(labels_of(items).contains("c:"));
    CHECK(!labels_of(items).contains("times:"));

    // Not in the middle of an argument.
    items = completed(client, uri, "    Local n = Paint(shade + ");
    CHECK(!labels_of(items).contains("c:"));
}

TEST_CASE("completion offers the keywords that go where the word is", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("places.fb");
    const std::string text = "Import std.core\n"
                             "\n"
                             "Class Box\n"
                             "    Field Size: Int\n"
                             "\n"
                             "    Method Grow()\n"
                             "        Size = Size + 1\n"
                             "\n"
                             "    End Method\n"
                             "End Class\n"
                             "\n";
    client_t client({});
    client.open(uri, text);
    const auto keywords_at = [&](const std::uint32_t line, const std::uint32_t character) {
        std::set<std::string> found;
        const json_t items =
            client.request("textDocument/completion", completion_at(uri, line, character))["items"];
        for (const json_t& item : items) {
            if (item["kind"] == 14) {
                found.insert(item["label"].get<std::string>());
            }
        }

        return found;
    };

    // The top level: declarations, and a main file's statements.
    std::set<std::string> found = keywords_at(10, 0);
    CHECK(found.contains("Function"));
    CHECK(found.contains("Struct"));
    CHECK(found.contains("If"));
    CHECK(!found.contains("Field"));

    // A Class's body: its members.
    found = keywords_at(4, 4);
    CHECK(found.contains("Field"));
    CHECK(found.contains("Method"));
    CHECK(found.contains("Override"));
    CHECK(!found.contains("Return"));
    CHECK(!found.contains("Struct"));

    // A Method's body: statements.
    found = keywords_at(7, 8);
    CHECK(found.contains("Return"));
    CHECK(found.contains("If"));
    CHECK(!found.contains("Field"));
    CHECK(!found.contains("Function"));

    // An expression: the values and what goes on in one.
    found = keywords_at(6, 22);
    CHECK(found.contains("True"));
    CHECK(found.contains("Null"));
    CHECK(!found.contains("If"));
    CHECK(!found.contains("Return"));
}

TEST_CASE("postfix completions write the value before the '.' into what they say", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("paint.fb");
    client_t client({});
    client.open(uri, painting);

    // A Bool: an If on it, and its opposite.
    std::vector<json_t> items = completed(client, uri, "    ok.");
    json_t made = item_of(items, "if");
    CHECK(made["insertTextFormat"] == 2);
    CHECK(made["textEdit"]["newText"] == "If ok\n        $0\n    End If");
    CHECK(made["textEdit"]["range"]["start"] == client_t::position(15, 4));
    CHECK(made["filterText"] == "ok.if");
    CHECK(item_of(items, "not")["textEdit"]["newText"] == "!ok");
    CHECK(item_of(items, "local")["textEdit"]["newText"] == "Local ${1:name} = ok");
    CHECK(!labels_of(items).contains("for"));
    CHECK(!labels_of(items).contains("try"));

    // An array: a For over it.
    items = completed(client, uri, "    Local all = [count]\n    all.");
    CHECK(item_of(items, "for")["textEdit"]["newText"] ==
          "For ${1:each} In all\n        $0\n    End For");
    CHECK(!labels_of(items).contains("if"));

    // A nullable: an If on its being Null.
    items = completed(client, uri, "    Local maybe: Int? = Null\n    maybe.");
    CHECK(item_of(items, "ifnull")["textEdit"]["newText"] ==
          "If maybe == Null\n        $0\n    End If");

    // A call: Try before it.
    items = completed(client, uri, "    Paint(shade, 1).");
    CHECK(item_of(items, "try")["textEdit"]["newText"] == "Try Paint(shade, 1)");
}

TEST_CASE("a Class's body offers the Methods it can override and implement", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("shapes.fb");
    const std::string head = "Import std.core\n"
                             "\n"
                             "Interface Shape\n"
                             "    Method Area(): Int\n"
                             "    Method Name(): String => \"shape\"\n"
                             "End Interface\n"
                             "\n"
                             "Class Base\n"
                             "    Method Describe(): String\n"
                             "        Return \"base\"\n"
                             "    End Method\n"
                             "\n"
                             "    Final Method Kept(): Int\n"
                             "        Return 1\n"
                             "    End Method\n"
                             "End Class\n"
                             "\n"
                             "Class Square Extends Base Implements Shape\n";
    client_t client({});
    const auto offered = [&](const std::string& body) {
        const std::string text = head + body + "    \nEnd Class\n";
        client.change(uri, text);
        const std::uint32_t line = 18 + static_cast<std::uint32_t>(std::ranges::count(body, '\n'));
        std::vector<json_t> items;
        const json_t found = client.request("textDocument/completion", completion_at(uri, line, 4));
        for (const json_t& item : found["items"]) {
            items.push_back(item);
        }

        return items;
    };
    client.open(uri, head + "End Class\n");

    std::vector<json_t> items = offered("");
    const std::set<std::string> labels = labels_of(items);
    CHECK(labels.contains("Override Method Describe(): String"));
    CHECK(labels.contains("Method Area(): Int32"));
    // A default is overridden, and a Final Method isn't offered.
    CHECK(labels.contains("Override Method Name(): String"));
    CHECK(!labels.contains("Override Method Kept(): Int32"));
    const json_t area = item_of(items, "Method Area(): Int32");
    CHECK(area["insertText"] == "Method Area(): Int32\n        $0\n    End Method");
    CHECK(area["insertTextFormat"] == 2);
    CHECK(area["filterText"] == "Area");

    // Once it has one, it isn't offered again.
    items = offered("    Method Area(): Int\n        Return 4\n    End Method\n\n");
    CHECK(!labels_of(items).contains("Method Area(): Int32"));
    CHECK(labels_of(items).contains("Override Method Describe(): String"));
}

TEST_CASE("completion offers the names of modules the file doesn't import, with the Import",
          "[lsp]") {
    const folder_t folder({
        { "project.toml", "[project]\nname = \"geo\"\nversion = \"0.1.0\"\n" },
        {
            "shapes.fb",
            "Import std.core\nPublic Struct Point\n    Public Field X: Int\nEnd Struct\n"
            "Public Function Length(p: Point): Int => p.X\n",
        },
        { "report.fb", "Import std.core\n\nPublic Function Report(): Int => 1\n" },
    });
    client_t client({ .folders = { folder.root() } });
    client.settle();
    const std::string report = folder.uri("report.fb");
    client.open(report, "Import std.core\n\nPublic Function Report(): Int => 1\n");
    const auto offered = [&](const std::string& typed) {
        const std::string text =
            "Import std.core\n\nPublic Function Report(): Int\n    Local p = " + typed +
            "\n    Return 1\nEnd Function\n";
        client.change(report, text);
        return client.request("textDocument/completion",
                              client.at(report, typed, static_cast<std::uint32_t>(typed.size())));
    };

    // A name of another module of the package, after what the file sees, adding its Import
    // after the file's.
    json_t found = offered("Poi");
    CHECK(found["isIncomplete"] == true);
    std::vector<json_t> items(found["items"].begin(), found["items"].end());
    const json_t point = item_of(items, "Point");
    CHECK(point["detail"] == "Struct from geo.shapes");
    CHECK(point["sortText"].get<std::string>().starts_with('5'));
    REQUIRE(point["additionalTextEdits"].size() == 1);
    CHECK(point["additionalTextEdits"][0]["newText"] == "Import geo.shapes\n");
    CHECK(point["additionalTextEdits"][0]["range"]["start"] == client_t::position(1, 0));

    // And std's.
    found = offered("Stac");
    items.assign(found["items"].begin(), found["items"].end());
    const json_t stack = item_of(items, "Stack");
    CHECK(stack["additionalTextEdits"][0]["newText"] == "Import std.collections\n");

    // A name the file sees already adds nothing.
    found = offered("Pri");
    items.assign(found["items"].begin(), found["items"].end());
    CHECK(!item_of(items, "Print").contains("additionalTextEdits"));
}
