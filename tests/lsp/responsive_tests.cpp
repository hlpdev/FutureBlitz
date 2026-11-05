#include "client.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/process.hpp"
#include "fb/stack_thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <pthread.h>
#endif

// 1.2.2: fb-lsp answers every request while code is typed, none of them kept waiting for a
// check on the message thread, and none dropped.

using fb::lsp::json_t;
using fb::lsp::test::check_gate_t;
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
    std::atomic<bool> answered = false;
    std::thread waiter([&] {
        (void)client.response(hover);
        answered = true;
    });
    // The typing stops when it's answered, or, with it waiting for as long as typing goes on, never
    // does but for this.
    const auto asked = std::chrono::steady_clock::now();
    while (!answered && std::chrono::steady_clock::now() - asked < std::chrono::minutes(1)) {
        text += '1';
        client.queue_change(uri, text);
        client.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(answered);
    waiter.join();
    client.settle();
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
    const json_t cancelled = client.response(hover);
    INFO(cancelled.dump());
    CHECK(cancelled["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_CANCELLED));
    // It stopped waiting, however long the check takes.
    CHECK_FALSE(client.server().workspace().checked_now(folder.path("cancelled.fb")));

    // One not cancelled is answered, on a line the last check knows, as this check can outlast a
    // request's deadline on a machine that's busy.
    CHECK(client
              .request("textDocument/hover",
                       {
                           { "textDocument", { { "uri", uri } } },
                           { "position", client_t::position(1, 11) },
                       })
              .is_object());
    client.settle();
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

namespace {

/// `twice`, and what the tests whose checks a gate holds ask about.
std::string spinning(const std::string_view after = {}) {
    return std::format("{}Const Function Spin(n: Int): Int\n"
                       "    Local t = 0\n"
                       "    For i = 1 To n\n"
                       "        t = t + 1\n"
                       "    End For\n"
                       "    Return t\n"
                       "End Function\n"
                       "Const Slow = Spin(3)\n"
                       "Local more = Twice(total)\n{}",
                       twice, after);
}

/// Where the response to request `id` is among what the client received, or its size.
std::size_t answered_at(const client_t& client, const std::int64_t id) {
    const std::vector<json_t> received = client.received();
    return last_where(received, received.size(), [&](const json_t& message) {
        return !message.contains("method") && message.value("id", json_t()) == id;
    });
}

} // namespace

TEST_CASE("a request whose check is stuck is answered by its deadline, and nothing else waits",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("stuck.fb");
    check_gate_t gate;
    client_t client({ .request_deadline = std::chrono::milliseconds(500), .gate = &gate });
    client.open(uri, twice);
    client.settle();

    gate.close();
    client.change(uri, spinning());
    const json_t document{ { "uri", uri } };

    // A hover on the changed line needs its check, and has until its deadline.
    const std::int64_t waiting =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(12, 15) } });
    // A second, behind it, goes as soon as the first does.
    const std::int64_t behind =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(12, 15) } });
    client.drain();

    // A save's formatting is answered meanwhile, as is the extension's ping, whatever the machine's
    // doing: that check isn't done when they are.
    const json_t formatting =
        client.send("textDocument/formatting",
                    {
                        { "textDocument", document },
                        { "options", { { "tabSize", 4 }, { "insertSpaces", true } } },
                    });
    CHECK(formatting.contains("result"));
    CHECK(client.send("fb/ping", json_t::object()).contains("result"));

    // A hover on an unchanged line is the last check's.
    json_t unchanged =
        client.send("textDocument/hover",
                    { { "textDocument", document }, { "position", client_t::position(1, 11) } });
    INFO(unchanged.dump());
    CHECK(unchanged["result"]["contents"]["value"].get<std::string>().contains("Twice"));
    CHECK_FALSE(client.server().workspace().checked_now(folder.path("stuck.fb")));

    CHECK(client.response(waiting)["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_FAILED));
    CHECK(client.response(behind)["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_FAILED));
    gate.open();
    client.settle();
}

TEST_CASE("a request cancelled while its check outlasts its deadline is answered as cancelled",
          "[lsp]") {
    // As on CI's machines: the check takes longer than the deadline, which the request sat out.
    const folder_t folder({});
    const std::string uri = folder.uri("outlasts.fb");
    check_gate_t gate;
    client_t client({ .request_deadline = std::chrono::seconds(5), .gate = &gate });
    client.open(uri, twice);
    client.settle();

    gate.close();
    client.change(uri, spinning());
    const json_t document{ { "uri", uri } };
    const std::int64_t hover =
        client.queue("textDocument/hover",
                     { { "textDocument", document }, { "position", client_t::position(12, 15) } });
    client.drain();
    gate.wait_for_one();
    client.notify("$/cancelRequest", { { "id", hover } });
    const json_t cancelled = client.response(hover);
    INFO(cancelled.dump());
    CHECK(cancelled["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_CANCELLED));
    gate.open();
    client.settle();
}

TEST_CASE("a request cancelled while a whole package's checks run stops waiting for them",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("package.fb");
    check_gate_t gate;
    client_t client({ .gate = &gate });
    client.open(uri, spinning());
    client.settle();

    // References compile the file again, off the worker, and never end here.
    gate.close();
    const std::int64_t references = client.queue(
        "textDocument/references", {
                                       { "textDocument", { { "uri", uri } } },
                                       { "position", client_t::position(1, 11) },
                                       { "context", { { "includeDeclaration", true } } },
                                   });
    client.drain();
    gate.wait_for_one();
    client.notify("$/cancelRequest", { { "id", references } });
    // With the check still held.
    const json_t cancelled = client.response(references);
    INFO(cancelled.dump());
    CHECK(cancelled["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_CANCELLED));
}

namespace {

/// Uses `depth` pieces of 4 KiB of the stack, and says how many.
// NOLINTNEXTLINE(misc-no-recursion): the stack's what it's for
std::size_t use_stack(const std::size_t depth) {
    std::array<volatile char, 4096> piece{};
    piece[depth % piece.size()] = 1;
    const std::size_t below = depth == 0 ? 0 : 1 + use_stack(depth - 1);
    return below + (piece[0] == 2 ? 1U : 0U);
}

} // namespace

TEST_CASE("a thread for the compiler has the stack a main thread has for it", "[lsp]") {
    // 48 MiB, which macOS' 512 KiB and Linux's 8 MiB for a std::thread don't hold.
    std::size_t used = 0;
    fb::stack_thread_t thread([&used] { used = use_stack(12000); });
    REQUIRE(thread.joinable());
    fb::stack_thread_t moved = std::move(thread);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what it says of itself
    CHECK_FALSE(thread.joinable());
    moved.join();
    CHECK_FALSE(moved.joinable());
    CHECK(used == 12000);
}

TEST_CASE("code nested as deep as the compiler goes is checked, and asked about", "[lsp]") {
    // On the threads the checks and requests have, not the main thread's, where parser_tests has
    // it.
    std::string text = "_ = 1";
    for (int i = 0; i < 1000; ++i) {
        text += " + 1";
    }

    const folder_t folder({});
    const std::string uri = folder.uri("deep.fb");
    client_t client({});
    client.open(uri, text + "\n");
    client.settle();
    CHECK(client.server().workspace().checked_now(folder.path("deep.fb")));
    CHECK(client
              .send("textDocument/hover",
                    {
                        { "textDocument", { { "uri", uri } } },
                        { "position", client_t::position(0, 4) },
                    })
              .is_object());

    // And past it, which is one error.
    std::string deeper = "_ = (";
    for (int i = 0; i < 5000; ++i) {
        deeper += '(';
    }

    client.change(uri, deeper + "\n");
    const json_t shown = client.published(uri);
    CHECK(shown.dump().contains("deeper than the compiler goes"));
}

TEST_CASE("a file: URI has an authority or doesn't", "[lsp]") {
    CHECK(fb::lsp::path_of("file:/a/b.fb") == fb::lsp::path_of("file:///a/b.fb"));
    CHECK(fb::lsp::path_of("FILE:/a/b.fb") == fb::lsp::path_of("file:///a/b.fb"));
    CHECK(fb::lsp::path_of("file:/C:/a/b.fb") == fb::lsp::path_of("file:///C:/a/b.fb"));
    CHECK(fb::lsp::path_of("file:///a/b.fb").has_value());
    CHECK_FALSE(fb::lsp::path_of("file:a/b.fb"));
    CHECK_FALSE(fb::lsp::path_of("https://a/b.fb"));
    // localhost is this machine.
    CHECK(fb::lsp::path_of("file://localhost/a/b.fb") == fb::lsp::path_of("file:///a/b.fb"));
    CHECK(fb::lsp::path_of("file://LOCALHOST/C:/a/b.fb") == fb::lsp::path_of("file:///C:/a/b.fb"));
    CHECK_FALSE(fb::lsp::path_of("file://localhost"));
    CHECK_FALSE(fb::lsp::path_of("file://"));
}

#ifdef _WIN32
TEST_CASE("a file on a Windows share has the URI VS Code has for it", "[lsp]") {
    constexpr std::string_view shared = "file://server/share/a%20b/x.fb";
    const std::filesystem::path path = fb::lsp::path_of(shared).value_or(std::filesystem::path());
    CHECK(path == std::filesystem::path("//server/share/a b/x.fb"));
    CHECK(fb::lsp::uri_of(path) == shared);
    // As the older form has it too, for a client that wrote what fb-lsp used to.
    CHECK(fb::lsp::path_of("file:////server/share/a%20b/x.fb") == path);
    CHECK(fb::lsp::uri_of(R"(\\server\share\a b\x.fb)") == shared);
    // A drive is as it was.
    CHECK(fb::lsp::uri_of("C:/a/x.fb") == "file:///C:/a/x.fb");
    // Normal, and not on the current drive, whatever spells it.
    CHECK(fb::lsp::uri_of("//server/share/a b/c/../x.fb") == shared);
    CHECK(fb::lsp::file_key("//Server/Share/x.fb") == fb::lsp::file_key("//server/share/x.fb"));
    CHECK(fb::lsp::file_key("//other/share/x.fb") != fb::lsp::file_key("//server/share/x.fb"));
}

TEST_CASE("a document on a Windows share has its diagnostics published to its own URI", "[lsp]") {
    // Loopback, where no share of this name is, says so at once, and the document's text is the
    // client's.
    const std::string uri = "file://127.0.0.1/fb-no-such-share/shared.fb";
    client_t client({ .load_folders = false });
    client.open(uri, "Local n: Nothing = 1\n");
    CHECK(client.published(uri).dump().contains("Nothing"));
    // Not one the client's path_of makes the same: VS Code compares what it's sent.
    const std::vector<json_t> received = client.received();
    CHECK(std::ranges::any_of(received, [&](const json_t& message) {
        return message.value("method", "") == "textDocument/publishDiagnostics" &&
               message["params"]["uri"] == uri;
    }));
}
#endif

#ifndef _WIN32
namespace {

/// Fails each start that asks for a stack of its own, as with memory limited.
int no_memory_for_stack(pthread_t* const thread, const pthread_attr_t* const attributes,
                        void* (*const work)(void*), void* const argument) {
    return attributes != nullptr ? EAGAIN : pthread_create(thread, nullptr, work, argument);
}

/// Fails everything.
int no_threads(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) {
    return EPERM;
}

} // namespace

TEST_CASE("a thread starts with the system's stack where the compiler's won't fit", "[lsp]") {
    std::atomic<bool> ran = false;
    fb::stack_thread_t thread([&ran] { ran = true; }, &no_memory_for_stack);
    thread.join();
    CHECK(ran);
    // Only for that: another failure is one.
    CHECK_THROWS_AS(fb::stack_thread_t([] {}, &no_threads), std::system_error);
}
#endif

TEST_CASE("an inbox finds a waiting $/cancelRequest, however messages have left it", "[lsp]") {
    const auto cancel = [](const int id) {
        return json_t{
            { "method", "$/cancelRequest" },
            { "params", { { "id", id } } },
        };
    };
    fb::lsp::inbox_t inbox;
    CHECK_FALSE(inbox.take_cancel(1));

    inbox.push(cancel(1));
    inbox.push(cancel(2));
    CHECK_FALSE(inbox.take_cancel(3));
    CHECK(inbox.take_cancel(1));
    CHECK_FALSE(inbox.take_cancel(1));

    // One popped isn't there, and the next one pushed still is.
    CHECK(inbox.pop().has_value());
    CHECK_FALSE(inbox.take_cancel(2));
    inbox.push(cancel(4));
    CHECK(inbox.try_pop().has_value());
    CHECK_FALSE(inbox.take_cancel(4));
    inbox.push(cancel(5));
    CHECK(inbox.take_cancel(5));

    // Nor is one dropped, and what comes after is taken as ever.
    inbox.push(cancel(6));
    inbox.push({ { "method", "exit" } });
    inbox.drop_waiting();
    CHECK_FALSE(inbox.try_pop().has_value());
    CHECK_FALSE(inbox.take_cancel(6));
    inbox.push(cancel(7));
    CHECK(inbox.take_cancel(7));
}

TEST_CASE("one waiting $/cancelRequest doesn't have each request look through what's waiting",
          "[lsp]") {
    fb::lsp::inbox_t inbox;
    for (int id = 0; id < 12000; ++id) {
        inbox.push({ { "id", id }, { "method", "textDocument/documentSymbol" } });
    }

    inbox.push({ { "method", "$/cancelRequest" }, { "params", { { "id", 11999 } } } });
    // Two of an id, and one with no id, which cancels nothing.
    inbox.push({ { "method", "$/cancelRequest" }, { "params", { { "id", 5 } } } });
    inbox.push({ { "method", "$/cancelRequest" }, { "params", { { "id", 5 } } } });
    inbox.push({ { "method", "$/cancelRequest" } });
    for (int id = 6; id < 11999; ++id) {
        CHECK_FALSE(inbox.take_cancel(id));
    }

    CHECK_FALSE(inbox.take_cancel(nullptr));
    CHECK(inbox.looked() == 0);
    CHECK(inbox.take_cancel(11999));
    CHECK(inbox.take_cancel(5));
    CHECK(inbox.take_cancel(5));
    CHECK_FALSE(inbox.take_cancel(5));
    CHECK(inbox.looked() == 3);
}

TEST_CASE("a request moved for its check doesn't wait behind a whole package's", "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("moved.fb");
    check_gate_t gate(true);
    client_t client({ .gate = &gate });
    client.open(uri, spinning());
    client.settle();

    // References compile the file again, which the gate holds, and an edit's check it doesn't.
    gate.close();
    const json_t document{ { "uri", uri } };
    const std::int64_t references = client.queue(
        "textDocument/references", {
                                       { "textDocument", document },
                                       { "position", client_t::position(1, 11) },
                                       { "context", { { "includeDeclaration", true } } },
                                   });
    client.drain();
    // Once they've started: a version typed past before its turn compiles nothing.
    gate.wait_for_one();
    client.queue_change(uri, std::string(twice) + "Local y = ");
    const std::int64_t completion =
        client.queue("textDocument/completion",
                     { { "textDocument", document }, { "position", client_t::position(4, 10) } });
    client.drain();
    const json_t answered = client.response(completion);
    INFO(answered.dump());
    CHECK(answered.contains("result"));
    gate.open();
    CHECK(client.response(references).contains("error"));
    CHECK(answered_at(client, completion) < answered_at(client, references));
    client.settle();
}

TEST_CASE("references past their deadline are answered, though what they need goes on compiling",
          "[lsp]") {
    const folder_t folder({});
    const std::string uri = folder.uri("referenced.fb");
    check_gate_t gate;
    client_t client({ .request_deadline = std::chrono::milliseconds(300), .gate = &gate });
    client.open(uri, spinning());
    client.settle();

    gate.close();
    const json_t params{
        { "textDocument", { { "uri", uri } } },
        { "position", client_t::position(1, 11) },
        { "context", { { "includeDeclaration", true } } },
    };
    const json_t first = client.send("textDocument/references", params);
    const json_t second = client.send("textDocument/references", params);
    // Both with what they need still held.
    CHECK(first["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_FAILED));
    CHECK(second["error"]["code"] ==
          static_cast<std::int32_t>(fb::lsp::error_code_t::REQUEST_FAILED));
    gate.open();
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
    // But those on a worker as shutdown's wait ran out, on a machine that's busy: one for each, and
    // never shutdown's own, whatever comes behind it.
    CHECK(answers.size() + 3 >= asked.size());
    CHECK(answers.contains(next - 1));
    CHECK(std::ranges::all_of(answers, [](const auto& each) { return each.second == 1; }));
}
