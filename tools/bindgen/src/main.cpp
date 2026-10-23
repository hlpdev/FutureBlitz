#include "fb/bindgen.hpp"
#include "fb/manifest.hpp"
#include "fb/terminal.hpp"
#include "fb/utf8.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// fb-bindgen: turns a C header into a FutureBlitz module. `fb bindgen` runs it.

namespace {

constexpr int failed_exit_code = 1;
constexpr int usage_exit_code = 2;

constexpr std::string_view help = R"(fb-bindgen turns a C header into a FutureBlitz module.

Usage: fb-bindgen <header.h> [options]

Options:
  -o, --output <file>   Where the module goes (default: <header>.fb in this folder)
  --lib <name>          The [native] library its Extern block names (default: the header's
                        name; "" leaves Lib out)
  --target <triple>     The target to read the header for (default: this build's)
  --std <standard>      The C standard to read it with, like c17 (default: the C compiler's)
  -I <folder>           A folder the header's #includes are found in
  -D <name>[=<value>]   Defines a macro
  -h, --help            Show this help
)";

/// What the command line asked for.
struct request_t {
    fb::bindgen_options_t options;
    std::filesystem::path output;
    /// Whether -o named the output, rather than the header's name.
    bool named_output = false;
};

/// How people run it: "fb bindgen" when fb runs it, which says so with "--fb" first.
std::string_view command_name = "fb-bindgen";

/// Checks that an option's value isn't empty: `-D ""` defines nothing, and `--target ""` would
/// quietly mean this build's.
auto not_empty(const std::string_view option, const std::string_view what) {
    return [option, what](std::string value) -> std::expected<std::string, std::string> {
        if (value.empty()) {
            return std::unexpected(std::format("'{}' needs {}", option, what));
        }

        return value;
    };
}

/// The request, or nullopt for the help. Errors are messages for people.
std::expected<std::optional<request_t>, std::string> parse(const std::vector<std::string>& args) {
    request_t request;
    std::optional<std::string> library;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            return std::nullopt;
        }

        // "-I dir" or "-Idir", "-D name" or "-Dname", and "--lib name" or "--lib=name"; -o goes
        // apart from its value, as fb's does.
        const auto is = [&](const std::string_view name) {
            return arg == name ||
                   (arg.starts_with(name) &&
                    ((name.size() == 2 && name != "-o") ||
                     (name.size() > 2 && arg.size() > name.size() && arg[name.size()] == '=')));
        };
        const auto value =
            [&](const std::string_view name) -> std::expected<std::string, std::string> {
            if (arg.size() > name.size()) {
                return arg.substr(name.size() + (arg[name.size()] == '=' ? 1 : 0));
            }

            if (i + 1 < args.size()) {
                return args[++i];
            }

            return std::unexpected(std::format("'{}' needs a value", name));
        };

        std::expected<std::string, std::string> taken;
        if (is("-o") || is("--output")) {
            // A name the module can have, not a folder's, as fb's -o takes.
            const std::string_view name = is("-o") ? "-o" : "--output";
            taken = value(name).and_then(
                [name](std::string path) -> std::expected<std::string, std::string> {
                    if (path.empty() || path.ends_with('/') || path.ends_with('\\')) {
                        return std::unexpected(std::format(
                            "'{}' takes the module's file name, like 'out/vecmath.fb'", name));
                    }

                    return path;
                });
            request.output = fb::path_from_utf8(taken.value_or(""));
            request.named_output = taken.has_value();
        } else if (is("--lib")) {
            taken = value("--lib");
            library = taken.value_or("");
        } else if (is("--target")) {
            taken = value("--target")
                        .and_then(not_empty("--target", "a target, like x86_64-pc-linux-gnu"));
            request.options.target = taken.value_or("");
        } else if (is("--std")) {
            taken = value("--std").and_then(not_empty("--std", "a C standard, like c17"));
            request.options.standard = taken.value_or("");
        } else if (is("-I")) {
            taken = value("-I");
            request.options.includes.push_back(taken.value_or(""));
        } else if (is("-D")) {
            taken = value("-D").and_then(not_empty("-D", "a macro's name"));
            request.options.defines.push_back(taken.value_or(""));
        } else if (arg.starts_with('-')) {
            return std::unexpected(std::format("unknown option '{}'", arg));
        } else if (!request.options.header.empty()) {
            return std::unexpected(
                std::format("unexpected argument '{}'; {} takes one header", arg, command_name));
        } else {
            request.options.header = fb::path_from_utf8(arg);
        }

        if (!taken) {
            return std::unexpected(taken.error());
        }
    }

    if (request.options.header.empty()) {
        return std::unexpected(
            std::format("{0} takes a C header, like '{0} vecmath.h'", command_name));
    }

    // The library and the module are named after the header unless they're given, and the
    // library's name is one a project.toml's [native] can have.
    request.options.library = library.value_or(fb::to_utf8(request.options.header.stem()));
    if (!request.options.library.empty() && !fb::is_library_name(request.options.library)) {
        return std::unexpected(std::format(
            "{} isn't a name a [native] library can have: letters, digits, and '_', '-', '.', or "
            "'+'; give one with --lib",
            library ? std::format("'{}'", request.options.library)
                    : std::format("the header's name, '{}',", request.options.library)));
    }
    if (!request.named_output) {
        request.output = request.options.header.stem();
        request.output += ".fb";
    }

    // Writing over the header would lose it, as fb build won't write over a program's files,
    // and a folder can't be written as a file.
    std::error_code error;
    if (std::filesystem::is_directory(request.output, error)) {
        const std::string path = fb::to_utf8(request.output);
        return std::unexpected(
            request.named_output
                ? std::format("'-o' names a folder, {}; give the module's file name", path)
                : std::format("the module would be {}, which is a folder; give its path with -o",
                              path));
    }

    if (std::filesystem::equivalent(request.output, request.options.header, error)) {
        const std::string path = fb::to_utf8(request.output);
        return std::unexpected(
            request.named_output
                ? std::format("'-o' names the header, {}; give another path", path)
                : std::format("the module would be {}, the header; give its path with -o", path));
    }

    return request;
}

int run(std::vector<std::string> args) {
    if (!args.empty() && args.front() == "--fb") {
        command_name = "fb bindgen";
        args.erase(args.begin());
    }

    const std::expected<std::optional<request_t>, std::string> parsed = parse(args);
    if (!parsed) {
        std::fputs(std::format("error: {}\n\nRun '{} --help' to see the options.\n", parsed.error(),
                               command_name)
                       .c_str(),
                   stderr);
        return usage_exit_code;
    }

    if (!*parsed) {
        std::fwrite(help.data(), 1, help.size(), stdout);
        return 0;
    }

    const request_t& request = **parsed;
    const std::expected<fb::bindings_t, std::string> bindings =
        fb::generate_bindings(request.options);
    if (!bindings) {
        // clang's errors come a line each.
        for (const auto line : std::views::split(bindings.error(), '\n')) {
            std::fputs(std::format("error: {}\n", std::string_view(line)).c_str(), stderr);
        }

        return failed_exit_code;
    }

    // Nor over a header the header reads, which would lose it too.
    for (const std::filesystem::path& read : bindings->files) {
        std::error_code different;
        if (!std::filesystem::equivalent(request.output, read, different)) {
            continue;
        }

        const std::string path = fb::to_utf8(request.output);
        const std::string header = fb::to_utf8(request.options.header);
        std::fputs(std::format(
                       "error: {}\n\nRun '{} --help' to see the options.\n",
                       request.named_output
                           ? std::format("'-o' names {}, a header {} reads; give another path",
                                         path, header)
                           : std::format(
                                 "the module would be {}, a header {} reads; give its path with -o",
                                 path, header),
                       command_name)
                       .c_str(),
                   stderr);
        return usage_exit_code;
    }

    for (const std::string& warning : bindings->warnings) {
        std::fputs(std::format("warning: {}\n", warning).c_str(), stderr);
    }

    // A folder -o names that isn't there yet is made, as fb build makes one.
    std::error_code ignored;
    if (request.output.has_parent_path()) {
        std::filesystem::create_directories(request.output.parent_path(), ignored);
    }

    std::ofstream file(request.output, std::ios::binary);
    file << bindings->text;
    file.close();
    if (!file) {
        std::fputs(std::format("error: couldn't write {}: {}\n", fb::to_utf8(request.output),
                               std::generic_category().message(errno))
                       .c_str(),
                   stderr);
        return failed_exit_code;
    }

    std::fputs(std::format("Wrote {}\n", fb::to_utf8(request.output)).c_str(), stderr);
    return 0;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        fb::prepare_terminal();
        return run(fb::command_line(argc, argv));
    } catch (const std::exception& error) {
        // Plain stdio, so reporting the error can't throw too.
        std::fputs("error: fb-bindgen failed unexpectedly: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return failed_exit_code;
    }
}
