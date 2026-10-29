#include "fb/options.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The `--show` names, indexed by stage_t, so they keep its order.
constexpr std::array<std::string_view, 5> stage_names = { "tokens", "ast", "types", "mir", "llvm" };

/// One of the commands that take a file, by name. Anything else starting with '-' is reported as
/// an unknown option.
std::expected<command_t, std::string> find_command(const std::string_view name) {
    for (const command_t command : { command_t::CHECK, command_t::BUILD, command_t::RUN,
                                     command_t::TEST, command_t::BINDGEN }) {
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
    if (list.empty()) {
        return std::unexpected("'--show' needs stages: tokens, ast, types, mir, llvm, or all");
    }

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
    case command_t::TEST:
        return "test";
    case command_t::BINDGEN:
        return "bindgen";
    }

    std::unreachable();
}

std::string_view stage_name(const stage_t stage) {
    return stage_names[std::to_underlying(stage)];
}

namespace {

/// Reads `all` into `options`, which keeps what it read before a mistake, like a --color.
std::expected<void, std::string> parse_into(const std::span<const std::string_view> all,
                                            options_t& options) {
    // fb's --color can come before the command too: "fb --color never check x.fb".
    std::size_t start = 0;
    while (start < all.size() && (all[start] == "--color" || all[start].starts_with("--color="))) {
        std::string_view when;
        if (all[start] != "--color") {
            when = all[start].substr(std::string_view("--color=").size());
        } else if (start + 1 < all.size()) {
            when = all[++start];
        } else {
            return std::unexpected("'--color' needs a value");
        }

        const std::expected<color_t, std::string> color = parse_color(when);
        if (!color) {
            return std::unexpected(color.error());
        }

        options.color = *color;
        ++start;
    }

    const std::span<const std::string_view> arguments = all.subspan(start);
    // "fb help build" and "fb --help build" show a command's help, and "fb help help" fb's.
    if (arguments.empty() || arguments[0] == "-h" || arguments[0] == "--help" ||
        arguments[0] == "help") {
        if (arguments.size() > 2) {
            return std::unexpected(
                std::format("unexpected argument '{}'; 'fb help' takes one command", arguments[2]));
        }

        if (arguments.size() == 2 && arguments[1] != "help") {
            const std::expected<command_t, std::string> command = find_command(arguments[1]);
            if (!command) {
                return std::unexpected(command.error());
            }

            options.help_for = *command;
        }

        return {};
    }

    if (arguments[0] == "--version") {
        if (arguments.size() > 1) {
            return std::unexpected(
                std::format("unexpected argument '{}'; 'fb --version' takes none", arguments[1]));
        }

        options.command = command_t::VERSION;
        return {};
    }

    const std::expected<command_t, std::string> command = find_command(arguments[0]);
    if (!command) {
        return std::unexpected(command.error());
    }

    options.command = *command;
    // fb-bindgen reads its own options, but fb's --color is fb's.
    if (options.command == command_t::BINDGEN) {
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const std::string_view argument = arguments[i];
            if (argument == "-h" || argument == "--help") {
                options.help_for = command_t::BINDGEN;
                options.command = command_t::HELP;
                return {};
            }

            if (argument == "--color" || argument.starts_with("--color=")) {
                std::string_view when;
                if (argument != "--color") {
                    when = argument.substr(std::string_view("--color=").size());
                } else if (i + 1 < arguments.size()) {
                    when = arguments[++i];
                } else {
                    return std::unexpected("'--color' needs a value");
                }

                const std::expected<color_t, std::string> color = parse_color(when);
                if (!color) {
                    return std::unexpected(color.error());
                }

                options.color = *color;
                continue;
            }

            options.passed.emplace_back(argument);
        }

        return {};
    }

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
            return {};
        }

        // What comes after "--" goes to the program `fb run` runs.
        if (argument == "--") {
            if (options.command != command_t::RUN) {
                return std::unexpected(
                    "'--' passes what follows it to the program, which only 'fb run' runs");
            }

            options.program_arguments.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                             arguments.end());
            break;
        }

        // Stays a success unless an option's value is bad.
        std::expected<void, std::string> parsed;
        if (argument == "--release") {
            options.mode = build_mode_t::RELEASE;
        } else if (name == "-o" || name == "--output") {
            if (options.command != command_t::BUILD) {
                return std::unexpected(std::format("'{}' only works with 'fb build'", name));
            }

            parsed = value().and_then(
                [&](const std::string_view path) -> std::expected<void, std::string> {
                    // A name the executable can have, not a folder's.
                    if (path.empty() || path.ends_with('/') || path.ends_with('\\')) {
                        return std::unexpected(std::format(
                            "'{}' takes the executable's file name, like 'out/game'", name));
                    }

                    options.output = path_from_utf8(path);
                    return {};
                });
        } else if (name == "--color") {
            parsed = value().and_then(parse_color).transform([&](const color_t color) {
                options.color = color;
            });
        } else if (name == "--show") {
            if (options.command == command_t::TEST) {
                return std::unexpected("'--show' only works with 'fb check', 'fb build', and "
                                       "'fb run'");
            }

            parsed = value().and_then(
                [&](const std::string_view list) { return add_stages(list, options.show); });
        } else if (name == "--filter") {
            if (options.command != command_t::TEST) {
                return std::unexpected("'--filter' only works with 'fb test'");
            }

            parsed = value().transform(
                [&](const std::string_view text) { options.filter = std::string(text); });
        } else if (argument.starts_with('-')) {
            return std::unexpected(std::format("unknown option '{}'", argument));
        } else if (!options.input.empty()) {
            return std::unexpected(std::format(
                "unexpected argument '{}'; 'fb {}' takes one .fb file or package folder", argument,
                command_name(options.command)));
        } else {
            options.input = path_from_utf8(argument);
        }

        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    }

    // Views print once each, in pipeline order, however they were given.
    std::ranges::sort(options.show);
    const auto repeats = std::ranges::unique(options.show);
    options.show.erase(repeats.begin(), repeats.end());
    return {};
}

} // namespace

std::expected<options_t, usage_error_t> parse_options(const std::span<const std::string_view> all) {
    // A new options_t is HELP without help_for: the help for fb as a whole.
    options_t options;
    if (std::expected<void, std::string> parsed = parse_into(all, options); !parsed) {
        return std::unexpected(
            usage_error_t{ .message = std::move(parsed.error()), .color = options.color });
    }

    return options;
}

} // namespace fb
