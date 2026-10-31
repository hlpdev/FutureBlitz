#include "fb/options.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The `--show` names, indexed by stage_t, so they keep its order.
constexpr std::array<std::string_view, 5> stage_names = { "tokens", "ast", "types", "mir", "llvm" };

/// One of the commands that take a file, by name. Anything else starting with '-' is reported as
/// an unknown option.
std::expected<command_t, std::string> find_command(const std::string_view name) {
    for (const command_t command : {
             command_t::NEW,
             command_t::CHECK,
             command_t::BUILD,
             command_t::RUN,
             command_t::TEST,
             command_t::BINDGEN,
             command_t::FMT,
             command_t::DOC,
             command_t::ADD,
             command_t::UPDATE,
             command_t::SEARCH,
             command_t::PUBLISH,
             command_t::YANK,
             command_t::OWNER,
             command_t::LOGIN,
             command_t::LOGOUT,
         }) {
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
    case command_t::NEW:
        return "new";
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
    case command_t::FMT:
        return "fmt";
    case command_t::DOC:
        return "doc";
    case command_t::ADD:
        return "add";
    case command_t::UPDATE:
        return "update";
    case command_t::SEARCH:
        return "search";
    case command_t::PUBLISH:
        return "publish";
    case command_t::YANK:
        return "yank";
    case command_t::OWNER:
        return "owner";
    case command_t::LOGIN:
        return "login";
    case command_t::LOGOUT:
        return "logout";
    }

    std::unreachable();
}

bool is_package_command(const command_t command) {
    return command >= command_t::ADD;
}

std::string_view stage_name(const stage_t stage) {
    return stage_names[std::to_underlying(stage)];
}

namespace {

/// The package manager's commands' arguments, after the command's name, into `options`.
std::expected<void, std::string>
parse_package_arguments(const std::span<const std::string_view> arguments, options_t& options) {
    const command_t command = options.command;
    const std::string usage = std::format("'fb {}'", command_name(command));
    // Which options each command takes.
    const auto takes = [&](const std::string_view name) {
        switch (command) {
        case command_t::ADD:
            return name == "--registry" || name == "--beside" || name == "--offline";
        case command_t::UPDATE:
            return name == "--offline";
        case command_t::SEARCH:
            return name == "--registry" || name == "--limit";
        case command_t::PUBLISH:
            return name == "--registry" || name == "--dry-run" || name == "--offline" ||
                   name == "--no-verify";
        case command_t::YANK:
            return name == "--registry" || name == "--undo";
        case command_t::OWNER:
            return name == "--registry" || name == "--add" || name == "--remove";
        case command_t::LOGIN:
            return name == "--registry" || name == "--token";
        default:
            return name == "--registry";
        }
    };

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        const std::size_t equals =
            argument.starts_with("--") ? argument.find('=') : std::string_view::npos;
        const std::string_view name = argument.substr(0, equals);
        const auto value = [&] -> std::expected<std::string_view, std::string> {
            if (equals != std::string_view::npos) {
                return argument.substr(equals + 1);
            }

            if (i + 1 < arguments.size()) {
                return arguments[++i];
            }

            return std::unexpected(std::format("'{}' needs a value", name));
        };

        if (argument == "-h" || argument == "--help") {
            options.help_for = command;
            options.command = command_t::HELP;
            return {};
        }

        if (name == "--color") {
            const std::expected<color_t, std::string> color = value().and_then(parse_color);
            if (!color) {
                return std::unexpected(color.error());
            }

            options.color = *color;
            continue;
        }

        if (!argument.starts_with('-')) {
            if (command == command_t::PUBLISH) {
                if (!options.input.empty()) {
                    return std::unexpected(std::format(
                        "unexpected argument '{}'; {} takes one package folder", argument, usage));
                }

                options.input = path_from_utf8(argument);
            } else {
                options.names.emplace_back(argument);
            }

            continue;
        }

        if (!takes(name)) {
            if (name == "--registry" || name == "--beside" || name == "--offline" ||
                name == "--limit" || name == "--dry-run" || name == "--undo" || name == "--add" ||
                name == "--remove" || name == "--token" || name == "--no-verify") {
                return std::unexpected(std::format("'{}' doesn't work with {}", name, usage));
            }

            return std::unexpected(std::format("unknown option '{}'", argument));
        }

        const bool flag = name == "--beside" || name == "--offline" || name == "--dry-run" ||
                          name == "--undo" || name == "--token" || name == "--no-verify";
        if (flag && equals != std::string_view::npos) {
            return std::unexpected(std::format("'{}' takes no value", name));
        }

        if (name == "--beside") {
            options.beside = true;
        } else if (name == "--offline") {
            options.offline = true;
        } else if (name == "--dry-run") {
            options.dry_run = true;
        } else if (name == "--no-verify") {
            options.no_verify = true;
        } else if (name == "--undo") {
            options.undo = true;
        } else if (name == "--token") {
            options.token = true;
        } else {
            const std::expected<std::string_view, std::string> given = value();
            if (!given) {
                return std::unexpected(given.error());
            }

            if (given->empty()) {
                return std::unexpected(std::format("'{}' needs a value", name));
            }

            if (name == "--registry") {
                options.registry = std::string(*given);
            } else if (name == "--limit") {
                std::size_t limit = 0;
                const auto [end, error] =
                    std::from_chars(given->data(), given->data() + given->size(), limit);
                if (error != std::errc() || end != given->data() + given->size() || limit == 0 ||
                    limit > 100) {
                    return std::unexpected(
                        std::format("'--limit' takes a number from 1 to 100, not '{}'", *given));
                }

                options.limit = limit;
            } else {
                if (options.owner) {
                    return std::unexpected("'fb owner' takes one --add or --remove at a time");
                }

                options.owner = std::string(*given);
                options.remove_owner = name == "--remove";
            }
        }
    }

    // How many names each command takes.
    const std::size_t count = options.names.size();
    if (command == command_t::ADD && count == 0) {
        return std::unexpected("'fb add' needs the packages it adds, like 'fb add physics' or "
                               "'fb add physics@1.2'");
    }

    if (command == command_t::SEARCH && count == 0) {
        return std::unexpected("'fb search' needs what it looks for, like 'fb search physics'");
    }

    if (command == command_t::YANK && (count != 1 || !options.names[0].contains('@'))) {
        return std::unexpected(
            "'fb yank' takes one package's version, like 'fb yank physics@1.2.0'");
    }

    if (command == command_t::OWNER && count != 1) {
        return std::unexpected("'fb owner' takes one package, like 'fb owner physics --add alice'");
    }

    if ((command == command_t::LOGIN || command == command_t::LOGOUT) && count > 0) {
        return std::unexpected(
            std::format("unexpected argument '{}'; {} takes none", options.names[0], usage));
    }

    return {};
}

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
    if (is_package_command(options.command)) {
        return parse_package_arguments(arguments.subspan(1), options);
    }

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

        // fb fmt takes files and folders, and --check.
        if (options.command == command_t::FMT && argument == "--check") {
            options.check = true;
            continue;
        }

        if (options.command == command_t::FMT && !argument.starts_with('-')) {
            options.paths.push_back(path_from_utf8(argument));
            continue;
        }

        if (options.command == command_t::FMT && name == "--check") {
            return std::unexpected("'--check' takes no value");
        }

        if (options.command == command_t::FMT && name != "--color") {
            return std::unexpected(std::format(
                "'{}' doesn't work with 'fb fmt', whose options are --check and --color", name));
        }

        // fb doc --check checks the docs, and writes nothing.
        if (options.command == command_t::DOC && argument == "--check") {
            options.check = true;
            continue;
        }

        if (name == "--check") {
            return std::unexpected(options.command == command_t::DOC
                                       ? "'--check' takes no value"
                                       : "'--check' only works with 'fb fmt' and 'fb doc'");
        }

        // fb new takes the folder it makes, and --color.
        if (options.command == command_t::NEW && argument.starts_with('-') && name != "--color") {
            return std::unexpected(
                std::format("'{}' doesn't work with 'fb new', whose only option is --color", name));
        }

        // Stays a success unless an option's value is bad.
        std::expected<void, std::string> parsed;
        if (argument == "--offline") {
            options.offline = true;
        } else if (name == "--offline") {
            return std::unexpected("'--offline' takes no value");
        } else if (argument == "--release") {
            options.mode = build_mode_t::RELEASE;
        } else if (name == "-o" || name == "--output") {
            if (options.command != command_t::BUILD && options.command != command_t::TEST &&
                options.command != command_t::DOC) {
                return std::unexpected(
                    std::format("'{}' only works with 'fb build', 'fb test', and 'fb doc'", name));
            }

            parsed = value().and_then(
                [&](const std::string_view path) -> std::expected<void, std::string> {
                    // A name the executable or the JSON can have, not a folder's.
                    if (path.empty() || path.ends_with('/') || path.ends_with('\\')) {
                        return std::unexpected(
                            options.command == command_t::DOC
                                ? std::format("'{}' takes the JSON's file name, like "
                                              "'out/game.json', or '-' for stdout",
                                              name)
                                : std::format("'{}' takes the executable's file name, like "
                                              "'out/game'",
                                              name));
                    }

                    options.output = path_from_utf8(path);
                    return {};
                });
        } else if (name == "--color") {
            parsed = value().and_then(parse_color).transform([&](const color_t color) {
                options.color = color;
            });
        } else if (name == "--show") {
            if (options.command == command_t::TEST || options.command == command_t::DOC) {
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
            return std::unexpected(std::format("unexpected argument '{}'; 'fb {}' takes {}",
                                               argument, command_name(options.command),
                                               options.command == command_t::NEW
                                                   ? "the new package's folder"
                                                   : "one .fb file or package folder"));
        } else {
            options.input = path_from_utf8(argument);
        }

        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    }

    if (options.check && options.output) {
        return std::unexpected("'fb doc --check' writes no JSON, so it takes no '-o'");
    }

    if (options.command == command_t::NEW && options.input.empty()) {
        return std::unexpected("'fb new' needs the new package's folder, like 'fb new game'");
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
