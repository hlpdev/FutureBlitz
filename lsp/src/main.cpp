// fb-lsp: FutureBlitz's language server, which an editor starts and talks to in JSON-RPC over
// stdin and stdout.

#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"

#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main() {
#ifdef _WIN32
    // A message's length counts its bytes, which text mode would change at each line break.
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
#endif
    std::ios::sync_with_stdio(false);

    std::mutex output;
    const auto send = [&output](const fb::lsp::json_t& message) {
        const std::scoped_lock lock(output);
        fb::lsp::write_message(
            std::cout, message.dump(-1, ' ', false, fb::lsp::json_t::error_handler_t::replace));
    };
    fb::lsp::server_t server(send);
    while (const std::optional<std::string> body = fb::lsp::read_message(std::cin)) {
        const fb::lsp::json_t message = fb::lsp::json_t::parse(*body, nullptr, false);
        if (message.is_discarded()) {
            send({
                { "jsonrpc", "2.0" },
                { "id", nullptr },
                { "error",
                  { { "code", static_cast<int>(fb::lsp::error_code_t::PARSE_ERROR) },
                    { "message", "the message isn't JSON" } } },
            });
            continue;
        }

        server.handle(message);
        if (const std::optional<int> code = server.exit_code()) {
            return *code;
        }
    }

    // The client went away without saying exit.
    return 1;
}
