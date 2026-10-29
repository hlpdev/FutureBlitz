// fb-lsp: FutureBlitz's language server, which an editor starts and talks to in JSON-RPC over
// stdin and stdout.

#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

std::mutex output;

/// Writes the messages written so far, and ends the process with `code`, without waiting for a
/// check that's running, which can take seconds, as a whole package's does, and with no message
/// half written.
[[noreturn]] void finish(const int code) {
    const std::scoped_lock lock(output);
    std::cout.flush();
    std::_Exit(code);
}

/// Answers the client's messages until it says exit, or goes away without saying it.
void serve() {
    const auto send = [](const fb::lsp::json_t& message) {
        const std::scoped_lock lock(output);
        fb::lsp::write_message(
            std::cout, message.dump(-1, ' ', false, fb::lsp::json_t::error_handler_t::replace));
    };
    fb::lsp::server_t server(send);
    while (const std::optional<std::string> body = fb::lsp::read_message(std::cin)) {
        const fb::lsp::json_t message = fb::lsp::json_t::parse(*body, nullptr, false);
        if (message.is_discarded()) {
            const fb::lsp::json_t error{
                { "code", static_cast<int>(fb::lsp::error_code_t::PARSE_ERROR) },
                { "message", "the message isn't JSON" },
            };
            send({ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", error } });
            continue;
        }

        server.handle(message);
        if (const std::optional<int> code = server.exit_code()) {
            finish(*code);
        }
    }

    finish(1);
}

} // namespace

int main() {
#ifdef _WIN32
    // A message's length counts its bytes, which text mode would change at each line break.
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
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
