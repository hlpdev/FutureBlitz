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
#include <optional>
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
    mcp_server_t server(folder);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }

        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }

        if (const std::optional<std::string> answer = server.handle(line)) {
            std::fwrite(answer->data(), 1, answer->size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
    }

    // The server may still be reading std's reference on a thread of its own, which a client
    // that's done shouldn't wait for.
    end_process(0);
}

} // namespace fb
