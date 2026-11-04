// fb-lsp: FutureBlitz's language server, which an editor starts and talks to in JSON-RPC over
// stdin and stdout.

#include "fb/lsp/inbox.hpp"
#include "fb/lsp/outbox.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

/// Writes the messages sent so far, for a moment, as a client that's gone reads none of them, and
/// ends the process with `code`, without waiting for a check that's running, which can take
/// seconds, as a whole package's does.
[[noreturn]] void finish(fb::lsp::outbox_t& outbox, const int code) {
    static_cast<void>(outbox.flush(std::chrono::seconds(2)));
    std::_Exit(code);
}

/// Answers the client's messages until it says exit, or goes away without saying it.
void serve() {
    // Reading std::cin would flush std::cout, which it's tied to, on the reader's thread, and two
    // threads in its buffer sent a message twice, in pieces, or with bytes gone, and the client's
    // reader, lost from there on, got no answer again. Only the outbox's thread writes messages,
    // to a stdout nothing else can reach.
    std::cin.tie(nullptr);
    fb::lsp::fd_buffer_t output(fb::lsp::take_stdout());
    std::ostream out(&output);
    fb::lsp::outbox_t outbox(out);
    const auto send = [&outbox](const fb::lsp::json_t& message) {
        outbox.push(message.dump(-1, ' ', false, fb::lsp::json_t::error_handler_t::replace));
    };
    fb::lsp::server_t server(send);
    // A thread of its own reads, so what waits behind a request is there for the server to see,
    // as a $/cancelRequest of it is.
    fb::lsp::inbox_t inbox;
    server.set_inbox(&inbox);
    std::thread reader([&] {
        while (const std::optional<std::string> body = fb::lsp::read_message(std::cin)) {
            fb::lsp::json_t message = fb::lsp::json_t::parse(*body, nullptr, false);
            if (message.is_discarded()) {
                const fb::lsp::json_t error{
                    { "code", static_cast<int>(fb::lsp::error_code_t::PARSE_ERROR) },
                    { "message", "the message isn't JSON" },
                };
                send({ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", error } });
                continue;
            }

            inbox.push(std::move(message));
        }

        inbox.close();
    });
    reader.detach();
    while (const std::optional<fb::lsp::json_t> message = inbox.pop()) {
        server.handle(*message);
        if (const std::optional<int> code = server.exit_code()) {
            finish(outbox, *code);
        }
    }

    finish(outbox, 1);
}

} // namespace

int main() {
#ifdef _WIN32
    // A message's length counts its bytes, which text mode would change at each line break.
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
#endif
    std::ios::sync_with_stdio(false);
    try {
        serve();
    } catch (const std::exception& error) {
        // std::print doesn't link with MinGW's C++ library.
        std::fputs("fb-lsp: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
    } catch (...) {
        std::fputs("fb-lsp: an unknown error\n", stderr);
    }

    return 1;
}
