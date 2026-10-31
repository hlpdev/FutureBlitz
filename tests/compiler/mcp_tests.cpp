#include "fb/driver.hpp"
#include "fb/mcp.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using json_t = nlohmann::json;

/// The answer to a request, as JSON.
json_t ask(fb::mcp_server_t& server, const json_t& request) {
    const std::optional<std::string> answer = server.handle(request.dump());
    REQUIRE(answer);
    return json_t::parse(*answer);
}

/// A tool's result: its text, and whether it's an error.
struct called_t {
    std::string text;
    bool error = false;
};

called_t call(fb::mcp_server_t& server, const std::string_view tool, const json_t& arguments) {
    static int next = 100;
    json_t request{ { "jsonrpc", "2.0" }, { "id", ++next }, { "method", "tools/call" } };
    request["params"] = { { "name", tool }, { "arguments", arguments } };
    const json_t answer = ask(server, request);
    REQUIRE(answer.contains("result"));
    const json_t& result = answer.at("result");
    REQUIRE(result.at("content").size() == 1);
    CHECK(result.at("content")[0].at("type") == "text");
    std::string text = result.at("content")[0].at("text").get<std::string>();
    return { .text = std::move(text), .error = result.at("isError").get<bool>() };
}

json_t initialize(fb::mcp_server_t& server, const std::string_view version = "2025-06-18") {
    json_t request{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" } };
    request["params"] = { { "protocolVersion", version }, { "capabilities", json_t::object() } };
    request["params"]["clientInfo"] = { { "name", "test" }, { "version", "1" } };
    return ask(server, request);
}

/// One server for the tests that search, since reading std's reference takes seconds.
fb::mcp_server_t& searcher() {
    static fb::mcp_server_t server(FB_TEST_OUTPUT);
    static const json_t initialized = initialize(server);
    return server;
}

/// A new, empty folder for a test's package.
std::filesystem::path package_folder(const std::string_view name) {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "mcp" / name;
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

void write_file(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

/// How many results a search gave: its lines that start with "## ".
std::size_t results(const std::string_view text) {
    std::size_t count = text.starts_with("## ") ? 1 : 0;
    for (std::size_t at = text.find("\n## "); at != std::string_view::npos;
         at = text.find("\n## ", at + 1)) {
        ++count;
    }

    return count;
}

/// The first result's title.
std::string first_title(const std::string_view text) {
    REQUIRE(text.starts_with("## "));
    return std::string(text.substr(3, text.find('\n') - 3));
}

} // namespace

TEST_CASE("fb mcp answers MCP's lifecycle and JSON-RPC's errors", "[mcp]") {
    fb::mcp_server_t server(FB_TEST_OUTPUT);
    const json_t initialized = initialize(server);
    CHECK(initialized.at("id") == 1);
    const json_t& result = initialized.at("result");
    CHECK(result.at("protocolVersion") == "2025-06-18");
    CHECK(result.at("capabilities").at("tools").is_object());
    CHECK(result.at("serverInfo").at("name") == "fb");
    CHECK(result.at("instructions") == fb::agents_primer());

    // An older version it speaks is the client's, and one it doesn't is its newest.
    CHECK(initialize(server, "2024-11-05").at("result").at("protocolVersion") == "2024-11-05");
    CHECK(initialize(server, "1999-01-01").at("result").at("protocolVersion") == "2025-06-18");

    // A notification has no answer.
    CHECK_FALSE(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    CHECK(ask(server, json_t{ { "jsonrpc", "2.0" }, { "id", "p" }, { "method", "ping" } }) ==
          json_t{ { "jsonrpc", "2.0" }, { "id", "p" }, { "result", json_t::object() } });

    const json_t listed =
        ask(server, json_t{ { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" } });
    std::vector<std::string> names;
    for (const json_t& tool : listed.at("result").at("tools")) {
        names.push_back(tool.at("name").get<std::string>());
        CHECK_FALSE(tool.at("description").get<std::string>().empty());
        CHECK(tool.at("inputSchema").at("type") == "object");
    }

    CHECK(names == std::vector<std::string>{ "search_docs", "lookup", "check", "format" });

    const auto error_of = [&](const std::string_view line) {
        const std::optional<std::string> answer = server.handle(line);
        REQUIRE(answer);
        const json_t parsed = json_t::parse(*answer);
        REQUIRE(parsed.contains("error"));
        return std::pair{ parsed.at("id"), parsed.at("error").at("code").get<int>() };
    };
    CHECK(error_of("not json") == std::pair{ json_t(), -32700 });
    CHECK(error_of("[1, 2]") == std::pair{ json_t(), -32600 });
    CHECK(error_of(R"({"jsonrpc":"2.0","id":3})") == std::pair{ json_t(3), -32600 });
    CHECK(error_of(R"({"jsonrpc":"2.0","id":4,"method":"resources/list"})") ==
          std::pair{ json_t(4), -32601 });
    CHECK(error_of(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{}})") ==
          std::pair{ json_t(5), -32602 });
    CHECK(error_of(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nope"}})") ==
          std::pair{ json_t(6), -32602 });
    CHECK(
        error_of(
            R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"lookup","arguments":{"name":3}}})") ==
        std::pair{ json_t(7), -32602 });
}

TEST_CASE("fb mcp's primer is a program that compiles", "[mcp]") {
    const std::string_view primer = fb::agents_primer();
    const std::size_t begin = primer.find("```fb\n");
    REQUIRE(begin != std::string_view::npos);
    const std::size_t end = primer.find("```", begin + 6);
    const fb::compilation_t compiled =
        fb::compile("primer.fb", std::string(primer.substr(begin + 6, end - begin - 6)));
    CHECK(compiled.diagnostics.diagnostics().empty());
    CHECK_FALSE(primer.contains('\r'));
}

TEST_CASE("fb mcp's search_docs finds the spec's sections and std's declarations", "[mcp]") {
    fb::mcp_server_t& server = searcher();
    const called_t loop = call(server, "search_docs", { { "query", "For loop" } });
    CHECK_FALSE(loop.error);
    CHECK(first_title(loop.text).ends_with("Control Flow > For"));
    CHECK(loop.text.contains("(docs/spec/04-control-flow.md)"));
    CHECK(loop.text.contains("End For"));
    CHECK(results(loop.text) == 6);

    CHECK(
        first_title(call(server, "search_docs", { { "query", "how do I split a string" } }).text) ==
        "std.text.string.String.Split");
    CHECK(first_title(call(server, "search_docs", { { "query", "Select Case" } }).text)
              .ends_with("Control Flow > Select"));

    // Overloads, which share a name, are one result with each of them.
    const called_t map = call(server, "search_docs", { { "query", "Map" }, { "limit", 10 } });
    CHECK(results(map.text) == 10);
    std::vector<std::string> titles;
    for (std::size_t at = map.text.find("## "); at != std::string::npos;
         at = map.text.find("\n## ", at + 1)) {
        const std::size_t from = map.text[at] == '\n' ? at + 4 : at + 3;
        titles.push_back(map.text.substr(from, map.text.find('\n', from) - from));
    }

    std::vector<std::string> unique = titles;
    std::ranges::sort(unique);
    CHECK(std::ranges::adjacent_find(unique) == unique.end());
    CHECK(std::ranges::contains(titles, "std.collections.map.Map"));

    CHECK(results(call(server, "search_docs", { { "query", "Map" }, { "limit", 2 } }).text) == 2);
    CHECK(results(call(server, "search_docs", { { "query", "Map" }, { "limit", -4 } }).text) == 1);

    // Long sections are cut at a line, never inside a character, as JSON has to have UTF-8.
    std::size_t cut = 0;
    for (const std::string_view query :
         { "types", "operators", "string interpolation", "generics" }) {
        INFO(query);
        const called_t found = call(server, "search_docs", { { "query", query }, { "limit", 20 } });
        cut += found.text.contains("\n...\n") ? 1U : 0U;
        CHECK_FALSE(found.text.contains("\xEF\xBF\xBD"));
        // nlohmann/json throws on a string that isn't UTF-8.
        CHECK_NOTHROW(json_t(found.text).dump());
    }

    CHECK(cut >= 3);

    const called_t none = call(server, "search_docs", { { "query", "zzqqxv" } });
    CHECK_FALSE(none.error);
    CHECK(none.text == "Nothing in the spec or std's reference has 'zzqqxv'.");
    const called_t empty = call(server, "search_docs", { { "query", "the a ?!" } });
    CHECK(empty.error);
    CHECK(empty.text == "The query has no words to search for.");
}

TEST_CASE("fb mcp's lookup finds the package's names, std's, and the built-in ones", "[mcp]") {
    fb::mcp_server_t& server = searcher();
    const called_t print = call(server, "lookup", { { "name", "Print" } });
    CHECK_FALSE(print.error);
    CHECK(print.text.contains("## std.core.print.Print\n(std/core/print.fb:"));
    CHECK(print.text.contains("Public Function Print(values: Printable...)"));
    CHECK(call(server, "lookup", { { "name", "std.text.string.String.Split" } })
              .text.starts_with("## std.text.string.String.Split\n"));

    const called_t panic = call(server, "lookup", { { "name", "panic" } });
    CHECK(panic.text.contains("## Panic\n(built in)\n\nFunction Panic("));

    const called_t none = call(server, "lookup", { { "name", "Zzqqxv" } });
    CHECK_FALSE(none.error);
    CHECK(none.text.starts_with("Nothing in the package or std is named 'Zzqqxv'"));
    CHECK(call(server, "lookup", { { "name", "" } }).error);

    // A package's own, as they're saved now.
    const std::filesystem::path folder = package_folder("lookup");
    write_file(folder / "project.toml", "[project]\nname = \"shop\"\nversion = \"0.1.0\"\n");
    write_file(folder / "main.fb", "Import std.core\nImport \"./prices\"\n\nPrint(Total(2))\n");
    write_file(folder / "prices.fb",
               "Public Function Total(count: Int): Int\n    Return count * Price\nEnd Function\n\n"
               "Public Const Price = 3\n");
    fb::mcp_server_t package(folder);
    const called_t total = call(package, "lookup", { { "name", "total" } });
    // Paths are as fb check shows them, from the folder fb mcp runs in.
    CHECK(total.text.starts_with("## Total\n("));
    CHECK(total.text.contains("prices.fb:1, in the package)\n\n"
                              "Public Function Total(count: Int): Int\n"));
    CHECK(call(package, "lookup", { { "name", "prices.Price" } })
              .text.contains("prices.fb:5, in the package)\n\nPublic Const Price = 3\n"));
    write_file(folder / "prices.fb", "\n\nPublic Function Total(count: Int): Int => count\n");
    CHECK(call(package, "lookup", { { "name", "Total" } })
              .text.contains("prices.fb:3, in the package)\n\n"));
}

TEST_CASE("fb mcp's check gives a snippet's, a file's, and a package's errors", "[mcp]") {
    fb::mcp_server_t server(FB_TEST_OUTPUT);
    CHECK(call(server, "check", { { "code", "Import std.core\n\nPrint(\"hi\")\n" } }).text ==
          "No errors or warnings.");

    const called_t wrong =
        call(server, "check",
             { { "code", "Import std.core\n\nprint(\"hi\")\nLocal n: Int = \"no\"\n" } });
    CHECK_FALSE(wrong.error);
    CHECK(wrong.text.starts_with(
        "2 errors, 0 warnings.\n\nmain.fb:3:1: error: 'print' isn't declared\n"));
    CHECK(wrong.text.contains("main.fb:4:16: error: "));

    // A fix says what it puts where.
    const called_t miscased =
        call(server, "check", { { "code", "Import std.core\n\nif True Then Print(\"yes\")\n" } });
    CHECK(miscased.text.contains("  fix: at line 3, put `If` in place of `if`\n"));

    const std::filesystem::path folder = package_folder("check");
    write_file(folder / "project.toml", "[project]\nname = \"game\"\nversion = \"0.1.0\"\n");
    write_file(folder / "main.fb", "Import std.core\nImport \"./score\"\n\nPrint(Doubled(2))\n");
    write_file(folder / "score.fb", "Public Function Doubled(n: Int): Int => n * 2\n");
    fb::mcp_server_t package(folder);
    CHECK(call(package, "check", json_t::object()).text == "No errors or warnings.");
    CHECK(call(package, "check", { { "file", "score.fb" } }).text == "No errors or warnings.");

    // A file's text that isn't saved is what's checked, and the file stays as it was.
    const std::string unsaved_code = "Public Function Doubled(n: Int): Int => n * \"2\"\n";
    const called_t unsaved =
        call(package, "check", { { "file", "score.fb" }, { "code", unsaved_code } });
    // Paths are as fb check shows them, from the folder fb mcp runs in.
    CHECK(unsaved.text.starts_with("1 error, 0 warnings.\n\n"));
    CHECK(unsaved.text.contains("score.fb:1:"));
    CHECK(read_file(folder / "score.fb") == "Public Function Doubled(n: Int): Int => n * 2\n");
    write_file(folder / "score.fb", "Public Function Doubled(n: Int): Int => n * 2.5\n");
    const called_t saved = call(package, "check", json_t::object());
    CHECK(saved.text.starts_with("1 error, 0 warnings.\n\n"));
    CHECK(saved.text.contains("score.fb:1:"));

    const called_t missing = call(package, "check", { { "file", "nope.fb" } });
    CHECK(missing.error);
    CHECK(missing.text == std::format("There's no file at {}.", fb::to_utf8(folder / "nope.fb")));
}

TEST_CASE("fb mcp's format formats code without writing it", "[mcp]") {
    fb::mcp_server_t server(FB_TEST_OUTPUT);
    const called_t formatted =
        call(server, "format", { { "code", "Import std.core\nLocal   x   =  1\nPrint( x )\n" } });
    CHECK_FALSE(formatted.error);
    CHECK(formatted.text == "Import std.core\nLocal x = 1\nPrint(x)\n");

    const std::string call_text =
        "Print(\"aaaaaaaaaa\", \"bbbbbbbbbb\", \"cccccccccc\", \"dddddddddd\")\n";
    CHECK(call(server, "format", { { "code", call_text } }).text == call_text);
    CHECK(call(server, "format", { { "code", call_text }, { "width", 40 } }).text != call_text);

    const std::filesystem::path folder = package_folder("format");
    write_file(folder / "main.fb", "Local  y=2\n");
    fb::mcp_server_t package(folder);
    CHECK(call(package, "format", { { "file", "main.fb" } }).text == "Local y = 2\n");
    CHECK(read_file(folder / "main.fb") == "Local  y=2\n");

    const called_t broken = call(server, "format", { { "code", "Local = = (\n" } });
    CHECK(broken.error);
    CHECK(broken.text.starts_with("It doesn't parse, so it isn't formatted:\n\nmain.fb:1:"));
    CHECK(call(server, "format", json_t::object()).error);
    CHECK(call(package, "format", { { "file", "nope.fb" } }).error);
}

// The step's done-when: an assistant that has only fb mcp's tools turns a one-line request into
// a program that compiles. This one is scripted: it guesses as one that knows other BASICs
// would, reads the errors, and looks up what it got wrong, taking the fix from what the tools
// say, not from what it was written knowing.
TEST_CASE("an assistant given only fb mcp writes a program that compiles from a request", "[mcp]") {
    const std::string_view request = "Print the squares of the numbers from 1 to 10.";
    const std::filesystem::path folder = package_folder("assistant");
    fb::mcp_server_t& server = searcher();

    // What it knows from the start: the primer, which says what a program imports.
    const std::string primer(fb::agents_primer());
    const std::size_t fence = primer.find("```fb\n");
    REQUIRE(fence != std::string::npos);
    const std::string imports = primer.substr(fence + 6, primer.find('\n', fence + 6) - fence - 6);
    CHECK(imports == "Import std.core");
    INFO(request);

    // Its first try is QBasic's.
    std::string program = std::format("{}\n\nFor i = 1 To 10\n    Print i * i\nNext\n", imports);
    called_t checked = call(server, "check", { { "code", program } });
    CHECK_FALSE(checked.text.starts_with("No errors"));
    CHECK(checked.text.contains("error"));

    // It searches for what a For loop is, and finds the line that shows its end.
    const called_t loop = call(server, "search_docs", { { "query", "For loop" } });
    const std::size_t shown = loop.text.find("For i = 0 To 9 ... ");
    REQUIRE(shown != std::string::npos);
    const std::size_t after = shown + std::string_view("For i = 0 To 9 ... ").size();
    const std::string end_for = loop.text.substr(after, loop.text.find("  ", after) - after);
    CHECK(end_for == "End For");

    // And how Print is called: its signature has its parentheses.
    const called_t print = call(server, "lookup", { { "name", "Print" } });
    CHECK(print.text.contains("Function Print(values: Printable...)"));

    program = std::format("{}\n\nFor i = 1 To 10\n    Print(i * i)\n{}\n", imports, end_for);
    checked = call(server, "check", { { "code", program } });
    INFO(checked.text);
    CHECK(checked.text == "No errors or warnings.");
    const called_t formatted = call(server, "format", { { "code", program } });
    CHECK_FALSE(formatted.error);

    write_file(folder / "squares.fb", formatted.text);
    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_file(folder / "squares.fb");
    REQUIRE(compiled);
    CHECK(compiled->diagnostics.diagnostics().empty());
}
