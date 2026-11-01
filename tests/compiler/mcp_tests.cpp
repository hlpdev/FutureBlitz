#include "fb/driver.hpp"
#include "fb/mcp.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using json_t = nlohmann::json;
using namespace std::chrono_literals;

/// A client of a server in this process, as stdio would carry their lines: it sends a line, and
/// takes the answers as they come.
class client_t {
public:
    explicit client_t(const std::filesystem::path& root, const fb::mcp_options_t options = {})
        : m_server(
              root,
              [this](const std::string& line) {
                  {
                      const std::scoped_lock lock(m_mutex);
                      m_answers.push_back(line);
                  }

                  m_arrived.notify_all();
              },
              options) {}

    void send(const std::string_view line) {
        m_server.receive(line);
    }

    /// The next answer, as JSON, which has to come within `timeout`.
    json_t next(const std::chrono::milliseconds timeout = 2min) {
        std::unique_lock lock(m_mutex);
        REQUIRE(m_arrived.wait_for(lock, timeout, [this] { return !m_answers.empty(); }));
        const std::string line = std::move(m_answers.front());
        m_answers.pop_front();
        return json_t::parse(line);
    }

    /// Whether an answer comes within `wait`.
    bool answers_within(const std::chrono::milliseconds wait) {
        std::unique_lock lock(m_mutex);
        return m_arrived.wait_for(lock, wait, [this] { return !m_answers.empty(); });
    }

    /// The answer to a line, the next one.
    json_t exchange(const std::string_view line) {
        send(line);
        return next();
    }

    json_t ask(const json_t& request) {
        return exchange(request.dump());
    }

    void settle() {
        m_server.settle();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_arrived;
    std::deque<std::string> m_answers;
    /// Last, so it's destroyed first, and stops sending before what it sends to goes.
    fb::mcp_server_t m_server;
};

json_t ask(client_t& client, const json_t& request) {
    return client.ask(request);
}

/// A tool's result: its text, and whether it's an error.
struct called_t {
    std::string text;
    bool error = false;
};

json_t call_request(const json_t& id, const std::string_view tool, const json_t& arguments) {
    json_t request{ { "jsonrpc", "2.0" }, { "id", id }, { "method", "tools/call" } };
    request["params"] = { { "name", tool }, { "arguments", arguments } };
    return request;
}

called_t call(client_t& client, const std::string_view tool, const json_t& arguments) {
    static int next = 100;
    const json_t answer = ask(client, call_request(++next, tool, arguments));
    CHECK(answer.at("id") == next);
    REQUIRE(answer.contains("result"));
    const json_t& result = answer.at("result");
    REQUIRE(result.at("content").size() == 1);
    CHECK(result.at("content")[0].at("type") == "text");
    std::string text = result.at("content")[0].at("text").get<std::string>();
    return { .text = std::move(text), .error = result.at("isError").get<bool>() };
}

json_t initialize(client_t& client, const std::string_view version = "2025-06-18") {
    json_t request{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" } };
    request["params"] = { { "protocolVersion", version }, { "capabilities", json_t::object() } };
    request["params"]["clientInfo"] = { { "name", "test" }, { "version", "1" } };
    return ask(client, request);
}

/// One server for the tests that search, since reading std's reference takes seconds.
client_t& searcher() {
    static client_t client(FB_TEST_OUTPUT);
    static const json_t initialized = initialize(client);
    return client;
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

/// An error's id and code.
std::pair<json_t, int> error_of(client_t& client, const std::string_view line) {
    const json_t answer = client.exchange(line);
    REQUIRE(answer.contains("error"));
    return { answer.at("id"), answer.at("error").at("code").get<int>() };
}

/// Whether a file of that name is anywhere in `folder`.
bool anywhere(const std::filesystem::path& folder, const std::string_view name) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator each(folder, error), end;
         !error && each != end; each.increment(error)) {
        if (each->path().filename() == name) {
            return true;
        }
    }

    return false;
}

/// A Const Function that computes for a while, called `count` times by Consts, which a check
/// computes as it checks them.
std::string slow_code(const int count) {
    std::string code = "Import std.core\n\nConst Function Spin(n: Int): Int\n    Local t = 0\n"
                       "    For i = 1 To n\n        t = t + 1\n    End For\n    Return t\n"
                       "End Function\n\n";
    for (int k = 0; k < count; ++k) {
        code += std::format("Const C{} = Spin(3000000)\n", k);
    }

    return code;
}

} // namespace

TEST_CASE("fb mcp answers MCP's lifecycle and JSON-RPC's errors", "[mcp]") {
    client_t server(FB_TEST_OUTPUT);
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
    server.send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
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

    CHECK(error_of(server, "not json") == std::pair{ json_t(), -32700 });
    CHECK(error_of(server, "[1, 2]") == std::pair{ json_t(), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":3})") == std::pair{ json_t(3), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":4,"method":"resources/list"})") ==
          std::pair{ json_t(4), -32601 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{}})") ==
          std::pair{ json_t(5), -32602 });
    CHECK(error_of(server,
                   R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nope"}})") ==
          std::pair{ json_t(6), -32602 });

    // Arguments a tool doesn't take are its error, which the assistant reads.
    const called_t wrong = call(server, "lookup", { { "name", 3 } });
    CHECK(wrong.error);
    CHECK(wrong.text.starts_with("Its arguments aren't what it takes"));
}

// From the review: only "jsonrpc": "2.0" requests, with ids that are strings or whole numbers;
// a client's responses get no answer; and a batch, which 2025-03-26 has, is answered as one.
TEST_CASE("fb mcp takes JSON-RPC's messages as the version agreed on has them", "[mcp]") {
    client_t server(FB_TEST_OUTPUT);
    initialize(server, "2025-06-18");
    CHECK(error_of(server, R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])") ==
          std::pair{ json_t(), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":{"x":1},"method":"ping"})") ==
          std::pair{ json_t(), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":true,"method":"ping"})") ==
          std::pair{ json_t(), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})") ==
          std::pair{ json_t(), -32600 });
    CHECK(error_of(server, R"({"id":8,"method":"ping"})") == std::pair{ json_t(8), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"1.0","id":9,"method":"ping"})") ==
          std::pair{ json_t(9), -32600 });
    server.send(R"({"jsonrpc":"2.0","id":5,"result":{}})");
    server.send(R"({"jsonrpc":"2.0","id":6,"error":{"code":-1,"message":"no"}})");
    CHECK_FALSE(server.answers_within(300ms));

    initialize(server, "2025-03-26");
    const json_t answers = server.exchange(
        R"([{"jsonrpc":"2.0","id":"a","method":"ping"},)"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"},)"
        R"({"jsonrpc":"2.0","id":"b","method":"tools/call","params":{"name":"format","arguments":{"code":"Local  x=1\n"}}},)"
        R"({"jsonrpc":"2.0","id":"c","method":"nope"}])");
    REQUIRE(answers.is_array());
    REQUIRE(answers.size() == 3);
    std::set<std::string> ids;
    for (const json_t& answer : answers) {
        ids.insert(answer.at("id").get<std::string>());
        if (answer.at("id") == "b") {
            CHECK(answer.at("result").at("content")[0].at("text") == "Local x = 1\n");
        }
    }

    CHECK(ids == std::set<std::string>{ "a", "b", "c" });
    server.send(R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])");
    CHECK_FALSE(server.answers_within(300ms));
    CHECK(error_of(server, "[]") == std::pair{ json_t(), -32600 });

    // From the review: a cancelled call in a batch is one its answer doesn't wait for.
    const json_t calls = json_t::array({
        call_request("slow", "check", { { "code", slow_code(3) } }),
        call_request("quick", "format", { { "code", "Local  y=2\n" } }),
    });
    server.send(calls.dump());
    server.send(
        R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"slow"}})");
    const json_t rest = server.next();
    REQUIRE(rest.is_array());
    REQUIRE(rest.size() == 1);
    CHECK(rest[0].at("id") == "quick");
}

// From the review: nlohmann/json copies a value nested 100,000 deep recursively, which overflowed
// the stack.
TEST_CASE("fb mcp refuses a message nested deeper than MCP's are, and params that aren't objects",
          "[mcp]") {
    client_t server(FB_TEST_OUTPUT);
    initialize(server);
    const std::string deep = std::string(100'000, '[') + std::string(100'000, ']');
    CHECK(error_of(server, deep) == std::pair{ json_t(), -32600 });
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":)"
                           R"("check","arguments":{"code":)" +
                               deep + "}}}") == std::pair{ json_t(), -32600 });
    const std::string shallow = std::string(40, '[') + std::string(40, ']');
    const called_t nested = call(server, "check", { { "code", json_t::parse(shallow) } });
    CHECK(nested.error);
    CHECK(error_of(server, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":"check"})") ==
          std::pair{ json_t(4), -32602 });
    CHECK(
        error_of(
            server,
            R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"check","arguments":[1]}})") ==
        std::pair{ json_t(5), -32602 });
}

// From the review: a call that takes long held up the server, which answered nothing else,
// read no more, and ignored its cancellation.
TEST_CASE("fb mcp answers while a call runs, cancels one, and gives up on one at its deadline",
          "[mcp]") {
    client_t server(FB_TEST_OUTPUT, { .deadline = 300ms });
    initialize(server);
    server.send(call_request("slow", "check", { { "code", slow_code(3) } }).dump());
    CHECK(
        ask(server, json_t{ { "jsonrpc", "2.0" }, { "id", "p" }, { "method", "ping" } }).at("id") ==
        "p");
    const json_t late = server.next();
    CHECK(late.at("id") == "slow");
    CHECK(late.at("result").at("isError") == true);
    CHECK(late.at("result").at("content")[0].at("text").get<std::string>().starts_with(
        "It didn't finish in 0.3 s"));

    // A cancelled call isn't answered.
    server.send(call_request(20, "check", { { "code", slow_code(3) } }).dump());
    server.send(
        R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":20}})");
    server.settle();
    CHECK_FALSE(server.answers_within(1s));

    // Two calls of one id at once are a mistake.
    client_t patient(FB_TEST_OUTPUT);
    initialize(patient);
    patient.send(call_request(30, "check", { { "code", slow_code(1) } }).dump());
    CHECK(error_of(patient, call_request(30, "format", { { "code", "Local x = 1\n" } }).dump()) ==
          std::pair{ json_t(30), -32600 });
    CHECK(patient.next().at("id") == 30);
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
    client_t& server = searcher();
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
    client_t& server = searcher();
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
    client_t package(folder);
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
    client_t server(FB_TEST_OUTPUT);
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
    client_t package(folder);
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
    CHECK(missing.text == "There's no file nope.fb.");
}

// From the review: check of a folder that isn't a package answered in the CLI's words.
TEST_CASE("fb mcp's check of a folder of loose files checks each of them", "[mcp]") {
    const std::filesystem::path folder = package_folder("loose");
    write_file(folder / "a.fb", "Import std.core\n\nPrint(\"a\")\n");
    write_file(folder / "b.fb", "Import std.core\n\nLocal n: Int = \"b\"\n");
    client_t loose(folder);
    const called_t checked = call(loose, "check", json_t::object());
    CHECK_FALSE(checked.error);
    CHECK(checked.text.starts_with("1 error, 0 warnings.\n\n"));
    CHECK(checked.text.contains("b.fb:3:"));

    client_t empty(package_folder("empty"));
    const called_t nothing = call(empty, "check", json_t::object());
    CHECK(nothing.error);
    CHECK(nothing.text ==
          "There's no package here, and no .fb file; check takes 'file' or 'code'.");
}

// From the review: format and check read any file a path named, and showed its lines.
TEST_CASE("fb mcp's tools read only the package's .fb files", "[mcp]") {
    const std::filesystem::path folder = package_folder("paths");
    const std::filesystem::path outside = package_folder("paths-outside");
    write_file(folder / "project.toml", "[project]\nname = \"paths\"\nversion = \"0.1.0\"\n");
    write_file(folder / "main.fb", "Import std.core\n\nPrint(\"main\")\n");
    write_file(folder / "notes.txt", "Local token = \"hunter2\"\n");
    write_file(outside / "secret.fb", "Local token = \"hunter2\" (\n");
    client_t package(folder);
    for (const std::string& given : {
             std::string("../paths-outside/secret.fb"),
             fb::to_utf8(outside / "secret.fb"),
             std::string("notes.txt"),
             std::string("main.fb/../../paths-outside/secret.fb"),
         }) {
        INFO(given);
        for (const std::string_view tool : { "check", "format" }) {
            const called_t refused = call(package, tool, { { "file", given } });
            CHECK(refused.error);
            CHECK(refused.text ==
                  std::format("'{}' isn't a .fb file in the package's folder, which is all a "
                              "tool reads",
                              given));
        }
    }

    CHECK(call(package, "check", { { "file", fb::to_utf8(folder / "main.fb") } }).text ==
          "No errors or warnings.");
}

// From the review: lookup and check built and ran a package's build step, without a bound.
TEST_CASE("fb mcp's tools run none of a package's build steps", "[mcp]") {
    const std::filesystem::path folder = package_folder("stepped");
    write_file(
        folder / "project.toml",
        "[project]\nname = \"stepped\"\nversion = \"0.1.0\"\n\n[build]\nprogram = \"gen.fb\"\n");
    write_file(folder / "gen.fb",
               "Import std.core\nImport std.io\nImport std.os\n\n"
               "Local marker = Path(Env.Get(\"FB_BUILD_OUT\") ?? \"\") / \"step-ran.txt\"\n"
               "Try File.WriteText(marker, \"the build step ran\")\n");
    write_file(folder / "main.fb", "Import std.core\n\nPrint(\"main\")\n");

    // One that never ran is an error that says fb build runs it.
    {
        client_t package(folder);
        CHECK_FALSE(call(package, "lookup", { { "name", "marker" } }).error);
        for (const json_t& arguments : { json_t::object(), json_t{ { "file", "main.fb" } } }) {
            const called_t checked = call(package, "check", arguments);
            CHECK(checked.text.contains(
                "stepped's build step hasn't run, and this check runs none; fb build runs it"));
        }
    }

    CHECK_FALSE(std::filesystem::exists(folder / "build"));

    // One that ran gives what it made, though its program has changed since.
    REQUIRE(fb::compile_file(folder));
    REQUIRE(anywhere(folder, "step-ran.txt"));
    for (std::filesystem::recursive_directory_iterator each(folder), end; each != end; ++each) {
        if (each->path().filename() == "step-ran.txt") {
            std::filesystem::remove(each->path());
            break;
        }
    }

    write_file(folder / "gen.fb", read_file(folder / "gen.fb") + "// changed\n");
    client_t package(folder);
    CHECK(call(package, "check", json_t::object()).text == "No errors or warnings.");
    CHECK(call(package, "check", { { "file", "main.fb" } }).text == "No errors or warnings.");
    (void)call(package, "lookup", { { "name", "marker" } });
    CHECK_FALSE(anywhere(folder, "step-ran.txt"));
}

TEST_CASE("fb mcp's format formats code without writing it", "[mcp]") {
    client_t server(FB_TEST_OUTPUT);
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
    client_t package(folder);
    CHECK(call(package, "format", { { "file", "main.fb" } }).text == "Local y = 2\n");
    CHECK(read_file(folder / "main.fb") == "Local  y=2\n");

    const called_t broken = call(server, "format", { { "code", "Local = = (\n" } });
    CHECK(broken.error);
    CHECK(broken.text.starts_with("It doesn't parse, so it isn't formatted:\n\nmain.fb:1:"));
    CHECK(call(server, "format", json_t::object()).error);
    CHECK(call(package, "format", { { "file", "nope.fb" } }).error);
}

namespace {

/// What a scripted assistant makes of check's errors: each edit the errors and their fixes say,
/// made to `program`, as one that reads them would: a call written without its parentheses gets
/// them, a statement that isn't declared goes, a block's missing end is added, and each fix's
/// text goes where it says.
std::string edited(const std::string& program, const std::string& errors) {
    std::vector<std::string> lines;
    std::istringstream reading(program);
    for (std::string line; std::getline(reading, line);) {
        lines.push_back(line);
    }

    std::vector<std::string> ends;
    std::set<std::size_t> removed;
    std::istringstream said(errors);
    for (std::string message; std::getline(said, message);) {
        const std::size_t error = message.find(": error: ");
        if (message.starts_with("main.fb:") && error != std::string::npos) {
            const std::size_t line = std::stoul(message.substr(8)) - 1;
            const std::string text = message.substr(error + 9);
            const std::size_t parentheses = text.find("calls need parentheses, like ");
            if (parentheses != std::string::npos && line < lines.size()) {
                const std::string name =
                    text.substr(parentheses + 29, text.find('(', parentheses) - parentheses - 29);
                std::string& code = lines[line];
                const std::size_t at = code.find(name + ' ');
                if (at != std::string::npos) {
                    code = std::format("{}{}({})", code.substr(0, at), name,
                                       code.substr(at + name.size() + 1));
                }
            }

            if (text.starts_with("expected '") && text.ends_with("', found end of file")) {
                ends.push_back(text.substr(10, text.size() - 10 - 20));
            }

            const std::size_t undeclared = text.find("' isn't declared");
            if (text.starts_with('\'') && undeclared != std::string::npos && line < lines.size() &&
                lines[line] == text.substr(1, undeclared - 1)) {
                removed.insert(line);
            }
        }

        // "  fix: at line 3, put `If` in place of `if`"
        const std::size_t fix = message.find("  fix: at line ");
        if (fix == 0) {
            const std::size_t line = std::stoul(message.substr(15)) - 1;
            const std::size_t put = message.find("put `") + 5;
            const std::string replacement = message.substr(put, message.find('`', put) - put);
            const std::size_t place = message.find("in place of `") + 13;
            const std::string replaced = message.substr(place, message.rfind('`') - place);
            if (line < lines.size()) {
                if (const std::size_t at = lines[line].find(replaced); at != std::string::npos) {
                    lines[line].replace(at, replaced.size(), replacement);
                }
            }
        }
    }

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!removed.contains(i)) {
            out += lines[i] + '\n';
        }
    }

    for (const std::string& end : ends) {
        out += end + '\n';
    }

    return out;
}

} // namespace

// The step's done-when: an assistant that has only fb mcp's tools turns a one-line request into
// a program that compiles. This one is scripted: it guesses as one that knows other BASICs
// would, and makes only the edits check's errors and fixes say, until there are none.
TEST_CASE("an assistant given only fb mcp writes a program that compiles from a request", "[mcp]") {
    const std::string_view request = "Print the squares of the numbers from 1 to 10.";
    const std::filesystem::path folder = package_folder("assistant");
    client_t& server = searcher();
    INFO(request);

    // What it knows from the start: the primer, which says what a program imports.
    const std::string primer(fb::agents_primer());
    const std::size_t fence = primer.find("```fb\n");
    REQUIRE(fence != std::string::npos);
    const std::string imports = primer.substr(fence + 6, primer.find('\n', fence + 6) - fence - 6);
    CHECK(imports == "Import std.core");

    // Its first try is QBasic's, which it edits as check says until it's right.
    std::string program = std::format("{}\n\nFor i = 1 To 10\n    Print i * i\nNext\n", imports);
    int tries = 0;
    for (called_t checked = call(server, "check", { { "code", program } });
         checked.text != "No errors or warnings.";
         checked = call(server, "check", { { "code", program } })) {
        INFO(checked.text);
        REQUIRE(++tries < 4);
        const std::string next = edited(program, checked.text);
        REQUIRE(next != program);
        program = next;
    }

    CHECK(tries > 0);
    CHECK(program == std::format("{}\n\nFor i = 1 To 10\n    Print(i * i)\nEnd For\n", imports));
    const called_t formatted = call(server, "format", { { "code", program } });
    CHECK_FALSE(formatted.error);

    write_file(folder / "squares.fb", formatted.text);
    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_file(folder / "squares.fb");
    REQUIRE(compiled);
    CHECK(compiled->diagnostics.diagnostics().empty());
}
