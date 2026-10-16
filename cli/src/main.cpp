#include "fb/commands.hpp"
#include "fb/options.hpp"
#include "fb/terminal.hpp"

#include <cstdio>
#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

int main(const int argc, char** argv) {
    try {
        fb::prepare_terminal();
        const std::vector<std::string> arguments = fb::command_line(argc, argv);
        const std::vector<std::string_view> views(arguments.begin(), arguments.end());
        const std::expected<fb::options_t, std::string> options = fb::parse_options(views);
        if (!options) {
            fb::report_usage_error(options.error());
            return fb::usage_exit_code;
        }

        return fb::run_command(*options);
    } catch (const std::exception& error) {
        // Plain stdio, so reporting the error can't throw too.
        std::fputs("error: fb failed unexpectedly: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return fb::compile_errors_exit_code;
    }
}
