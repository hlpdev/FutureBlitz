#include "client.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/process.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

// 1.2.2: fb-lsp answers every request while code is typed, none of them kept waiting for a
// check on the message thread, and none dropped.

using fb::lsp::json_t;
using fb::lsp::test::client_t;
using fb::lsp::test::folder_t;

TEST_CASE("every request is answered while code is typed without a pause for ten seconds",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("typed.fb");
    const std::string head = "Import std.core\n"
                             "Function Twice(n: Int): Int => n * 2\n"
                             "Local total = Twice(2)\n"
                             "Print(";
    client_t client({ .delay = std::chrono::milliseconds(150) });
    client.open(uri, head + ")\n");
    client.settle();

    // A keystroke at a time, each followed by what VS Code asks for after an edit.
    const json_t document{ { "uri", uri } };
    const json_t everything{
        { "start", client_t::position(0, 0) },
        { "end", client_t::position(100, 0) },
    };
    std::vector<std::int64_t> asked;
    std::string typed;
    const std::string keys = "total + Twice(total) + ";
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t key = 0; std::chrono::steady_clock::now() - started < std::chrono::seconds(10);
         ++key) {
        typed += keys[key % keys.size()];
        const std::string text = head + typed + ")\n";
        client.queue_change(uri, text);
        const json_t at{
            { "textDocument", document },
            { "position", client_t::position(3, static_cast<std::uint32_t>(6 + typed.size())) },
        };
        const json_t start{
            { "textDocument", document },
            { "position", client_t::position(2, 7) },
        };
        for (const auto& [method, params] : std::vector<std::pair<std::string, json_t>>{
                 { "textDocument/semanticTokens/full", { { "textDocument", document } } },
                 {
                     "textDocument/inlayHint",
                     { { "textDocument", document }, { "range", everything } },
                 },
                 { "textDocument/codeLens", { { "textDocument", document } } },
                 { "textDocument/documentSymbol", { { "textDocument", document } } },
                 { "textDocument/foldingRange", { { "textDocument", document } } },
                 {
                     "textDocument/codeAction",
                     {
                         { "textDocument", document },
                         { "range", { { "start", at["position"] }, { "end", at["position"] } } },
                         { "context", { { "diagnostics", json_t::array() } } },
                     },
                 },
                 { "textDocument/documentHighlight", start },
                 { "textDocument/hover", start },
                 { "textDocument/completion", at },
                 { "textDocument/signatureHelp", at },
             }) {
            asked.push_back(client.queue(method, params));
        }

        client.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    client.settle();
    CHECK(asked.size() > 100);
    for (const std::int64_t id : asked) {
        const json_t response = client.response(id);
        INFO(response.dump());
        CHECK((response.contains("result") || response.contains("error")));
    }
}

namespace {

constexpr std::string_view twice = "Import std.core\n"
                                   "Function Twice(n: Int): Int => n * 2\n"
                                   "Local total = Twice(2)\n"
                                   "Print(total)\n";

/// The index of the last of `messages`, before `end`, for which `is` holds, or `end` for none.
std::size_t last_where(const std::vector<json_t>& messages, const std::size_t end,
                       const std::function<bool(const json_t&)>& is) {
    for (std::size_t at = end; at > 0; --at) {
        if (is(messages[at - 1])) {
            return at - 1;
        }
    }

    return end;
}

bool is_method(const json_t& message, const std::string_view method) {
    return message.contains("method") && message["method"].get<std::string>() == method;
}

} // namespace

TEST_CASE("a request a worker waits with is answered about the text it was asked about", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("asked.fb");
    client_t client({ .delay = std::chrono::milliseconds(150) });
    client.open(uri, twice);
    client.settle();

    // A completion that waits for its version's check, and behind it a hover on Twice, then a
    // line put in at the top before either is answered.
    const json_t document{ { "uri", uri } };
    const std::string asked = std::string(twice) + "Local y = ";
    client.queue_change(uri, asked);
    const std::int64_t completion =
        client.queue("textDocument/completion",
                     { { "textDocument", document }, { "position", client_t::position(4, 10) } });
    const std::int64_t hover =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(1, 11) } });
    client.queue_change(uri, "\n" + asked);
    client.drain();
    client.settle();
    CHECK(client.response(completion).contains("error"));
    const json_t answered = client.response(hover);
    INFO(answered.dump());
    CHECK(answered["result"]["contents"]["value"].get<std::string>().contains("Twice"));
}

TEST_CASE("a request that waits for a check gives way once typing moves past its version",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("typing.fb");
    client_t client({ .delay = std::chrono::milliseconds(150) });
    client.open(uri, twice);
    client.settle();

    // A completion after "= ", which waits for the check, then a hover the last check answers,
    // while typing goes on.
    const json_t document{ { "uri", uri } };
    std::string text = std::string(twice) + "Local y = ";
    client.queue_change(uri, text);
    const std::int64_t completion =
        client.queue("textDocument/completion",
                     { { "textDocument", document }, { "position", client_t::position(4, 10) } });
    const std::int64_t hover =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(1, 11) } });
    client.drain();
    const auto asked = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration took{};
    std::thread waiter([&] {
        (void)client.response(hover);
        took = std::chrono::steady_clock::now() - asked;
    });
    while (std::chrono::steady_clock::now() - asked < std::chrono::seconds(2)) {
        text += '1';
        client.queue_change(uri, text);
        client.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    waiter.join();
    client.settle();
    CHECK(took < std::chrono::milliseconds(500));
    CHECK(client.response(completion)["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::CONTENT_MODIFIED));
}

TEST_CASE("a request cancelled while a worker answers it is answered as cancelled", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("cancelled.fb");
    client_t client({ .delay = std::chrono::milliseconds(300) });
    client.open(uri, twice);
    client.settle();

    // A hover on a line that changed, which waits for the change's check, which a Const that
    // computes a lot makes take seconds, and its cancel once the worker has it.
    const std::string changed = std::string(twice) + "Const Function Spin(n: Int): Int\n"
                                                     "    Local t = 0\n"
                                                     "    For i = 1 To n\n"
                                                     "        t = t + 1\n"
                                                     "    End For\n"
                                                     "    Return t\n"
                                                     "End Function\n"
                                                     "Const Slow = Spin(1000000)\n"
                                                     "Local more = Twice(total)\n";
    client.change(uri, changed);
    const std::int64_t hover =
        client.queue("textDocument/hover", {
                                               { "textDocument", { { "uri", uri } } },
                                               { "position", client_t::position(12, 15) },
                                           });
    client.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.notify("$/cancelRequest", { { "id", hover } });
    client.settle();
    CHECK(client.response(hover)["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_CANCELLED));

    // One not cancelled is answered.
    CHECK(client
              .request("textDocument/hover",
                       {
                           { "textDocument", { { "uri", uri } } },
                           { "position", client_t::position(12, 15) },
                       })
              .is_object());
}

TEST_CASE("shutdown is answered with requests waiting for a check that's in flight", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("closing.fb");
    client_t client({ .delay = std::chrono::milliseconds(150) });
    client.open(uri, twice);
    client.settle();

    const json_t document{ { "uri", uri } };
    client.queue_change(uri, std::string(twice) + "Local y = ");
    std::vector<std::int64_t> asked;
    for (int i = 0; i < 5; ++i) {
        asked.push_back(
            client.queue("textDocument/completion", {
                                                        { "textDocument", document },
                                                        { "position", client_t::position(4, 10) },
                                                    }));
        asked.push_back(client.queue("textDocument/references",
                                     {
                                         { "textDocument", document },
                                         { "position", client_t::position(1, 11) },
                                         { "context", { { "includeDeclaration", true } } },
                                     }));
    }

    const std::int64_t shutdown = client.queue("shutdown", json_t());
    client.drain();
    CHECK(client.response(shutdown).contains("result"));
    for (const std::int64_t id : asked) {
        const json_t response = client.response(id);
        CHECK((response.contains("result") || response.contains("error")));
    }
}

TEST_CASE("a definition in the document moves with an edit, though the client spells its URI "
          "otherwise",
          "[lsp]") {
    const folder_t folder({});
    std::string uri = folder.uri("spelled.fb");
    // VS Code's on Windows: the drive's letter lowercase, and its ':' escaped.
    if (uri.size() > 10 && uri[9] == ':') {
        uri = "file:///" + std::string(1, static_cast<char>(std::tolower(uri[8]))) + "%3A" +
              uri.substr(10);
    }

    client_t client({ .delay = std::chrono::milliseconds(300) });
    client.open(uri, twice);
    client.settle();
    // Twice's use is on line 3 now, and its declaration on line 2.
    client.change(uri, "\n" + std::string(twice));
    const json_t found =
        client.request("textDocument/definition", {
                                                      { "textDocument", { { "uri", uri } } },
                                                      { "position", client_t::position(3, 15) },
                                                  });
    INFO(found.dump());
    const json_t first = found.is_array() ? found[0] : found;
    CHECK(first["range"]["start"]["line"] == 2);
}

TEST_CASE("what a '(' or a '.' just typed goes after is what's before it, not an operator's",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("after.fb");
    const std::string head = "Import std.core\n"
                             "Function Twice(n: Int): Int => n * 2\n"
                             "Struct Player\n"
                             "    Field Alive: Bool\n"
                             "End Struct\n"
                             "Local player = Player(True)\n";
    client_t client({ .delay = std::chrono::milliseconds(300) });
    client.open(uri, head + "Local total = 1 + Twice\n");
    client.settle();

    client.change(uri, head + "Local total = 1 + Twice(\n");
    const json_t help =
        client.send("textDocument/signatureHelp", {
                                                      { "textDocument", { { "uri", uri } } },
                                                      { "position", client_t::position(6, 24) },
                                                  });
    INFO(help.dump());
    REQUIRE(help["result"].is_object());
    CHECK(help["result"]["signatures"][0]["label"].get<std::string>().contains("Twice"));

    client.change(uri, head + "Local dead = !player\n");
    client.settle();
    client.change(uri, head + "Local dead = !player.\n");
    const json_t members =
        client.request("textDocument/completion", {
                                                      { "textDocument", { { "uri", uri } } },
                                                      { "position", client_t::position(6, 21) },
                                                  });
    INFO(members.dump());
    const json_t& items = members.is_object() ? members["items"] : members;
    CHECK(std::ranges::any_of(items, [](const json_t& item) { return item["label"] == "Alive"; }));
}

TEST_CASE("signature help at a '(' just typed is the call's, though its name alone wasn't a "
          "value",
          "[lsp]") {
    // A name on its own line, before the '(', is a function value, which one that throws, or a
    // built-in one, can't be: what it names is still the call's, from the check before the '('
    // and from its own, as a busy machine has it done before the request's answered.
    const folder_t folder({});
    const std::string uri = folder.uri("typed.fb");
    const std::string head = "Import std.core\n"
                             "Function Twice(n: Int): Int => n * 2\n"
                             "Function Risky(n: Int): Int Throws Error\n"
                             "    Throw Error(\"no\")\n"
                             "End Function\n";
    for (const bool checked : { false, true }) {
        client_t client({ .delay = std::chrono::milliseconds(checked ? 0 : 300) });
        for (const auto& [before, after, named] :
             std::vector<std::tuple<std::string, std::string, std::string>>{
                 { "Local total = 1 + Twice", "\n", "Twice" },
                 { "Function Use()\n    Twice", "\nEnd Function\n", "Twice" },
                 {
                     "Function Use(): Int Throws Error\n    Risky",
                     "\n    Return 1\nEnd Function\n",
                     "Risky",
                 },
                 { "Print", "\n", "Print" },
             }) {
            client.open(uri, std::format("{}{}{}", head, before, after));
            client.settle();
            client.change(uri, std::format("{}{}({}", head, before, after));
            if (checked) {
                client.settle();
            }

            const std::string& typed = client.text(uri);
            const std::size_t open = typed.rfind('(');
            const json_t position = client_t::position(
                static_cast<std::uint32_t>(std::ranges::count(typed.substr(0, open), '\n')),
                static_cast<std::uint32_t>(open - typed.rfind('\n', open)));
            const json_t help =
                client.send("textDocument/signatureHelp",
                            { { "textDocument", { { "uri", uri } } }, { "position", position } });
            INFO(before << (checked ? ", checked: " : ": ") << help.dump());
            REQUIRE(help["result"].is_object());
            CHECK(help["result"]["signatures"][0]["label"].get<std::string>().contains(named));
            client.close(uri);
        }
    }
}

TEST_CASE("each change's check brings the refreshes it asks for, though checks and changes cross",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("crossed.fb");
    client_t client({
        .delay = std::chrono::milliseconds(20),
        .tokens_refresh = true,
        .hints_refresh = true,
    });
    const std::string head = "Import std.core\nFunction Twice(n: Int): Int => n * 2\n";
    client.open(uri, head + "Print(Twice(0))\n");
    client.settle();

    // Changes as the checks of those before them are done, or not, with semantic tokens asked
    // for after each, as the editor asks.
    const json_t document{ { "uri", uri } };
    std::int64_t tokens = 0;
    for (int round = 1; round <= 30; ++round) {
        client.change(uri, std::format("{}Print(Twice({}))\n", head, round));
        tokens = client.queue("textDocument/semanticTokens/full", { { "textDocument", document } });
        client.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 7 * 5));
    }

    client.settle();
    const std::vector<json_t> received = client.received();
    const auto published = [](const json_t& message) {
        return is_method(message, "textDocument/publishDiagnostics");
    };
    // The last check's publish, after its inlay hints' and lenses' refreshes.
    const std::size_t last = last_where(received, received.size(), published);
    REQUIRE(last < received.size());
    const std::size_t before = last_where(received, last, published);
    const auto refreshed = [&](const std::string_view method) {
        for (std::size_t at = before < last ? before + 1 : 0; at < last; ++at) {
            if (is_method(received[at], method)) {
                return true;
            }
        }

        return false;
    };
    CHECK(refreshed("workspace/inlayHint/refresh"));
    CHECK(refreshed("workspace/codeLens/refresh"));

    // The last tokens are the check's, or a refresh came after them.
    const std::size_t answered = last_where(received, received.size(), [&](const json_t& message) {
        return !message.contains("method") && message.value("id", json_t()) == tokens;
    });
    REQUIRE(answered < received.size());
    const bool told = std::any_of(received.begin() + static_cast<std::ptrdiff_t>(answered),
                                  received.end(), [](const json_t& message) {
                                      return is_method(message, "workspace/semanticTokens/refresh");
                                  });
    const json_t checked =
        client.request("textDocument/semanticTokens/full", { { "textDocument", document } });
    CHECK((told || received[answered]["result"] == checked));
}

TEST_CASE("a request whose check is stuck is answered by its deadline, and nothing else waits",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("stuck.fb");
    client_t client({ .request_deadline = std::chrono::milliseconds(300) });
    client.open(uri, twice);
    client.settle();

    // A Const that computes a lot makes the change's check take seconds, as a stuck one would.
    client.change(uri, std::string(twice) + "Const Function Spin(n: Int): Int\n"
                                            "    Local t = 0\n"
                                            "    For i = 1 To n\n"
                                            "        t = t + 1\n"
                                            "    End For\n"
                                            "    Return t\n"
                                            "End Function\n"
                                            "Const Slow = Spin(1000000)\n"
                                            "Local more = Twice(total)\n");
    const json_t document{ { "uri", uri } };
    const auto asked = std::chrono::steady_clock::now();

    // A hover on the changed line needs it, and has until its deadline.
    const std::int64_t waiting =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(12, 15) } });
    // A second, behind it, goes as soon as the first does.
    const std::int64_t behind =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(12, 15) } });
    client.drain();

    // A save's formatting is answered meanwhile, within its budget's order, as is the extension's
    // ping.
    const json_t formatting =
        client.send("textDocument/formatting",
                    {
                        { "textDocument", document },
                        { "options", { { "tabSize", 4 }, { "insertSpaces", true } } },
                    });
    CHECK(formatting.contains("result"));
    CHECK(client.send("fb/ping", json_t::object()).contains("result"));
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(300));

    // A hover on an unchanged line is the last check's.
    json_t unchanged =
        client.send("textDocument/hover",
                    { { "textDocument", document }, { "position", client_t::position(1, 11) } });
    INFO(unchanged.dump());
    CHECK(unchanged["result"]["contents"]["value"].get<std::string>().contains("Twice"));

    CHECK(client.response(waiting)["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_FAILED));
    CHECK(client.response(behind)["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_FAILED));
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds(3));
    client.settle();
}

TEST_CASE("fb-lsp's messages stay whole while it reads more, each request answered once", "[lsp]") {
    // The user's program, typed into with VS Code's requests after each keystroke and a save's
    // formatting. Reading std::cin flushed std::cout, tied to it, on the reader's thread: with
    // another thread writing, a message came out twice or in pieces, and the client's reader never
    // found its place again.
    const std::string program = "Import std.core\n"
                                "Import std.concurrent\n\n"
                                "Global total = 0\n\n"
                                "Function AddUp(n: Int)\n"
                                "    total.Add(n)\n"
                                "End Function\n\n"
                                "ParallelForEach([1, 2, 3, 4], AddUp)\n"
                                "Expect(total == 10)\n";
    const folder_t folder({ { "main.fb", program } });
    const std::string uri = folder.uri("main.fb");
    const json_t document{ { "uri", uri } };
    std::string input;
    const auto message = [&](const json_t& body) {
        const std::string text = body.dump();
        input += std::format("Content-Length: {}\r\n\r\n{}", text.size(), text);
    };
    std::set<std::int64_t> asked;
    std::int64_t next = 1;
    const auto request = [&](const std::string& method, const json_t& params) {
        asked.insert(next);
        message(
            { { "jsonrpc", "2.0" }, { "id", next++ }, { "method", method }, { "params", params } });
    };
    request("initialize", { { "capabilities", json_t::object() } });
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
                        { "text", program },
                    },
                },
            },
        },
    });
    for (int round = 2; round < 400; ++round) {
        const auto column = static_cast<std::uint32_t>(round % 20);
        message({
            { "jsonrpc", "2.0" },
            { "method", "textDocument/didChange" },
            {
                "params",
                {
                    { "textDocument", { { "uri", uri }, { "version", round } } },
                    {
                        "contentChanges",
                        json_t::array({ { { "text", program + std::string(column, 'x') } } }),
                    },
                },
            },
        });
        const json_t at{
            { "textDocument", document },
            { "position", client_t::position(11, column) },
        };
        request("textDocument/semanticTokens/full", { { "textDocument", document } });
        request("textDocument/codeLens", { { "textDocument", document } });
        request("textDocument/documentSymbol", { { "textDocument", document } });
        request("textDocument/foldingRange", { { "textDocument", document } });
        request("textDocument/hover", at);
        request("textDocument/completion", at);
        request("textDocument/formatting",
                {
                    { "textDocument", document },
                    { "options", { { "tabSize", 4 }, { "insertSpaces", true } } },
                });
    }

    request("shutdown", nullptr);
    message({ { "jsonrpc", "2.0" }, { "method", "exit" } });

    const std::expected<fb::process_result_t, std::string> run = fb::run_process(FB_LSP, {}, input);
    REQUIRE(run);
    CHECK(run->exit_code == 0);
    std::istringstream output(run->output);
    std::map<std::int64_t, int> answers;
    while (const std::optional<std::string> body = fb::lsp::read_message(output)) {
        const json_t received = json_t::parse(*body, nullptr, false);
        INFO(body->substr(0, 200));
        REQUIRE(!received.is_discarded());
        if (!received.contains("method") && received.contains("id")) {
            ++answers[received["id"].get<std::int64_t>()];
        }
    }

    CHECK(output.eof());
    CHECK(answers.size() == asked.size());
    CHECK(std::ranges::all_of(answers, [](const auto& each) { return each.second == 1; }));
}
