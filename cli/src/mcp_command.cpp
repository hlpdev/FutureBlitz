#include "fb/mcp_command.hpp"

#include "fb/commands.hpp"
#include "fb/console.hpp"
#include "fb/mcp.hpp"
#include "fb/terminal.hpp"
#include "fb/utf8.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fb {

int mcp_command(const options_t& options) {
    const std::filesystem::path folder =
        options.input.empty() ? std::filesystem::current_path() : options.input;
    std::error_code failed;
    if (!std::filesystem::is_directory(folder, failed)) {
        console_t(options.color)
            .error(std::format("{} isn't a folder; 'fb mcp' takes the package's folder",
                               to_utf8(folder)));
        return usage_exit_code;
    }

#ifdef _WIN32
    // A message is a line, which text mode would end in "\r\n".
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
#endif
    // A tool call runs on a thread of its own, so the loop goes on reading: a ping, a call's
    // cancellation, and stdin's end, which ends fb, even while a call runs.
    mcp_server_t server(folder, [](const std::string& answer) {
        std::fwrite(answer.data(), 1, answer.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }

        if (line.find_first_not_of(" \t") != std::string::npos) {
            server.receive(line);
        }
    }

    // The server may still be reading std's reference, or running a call, on threads of its own,
    // which a client that's done doesn't wait for.
    end_process(0);
}

} // namespace fb
