#include "client.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using fb::lsp::json_t;
using fb::lsp::test::client_t;

TEST_CASE("the server says what it can do and shuts down", "[lsp]") {
    std::optional<int> code;
    {
        client_t client({});
        const json_t& capabilities = client.initialized()["capabilities"];
        CHECK(capabilities["positionEncoding"] == "utf-16");
        CHECK(capabilities["hoverProvider"] == true);
        CHECK(capabilities["definitionProvider"] == true);
        CHECK(capabilities["referencesProvider"] == true);
        CHECK(capabilities["renameProvider"]["prepareProvider"] == true);
        CHECK(capabilities["completionProvider"]["triggerCharacters"] == json_t::array({ "." }));
        CHECK(capabilities["signatureHelpProvider"].contains("triggerCharacters"));
        CHECK(capabilities["documentSymbolProvider"] == true);
        CHECK(capabilities["workspaceSymbolProvider"] == true);
        CHECK(capabilities["semanticTokensProvider"]["full"] == true);
        CHECK(capabilities["codeActionProvider"]["codeActionKinds"] ==
              json_t::array({ "quickfix" }));
        CHECK(capabilities["documentFormattingProvider"] == true);
        CHECK(capabilities["textDocumentSync"]["change"] == 2);
        CHECK(client.initialized()["serverInfo"]["name"] == "fb-lsp");

        CHECK(client.send("shutdown", nullptr)["result"].is_null());
        client.notify("exit", nullptr);
        code = client.server().exit_code();
    }

    CHECK(code == 0);
}

TEST_CASE("the server exits with 1 without a shutdown", "[lsp]") {
    client_t client({});
    client.notify("exit", nullptr);
    CHECK(client.server().exit_code() == 1);
}

TEST_CASE("the server answers requests it doesn't know, and ones before initialize", "[lsp]") {
    std::vector<json_t> sent;
    fb::lsp::server_t server([&](const json_t& message) { sent.push_back(message); });
    server.handle({ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "textDocument/hover" } });
    REQUIRE(sent.size() == 1);
    CHECK(sent[0]["error"]["code"] == -32002);

    client_t client({});
    const json_t unknown = client.send("textDocument/unknown", json_t::object());
    CHECK(unknown["error"]["code"] == -32601);
    // A request whose params are wrong is answered with an error, not a crash.
    const json_t broken = client.send("textDocument/hover", { { "textDocument", 5 } });
    CHECK(broken["error"]["code"] == -32602);
}

TEST_CASE("the server takes the client's UTF-8 positions", "[lsp]") {
    client_t client({ .encodings = { "utf-16", "utf-8" } });
    CHECK(client.initialized()["capabilities"]["positionEncoding"] == "utf-8");
}

TEST_CASE("messages are framed with their length", "[lsp]") {
    std::ostringstream out;
    fb::lsp::write_message(out, R"({"a":"é"})");
    CHECK(out.str() == "Content-Length: 10\r\n\r\n{\"a\":\"é\"}");

    std::istringstream in(out.str() + "content-length: 2\r\nContent-Type: x\r\n\r\n{}");
    CHECK(fb::lsp::read_message(in) == R"({"a":"é"})");
    CHECK(fb::lsp::read_message(in) == "{}");
    CHECK(fb::lsp::read_message(in) == std::nullopt);

    // A header without its length, and a body cut short.
    std::istringstream no_length("Content-Type: x\r\n\r\n{}");
    CHECK(fb::lsp::read_message(no_length) == std::nullopt);
    std::istringstream short_body("Content-Length: 10\r\n\r\n{}");
    CHECK(fb::lsp::read_message(short_body) == std::nullopt);
}

TEST_CASE("URIs name files", "[lsp]") {
#ifdef _WIN32
    CHECK(fb::lsp::path_of("file:///c%3A/Users/A%20B/x.fb") ==
          std::filesystem::path("c:/Users/A B/x.fb"));
    CHECK(fb::lsp::path_of("file:///C:/x.fb") == std::filesystem::path("C:/x.fb"));
    CHECK(fb::lsp::uri_of("C:/A B/x#1.fb") == "file:///C:/A%20B/x%231.fb");
    CHECK(fb::lsp::file_key("C:/A/B/../x.fb") == fb::lsp::file_key("c:/a/x.fb"));
#else
    CHECK(fb::lsp::path_of("file:///home/a%20b/x.fb") == std::filesystem::path("/home/a b/x.fb"));
    CHECK(fb::lsp::uri_of("/home/a b/x#1.fb") == "file:///home/a%20b/x%231.fb");
#endif
    CHECK(fb::lsp::path_of("untitled:Untitled-1") == std::nullopt);
}
