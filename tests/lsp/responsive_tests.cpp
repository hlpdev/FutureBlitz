#include "client.hpp"
#include "fb/lsp/protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <string>
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
