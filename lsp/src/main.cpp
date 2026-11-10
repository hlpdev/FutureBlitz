// fb-lsp: FutureBlitz's language server, which an editor starts and talks to in JSON-RPC over
// stdin and stdout.

#include "fb/lsp/inbox.hpp"
#include "fb/lsp/outbox.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/server.hpp"
#include "fb/stack_thread.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
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

/// How many bytes wait for a client that isn't reading before it's taken to be gone.
constexpr std::size_t most_waiting = std::size_t{ 256 } * 1024 * 1024;
/// How long one message can hold the message thread, once the client's said exit or gone, before
/// the reader ends the process for it: longer than shutdown waits, or its answer would be lost. And
/// how long one can hold it before fb/ping goes unanswered.
constexpr std::chrono::seconds default_exit_wait(6);
constexpr std::chrono::seconds busy_limit(60);

/// default_exit_wait, or for the tests, FB_LSP_EXIT_WAIT's seconds: a test of what's answered
/// behind a message that holds the thread can't let how busy its machine is decide whether that
/// message outlasts the wait.
std::chrono::seconds exit_wait() {
    static const std::chrono::seconds wait = [] {
        const char* const given = std::getenv("FB_LSP_EXIT_WAIT");
        const long seconds = given != nullptr ? std::strtol(given, nullptr, 10) : 0;
        return seconds > 0 ? std::chrono::seconds(seconds) : default_exit_wait;
    }();
    return wait;
}

/// Writes the messages sent so far, for as long as the client's reading them and a moment past
/// when it stops, as one that's gone reads none of them, and ends the process with `code`,
/// without waiting for a check that's running, which can take seconds, as a whole package's does.
[[noreturn]] void finish(fb::lsp::outbox_t& outbox, const int code) {
    static_cast<void>(outbox.flush(std::chrono::seconds(5)));
    std::_Exit(code);
}

/// Says what went wrong on stderr, which only a person looks at. std::print doesn't link with
/// MinGW's C++ library.
void say_error(const std::exception& error) {
    std::fputs("fb-lsp: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputs("\n", stderr);
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
    // Whether the client's said shutdown, so one that closes what it reads after is one that's
    // done.
    std::atomic<bool> shutdown = false;
    fb::lsp::outbox_t outbox(
        out, most_waiting,
        [] {
            std::fputs("fb-lsp: the client isn't reading what it's sent\n", stderr);
            std::_Exit(1);
        },
        [&shutdown] {
            if (shutdown) {
                std::_Exit(0);
            }

            std::fputs("fb-lsp: the client closed what it reads\n", stderr);
            std::_Exit(1);
        });
    outbox.set_progress([&output] { return output.written(); });
    const auto send = [&outbox](const fb::lsp::json_t& message) {
        std::string replaces;
        if (message.value("method", std::string()) == "textDocument/publishDiagnostics") {
            replaces = message["params"].value("uri", std::string());
        }

        outbox.push(message.dump(-1, ' ', false, fb::lsp::json_t::error_handler_t::replace),
                    std::move(replaces));
    };
    fb::lsp::server_t server(send);
    // A thread of its own reads, so what waits behind a request is there for the server to see,
    // as a $/cancelRequest of it is, and the client's told the server's going on while the message
    // thread's busy with one message, and that the client's gone whatever it's doing.
    fb::lsp::inbox_t inbox;
    server.set_inbox(&inbox);
    fb::lsp::liveness_t liveness(busy_limit);
    const auto read_all = [&] {
        // The message thread ends the process once it gets to the end of what's waiting, unless
        // one message is holding it there. What's waiting is answered only for a client that said
        // shutdown, which those before it are owed; one that exits or goes without it wants none,
        // and a long queue of quick messages would keep it here with none of them holding it.
        const auto wait_for_it = [&] {
            while (!liveness.held_for(exit_wait())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
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

            const fb::lsp::json_t method = message.is_object()
                                               ? message.value("method", fb::lsp::json_t())
                                               : fb::lsp::json_t();
            if (method == "fb/ping" && message.contains("id")) {
                if (liveness.going()) {
                    send({ { "jsonrpc", "2.0" }, { "id", message["id"] }, { "result", nullptr } });
                }

                continue;
            }

            if (method == "shutdown") {
                shutdown = true;
            }

            const bool exiting = method == "exit";
            if (exiting && !shutdown) {
                inbox.drop_waiting();
            }

            inbox.push(std::move(message));
            if (exiting) {
                wait_for_it();
                finish(outbox, shutdown ? 0 : 1);
            }
        }

        if (!shutdown) {
            inbox.drop_waiting();
        }

        inbox.close();
        wait_for_it();
        finish(outbox, 1);
    };
    // The reader is never detached, and never joined, as nothing past here returns: what this
    // has would wait for checks, and for a client to read, and the reader has it all. A thread
    // detached as it starts is now and then never run with MinGW's winpthreads, on a busy
    // machine, which would leave the server reading nothing.
    const fb::stack_thread_t reader([&] {
        try {
            read_all();
        } catch (const std::exception& error) {
            say_error(error);
            finish(outbox, 1);
        }
    });
    try {
        while (const std::optional<fb::lsp::json_t> message = inbox.pop()) {
            liveness.handling();
            server.handle(*message);
            liveness.handled();
            if (const std::optional<int> code = server.exit_code()) {
                finish(outbox, *code);
            }
        }
    } catch (const std::exception& error) {
        say_error(error);
    }

    finish(outbox, 1);
}

} // namespace

int main() {
#ifdef _WIN32
    // A message's length counts its bytes, which text mode would change at each line break.
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
#else
    // A write to a client that's gone fails, as on Windows, and the outbox ends the process, with
    // its exit code; a registry's socket that's closed mustn't end it at all.
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
#endif
    std::ios::sync_with_stdio(false);
    try {
        serve();
    } catch (const std::exception& error) {
        say_error(error);
    } catch (...) {
        std::fputs("fb-lsp: an unknown error\n", stderr);
    }

    return 1;
}
