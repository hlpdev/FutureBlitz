#include "client.hpp"
#include "fb/lsp/protocol.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
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
