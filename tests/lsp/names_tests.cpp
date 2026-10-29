#include "client.hpp"
#include "fb/driver.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

namespace {

/// A file with every kind of name a rename has to change: assigned variables and fields, a Method
/// that's overridden, an Interface's Method, Enum values in code and Cases, a Class after Extends,
/// Extend, and before its Statics, an Alias as a type and a constructor, and named arguments.
constexpr std::string_view names = "Import std.core\n"
                                   "Import std.collections\n"
                                   "Enum Color\n"
                                   "    Red\n"
                                   "    Green\n"
                                   "End Enum\n"
                                   "Interface Shown\n"
                                   "    Method Show(): Int\n"
                                   "End Interface\n"
                                   "Class Shape Implements Shown\n"
                                   "    Field count: Int\n"
                                   "    Static Field made: Int\n"
                                   "    Method Area(): Int => 1\n"
                                   "    Method Show(): Int => count\n"
                                   "    Static Method Make(): Shape => Shape()\n"
                                   "    Method Bump(v: Int)\n"
                                   "        count = v\n"
                                   "    End Method\n"
                                   "End Class\n"
                                   "Class Square Extends Shape\n"
                                   "    Override Method Area(): Int => Super.Area() * 2\n"
                                   "End Class\n"
                                   "Extend Shape\n"
                                   "    Method Twice(): Int => Area() * 2\n"
                                   "End Extend\n"
                                   "Alias Figure = Shape\n"
                                   "Function Max<T: Comparable>(a: T, b: T): T => a > b ? a : b\n"
                                   "Local c = Color.Red\n"
                                   "Select c\n"
                                   "    Case Color.Green\n"
                                   "        Print(\"green\")\n"
                                   "    Default\n"
                                   "End Select\n"
                                   "Shape.made += 1\n"
                                   "Local s: Figure = Figure()\n"
                                   "s.Bump(v: 3)\n"
                                   "Local q: Shape = Square()\n"
                                   "Local m = 1\n"
                                   "m = 2\n"
                                   "Print(q.Area(), s.Show(), Shape.Make().Twice(), m, Max(1, 2))\n"
                                   "Local nums = [3, 1, 2]\n"
                                   "nums.Sort(Function(a, b) => a < b)\n";

/// `text` with LSP TextEdits made, whose positions count bytes, as ASCII's UTF-16 does.
std::string applied(std::string text, const json_t& edits) {
    std::vector<json_t> sorted(edits.begin(), edits.end());
    const fb::source_file_t file(fb::file_id_t{}, "", text);
    const fb::lsp::positions_t positions;
    std::ranges::sort(sorted, [&](const json_t& a, const json_t& b) {
        return positions.offset(file, a["range"]["start"]) >
               positions.offset(file, b["range"]["start"]);
    });
    for (const json_t& edit : sorted) {
        const std::uint32_t begin = positions.offset(file, edit["range"]["start"]);
        const std::uint32_t end = positions.offset(file, edit["range"]["end"]);
        text.replace(begin, end - begin, edit["newText"].get<std::string>());
    }

    return text;
}

/// The errors of a program, with the standard library.
std::vector<std::string> errors_of(const std::string& text) {
    const fb::compilation_t compilation = fb::compile("names.fb", text);
    std::vector<std::string> errors;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (diagnostic.severity == fb::severity_t::ERROR) {
            errors.push_back(diagnostic.message);
        }
    }

    return errors;
}

/// A client with `names` open, and its URI.
struct opened_t {
    folder_t folder{ {} };
    std::string uri = folder.uri("names.fb");
    client_t client{ {} };

    opened_t() {
        client.open(uri, names);
    }

    /// The program after a rename at `text`'s first occurrence, plus `offset`, to `name`, which
    /// must be made.
    std::string renamed(const std::string& text, const std::uint32_t offset,
                        const std::string& name) {
        json_t params = client.at(uri, text, offset);
        params["newName"] = name;
        const json_t edit = client.request("textDocument/rename", params);
        INFO(edit.dump());
        REQUIRE(edit["changes"].contains(uri));
        return applied(std::string(names), edit["changes"][uri]);
    }

    /// How many places references at `text`'s first occurrence, plus `offset`, give.
    std::size_t references(const std::string& text, const std::uint32_t offset = 0) {
        json_t params = client.at(uri, text, offset);
        params["context"] = { { "includeDeclaration", true } };
        return client.request("textDocument/references", params).size();
    }

    std::string hover(const std::string& text, const std::uint32_t offset = 0) {
        const json_t found = client.request("textDocument/hover", client.at(uri, text, offset));
        return found.is_null() ? std::string() : found["contents"]["value"].get<std::string>();
    }
};

} // namespace

TEST_CASE("the names file checks", "[lsp]") {
    CHECK(errors_of(std::string(names)).empty());
}

TEST_CASE("references and renames reach a variable's and a field's assignments", "[lsp]") {
    opened_t opened;
    CHECK(opened.references("Local m", 6) == 3);
    CHECK(opened.hover("m = 2").contains("Local m: Int32"));
    const std::string renamed = opened.renamed("Local m", 6, "total");
    CHECK(renamed.contains("total = 2"));
    CHECK(errors_of(renamed).empty());

    // A field assigned by name in a Method.
    CHECK(opened.references("Field count", 6) == 3);
    const std::string field = opened.renamed("Field count", 6, "n");
    CHECK(field.contains("n = v"));
    CHECK(errors_of(field).empty());
}

TEST_CASE("a Method's own name is the Method's, not its Self's", "[lsp]") {
    opened_t opened;
    const std::string hover = opened.hover("Area(): Int => 1");
    CHECK(hover.contains("Method Shape.Area(): Int32"));
    CHECK(!hover.contains("Self"));
    CHECK(opened.references("Area(): Int => 1") >= 3);

    // Its Override and the code that calls either are renamed with it.
    const std::string renamed = opened.renamed("Area(): Int => 1", 0, "Size");
    CHECK(renamed.contains("Override Method Size()"));
    CHECK(renamed.contains("Super.Size()"));
    CHECK(errors_of(renamed).empty());

    // And from the Override, the Method it overrides.
    CHECK(errors_of(opened.renamed("Area(): Int => Super", 0, "Measure")).empty());
}

TEST_CASE("an Interface's Method is renamed with the Methods that implement it", "[lsp]") {
    opened_t opened;
    const std::string renamed = opened.renamed("Show(): Int\n", 0, "Display");
    CHECK(renamed.contains("Method Display(): Int => count"));
    CHECK(renamed.contains("s.Display()"));
    CHECK(errors_of(renamed).empty());
}

TEST_CASE("an Enum's values are named where code uses them", "[lsp]") {
    opened_t opened;
    CHECK(opened.references("Green\n") == 2);
    CHECK(opened.hover("Red\n").contains("Color.Red = 0"));
    CHECK(opened.hover("Red\nSelect").contains("Color.Red"));
    const std::string renamed = opened.renamed("Red\n", 0, "Crimson");
    CHECK(renamed.contains("Color.Crimson"));
    CHECK(errors_of(renamed).empty());
}

TEST_CASE("a Class is named after Extends, Extend, and before its Statics", "[lsp]") {
    opened_t opened;
    CHECK(opened.hover("Shape.made").contains("Class Shape"));
    CHECK(opened.hover("Shape\n    Override").contains("Class Shape"));
    const std::string renamed = opened.renamed("Class Shape", 6, "Form");
    CHECK(renamed.contains("Extends Form"));
    CHECK(renamed.contains("Extend Form"));
    CHECK(renamed.contains("Form.made"));
    CHECK(errors_of(renamed).empty());
}

TEST_CASE("an Alias is named as a type and as a constructor", "[lsp]") {
    opened_t opened;
    CHECK(opened.hover("Figure =").contains("Alias Figure = Shape"));
    CHECK(opened.references("Alias Figure", 6) == 3);
    const std::string renamed = opened.renamed("Alias Figure", 6, "Body");
    CHECK(renamed.contains("Local s: Body = Body()"));
    CHECK(errors_of(renamed).empty());
}

TEST_CASE("a parameter is named by its call's named arguments", "[lsp]") {
    opened_t opened;
    const std::string renamed = opened.renamed("v: Int", 0, "amount");
    CHECK(renamed.contains("s.Bump(amount: 3)"));
    CHECK(errors_of(renamed).empty());
}

TEST_CASE("a rename to a name code already sees there is refused", "[lsp]") {
    opened_t opened;
    for (const std::string name : { "c", "Print", "nums", "Int" }) {
        INFO(name);
        json_t params = opened.client.at(opened.uri, "Local m", 6);
        params["newName"] = name;
        CHECK(opened.client.send("textDocument/rename", params)["error"]["code"] == -32803);
    }

    // A member's, by another member of its type.
    json_t params = opened.client.at(opened.uri, "Field count", 6);
    params["newName"] = "made";
    CHECK(opened.client.send("textDocument/rename", params)["error"]["code"] == -32803);
}

TEST_CASE("a rename takes only ASCII names, and never a New's", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("point.fb");
    client_t client({});
    client.open(uri, "Import std.core\n"
                     "Struct Point\n"
                     "    Field X: Int\n"
                     "    New(x: Int)\n"
                     "        X = x\n"
                     "    End New\n"
                     "End Struct\n"
                     "Local count = Point(1).X\n");
    json_t params = client.at(uri, "Local count", 6);
    params["newName"] = "z\xC3\xA4hler";
    CHECK(client.send("textDocument/rename", params)["error"]["code"] == -32602);

    CHECK(client.request("textDocument/prepareRename", client.at(uri, "New(")).is_null());
    params = client.at(uri, "New(");
    params["newName"] = "Make";
    CHECK(client.send("textDocument/rename", params)["error"]["code"] == -32803);
}

TEST_CASE("hover shows an inferred lambda parameter's type, and a constraint", "[lsp]") {
    opened_t opened;
    CHECK(opened.hover("a < b").contains("a: Int32"));
    CHECK(opened.hover("(a, b) =>", 1).contains("a: Int32"));
    CHECK(opened.hover("Max<T", 0).contains("Function Max<T: Comparable>(a: T, b: T): T"));
}

TEST_CASE("module qualifiers have hovers and definitions, and aren't renamed", "[lsp]") {
    const folder_t folder({
        { "lib.fb", "Public Function Area(): Int => 1\n" },
        { "main.fb", "Import std.core\nImport \"./lib\" As L\nPrint(L.Area())\n" },
    });
    const std::string uri = folder.uri("main.fb");
    client_t client({});
    client.open(uri, "Import std.core\nImport \"./lib\" As L\nPrint(L.Area())\n");
    const json_t hover = client.request("textDocument/hover", client.at(uri, "L.Area"));
    REQUIRE(hover.is_object());
    CHECK(hover["contents"]["value"].get<std::string>().contains("Import"));
    const json_t definition = client.request("textDocument/definition", client.at(uri, "L.Area"));
    CHECK(fb::lsp::test::place_of(definition) ==
          fb::lsp::test::place_t{ .uri = folder.uri("lib.fb") });

    json_t params = client.at(uri, "L.Area");
    params["newName"] = "Lib";
    const json_t refused = client.send("textDocument/rename", params);
    CHECK(refused["error"]["message"].get<std::string>().contains("module"));
}
