#include "fb/options.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <utility>

namespace fb {
namespace {

/// The `--show` names, indexed by stage_t, so they keep its order.
constexpr std::array<std::string_view, 5> stage_names = { "tokens", "ast", "types", "mir", "llvm" };

/// One of the commands that take a file, by name. Anything else starting with '-' is reported as
/// an unknown option.
std::expected<command_t, std::string> find_command(const std::string_view name) {
    for (const command_t command : { command_t::CHECK, command_t::BUILD, command_t::RUN }) {
        if (command_name(command) == name) {
            return command;
        }
    }

    if (name.starts_with('-')) {
        return std::unexpected(std::format("unknown option '{}'", name));
    }

    return std::unexpected(std::format("unknown command '{}'", name));
}

std::expected<color_t, std::string> parse_color(const std::string_view when) {
    if (when == "auto") {
        return color_t::AUTO;
    }

    if (when == "always") {
        return color_t::ALWAYS;
    }

    if (when == "never") {
        return color_t::NEVER;
    }

    return std::unexpected(std::format("'--color' takes auto, always, or never, not '{}'", when));
}

/// Adds a comma-separated list of stages, where "all" means every stage. Repeats are fine:
/// parse_options removes them.
std::expected<void, std::string> add_stages(const std::string_view list,
                                            std::vector<stage_t>& show) {
    for (const auto part : std::views::split(list, ',')) {
        const std::string_view name(part.begin(), part.end());
        if (name == "all") {
            for (std::size_t i = 0; i < stage_names.size(); ++i) {
                show.push_back(static_cast<stage_t>(i));
            }

            continue;
        }

        const auto found = std::ranges::find(stage_names, name);
        if (found == stage_names.end()) {
            return std::unexpected(std::format(
                "unknown stage '{}'; the stages are tokens, ast, types, mir, llvm, and all", name));
        }

        show.push_back(static_cast<stage_t>(found - stage_names.begin()));
    }

    return {};
}

} // namespace

std::string_view command_name(const command_t command) {
    switch (command) {
    case command_t::HELP:
        return "help";
    case command_t::VERSION:
        return "version";
    case command_t::CHECK:
        return "check";
    case command_t::BUILD:
        return "build";
    case command_t::RUN:
        return "run";
    }

    std::unreachable();
}

std::string_view stage_name(const stage_t stage) {
    return stage_names[std::to_underlying(stage)];
}

std::expected<options_t, std::string>
parse_options(const std::span<const std::string_view> arguments) {
    // A new options_t is HELP without help_for: the help for fb as a whole.
    options_t options;
    if (arguments.empty() || arguments[0] == "-h" || arguments[0] == "--help") {
        return options;
    }

    if (arguments[0] == "--version") {
        options.command = command_t::VERSION;
        return options;
    }

    if (arguments[0] == "help") {
        if (arguments.size() > 1) {
            const std::expected<command_t, std::string> command = find_command(arguments[1]);
            if (!command) {
                return std::unexpected(command.error());
            }

            options.help_for = *command;
        }

        return options;
    }

    const std::expected<command_t, std::string> command = find_command(arguments[0]);
    if (!command) {
        return std::unexpected(command.error());
    }

    options.command = *command;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];

        // "--name value" or "--name=value"
        const std::size_t equals =
            argument.starts_with("--") ? argument.find('=') : std::string_view::npos;
        const std::string_view name = argument.substr(0, equals);
        // Taking the next argument as the value also skips it in the loop.
        const auto value = [&] -> std::expected<std::string_view, std::string> {
            if (equals != std::string_view::npos) {
                return argument.substr(equals + 1);
            }

            if (i + 1 < arguments.size()) {
                return arguments[++i];
            }

            return std::unexpected(std::format("'{}' needs a value", name));
        };

        // "fb build --help" shows that command's help and ignores what comes after it.
        if (argument == "-h" || argument == "--help") {
            options.help_for = options.command;
            options.command = command_t::HELP;
            return options;
        }

        // Stays a success unless an option's value is bad.
        std::expected<void, std::string> parsed;
        if (argument == "--release") {
            options.mode = build_mode_t::RELEASE;
        } else if (name == "-o" || name == "--output") {
            if (options.command != command_t::BUILD) {
                return std::unexpected(std::format("'{}' only works with 'fb build'", name));
            }

            parsed = value().transform(
                [&](const std::string_view path) { options.output = path_from_utf8(path); });
        } else if (name == "--color") {
            parsed = value().and_then(parse_color).transform([&](const color_t color) {
                options.color = color;
            });
        } else if (name == "--show") {
            parsed = value().and_then(
                [&](const std::string_view list) { return add_stages(list, options.show); });
        } else if (argument.starts_with('-')) {
            return std::unexpected(std::format("unknown option '{}'", argument));
        } else if (!options.input.empty()) {
            return std::unexpected(std::format("unexpected argument '{}'; 'fb {}' takes one file",
                                               argument, command_name(options.command)));
        } else {
            options.input = path_from_utf8(argument);
        }

        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    }

    if (options.input.empty()) {
        return std::unexpected(
            std::format("'fb {}' needs a .fb file", command_name(options.command)));
    }

    // Views print once each, in pipeline order, however they were given.
    std::ranges::sort(options.show);
    const auto repeats = std::ranges::unique(options.show);
    options.show.erase(repeats.begin(), repeats.end());
    return options;
}

} // namespace fb
