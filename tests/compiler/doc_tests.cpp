#include "fb/doc.hpp"
#include "fb/driver.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

/// A file parsed with its doc comments, as the driver parses one.
struct parsed_t {
    fb::source_file_t file;
    fb::ast_t ast;

    explicit parsed_t(const std::string_view text, const std::string_view path = "test.fb",
                      const std::span<const std::string_view> defined = {})
        : file(fb::file_id_t{}, std::string(path), std::string(text)) {
        fb::diagnostic_bag_t bag;
        std::vector<fb::span_t> docs;
        const std::vector<fb::token_t> tokens = fb::lex(file, bag, defined, &docs);
        ast = fb::parse(file, tokens, bag, docs);
    }

    /// The doc comment's text on the declaration whose name is the first `name` in the file
    /// after `after`, or nullopt.
    [[nodiscard]] std::optional<std::string> doc(const std::string_view name,
                                                 const std::string_view after = {}) const {
        const std::size_t from = file.text().find(after);
        const std::size_t at = file.text().find(name, from == std::string_view::npos ? 0 : from);
        REQUIRE(at != std::string_view::npos);
        const fb::span_t span{
            .file = file.id(),
            .begin = static_cast<std::uint32_t>(at),
            .end = static_cast<std::uint32_t>(at + name.size()),
        };
        const fb::doc_comment_t* found = ast.doc(span);
        return found != nullptr ? std::optional(found->text) : std::nullopt;
    }
};

/// A doc comment of `text`, its lines at made-up places.
fb::doc_t read(const std::string_view text) {
    fb::doc_comment_t comment{ .text = std::string(text) };
    std::uint32_t at = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = std::min(text.find('\n', begin), text.size());
        comment.lines.push_back({
            .begin = at + static_cast<std::uint32_t>(begin),
            .end = at + static_cast<std::uint32_t>(end),
        });
        begin = end + 1;
        at += 4;
    }

    return fb::read_doc(comment);
}

std::vector<std::string> link_targets(const fb::doc_t& doc) {
    std::vector<std::string> targets;
    targets.reserve(doc.links.size());
    for (const fb::doc_link_t& link : doc.links) {
        targets.push_back(link.target);
    }

    return targets;
}

} // namespace

TEST_CASE("doc comments go on the declarations right after them", "[doc]") {
    const parsed_t parsed(R"(//! The module's doc,
//! on two lines.

Import std.core

/// A shape.
///
///   Indented.
@Inline
Public Function Area(): Int
    /// Not a Local's.
    Local x = 1
    Return x
End Function

/// A circle.
Public Struct Circle
    /// Its radius.
    Public Field Radius: Float

    /// Grows it.
    Public ByRef Method Grow()
    End Method

    /// Its width.
    Public Property Width: Float
        Get => Radius * 2
    End Property
End Struct

/// Kinds.
Public Enum Kind
    /// Round.
    Round,
    Flat
    /// Square,
    Square, Other
End Enum

Extend Circle
    /// Shrinks it.
    Public Method Shrink()
    End Method
End Extend

Extern "C"
    /// C's.
    Function puts(text: CString): Int
End Extern

/// After a blank line.

Function Apart()
End Function

//// Four slashes are an ordinary comment.
Function Ordinary()
End Function

/* A block comment
/// with what looks like a doc comment */
Function Commented()
End Function

Function Before() /// on its line
End Function
)");
    CHECK(parsed.doc("Area") == "A shape.\n\n  Indented.");
    CHECK(parsed.doc("x") == std::nullopt);
    CHECK(parsed.doc("Circle", "/// A circle.") == "A circle.");
    CHECK(parsed.doc("Radius") == "Its radius.");
    CHECK(parsed.doc("Grow", "Method") == "Grows it.");
    CHECK(parsed.doc("Width", "Property") == "Its width.");
    CHECK(parsed.doc("Kind", "Enum") == "Kinds.");
    CHECK(parsed.doc("Round", "    Round") == "Round.");
    CHECK(parsed.doc("Flat") == std::nullopt);
    CHECK(parsed.doc("Square", "    Square") == "Square,");
    CHECK(parsed.doc("Other") == std::nullopt);
    CHECK(parsed.doc("Shrink", "Method Shrink") == "Shrinks it.");
    CHECK(parsed.doc("puts") == "C's.");
    CHECK(parsed.doc("Apart") == std::nullopt);
    CHECK(parsed.doc("Ordinary") == std::nullopt);
    CHECK(parsed.doc("Commented") == std::nullopt);
    CHECK(parsed.doc("Before") == std::nullopt);

    const fb::doc_comment_t* module = parsed.ast.module_doc(parsed.file.id());
    REQUIRE(module != nullptr);
    CHECK(module->text == "The module's doc,\non two lines.");
    // Each line's text, where it is in the file.
    REQUIRE(module->lines.size() == 2);
    CHECK(parsed.file.slice(module->lines[1]) == "on two lines.");

    // A doc comment by statement, as the LSP finds one.
    const fb::stmt_id_t area = parsed.ast.items()[1];
    REQUIRE(parsed.ast.doc(area) != nullptr);
    CHECK(parsed.ast.doc(area)->text == "A shape.\n\n  Indented.");
    CHECK(parsed.ast.doc(parsed.ast.items()[0]) == nullptr);
}

TEST_CASE("a doc comment's lines keep where they are", "[doc]") {
    const parsed_t parsed(
        "Struct S\n    ///  Two spaces.\n    ///\n    /// One.\n    Field X: Int\nEnd Struct\n");
    const fb::stmt_id_t type = parsed.ast.items()[0];
    const auto& members = std::get<fb::type_stmt_t>(parsed.ast.get(type).node).members;
    const fb::doc_comment_t* doc = parsed.ast.doc(members[0]);
    REQUIRE(doc != nullptr);
    CHECK(doc->text == " Two spaces.\n\nOne.");
    REQUIRE(doc->lines.size() == 3);
    CHECK(parsed.file.slice(doc->lines[0]) == " Two spaces.");
    CHECK(doc->lines[1].empty());
    CHECK(parsed.file.slice(doc->lines[2]) == "One.");

    // Without the lexer's doc comments, as the formatter parses, nothing has one.
    fb::diagnostic_bag_t bag;
    const fb::ast_t plain = fb::parse(parsed.file, fb::lex(parsed.file, bag), bag);
    const auto& plain_members = std::get<fb::type_stmt_t>(plain.get(plain.items()[0]).node).members;
    CHECK(plain.doc(plain_members[0]) == nullptr);
}

TEST_CASE("the doc comments in an #If's branch that isn't taken are on nothing", "[doc]") {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb",
                                 "#If NEVER\n/// Never.\nFunction F()\nEnd Function\n#Else\n/// "
                                 "Always.\nFunction F()\nEnd Function\n#End If\n");
    fb::diagnostic_bag_t bag;
    std::vector<fb::span_t> docs;
    const std::vector<fb::token_t> tokens = fb::lex(file, bag, {}, &docs);
    REQUIRE(docs.size() == 1);
    CHECK(file.slice(docs[0]) == "/// Always.");
    const fb::ast_t ast = fb::parse(file, tokens, bag, docs);
    REQUIRE(ast.items().size() == 1);
    REQUIRE(ast.doc(ast.items()[0]) != nullptr);
    CHECK(ast.doc(ast.items()[0])->text == "Always.");
}

TEST_CASE("a doc's summary is its first sentence", "[doc]") {
    CHECK(read("Takes the top value off. Panics when it's empty.").summary ==
          "Takes the top value off.");
    CHECK(read("Gives `a.b` back, or 1.5 times it! Then more.").summary ==
          "Gives `a.b` back, or 1.5 times it!");
    CHECK(read("Is it empty?\nYes.").summary == "Is it empty?");
    CHECK(read("A summary that goes\non to the next line. Then more.").summary ==
          "A summary that goes on to the next line.");
    CHECK(read("No sentence's end\n\nNext paragraph.").summary == "No sentence's end");
    CHECK(read("\n\nAfter blank lines.").summary == "After blank lines.");
    CHECK(read("# Examples\n\nText.").summary.empty());
    CHECK(read("```\nCode()\n```\nText.").summary.empty());
    CHECK(read("").summary.empty());
}

TEST_CASE("a doc's sections are under its headings", "[doc]") {
    const fb::doc_t doc = read("Opens a file.\n\nMore.\n\n# Errors\n\nIOError when it can't.\n\n## "
                               "Panics ##\n\nNever.");
    REQUIRE(doc.sections.size() == 3);
    CHECK(doc.sections[0].heading.empty());
    CHECK(doc.sections[0].markdown == "Opens a file.\n\nMore.");
    CHECK(doc.sections[1].heading == "Errors");
    CHECK(doc.sections[1].markdown == "IOError when it can't.");
    CHECK(doc.sections[2].heading == "Panics");
    CHECK(doc.sections[2].markdown == "Never.");
    // "#Hash" isn't a heading, nor is "####### seven".
    CHECK(read("Text.\n#Hash\n####### seven").sections.size() == 1);
}

TEST_CASE("a doc's links name declarations", "[doc]") {
    const fb::doc_t doc =
        read("See [Stack], [Stack.Push], [`Pop`], [Peek()], [Map<K, V>.Get], and\n"
             "[the docs](https://futureblitz.dev), [a][label], a[i], ![image],\n"
             "[1, 2], [x + 1], `[Code]`, \\[Escaped], and [std.core.Print].\n"
             "Before a colon, [Colon]: a link, as a definition starts a line.\n"
             "\n"
             "``[Spans\n"
             "Lines]``, [After].\n"
             "\n"
             "```\n"
             "[InCode]\n"
             "```\n"
             "\n"
             "[label]: https://futureblitz.dev");
    CHECK(link_targets(doc) == std::vector<std::string>{ "Stack", "Stack.Push", "Pop", "Peek",
                                                         "Map.Get", "std.core.Print", "Colon",
                                                         "After" });
    // Each link's span covers its brackets.
    REQUIRE(!doc.links.empty());
    CHECK(doc.links[0].span.begin == 4);
    CHECK(doc.links[0].span.end == 11);
}

TEST_CASE("a doc's examples are the code blocks under # Examples", "[doc]") {
    const fb::doc_t doc = read("Pushes.\n"
                               "\n"
                               "```\n"
                               "NotUnderExamples()\n"
                               "```\n"
                               "\n"
                               "# Examples\n"
                               "\n"
                               "```\n"
                               "# Import std.collections\n"
                               "Local stack = Stack<Int>()\n"
                               "#\n"
                               "  #   Indented()\n"
                               "#If DEBUG\n"
                               "stack.Push(1)\n"
                               "```\n"
                               "\n"
                               "```fb no_run\n"
                               "Serve()\n"
                               "```\n"
                               "\n"
                               "~~~text\n"
                               "Not code.\n"
                               "~~~\n"
                               "\n"
                               "  ````fb\n"
                               "  Fenced()\n"
                               "  ```\n"
                               "    Still()\n"
                               "  ````\n"
                               "\n"
                               "```\n"
                               "Unterminated()");
    REQUIRE(doc.examples.size() == 4);
    const fb::doc_example_t& first = doc.examples[0];
    CHECK(first.kind == fb::example_kind_t::RUN);
    REQUIRE(first.lines.size() == 6);
    CHECK(first.lines[0].text == "Import std.collections");
    CHECK(first.lines[0].hidden);
    CHECK(first.lines[2].text.empty());
    CHECK(first.lines[2].hidden);
    CHECK(first.lines[3].text == "  Indented()");
    CHECK(first.lines[3].hidden);
    CHECK_FALSE(first.lines[4].hidden);
    CHECK(first.shown() == "Local stack = Stack<Int>()\n#If DEBUG\nstack.Push(1)\n");
    // A line's span is its code's.
    CHECK(first.lines[0].span.end - first.lines[0].span.begin == 22);

    CHECK(doc.examples[1].kind == fb::example_kind_t::NO_RUN);
    CHECK(doc.examples[1].shown() == "Serve()\n");
    // Up to the fence's indentation goes, and a shorter fence doesn't close it.
    CHECK(doc.examples[2].shown() == "Fenced()\n```\n  Still()\n");
    CHECK(doc.examples[3].shown() == "Unterminated()\n");

    // The docs show the hidden lines' code blocks without them.
    CHECK(doc.markdown.contains("```\nLocal stack = Stack<Int>()\n#If DEBUG\nstack.Push(1)\n```"));
    CHECK_FALSE(doc.markdown.contains("Import std.collections"));
}

TEST_CASE("an example is a module of its own, whose lines show where they are", "[doc]") {
    const parsed_t parsed(R"(Public Struct Box
    /// Holds it.
    ///
    /// # Examples
    ///
    /// ```
    /// # Import std.math
    /// Struct Held
    ///     Field X: Int
    /// End Struct
    ///
    /// Expect(Box().Holds(Held(1)))
    /// ```
    ///
    /// ```fb no_run
    /// Import std.core
    /// Print("hi")
    /// ```
    Public Method Holds(x: Held): Bool => True
End Struct
)",
                          "game/box.fb");
    const std::vector<fb::example_module_t> modules =
        fb::example_modules(parsed.ast, parsed.file, "game.box", "game.box");
    REQUIRE(modules.size() == 2);
    CHECK(modules[0].name == "game.box.Box.Holds (example)");
    CHECK(modules[0].text == "Import std.core\n"
                             "Import std.test\n"
                             "Import game.box\n"
                             "Import std.math\n"
                             "Struct Held\n"
                             "    Field X: Int\n"
                             "End Struct\n"
                             "@Test Function DocTest__Example() Throws Error\n"
                             "\n"
                             "Expect(Box().Holds(Held(1)))\n"
                             "End Function\n");
    // The Imports it adds, and its test's first and last lines, are at its fence; each of its own
    // lines is where its code is.
    const std::vector<fb::location_t> lines = modules[0].lines;
    REQUIRE(lines.size() == 12);
    CHECK(lines[0] == fb::location_t{ .line = 5, .column = 8 });
    CHECK(lines[3] == fb::location_t{ .line = 6, .column = 10 });
    CHECK(lines[4] == fb::location_t{ .line = 7, .column = 8 });
    CHECK(lines[9] == fb::location_t{ .line = 11, .column = 8 });

    // One that only compiles has its code in a Function nothing calls, and a test that's empty.
    CHECK(modules[1].name == "game.box.Box.Holds (example 2)");
    CHECK(modules[1].text == "Import std.test\n"
                             "Import game.box\n"
                             "Import std.core\n"
                             "Function DocTest__Example() Throws Error\n"
                             "Print(\"hi\")\n"
                             "End Function\n"
                             "@Test Function DocTest__Compiles()\n"
                             "End Function\n");
    // What fb adds shows at the fence, whatever its column; the example's own lines move on.
    CHECK(modules[1].added ==
          std::vector<bool>{ true, true, false, true, false, true, true, true, true });
    CHECK(modules[1].fence.begin < modules[1].fence.end);
}

namespace {

/// The item with `path` among `items`, or their members.
const fb::doc_item_t* item_at(const std::vector<fb::doc_item_t>& items,
                              const std::string_view path) {
    for (const fb::doc_item_t& item : items) {
        if (item.path == path) {
            return &item;
        }

        if (const fb::doc_item_t* member = item_at(item.members, path)) {
            return member;
        }
    }

    return nullptr;
}

std::vector<std::string> paths_of(const std::vector<fb::doc_item_t>& items) {
    std::vector<std::string> paths;
    paths.reserve(items.size());
    for (const fb::doc_item_t& item : items) {
        paths.push_back(item.path);
    }

    return paths;
}

} // namespace

TEST_CASE("fb doc has a package's public API, its docs, and what their links name", "[doc]") {
    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_docs(std::filesystem::path(FB_TEST_PACKAGES) / "documented");
    REQUIRE(compiled);
    REQUIRE_FALSE(compiled->diagnostics.has_errors());
    fb::diagnostic_bag_t warnings;
    const fb::doc_package_t package =
        fb::document(*compiled, false, fb::build_mode_t::DEBUG, warnings);
    CHECK(package.name == "documented");
    CHECK(package.version == "0.3.0");
    REQUIRE(package.modules.size() == 1);
    const fb::doc_module_t& module = package.modules[0];
    CHECK(module.name == "documented.shapes");
    CHECK(module.file == "shapes.fb");
    REQUIRE(module.doc);
    CHECK(module.doc.value_or(fb::doc_t{}).summary ==
          "Shapes, and what they cover: see [Shape] and [Circle].");

    // Its Public declarations, and the Extends of them, in order: not its Private or Package ones.
    const std::string prefix = "documented.shapes.";
    std::vector<std::string> expected;
    for (const std::string_view name : {
             "Shown",
             "Shape",
             "Circle",
             "Named",
             "Square",
             "Kind",
             "TotalArea",
             "Pi",
             "Made",
             "Extend.Circle",
             "Shapes",
         }) {
        expected.push_back(prefix + std::string(name));
    }

    CHECK(paths_of(module.items) == expected);

    const fb::doc_item_t* attribute = item_at(module.items, prefix + "Shown");
    REQUIRE(attribute);
    CHECK(attribute->kind == "attribute");
    CHECK(attribute->signature == "Public Attribute Shown(Why: String)");
    CHECK(attribute->members.empty());

    const fb::doc_item_t* circle = item_at(module.items, prefix + "Circle");
    REQUIRE(circle);
    CHECK(circle->kind == "struct");
    CHECK(circle->signature == "Public Struct Circle Implements Shape");
    CHECK(circle->attributes == std::vector<std::string>{ R"(@Shown("it's round"))" });
    CHECK(circle->file == "shapes.fb");
    CHECK(circle->line == 27);
    CHECK(paths_of(circle->members) == std::vector<std::string>{ prefix + "Circle.Radius",
                                                                 prefix + "Circle.Area",
                                                                 prefix + "Circle.Grow" });
    REQUIRE(circle->doc);
    REQUIRE(circle->doc.value_or(fb::doc_t{}).examples.size() == 1);

    // Each link, found as the doc's declaration's code would find it.
    std::vector<std::pair<std::string, std::string>> links;
    links.reserve(circle->links.size());
    for (const fb::doc_target_t& link : circle->links) {
        links.emplace_back(link.text, link.builtin ? "built-in" : link.path);
    }

    CHECK(links == std::vector<std::pair<std::string, std::string>>{
                       { "Shape", prefix + "Shape" },
                       { "Area", prefix + "Circle.Area" },
                       { "Radius", prefix + "Circle.Radius" },
                       { "Circle.Grow", prefix + "Circle.Grow" },
                       { "Printable", "std.core.interfaces.Printable" },
                       { "Print", "std.core.print.Print" },
                       { "core.Print", "std.core.print.Print" },
                       { "String", "built-in" },
                       { "stacks.Stack", "std.collections.stack.Stack" },
                       { "Clamp", "std.core.values.Clamp" },
                       { "std.collections.stack.Stack.Push", "std.collections.stack.Stack.Push" },
                       { "Kind.Round", prefix + "Kind.Round" },
                       { "Missing", "" },
                       { "Circle.Missing", "" },
                   });

    // A member without a doc of its own has what it implements or overrides.
    const fb::doc_item_t* area = item_at(module.items, prefix + "Circle.Area");
    REQUIRE(area);
    REQUIRE(area->doc);
    CHECK(area->doc.value_or(fb::doc_t{}).summary ==
          "How much of the plane it covers, in square units.");
    CHECK(area->doc_from == prefix + "Shape.Area");
    const fb::doc_item_t* described = item_at(module.items, prefix + "Square.Describe");
    REQUIRE(described);
    CHECK(described->doc_from == prefix + "Named.Describe");
    REQUIRE(described->links.size() == 1);
    CHECK(described->links[0].path == prefix + "Named");

    const fb::doc_item_t* grow = item_at(module.items, prefix + "Circle.Grow");
    REQUIRE(grow);
    CHECK(grow->kind == "method");
    CHECK(grow->signature == "Public ByRef Method Grow(by: Float)");
    CHECK(grow->attributes == std::vector<std::string>{ "@PanicsAtCaller" });
    REQUIRE(grow->doc);
    CHECK(grow->doc.value_or(fb::doc_t{}).examples.size() == 3);

    // A Class's Protected members are its users', and a Private one isn't.
    const fb::doc_item_t* named = item_at(module.items, prefix + "Named");
    REQUIRE(named);
    CHECK(paths_of(named->members) == std::vector<std::string>{ prefix + "Named.Name",
                                                                prefix + "Named.Describe",
                                                                prefix + "Named.Hidden" });
    CHECK(named->members[2].visibility == fb::visibility_t::PROTECTED);

    const fb::doc_item_t* round = item_at(module.items, prefix + "Kind.Round");
    REQUIRE(round);
    CHECK(round->kind == "enum member");
    CHECK(round->signature == "Round = 1");
    CHECK(item_at(module.items, prefix + "Pi")->signature == "Public Const Pi: Float = 3.14159");
    CHECK(item_at(module.items, prefix + "Made")->signature == "Public Global Made = 0");

    // An Extend's members are its type's, in its module, and its links find its type's members.
    const fb::doc_item_t* extension = item_at(module.items, prefix + "Extend.Circle");
    REQUIRE(extension);
    CHECK(extension->kind == "extend");
    CHECK(extension->extends == prefix + "Circle");
    CHECK(paths_of(extension->members) == std::vector<std::string>{ prefix + "Circle.Halve" });
    REQUIRE(extension->members[0].links.size() == 1);
    CHECK(extension->members[0].links[0].path == prefix + "Circle.Grow");

    std::vector<std::string> warned;
    for (const fb::diagnostic_t& warning : warnings.diagnostics()) {
        const fb::source_file_t& file = compiled->sources.get(warning.span.file);
        warned.push_back(std::format(
            "{}: {}", file.location_of(warning.span.begin, fb::column_unit_t::UTF8).line + 1,
            warning.message));
    }

    CHECK(warned ==
          std::vector<std::string>{
              "19: 'Missing' names nothing that code here sees",
              "19: 'Circle.Missing' names nothing that code here sees",
              "142: this doc comment documents nothing: a /// comment goes on the lines right "
              "before a declaration",
              "144: a //! comment documents its module, so it goes at the top of its file, before "
              "its first line of code",
          });
}

namespace {

/// The paths fb doc should give std's users' declarations in a file: its Public ones, their
/// members that code outside std sees, and what its Extends add, found without fb doc.
void public_paths(const fb::ast_t& ast, const fb::source_file_t& file, const std::string& module,
                  std::vector<std::string>& paths) {
    const auto visibility = [](const fb::stmt_node_t& node) {
        return std::visit(
            [](const auto& declared) {
                if constexpr (requires { declared.modifiers; }) {
                    return declared.modifiers.visibility;
                } else {
                    return fb::visibility_t::PRIVATE;
                }
            },
            node);
    };
    const auto member_name = [&](const fb::stmt_node_t& node) {
        const auto* function = std::get_if<fb::function_stmt_t>(&node);
        const std::string name(fb::declared_name(node)->text);
        return function != nullptr && function->kind == fb::function_kind_t::OPERATOR
                   ? "Operator" + name
                   : name;
    };
    // An Operator has its type's visibility, and an Override that says none what it overrides',
    // which in std is always seen.
    const auto typed = [](const fb::stmt_node_t& node) {
        const auto* function = std::get_if<fb::function_stmt_t>(&node);
        return function != nullptr &&
               (function->kind == fb::function_kind_t::OPERATOR ||
                (function->modifiers.is_override && !function->modifiers.visibility_span));
    };

    for (const fb::stmt_id_t id : ast.items()) {
        const fb::stmt_node_t& node = ast.get(id).node;
        if (const auto* block = std::get_if<fb::extern_stmt_t>(&node)) {
            for (const fb::stmt_id_t function : block->functions) {
                if (visibility(ast.get(function).node) == fb::visibility_t::PUBLIC) {
                    paths.push_back(
                        std::format("{}.{}", module, member_name(ast.get(function).node)));
                }
            }

            continue;
        }

        if (const auto* extension = std::get_if<fb::extend_stmt_t>(&node)) {
            const fb::type_expr_t& extended = ast.get(extension->type);
            const auto* named = std::get_if<fb::named_type_t>(&extended.node);
            std::string type = named != nullptr ? std::string(named->name) : "";
            if (named == nullptr) {
                for (const char c : file.slice(extended.span)) {
                    type += c == ' ' ? std::string() : std::string(1, c);
                }
            }

            for (const fb::stmt_id_t member : extension->members) {
                if (visibility(ast.get(member).node) == fb::visibility_t::PUBLIC ||
                    typed(ast.get(member).node)) {
                    paths.push_back(
                        std::format("{}.{}.{}", module, type, member_name(ast.get(member).node)));
                }
            }

            continue;
        }

        const std::optional<fb::name_t> name = fb::declared_name(node);
        if (!name || visibility(node) != fb::visibility_t::PUBLIC) {
            continue;
        }

        const std::string path = std::format("{}.{}", module, name->text);
        paths.push_back(path);
        if (const auto* declared = std::get_if<fb::enum_stmt_t>(&node)) {
            for (const fb::enum_member_t& member : declared->members) {
                paths.push_back(std::format("{}.{}", path, member.name.text));
            }
        }

        const auto* type = std::get_if<fb::type_stmt_t>(&node);
        if (type == nullptr || type->is_attribute) {
            continue;
        }

        for (const fb::stmt_id_t member : type->members) {
            const fb::visibility_t seen = visibility(ast.get(member).node);
            if (type->is_interface || seen == fb::visibility_t::PUBLIC ||
                (type->is_class && seen == fb::visibility_t::PROTECTED) ||
                typed(ast.get(member).node)) {
                paths.push_back(std::format("{}.{}", path, member_name(ast.get(member).node)));
            }
        }
    }
}

/// Every item's path, its members' too.
void paths_in(const std::vector<fb::doc_item_t>& items, std::vector<std::string>& all) {
    for (const fb::doc_item_t& item : items) {
        all.push_back(item.path);
        paths_in(item.members, all);
    }
}

} // namespace

TEST_CASE("fb doc std has every Public declaration of std's, each with a doc of its own", "[doc]") {
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_standard_docs();
    REQUIRE(compiled);
    REQUIRE_FALSE(compiled->diagnostics.has_errors());
    fb::diagnostic_bag_t warnings;
    const fb::doc_package_t package =
        fb::document(*compiled, true, fb::build_mode_t::DEBUG, warnings);
    CHECK(package.name == "std");
    // std's docs' links each name something, and each of its doc comments is on something.
    for (const fb::diagnostic_t& warning : warnings.diagnostics()) {
        const fb::source_file_t& file = compiled->sources.get(warning.span.file);
        UNSCOPED_INFO(
            std::format("{}:{}: {}", file.path(),
                        file.location_of(warning.span.begin, fb::column_unit_t::UTF8).line + 1,
                        warning.message));
    }

    CHECK(warnings.diagnostics().empty());

    // Each module has a //! summary, and each item, an Extend too, a /// one of its own, as fb doc
    // --check checks.
    fb::diagnostic_bag_t missing;
    fb::check_docs(package, missing);
    for (const fb::diagnostic_t& error : missing.diagnostics()) {
        const fb::source_file_t& file = compiled->sources.get(error.span.file);
        UNSCOPED_INFO(std::format(
            "{}:{}: {}", file.path(),
            file.location_of(error.span.begin, fb::column_unit_t::UTF8).line + 1, error.message));
    }

    CHECK(missing.diagnostics().empty());
    std::vector<std::string> all;
    for (const fb::doc_module_t& module : package.modules) {
        paths_in(module.items, all);
    }

    // Each file's Public declarations, as its own parse finds them, are in it.
    const std::expected<std::vector<fb::library_file_t>, std::string> library =
        fb::standard_library();
    REQUIRE(library);
    std::ranges::sort(all);
    const std::vector<std::string_view> defined = fb::defined_symbols(fb::build_mode_t::DEBUG);
    std::vector<std::string> absent;
    for (const fb::library_file_t& file : *library) {
        if (!file.path.ends_with(".fb")) {
            continue;
        }

        const parsed_t parsed(file.text, file.path, defined);
        std::vector<std::string> paths;
        public_paths(parsed.ast, parsed.file, fb::module_name(file.path), paths);
        for (const std::string& path : paths) {
            if (!std::ranges::binary_search(all, path)) {
                absent.push_back(path);
            }
        }
    }

    for (const std::string& path : absent) {
        UNSCOPED_INFO(path);
    }

    CHECK(absent.empty());
    CHECK(all.size() > 3000);
}

namespace {

/// Whether an item's doc, or one of its members', has an example.
bool exemplified(const fb::doc_item_t& item) {
    return (item.doc && !item.doc->examples.empty()) ||
           std::ranges::any_of(item.members, exemplified);
}

/// Each doc of `items`, and their members', of their own, with its item's path.
void docs_of(const std::vector<fb::doc_item_t>& items,
             std::vector<std::pair<std::string, const fb::doc_t*>>& found) {
    for (const fb::doc_item_t& item : items) {
        if (item.doc && item.doc_from.empty()) {
            found.emplace_back(item.path, &*item.doc);
        }

        docs_of(item.members, found);
    }
}

} // namespace

TEST_CASE("each of std's types and functions has an example and summaries read whole", "[doc]") {
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_standard_docs();
    REQUIRE(compiled);
    fb::diagnostic_bag_t warnings;
    const fb::doc_package_t package =
        fb::document(*compiled, true, fb::build_mode_t::DEBUG, warnings);

    // Each top-level Class, Struct, Interface, and Function has an example that fb test runs or
    // compiles, in its own doc, a member's or an overload's, or its module's. An iterator is left
    // out, since a For loop makes it, not code; so is what this list names, with why.
    const std::vector<std::string_view> allowed = {};
    std::vector<std::string> missing;
    for (const fb::doc_module_t& module : package.modules) {
        if (module.doc && !module.doc->examples.empty()) {
            continue;
        }

        for (const fb::doc_item_t& item : module.items) {
            const bool kind = item.kind == "class" || item.kind == "struct" ||
                              item.kind == "interface" || item.kind == "function";
            if (!kind || item.signature.contains("Implements Iterator<") ||
                std::ranges::contains(allowed, item.path)) {
                continue;
            }

            const bool shown = std::ranges::any_of(module.items, [&](const fb::doc_item_t& other) {
                return other.path == item.path && exemplified(other);
            });
            if (!shown && !std::ranges::contains(missing, item.path)) {
                missing.push_back(item.path);
            }
        }
    }

    for (const std::string& path : missing) {
        UNSCOPED_INFO(path);
    }

    CHECK(missing.empty());

    // A summary is a sentence that says what its item is or does: it isn't cut off by a "...", or
    // by a '?' that ends no question, as a T? does; "an" goes before a type whose name starts with
    // a vowel's sound; and it's more than a word for what its item's type is, or a heading.
    std::vector<std::pair<std::string, const fb::doc_t*>> docs;
    for (const fb::doc_module_t& module : package.modules) {
        if (module.doc) {
            docs.emplace_back(module.name, &*module.doc);
        }

        docs_of(module.items, docs);
    }

    std::vector<std::string> wrong;
    for (const auto& [path, doc] : docs) {
        const std::string& summary = doc->summary;
        const bool question = summary.starts_with("Is ") || summary.starts_with("Does ") ||
                              summary.starts_with("Can ") || summary.starts_with("What ");
        const bool cut = summary.ends_with("...") || (summary.ends_with('?') && !question);
        bool article = false;
        for (const std::string_view type :
             { " a Immutable", " a Interval", " a Array", " a Lru", " a Int", " a Optional" }) {
            article = article || summary.contains(type);
        }

        // "Args' Static Methods." names a heading, where "What reads INI files, in its Static
        // Methods." says what they do.
        const bool heading =
            summary.ends_with("Static Methods.") && std::ranges::count(summary, ' ') <= 2;
        const bool vague = heading || summary == "By SHA-256." || summary == "By SHA-512." ||
                           summary == "A Double." || summary == "A BOOL's.";
        if (cut || article || vague) {
            wrong.push_back(std::format("{}: {}", path, summary));
        }
    }

    for (const std::string& each : wrong) {
        UNSCOPED_INFO(each);
    }

    CHECK(wrong.empty());

    // What a review found wrong in these docs stays fixed: each says what its own item does, not
    // its neighbor's or its group's, and an iterator's says what the loop gives, not its fields.
    const auto text = [&](std::string_view path) {
        std::string all;
        for (const auto& [each, doc] : docs) {
            if (each == path) {
                all += doc->summary + "\n" + doc->markdown + "\n";
            }
        }

        // A doc that moved away would pass what's below without this.
        INFO(path);
        CHECK_FALSE(all.empty());
        return all;
    };
    CHECK_FALSE(text("std.time.zone").contains("has had and has have"));
    CHECK_FALSE(text("std.time.timers.FixedStep.Step").contains("in seconds"));
    CHECK(text("std.time.timers.RateLimiter.TryTake").contains("Takes a token"));
    CHECK_FALSE(text("std.time.zone.ZonedDateTime.Offset").contains("abbreviation"));
    CHECK_FALSE(text("std.time.calendar.Date.StartOfWeek").contains("its month"));
    CHECK_FALSE(text("std.time.timestamp.Timestamp.UnixMilliseconds").contains("2262"));
    CHECK_FALSE(text("std.math.big.decimal.Decimal.Ln").contains("base 10"));
    CHECK_FALSE(text("std.time.duration.Duration.MaxValue").contains("either way"));
    CHECK(text("std.term.terminal.FakeTerminal.ReadInput").contains("-2"));
    CHECK_FALSE(text("std.concurrent.concurrent_map.ConcurrentMapIterator").contains("began"));
    CHECK_FALSE(text("std.concurrent.locks.WaitGroup.Add").contains("to the `count`"));
    std::vector<std::string> internals;
    for (const auto& [path, doc] : docs) {
        if (!path.ends_with("Iterator")) {
            continue;
        }

        for (const std::string_view internal :
             { "FrontLeft", "BackAt", "on the way down", "-1 before" }) {
            if ((doc->summary + doc->markdown).contains(internal)) {
                internals.push_back(std::format("{}: {}", path, internal));
            }
        }
    }

    for (const std::string& each : internals) {
        UNSCOPED_INFO(each);
    }

    CHECK(internals.empty());

    // An example that needs no network, server, process, or person runs.
    for (const std::string_view path :
         { "std.log.files.RotatingFileSink", "std.crypto.pem.Certificate" }) {
        INFO(path);
        bool runs = false;
        for (const auto& [each, doc] : docs) {
            runs =
                runs || (each == path &&
                         std::ranges::any_of(doc->examples, [](const fb::doc_example_t& example) {
                             return example.kind == fb::example_kind_t::RUN;
                         }));
        }

        CHECK(runs);
    }
}

namespace {

/// Writes a package's files into the test output folder, after removing what was there.
std::filesystem::path write_package(const std::string_view name,
                                    const std::vector<std::pair<std::string, std::string>>& files) {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "doc" / name;
    std::filesystem::remove_all(folder);
    for (const auto& [path, text] : files) {
        const std::filesystem::path file = folder / path;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    return folder;
}

/// A compilation's diagnostics, as "<line>: <message>".
std::vector<std::string> lines_of(const fb::compilation_t& compilation,
                                  const fb::diagnostic_bag_t& diagnostics) {
    std::vector<std::string> found;
    for (const fb::diagnostic_t& diagnostic : diagnostics.diagnostics()) {
        const fb::source_file_t& file = compilation.sources.get(diagnostic.span.file);
        found.push_back(std::format(
            "{}: {}", file.location_of(diagnostic.span.begin, fb::column_unit_t::UTF8).line + 1,
            diagnostic.message));
    }

    return found;
}

constexpr std::string_view reviewed_shapes = R"(//! One.

//! Two.
//!
//! # Examples
//!
//! ```
//! Function Example(): Int => 2
//!
//! Expect(Twice(Example()) == 4)
//! ```

Import std.core
Import reviewed.other As things

/// A vector, as [things] has none, and [things.Animal.secret] names nothing.
Public Struct V
    /// Its x.
    Public Field X: Int

    /// Adds two.
    Operator +(a: V, b: V): V => V(a.X + b.X)
End Struct

/// A dog.
Public Class Dog Extends things.Animal
    Override Method Speak(): String => "woof"
End Class

/// Polite, as [Polite.Greet] says.
Public Struct Polite Implements things.Greeter
End Struct

/// Holds [T].
Public Class Box<T>
End Class

/// Colors.
Public Enum Color
    /// Red.
    Red,
    /// Green.
    Green
End Enum

/// What bytes have, as [Size] says.
Extend UInt8[]
    /// How many.
    Public Method Size(): Int => Count
End Extend

/// Its body's.
Public Function Body(): Int
    /// A Const in a Function's code.
    Const K = 2
    Return K
End Function

/// On nothing,
/// in two lines.

/// Twice an Int.
///
/// # Examples
///
/// ```
/// Expect(Twice(1) == 2)
/// ```
Public Function Twice(n: Int): Int => n * 2

/// Twice a Float.
///
/// # Examples
///
/// ```
/// Expect(Twice(1.5) == 3.0)
/// ```
Public Function Twice(n: Float): Float => n * 2

/// `text`'s number.
///
/// # Examples
///
/// ```
/// Const Size = 3
/// /// A doc in an example, on a line the example's module doesn't have.
/// Function Doubled(): Int => Size * 2
///
/// Local number = Try Parsed("12")
/// Expect(number == 12 && Doubled() == 6)
/// ```
Public Function Parsed(text: String): Int Throws ArgumentError
    Local value = Int.Parse(text)
    If value == Null Then Throw ArgumentError(ArgumentErrorKind.INVALID, "not a number")
    Return value
End Function

/// An Extern block has no doc of its own.
Extern "C"
    /// Nothing.
    Function nothing()
End Extern
)";

constexpr std::string_view reviewed_other = R"(//! Other things.

Import std.core

/// What greets.
Public Interface Greeter
    /// Greets.
    Method Greet(): String => "hi"
End Interface

/// An animal.
Public Class Animal
    /// Its name.
    Public Field Name: String
    Static Field secret: Int

    /// Speaks.
    Public Method Speak(): String => "..."
End Class
)";

} // namespace

TEST_CASE("fb doc has Operators and Overrides, finds links as code would, and warns once",
          "[doc]") {
    const std::filesystem::path folder = write_package(
        "reviewed",
        {
            {
                "project.toml",
                "[project]\nname = \"reviewed\"\nversion = \"1.0.0\"\ntype = \"library\"\n",
            },
            { "shapes.fb", std::string(reviewed_shapes) },
            { "other.fb", std::string(reviewed_other) },
        });
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_docs(folder);
    REQUIRE(compiled);
    REQUIRE_FALSE(compiled->diagnostics.has_errors());
    fb::diagnostic_bag_t warnings;
    const fb::doc_package_t package =
        fb::document(*compiled, false, fb::build_mode_t::DEBUG, warnings);
    // A private member of another file's names nothing, a Function's code has no docs, a comment
    // on nothing warns once, and an Extern block's comment says why it's on nothing.
    CHECK(lines_of(*compiled, warnings) ==
          std::vector<std::string>{
              "16: 'things.Animal.secret' names nothing that code here sees",
              "54: this doc comment documents nothing: a /// comment goes on the lines right "
              "before a declaration",
              "59: this doc comment documents nothing: a /// comment goes on the lines right "
              "before a declaration",
              "98: this doc comment documents nothing: an Extern block has no doc of its own, but "
              "its Functions do",
          });
    const auto module =
        std::ranges::find(package.modules, "reviewed.shapes", &fb::doc_module_t::name);
    REQUIRE(module != package.modules.end());
    // The //! runs are one doc, whose example runs.
    const fb::doc_t doc = module->doc.value_or(fb::doc_t{});
    CHECK(doc.summary == "One.");
    CHECK(doc.markdown.starts_with("One.\n\nTwo.\n"));
    REQUIRE(doc.examples.size() == 1);

    const std::string prefix = "reviewed.shapes.";
    const fb::doc_item_t* plus = item_at(module->items, prefix + "V.Operator+");
    REQUIRE(plus);
    CHECK(plus->kind == "operator");
    CHECK(plus->doc.value_or(fb::doc_t{}).summary == "Adds two.");
    const fb::doc_item_t* speak = item_at(module->items, prefix + "Dog.Speak");
    REQUIRE(speak);
    CHECK(speak->visibility == fb::visibility_t::PUBLIC);
    CHECK(speak->doc_from == "reviewed.other.Animal.Speak");
    CHECK(item_at(module->items, prefix + "Color.Green")->doc.value_or(fb::doc_t{}).summary ==
          "Green.");

    // An alias's path is its module's, a qualified Interface's members are found, and a type
    // parameter isn't a link.
    const fb::doc_item_t* vector = item_at(module->items, prefix + "V");
    REQUIRE(vector);
    REQUIRE(vector->links.size() == 2);
    CHECK(vector->links[0].path == "reviewed.other");
    CHECK(vector->links[1].path.empty());
    const fb::doc_item_t* polite = item_at(module->items, prefix + "Polite");
    REQUIRE(polite);
    REQUIRE(polite->links.size() == 1);
    CHECK(polite->links[0].path == "reviewed.other.Greeter.Greet");
    CHECK(item_at(module->items, prefix + "Box")->links.empty());

    // An Extend of one array type is that type's, not every T[]'s.
    const fb::doc_item_t* bytes = item_at(module->items, prefix + "Extend.UInt8[]");
    REQUIRE(bytes);
    CHECK(bytes->name == "UInt8[]");
    CHECK(bytes->extends == "UInt8[]");
    // An Extend's doc says what it adds, and its links are found from inside it.
    CHECK(bytes->doc.value_or(fb::doc_t{}).summary == "What bytes have, as [Size] says.");
    REQUIRE(bytes->links.size() == 1);
    CHECK(bytes->links[0].path == prefix + "UInt8[].Size");
    CHECK(paths_of(bytes->members) == std::vector<std::string>{ prefix + "UInt8[].Size" });
}

TEST_CASE("an example sees its package's Package names, as the package's files do", "[doc]") {
    const std::filesystem::path folder = write_package(
        "inside_tests",
        {
            {
                "project.toml",
                "[project]\nname = \"inside\"\nversion = \"1.0.0\"\ntype = \"library\"\n",
            },
            {
                "math.fb",
                "/// Twice n.\n///\n/// # Examples\n///\n/// ```\n/// Expect(Twice(2) == 4)\n/// "
                "```\nPackage Function Twice(n: Int): Int => n * 2\n\n/// Three.\n///\n/// # "
                "Examples\n///\n/// ```\n/// Expect(Three() == 3)\n/// ```\nFunction Three(): "
                "Int => 3\n",
            },
        });
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_tests(folder);
    REQUIRE(compiled);
    // A Private one's example is outside its file, so it can't call it.
    const std::vector<std::string> lines = lines_of(*compiled, compiled->diagnostics);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().starts_with("15: 'Three' is private to "));
    CHECK(lines.front().ends_with("math.fb"));
}

TEST_CASE("fb test runs a module's examples, ones that throw and declare, and overloads' apart",
          "[doc]") {
    const std::filesystem::path folder = write_package(
        "reviewed_tests",
        {
            {
                "project.toml",
                "[project]\nname = \"reviewed\"\nversion = \"1.0.0\"\ntype = \"library\"\n",
            },
            { "shapes.fb", std::string(reviewed_shapes) },
            { "other.fb", std::string(reviewed_other) },
        });
    const std::expected<fb::compilation_t, std::string> compiled = fb::compile_tests(folder);
    REQUIRE(compiled);
    for (const fb::diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
        UNSCOPED_INFO(diagnostic.message);
    }

    // An example can Try what throws, a Const and a doc comment in it are its, and it can
    // declare a Function named as fb's test once was.
    REQUIRE_FALSE(compiled->diagnostics.has_errors());
    std::vector<std::string> names;
    for (const fb::test_case_t& test : fb::package_tests(*compiled)) {
        names.push_back(test.name);
    }

    CHECK(names == std::vector<std::string>{
                       "reviewed.shapes (example)",
                       "reviewed.shapes.Twice (example)",
                       "reviewed.shapes.Twice (example 2)",
                       "reviewed.shapes.Parsed (example)",
                   });

    // A file whose module no Import can name can't have its examples run, which says why.
    const std::filesystem::path unnamed = write_package(
        "unnamed",
        {
            {
                "project.toml",
                "[project]\nname = \"unnamed\"\nversion = \"1.0.0\"\ntype = \"library\"\n",
            },
            {
                "my-lib.fb",
                "/// One.\n///\n/// # Examples\n///\n/// ```\n/// "
                "Expect(One() == 1)\n/// ```\nPublic Function One(): Int => "
                "1\n",
            },
        });
    const std::expected<fb::compilation_t, std::string> stopped = fb::compile_tests(unnamed);
    REQUIRE(stopped);
    CHECK(lines_of(*stopped, stopped->diagnostics) ==
          std::vector<std::string>{
              "5: fb test can't run this example: it imports its file's module, unnamed.my-lib, "
              "which isn't a name an Import takes, as a file's name with a '-' in it isn't",
          });
}

TEST_CASE("a heading's links are links, and a backtick that nothing closes is a backtick",
          "[doc]") {
    const fb::doc_t heading = read("# See [Stack]\n\nText.");
    CHECK(link_targets(heading) == std::vector<std::string>{ "Stack" });
    // As CommonMark has it, a run of backticks opens code only when a run as long closes it.
    const fb::doc_t open = read("Use ` and see [Stack] here. Next.");
    CHECK(open.summary == "Use ` and see [Stack] here.");
    CHECK(link_targets(open) == std::vector<std::string>{ "Stack" });
    const fb::doc_t closed = read("Use ``a ` [Code]`` and [Stack]. Next.");
    CHECK(closed.summary == "Use ``a ` [Code]`` and [Stack].");
    CHECK(link_targets(closed) == std::vector<std::string>{ "Stack" });
    // A code span across lines still hides what's in it.
    CHECK(link_targets(read("A `span\n[Hidden]` and [Shown].")) ==
          std::vector<std::string>{ "Shown" });
}

TEST_CASE("an Enum member after a comma, the //! runs, and a Function's code take docs as the "
          "spec says",
          "[doc]") {
    const parsed_t parsed(R"(//! One.

//! Two,
//! three.
// An ordinary comment.
//! Four.

Import std.core

Public Enum Color
    /// Red.
    Red,
    /// Green.
    Green,
    Blue = 3,
    /// Large.
    Large
End Enum

Public Function F(): Int
    /// A Const in a Function's code.
    Const K = 2
    Return K
End Function
)");
    const fb::doc_comment_t* module = parsed.ast.module_doc(parsed.file.id());
    REQUIRE(module != nullptr);
    CHECK(module->text == "One.\n\nTwo,\nthree.\n\nFour.");
    REQUIRE(module->lines.size() == 6);
    CHECK(parsed.file.slice(module->lines[2]) == "Two,");
    CHECK(parsed.doc("Green", "    Green") == "Green.");
    CHECK(parsed.doc("Blue") == std::nullopt);
    CHECK(parsed.doc("Large", "    Large") == "Large.");
    CHECK(parsed.doc("K", "Const K") == std::nullopt);
}

TEST_CASE("a file made from another's lines shows them where they come from", "[doc]") {
    fb::source_manager_t sources;
    const fb::file_id_t original = sources.add("box.fb", "    /// Expect(Box().Holds(1))\n");
    const fb::file_id_t made =
        sources.add("box.fb", "@Test Function Example()\nExpect(Box().Holds(1))\nEnd Function\n",
                    fb::line_origin_t{
                        .file = original,
                        .lines = { { .line = 0, .column = 4 },
                                   { .line = 0, .column = 8 },
                                   { .line = 0, .column = 4 },
                                   { .line = 0, .column = 4 }, },
                        .made = { true, false, true, true },
                    });
    const fb::source_file_t& file = sources.get(made);
    CHECK(file.location_of(38, fb::column_unit_t::CODEPOINT) ==
          fb::location_t{ .line = 0, .column = 21 });
    // A line made for the file is all at its place, which a column past its text can't leave.
    CHECK(file.location_of(15, fb::column_unit_t::CODEPOINT) ==
          fb::location_t{ .line = 0, .column = 4 });
    // Its own line, which a doc comment there is looked for on, whatever its origin.
    CHECK(file.line_of(38) == 1);
    const fb::span_t shown = sources.shown({ .file = made, .begin = 38, .end = 43 });
    CHECK(shown.file == original);
    CHECK(sources.get(original).slice(shown) == "Holds");
    // Elsewhere a span is itself.
    const fb::span_t own{ .file = original, .begin = 1, .end = 2 };
    CHECK(sources.shown(own) == own);
}
