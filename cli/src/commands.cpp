#include "fb/commands.hpp"

#include "fb/build_config.hpp"
#include "fb/commit.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/doc.hpp"
#include "fb/driver.hpp"
#include "fb/dump.hpp"
#include "fb/fmt.hpp"
#include "fb/interface.hpp"
#include "fb/lexer.hpp"
#include "fb/lower.hpp"
#include "fb/new.hpp"
#include "fb/process.hpp"
#include "fb/style.hpp"
#include "fb/terminal.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// What a program exits with when it panics. The runtime's fb_panic uses the same code.
constexpr int panic_exit_code = 101;

/// What `fb test` exits with when a test failed.
constexpr int tests_failed_exit_code = 1;

/// The most diagnostics a command shows; the count of the rest follows them.
constexpr std::size_t shown_diagnostics = 100;

/// Stage views go to stdout so they pipe cleanly. Everything else goes to stderr, like cargo.
class console_t {
public:
    /// Each stream gets its own style, since only one of them may be a terminal.
    explicit console_t(const color_t color)
        : m_out{ .color = use_color(color, stream_t::STANDARD_OUTPUT) },
          m_err{ .color = use_color(color, stream_t::STANDARD_ERROR) } {}

    [[nodiscard]] const style_t& out() const {
        return m_out;
    }

    [[nodiscard]] const style_t& err() const {
        return m_err;
    }

    /// How diagnostics go to stderr: in its colors, with FB_COLUMNS's columns.
    [[nodiscard]] render_options_t diagnostics() const {
        return { .color = m_err.color, .columns = m_columns };
    }

    static void print(const std::string_view text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }

    static void print_error(const std::string_view text) {
        // Views and errors stay in order when both streams go to the same place.
        std::fflush(stdout);
        std::fwrite(text.data(), 1, text.size(), stderr);
    }

    /// "   Compiling game.fb", with the verb right-aligned like cargo's.
    void status(const std::string_view verb, const std::string_view message) const {
        const std::string label = m_err.paint(std::format("{:>12}", verb), tone_t::SUCCESS);
        print_error(std::format("{} {}\n", label, message));
    }

    void error(const std::string_view message) const {
        print_error(std::format("{}: {}\n", m_err.paint("error", tone_t::ERROR),
                                m_err.paint(message, tone_t::BOLD)));
    }

    void note(const std::string_view message) const {
        print_error(std::format("{}: {}\n", m_err.paint("note", tone_t::NOTE), message));
    }

private:
    style_t m_out;
    style_t m_err;
    column_unit_t m_columns = diagnostic_columns();
};

/// "1 error" or "3 errors".
std::string count(const std::size_t number, const std::string_view noun) {
    return std::format("{} {}{}", number, noun, number == 1 ? "" : "s");
}

/// The time since `start`, like "0.31s".
std::string seconds_since(const std::chrono::steady_clock::time_point start) {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    return std::format("{:.2f}s", elapsed.count());
}

/// A command's line in the help. Empty for HELP and VERSION.
std::string_view summary(const command_t command) {
    switch (command) {
    case command_t::NEW:
        return "Make a package in a new folder";
    case command_t::CHECK:
        return "Check a program for errors, without building it";
    case command_t::BUILD:
        return "Compile a program into an executable";
    case command_t::RUN:
        return "Compile a program and run it";
    case command_t::TEST:
        return "Compile a package's tests and run them";
    case command_t::BINDGEN:
        return "Turn a C header into a FutureBlitz module, with fb-bindgen";
    case command_t::FMT:
        return "Format .fb files in place";
    case command_t::DOC:
        return "Write a package's public API as JSON, docs and all";
    default:
        return "";
    }
}

/// `fb bindgen`'s help, which lists fb-bindgen's options. Names pad to 21 columns, the width of
/// "    --target <triple>".
std::string bindgen_help(const style_t& style) {
    const auto entry = [&](const std::string_view name, const std::string_view description) {
        return std::format("  {}  {}\n", style.paint(std::format("{:<21}", name), tone_t::NAME),
                           description);
    };
    std::string out =
        std::format("{}\n\n{} fb bindgen <header.h> [options]\n", summary(command_t::BINDGEN),
                    style.paint("Usage:", tone_t::SUCCESS));
    out += std::format("\n{}\n", style.paint("Options:", tone_t::SUCCESS));
    out += entry("-o, --output <file>", "Where the module goes (<header>.fb in this folder)");
    out += entry("    --lib <name>",
                 "The [native] library its Extern block names (the header's; \"\" for none)");
    out += entry("    --target <triple>", "The target to read the header for (this build's)");
    out += entry("    --std <standard>",
                 "The C standard to read it with, like c17 (the C compiler's)");
    out += entry("-I <folder>", "A folder the header's #includes are found in");
    out += entry("-D <name>[=<value>]", "Defines a macro");
    out += entry("    --color <when>", "Color fb's own output: auto, always, or never");
    out += entry("-h, --help", "Show this help");
    return out;
}

/// The help for one command, or for fb as a whole when `command` is nullopt.
std::string help_text(const std::optional<command_t> command, const style_t& style) {
    const auto heading = [&](const std::string_view text) {
        return style.paint(text, tone_t::SUCCESS);
    };
    // Names pad to 19 columns, the width of "-o, --output <path>".
    const auto entry = [&](const std::string_view name, const std::string_view description) {
        return std::format("  {}  {}\n", style.paint(std::format("{:<19}", name), tone_t::NAME),
                           description);
    };

    if (command == command_t::BINDGEN) {
        return bindgen_help(style);
    }

    if (command == command_t::NEW) {
        std::string out = std::format("{}\n\n{} fb new [options] <folder>\n",
                                      summary(command_t::NEW), heading("Usage:"));
        out += "\nThe package is named by the folder's name, which is an identifier. It has a\n"
               "project.toml, a main.fb, and a .gitignore. The folder mustn't be there yet, or\n"
               "must be empty.\n";
        out += std::format("\n{}\n", heading("Options:"));
        out += entry("    --color <when>", "Color the output: auto, always, or never");
        out += entry("-h, --help", "Show this help");
        return out;
    }

    if (command == command_t::FMT) {
        std::string out = std::format("{}\n\n{} fb fmt [options] [files and folders]\n",
                                      summary(command_t::FMT), heading("Usage:"));
        out += "\nWithout any, it's the package the current folder is in, from it up. Lines are\n"
               "as wide as project.toml's [format] width, or 100.\n";
        out += std::format("\n{}\n", heading("Options:"));
        out += entry("    --check", "List the files it would change, without changing them");
        out += entry("    --color <when>", "Color the output: auto, always, or never");
        out += entry("-h, --help", "Show this help");
        return out;
    }

    std::string out = command ? std::format("{}\n\n", summary(*command))
                              : std::format("FutureBlitz {}: build and run .fb programs\n\n",
                                            build_config::version);
    const bool standard = command == command_t::TEST || command == command_t::DOC;
    if (command) {
        out += std::format("{} fb {} [options] [file.fb | package folder{}]{}\n", heading("Usage:"),
                           command_name(*command), standard ? " | std" : "",
                           command == command_t::RUN ? " [-- <arguments>]" : "");
        out +=
            "\nWithout a file or folder, it's the package the current folder is in, from it up.\n";
        if (command == command_t::TEST) {
            out += "With std, it runs the examples in the standard library's docs.\n";
        } else if (command == command_t::DOC) {
            out += "With std, it's the standard library's API.\n";
        }
    } else {
        out += std::format("{} fb [--color <when>] <command> [options] [<file or folder>]\n",
                           heading("Usage:"));
    }

    if (command == command_t::RUN) {
        out += "What follows -- goes to the program.\n";
    }

    if (!command) {
        out += std::format("\n{}\n", heading("Commands:"));
        for (const command_t each : {
                 command_t::NEW,
                 command_t::CHECK,
                 command_t::BUILD,
                 command_t::RUN,
                 command_t::TEST,
                 command_t::BINDGEN,
                 command_t::FMT,
                 command_t::DOC,
             }) {
            out += entry(command_name(each), summary(each));
        }

        out += entry("help <command>", "Show help for a command");
    }

    out += std::format("\n{}\n", heading("Options:"));
    if (!command || command == command_t::BUILD) {
        out += entry("-o, --output <path>",
                     "Where to write the executable (next to the source, or in build/)");
    } else if (command == command_t::DOC) {
        out += entry("-o, --output <path>",
                     "Where to write the JSON, or - for stdout (build/<name>.json, or std.json)");
    }

    if (command == command_t::DOC) {
        out += entry("    --release", "Document a release build's #If branches");
    } else {
        out +=
            entry("    --release", "Optimize, and let integer overflow wrap instead of panicking");
    }

    if (command == command_t::TEST) {
        out += entry("    --filter <name>", "Run only the tests whose names have <name> in them");
    } else if (command != command_t::DOC) {
        out += entry("    --show <stages>", "Print stages: tokens, ast, types, mir, llvm, or all");
    }

    out += entry("    --color <when>", "Color the output: auto, always, or never");
    out += entry("-h, --help", "Show this help");
    if (!command) {
        out += entry("    --version", "Show fb's version");
    }

    return out;
}

/// "    Finished debug build in 0.31s", then each bundled C library the program links, whose
/// license notice the program ships: that's its author's job, and each library beside it, and
/// each dynamic library copied beside it, which it needs.
void report_built(const console_t& console, const options_t& options,
                  const compilation_t& compilation, const program_build_t& built,
                  const std::chrono::steady_clock::time_point start) {
    const std::string_view mode = options.mode == build_mode_t::RELEASE ? "release" : "debug";
    console.status("Finished", std::format("{} build in {}", mode, seconds_since(start)));
    for (const build_config::bundled_library_t* library : built.bundled) {
        // A part of another library, as mbedTLS's TLS is, has its notices on that one's line.
        if (library->notice.empty()) {
            continue;
        }

        console.status("Bundled",
                       std::format("{} {}, under {}; ship its notices with the program: {}",
                                   library->name, library->version, library->license,
                                   to_utf8(toolchain().notices / path_from_utf8(library->notice))));
    }

    for (const std::string& file : built.beside) {
        console.status("Built", std::format("{} beside the executable, which needs it", file));
    }

    for (const dynamic_native_t& library : compilation.natives.dynamic) {
        console.status("Copied",
                       std::format("{} beside the executable, which needs it", library.beside));
    }
}

/// Whether `--show` asked for `stage`.
bool wants(const options_t& options, const stage_t stage) {
    return std::ranges::contains(options.show, stage);
}

/// A stage view, with a header when there's more than one.
void print_view(const console_t& console, const options_t& options, const stage_t stage,
                const std::string_view text) {
    if (options.show.size() > 1) {
        const std::string header = std::format("── {} ──", stage_name(stage));
        console_t::print(console.out().paint(header, tone_t::GUTTER) + "\n");
    }

    console_t::print(text);
}

/// The tokens of each file of a library's own package, after a line with its path, lexed as
/// the build lexed them.
std::string package_tokens(const compilation_t& compilation, const build_mode_t mode,
                           const style_t& style) {
    const module_table_t& modules = compilation.ast.modules();
    const std::optional<module_id_t> main = modules.main();
    const std::vector<std::string_view> defined = defined_symbols(mode);
    std::string out;
    for (const module_t& module : modules.modules()) {
        if (!main || module.is_main || module.package != modules.get(*main).package) {
            continue;
        }

        const source_file_t& file = compilation.sources.get(module.file);
        diagnostic_bag_t reported; // the build reported them
        out += style.paint(file.path(), tone_t::BOLD) + "\n";
        out += dump_tokens(file, lex(file, reported, defined), style);
    }

    return out;
}

/// Reports a build that failed: at the Extern Functions whose C symbols no library defines,
/// and the project.toml lines of system libraries the linker can't find, then the rest as the
/// linker said.
void report_build_error(const console_t& console, const compilation_t& compilation,
                        const std::string_view name, const std::string& failure) {
    const link_errors_t found = link_errors(compilation, failure);
    for (const diagnostic_t& diagnostic : found.errors) {
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, console.diagnostics()));
    }

    if (!found.complete) {
        console.error(failure);
    }

    if (!found.errors.empty()) {
        console.error(std::format("couldn't link {} because of {}", name,
                                  count(found.errors.size(), "error")));
    }
}

/// `fb build`: writes the executable to the `-o` path, or for a package, to its build/ folder,
/// named after it, or else next to the source.
int build(const console_t& console, const options_t& options, const compilation_t& compilation,
          const std::string_view name, const mir_module_t& module,
          const std::chrono::steady_clock::time_point start) {
    std::filesystem::path output;
    if (options.output) {
        output = *options.output;
    } else if (compilation.package) {
        output = compilation.package->root / "build" / path_from_utf8(compilation.package->program);
        output += executable_extension;
    } else {
        output = std::filesystem::path(options.input).replace_extension(executable_extension);
    }

    // Without -o, the path can still be a folder's, as game/ beside game.fb is on Linux and
    // macOS, where its executable is game.
    std::error_code error;
    if (std::filesystem::is_directory(output, error)) {
        console.error(options.output
                          ? std::format("'-o' names a folder, {}; give the executable's file name",
                                        to_utf8(output))
                          : std::format("the executable would be {}, which is a folder; give its "
                                        "path with -o",
                                        to_utf8(output)));
        return usage_exit_code;
    }

    // Writing over one of the program's own files, a source or one it embeds, would lose it.
    std::vector<std::string> files = compilation.embedded;
    for (std::size_t i = 0; i < compilation.sources.size(); ++i) {
        files.push_back(compilation.sources.get(static_cast<file_id_t>(i)).path());
    }

    for (const std::string& path : files) {
        if (std::filesystem::equivalent(output, path_from_utf8(path), error)) {
            console.error(options.output
                              ? std::format("'-o' names {}, a file of the program; give another "
                                            "path",
                                            path)
                              : std::format("the executable would be {}, a file of the program; "
                                            "give its path with -o",
                                            path));
            return usage_exit_code;
        }
    }

    program_build_t report;
    if (auto built = build_program(module, compilation, output, options.mode, &report); !built) {
        report_build_error(console, compilation, name, built.error());
        return compile_errors_exit_code;
    }

    report_built(console, options, compilation, report, start);
    return 0;
}

/// `fb build` on a library: writes its interface file and its binary,
/// build/<target>/<mode>/<name>.fbi and .fblib in its folder, which programs compile against and
/// link in place of its source.
int build_library_files(const console_t& console, const options_t& options,
                        const compilation_t& compilation, const std::string_view name,
                        const std::chrono::steady_clock::time_point start) {
    const std::optional<package_info_t>& package = compilation.package;
    if (!package) {
        return usage_exit_code;
    }

    if (options.output) {
        console.error(std::format("{} is a library, whose build goes in its build/ folder; '-o' "
                                  "names an executable",
                                  package->name));
        return usage_exit_code;
    }

    // A prebuilt library's build is what it has; there's no source to build it from.
    if (!package->interface.empty()) {
        console.error(std::format("{} is prebuilt, {}; there's no source to build it from",
                                  package->name, to_utf8(package->interface)));
        return usage_exit_code;
    }

    diagnostic_bag_t diagnostics;
    const std::expected<library_build_t, std::string> built =
        fb::build_library(compilation, options.mode, diagnostics);
    for (const diagnostic_t& diagnostic : diagnostics.diagnostics()) {
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, console.diagnostics()));
    }

    if (!built && built.error().empty()) {
        console.error(std::format("couldn't build {} because of {}", name,
                                  count(diagnostics.error_count(), "error")));
        return compile_errors_exit_code;
    }

    if (!built) {
        console.error(built.error());
        return compile_errors_exit_code;
    }

    if (interface_modules(built->interface).value_or(std::vector<library_file_t>{}).empty()) {
        console.error(
            std::format("{} has no .fb files, so it has no modules to build", package->name));
        return usage_exit_code;
    }

    const std::filesystem::path folder = package->root / "build" / path_from_utf8(build_target()) /
                                         path_from_utf8(mode_folder(options.mode));
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    for (const auto& [extension, bytes] :
         { std::pair(".fbi", &built->interface), std::pair(".fblib", &built->binary) }) {
        const std::filesystem::path path = folder / path_from_utf8(package->name + extension);
        std::ofstream file(path, std::ios::binary);
        file << *bytes;
        file.close();
        if (error || !file) {
            console.error(std::format("can't write {}", to_utf8(path)));
            return compile_errors_exit_code;
        }
    }

    console.status("Finished",
                   std::format("{} build in {}", mode_folder(options.mode), seconds_since(start)));
    return 0;
}

/// "exited with code 3", for a program that didn't exit with 0. A program that crashes on Windows
/// gives an NTSTATUS, like 0xC0000409, which shows in hex.
std::string exited_with(const int code) {
    if (code == panic_exit_code) {
        return std::format("exited with code {}, which a panic gives", panic_exit_code);
    }

    if (code < 0) {
        return std::format("exited with code 0x{:08X}", static_cast<std::uint32_t>(code));
    }

    return std::format("exited with code {}", code);
}

/// `fb run`: builds the program in a temporary folder, runs it in this terminal, and deletes the
/// folder. Returns the program's exit code, or 1 when it can't be built or started.
int run(const console_t& console, const options_t& options, const compilation_t& compilation,
        const std::string_view shown, const mir_module_t& module,
        const std::chrono::steady_clock::time_point start) {
    // A folder of its own, so the program keeps its name, or its package's.
    const std::filesystem::path stem =
        compilation.package ? path_from_utf8(compilation.package->program) : options.input.stem();
    const std::filesystem::path folder = temporary_path(stem, "");
    std::filesystem::path program = folder / stem;
    program += executable_extension;
    const std::string name = to_utf8(program.filename());

    program_build_t report;
    const std::expected<void, std::string> built =
        build_program(module, compilation, program, options.mode, &report);
    if (!built) {
        remove_temporary(folder);
        report_build_error(console, compilation, shown, built.error());
        return compile_errors_exit_code;
    }

    report_built(console, options, compilation, report, start);
    console.status("Running", name);
    const std::expected<int, std::string> exit_code =
        run_attached(program, options.program_arguments);

    // Cleaning up is best effort. A folder left in temp isn't worth an error.
    remove_temporary(folder);
    if (!exit_code) {
        console.error(exit_code.error());
        return compile_errors_exit_code;
    }

    if (*exit_code != 0) {
        console.error(std::format("{} {}", name, exited_with(*exit_code)));
    }

    return *exit_code;
}

/// The .fb file or package folder a command was given, or without either, the package the current
/// folder is in, as cargo finds one. Nullopt, after saying so, when there's none.
std::optional<std::filesystem::path> input_of(const console_t& console, const options_t& options) {
    if (!options.input.empty()) {
        return options.input;
    }

    std::error_code error;
    std::optional<std::filesystem::path> found = find_package(std::filesystem::current_path(error));
    if (!found) {
        console.error("there's no project.toml in this folder or above it; give a package's "
                      "folder, or a .fb file");
    }

    return found;
}

/// How messages name what's compiled: a package as cargo names a crate, "game v0.1.0
/// (C:/games/game)", and else the file.
std::string shown_name(const compilation_t& compilation, const std::filesystem::path& input) {
    const std::optional<package_info_t>& package = compilation.package;
    return package
               ? std::format("{} v{} ({})", package->name, package->version, to_utf8(package->root))
               : to_utf8(input);
}

/// Prints a compilation's diagnostics, and gives how many are errors. A file with thousands of
/// errors, like one that isn't source code, shows the first ones.
std::size_t report_diagnostics(const console_t& console, const compilation_t& compilation) {
    const std::span<const diagnostic_t> diagnostics = compilation.diagnostics.diagnostics();
    for (const diagnostic_t& diagnostic : diagnostics | std::views::take(shown_diagnostics)) {
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, console.diagnostics()));
    }

    if (const std::size_t rest =
            diagnostics.size() - std::min(diagnostics.size(), shown_diagnostics);
        rest > 0) {
        console.note(rest == 1 ? std::string("1 more diagnostic isn't shown")
                               : std::format("{} more diagnostics aren't shown", rest));
    }

    return compilation.diagnostics.error_count();
}

/// check, build, and run all compile and lower the program, printing the views asked for. Then
/// build and run go on to write an executable.
int compile_command(const options_t& options) {
    const auto start = std::chrono::steady_clock::now();
    const console_t console(options.color);
    const bool checking = options.command == command_t::CHECK;

    const std::optional<std::filesystem::path> input = input_of(console, options);
    if (!input) {
        return usage_exit_code;
    }

    // This only fails when the file can't be read, or a folder isn't a package's, which counts as
    // bad usage.
    const std::expected<compilation_t, std::string> compiled = compile_file(*input, options.mode);
    if (!compiled) {
        console.error(compiled.error());
        return usage_exit_code;
    }

    const std::optional<package_info_t>& package = compiled->package;
    const std::string name = shown_name(*compiled, *input);
    console.status(checking ? "Checking" : "Compiling", name);
    if (package && package->library && options.command == command_t::RUN) {
        console.error(std::format("{} is a library, with no main file to {}", package->name,
                                  command_name(options.command)));
        return usage_exit_code;
    }

    if (package && package->library &&
        (wants(options, stage_t::MIR) || wants(options, stage_t::LLVM))) {
        console.error(std::format("{} is a library, with no program of its own to show as MIR or "
                                  "LLVM IR; its tokens, AST, and types show",
                                  package->name));
        return usage_exit_code;
    }

    // The front-end views print even when there are errors, so they can help find them. A
    // library's main file is a placeholder, so its views are of the package's own files.
    const compilation_t& compilation = *compiled;
    const style_t& style = console.out();
    const bool library = package && package->library;
    if (wants(options, stage_t::TOKENS)) {
        print_view(console, options, stage_t::TOKENS,
                   library ? package_tokens(compilation, options.mode, style)
                           : dump_tokens(compilation.source(), compilation.tokens, style));
    }

    if (wants(options, stage_t::AST)) {
        print_view(console, options, stage_t::AST, dump_tree(compilation.ast, style, library));
    }

    if (wants(options, stage_t::TYPES)) {
        print_view(console, options, stage_t::TYPES,
                   dump_types(compilation.ast, compilation.analysis, style, library));
    }

    if (const std::size_t errors = report_diagnostics(console, compilation); errors > 0) {
        console.error(checking ? std::format("{} has {}", name, count(errors, "error"))
                               : std::format("couldn't build {} because of {}", name,
                                             count(errors, "error")));
        return compile_errors_exit_code;
    }

    // What a library's interface file can't keep, like an Embed in generic code, its build
    // stops at, so checking it finds that too.
    if (library && checking && package->interface.empty()) {
        diagnostic_bag_t kept;
        if (!write_interface(compilation, options.mode, kept, "")) {
            for (const diagnostic_t& diagnostic : kept.diagnostics()) {
                console_t::print_error(
                    render_pretty(diagnostic, compilation.sources, console.diagnostics()));
            }

            console.error(std::format("{} has {}", name, count(kept.error_count(), "error")));
            return compile_errors_exit_code;
        }
    }

    // Lowering needs a program without errors, so the MIR and LLVM IR views only print from here.
    const mir_module_t module = lower(compilation.ast, compilation.analysis);
    if (const std::optional<mir_error_t>& error = module.error) {
        const diagnostic_t diagnostic{ .message = error->message, .span = error->span };
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, console.diagnostics()));
        console.error(checking ? std::format("{} has 1 error", name)
                               : std::format("couldn't build {} because of 1 error", name));
        return compile_errors_exit_code;
    }

    if (wants(options, stage_t::MIR)) {
        print_view(console, options, stage_t::MIR, dump(module, style));
    }

    if (wants(options, stage_t::LLVM)) {
        // A prebuilt library's binary is linked into the module the view shows; one beside the
        // executable keeps its code in its file, so the view declares it.
        codegen_options_t codegen{ .mode = options.mode };
        const std::expected<std::vector<library_binary_t>, std::string> binaries =
            prebuilt_binaries(compilation);
        if (!binaries) {
            console.error(binaries.error());
            return compile_errors_exit_code;
        }

        for (std::size_t i = 0; i < binaries->size(); ++i) {
            codegen.linked.emplace_back(to_utf8(compilation.interfaces[i]), binaries->at(i).code);
        }

        const auto ir = emit_ir(module, compilation.sources, compilation.file, codegen);
        if (!ir) {
            console.error(ir.error());
            return compile_errors_exit_code;
        }

        print_view(console, options, stage_t::LLVM, highlight_llvm(*ir, style));
    }

    switch (options.command) {
    case command_t::BUILD:
        return library ? build_library_files(console, options, compilation, name, start)
                       : build(console, options, compilation, name, module, start);
    case command_t::RUN:
        return run(console, options, compilation, name, module, start);
    default:
        console.status("Finished", std::format("check in {}", seconds_since(start)));
        return 0;
    }
}

/// One record of the report a test executable writes, as the runtime's runner does: its kind,
/// like "start", "fail", or "end", the test's index, if it's about one, and its text.
struct report_record_t {
    std::string kind;
    std::optional<std::size_t> test;
    std::string text;
};

/// A report's records: each is "<kind> <index> <length>" on a line, -1 for no test, then that
/// many bytes of text, and a line break. One the executable didn't finish writing is left out.
std::vector<report_record_t> read_report(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(file),
                            std::istreambuf_iterator<char>() };
    std::vector<report_record_t> records;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            break;
        }

        const std::string_view line = std::string_view(text).substr(at, end - at);
        const std::size_t first = line.find(' ');
        const std::size_t second = line.rfind(' ');
        long long index = 0;
        std::size_t length = 0;
        if (first == std::string_view::npos || second == first ||
            std::from_chars(line.data() + first + 1, line.data() + second, index).ec !=
                std::errc() ||
            std::from_chars(line.data() + second + 1, line.data() + line.size(), length).ec !=
                std::errc() ||
            end + 1 + length >= text.size()) {
            break;
        }

        records.push_back({
            .kind = std::string(line.substr(0, first)),
            .test = index < 0 ? std::nullopt : std::optional(static_cast<std::size_t>(index)),
            .text = text.substr(end + 1, length),
        });
        at = end + length + 2;
    }

    return records;
}

/// What one test did, once it's done.
struct test_result_t {
    bool done = false;
    bool failed = false;
    bool skipped = false;
    std::string reason;
    /// Its failures, a panic's, and its leaks, in order.
    std::vector<std::string> messages;
};

/// Sets an environment variable of fb's, which the programs it runs get.
void set_environment(const char* name, const std::filesystem::path& value) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::strlen(name));
    _wputenv_s(wide.c_str(), value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

/// "test game.shapes.Area ... ok" for each of `ended`, by their indexes.
void print_results(const console_t& console, const std::vector<test_case_t>& tests,
                   const std::vector<test_result_t>& results,
                   const std::vector<std::size_t>& ended) {
    const style_t& style = console.out();
    for (const std::size_t index : ended) {
        const test_result_t& result = results[index];
        std::string shown = style.paint("ok", tone_t::SUCCESS);
        if (result.failed) {
            shown = style.paint("FAILED", tone_t::ERROR);
        } else if (result.skipped) {
            shown = style.paint("skipped", tone_t::NOTE) +
                    (result.reason.empty() ? "" : std::format(": {}", result.reason));
        }

        console_t::print(std::format("test {} ... {}\n", tests[index].name, shown));
    }
}

/// What the runs of a test executable did besides its tests: what failed outside them, like a
/// panic in a Global's Destroy, or a leak no test made, and the exit code of a run that Ctrl+C
/// stopped, which stops fb test too.
struct runs_t {
    std::vector<std::string> failures;
    std::optional<int> interrupted;
};

/// Whether a program's exit code says Ctrl+C ended it: Windows' STATUS_CONTROL_C_EXIT, or
/// SIGINT's 128 + 2.
bool interrupted(const int code) {
#ifdef _WIN32
    return static_cast<std::uint32_t>(code) == 0xC000013AU;
#else
    return code == 128 + SIGINT;
#endif
}

/// Runs a test executable, built for `mode`, until each of `tests`, by their indexes, is done,
/// printing each one's result once a run of it ends. A test that panics ends the run, and fails;
/// the next run has the rest, and in a debug build, the ones that finished in it too, whose leaks
/// it didn't check. When a run ends with more than one test going and none to blame, the next
/// runs them one at a time, on the main thread, so the next one that ends a run is the one.
runs_t run_tests(const console_t& console, const std::filesystem::path& program,
                 const build_mode_t mode, const std::vector<test_case_t>& tests,
                 std::vector<test_result_t>& results) {
    runs_t runs;
    const auto fail_run = [&](const std::string& why) {
        if (!std::ranges::contains(runs.failures, why)) {
            runs.failures.push_back(why);
        }
    };
    const std::filesystem::path folder = program.parent_path();
    const std::filesystem::path plan = folder / "plan.txt";
    const std::filesystem::path report = folder / "report.txt";
    set_environment("FB_TEST_PLAN", plan);
    set_environment("FB_TEST_REPORT", report);
    std::vector<std::size_t> remaining(tests.size());
    // NOLINTNEXTLINE(modernize-use-ranges): Apple's libc++ has no std::ranges::iota.
    std::iota(remaining.begin(), remaining.end(), std::size_t{ 0 });
    bool serial = false;
    while (!remaining.empty()) {
        std::string planned = serial ? "serial\n" : "parallel\n";
        for (const std::size_t index : remaining) {
            planned += std::format("{}\n", index);
        }

        std::ofstream(plan, std::ios::binary) << planned;
        std::error_code ignored;
        std::filesystem::remove(report, ignored);
        const std::expected<int, std::string> ran = run_attached(program, {});
        if (!ran) {
            console.error(ran.error());
            for (const std::size_t index : remaining) {
                results[index] = { .done = true, .failed = true, .messages = { ran.error() } };
            }

            return runs;
        }

        // What each test of this run did: a run that ends early keeps only what its finished
        // tests did, and the one that panicked, since the rest run again.
        std::map<std::size_t, test_result_t> found;
        std::vector<std::size_t> started;
        std::optional<std::size_t> panicked;
        std::string panic;
        bool finished = false;
        for (const report_record_t& record : read_report(report)) {
            if (record.kind == "done") {
                finished = true;
                continue;
            }

            if (record.kind == "panic" && !record.test) {
                panic = record.text;
                continue;
            }

            // A leak no test made is the run's.
            if (record.kind == "leak" && !record.test) {
                fail_run(record.text);
                continue;
            }

            if (!record.test || *record.test >= tests.size()) {
                continue;
            }

            test_result_t& result = found[*record.test];
            if (record.kind == "start") {
                started.push_back(*record.test);
            } else if (record.kind == "end") {
                result.done = true;
            } else if (record.kind == "skip") {
                result.skipped = true;
                result.reason = record.text;
            } else if (record.kind == "fail" || record.kind == "leak" || record.kind == "panic") {
                result.failed = true;
                result.messages.push_back(record.text);
                if (record.kind == "panic") {
                    result.done = true;
                    panicked = record.test;
                }
            }
        }

        // Ctrl+C stops the tests, as it does a program fb runs.
        if (!finished && interrupted(*ran)) {
            runs.interrupted = *ran;
            return runs;
        }

        std::vector<std::size_t> going;
        std::ranges::copy_if(started, std::back_inserter(going),
                             [&](const std::size_t index) { return !found[index].done; });
        // A run that ends with one test going ended in it, as a crash or an Exit can.
        if (!finished && !panicked && going.size() == 1) {
            test_result_t& result = found[going.front()];
            result.done = true;
            result.failed = true;
            result.messages.push_back(
                panic.empty()
                    ? std::format("the tests' program {}, in this test", exited_with(*ran))
                    : panic);
            panicked = going.front();
        }

        // A run that ends early never checks its tests' leaks, which a debug build reports once the
        // Globals are let go of, so the tests that finished in it run again, but for the one to
        // blame. When there's none, and no test was going, the run ended outside them, and what
        // finished is kept, so the next run gets further.
        const bool keeps =
            finished || mode == build_mode_t::RELEASE || (!panicked && going.empty());
        std::vector<std::size_t> next;
        std::vector<std::size_t> ended;
        for (const std::size_t index : remaining) {
            if (const auto result = found.find(index);
                result != found.end() && result->second.done && (keeps || index == panicked)) {
                results[index] = std::move(result->second);
                ended.push_back(index);
            } else {
                next.push_back(index);
            }
        }

        print_results(console, tests, results, ended);
        // A run that ends with no test going ended outside them, as a panic in a Global's Destroy
        // does, so the run failed. One that ends before any test does can't be helped by another.
        if (!finished && !panicked && going.empty()) {
            const std::string what =
                panic.empty() ? std::format("the tests' program {}", exited_with(*ran)) : panic;
            if (ended.empty()) {
                for (const std::size_t index : next) {
                    results[index] = {
                        .done = true,
                        .failed = true,
                        .messages = { what + "\nbefore any test ran" },
                    };
                }

                print_results(console, tests, results, next);
                return runs;
            }

            fail_run(mode == build_mode_t::DEBUG
                         ? what + "\nso the leaks of the tests that had finished weren't checked"
                         : what);
        }

        serial = serial || (!finished && !panicked && going.size() > 1);
        remaining = std::move(next);
    }

    return runs;
}

/// `fb test`: builds the package's tests, those `--filter` names, into a test executable in a
/// temporary folder, runs them, and says how each did, then how they all did, and deletes the
/// folder. Returns 0 when none of them failed.
int test_command(const options_t& options) {
    const auto start = std::chrono::steady_clock::now();
    const console_t console(options.color);
    const std::optional<std::filesystem::path> input = input_of(console, options);
    if (!input) {
        return usage_exit_code;
    }

    // "std" is the standard library, whose tests are its docs' examples.
    const bool standard = options.input == "std";
    const std::expected<compilation_t, std::string> compiled =
        standard ? compile_standard_tests(options.mode) : compile_tests(*input, options.mode);
    if (!compiled) {
        console.error(compiled.error());
        return usage_exit_code;
    }

    const compilation_t& compilation = *compiled;
    const std::string name = standard ? "std" : shown_name(compilation, *input);
    console.status("Compiling", name);
    if (const std::size_t errors = report_diagnostics(console, compilation); errors > 0) {
        console.error(
            std::format("couldn't build {}'s tests because of {}", name, count(errors, "error")));
        return compile_errors_exit_code;
    }

    const std::vector<test_case_t> found = package_tests(compilation);
    std::vector<test_case_t> tests;
    std::ranges::copy_if(found, std::back_inserter(tests), [&](const test_case_t& test) {
        return !options.filter || test.name.contains(*options.filter);
    });
    const mir_module_t module = lower_tests(compilation.ast, compilation.analysis, tests);
    if (const std::optional<mir_error_t>& error = module.error) {
        const diagnostic_t diagnostic{ .message = error->message, .span = error->span };
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, console.diagnostics()));
        console.error(std::format("couldn't build {}'s tests because of 1 error", name));
        return compile_errors_exit_code;
    }

    // A folder of its own, so the executable is named after the package's tests.
    std::filesystem::path stem =
        compilation.package ? path_from_utf8(compilation.package->name) : input->stem();
    stem += "-tests";
    const std::filesystem::path folder = temporary_path(stem, "");
    std::filesystem::path program = folder / stem;
    program += executable_extension;
    program_build_t report;
    if (const std::expected<void, std::string> built =
            build_program(module, compilation, program, options.mode, &report);
        !built) {
        remove_temporary(folder);
        report_build_error(console, compilation, name, built.error());
        return compile_errors_exit_code;
    }

    report_built(console, options, compilation, report, start);
    console.status("Running", count(tests.size(), "test"));
    const auto running = std::chrono::steady_clock::now();
    std::vector<test_result_t> results(tests.size());
    const runs_t runs = run_tests(console, program, options.mode, tests, results);
    remove_temporary(folder);
    if (runs.interrupted) {
        console.error(
            std::format("the tests were interrupted, and {}", exited_with(*runs.interrupted)));
        return *runs.interrupted;
    }

    // Then each failure, with what it said, the run's own first, and how they all did.
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::string failures;
    if (!runs.failures.empty()) {
        failures += "\n---- the tests' program ----\n";
        for (const std::string& message : runs.failures) {
            failures += message + "\n";
        }
    }

    for (std::size_t i = 0; i < tests.size(); ++i) {
        const test_result_t& result = results[i];
        if (result.failed) {
            ++failed;
            failures += std::format("\n---- {} ----\n", tests[i].name);
            for (const std::string& message : result.messages) {
                failures += message + "\n";
            }
        } else if (result.skipped) {
            ++skipped;
        } else {
            ++passed;
        }
    }

    const style_t& style = console.out();
    const bool ok = failed == 0 && runs.failures.empty();
    if (!ok) {
        console_t::print(std::format("\nfailures:\n{}", failures));
    }

    console_t::print(std::format(
        "\ntest result: {}. {} passed; {} failed; {} skipped; {} filtered out; finished in {}\n",
        ok ? style.paint("ok", tone_t::SUCCESS) : style.paint("FAILED", tone_t::ERROR), passed,
        failed, skipped, found.size() - tests.size(), seconds_since(running)));
    return ok ? 0 : tests_failed_exit_code;
}

/// `fb doc`: writes the package's public API as JSON, to the `-o` path, or to its build/ folder,
/// named after it, or for std, to std.json in the current folder, after the warnings about its
/// doc comments. Returns 0 when it compiles.
int doc_command(const options_t& options) {
    const auto start = std::chrono::steady_clock::now();
    const console_t console(options.color);
    const std::optional<std::filesystem::path> input = input_of(console, options);
    if (!input) {
        return usage_exit_code;
    }

    const bool standard = options.input == "std";
    const std::expected<compilation_t, std::string> compiled =
        standard ? compile_standard_docs(options.mode) : compile_docs(*input, options.mode);
    if (!compiled) {
        console.error(compiled.error());
        return usage_exit_code;
    }

    const compilation_t& compilation = *compiled;
    const std::string name = standard ? "std" : shown_name(compilation, *input);
    console.status("Documenting", name);
    if (const std::size_t errors = report_diagnostics(console, compilation); errors > 0) {
        console.error(
            std::format("couldn't document {} because of {}", name, count(errors, "error")));
        return compile_errors_exit_code;
    }

    diagnostic_bag_t warnings;
    const doc_package_t package = document(compilation, standard, options.mode, warnings);
    for (const diagnostic_t& warning : warnings.diagnostics()) {
        console_t::print_error(
            render_pretty(warning, compilation.sources, { .color = console.err().color }));
    }

    const std::string json = doc_json(package, compilation.sources);
    const std::optional<package_info_t>& documented = compilation.package;
    std::filesystem::path written = "std.json";
    if (options.output) {
        written = *options.output;
    } else if (!standard && documented) {
        written = documented->root / "build" / path_from_utf8(documented->name + ".json");
    }

    const std::filesystem::path output = written;
    std::size_t items = 0;
    for (const doc_module_t& module : package.modules) {
        items += module.items.size();
    }

    const std::string what = std::format("{} and {}", count(package.modules.size(), "module"),
                                         count(items, "declaration"));
    if (output == "-") {
        console_t::print(json);
        console.status("Finished", std::format("{} in {}", what, seconds_since(start)));
        return 0;
    }

    std::error_code error;
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path(), error);
    }

    std::ofstream file(output, std::ios::binary);
    file << json;
    file.close();
    if (!file) {
        console.error(std::format("couldn't write {}", to_utf8(output)));
        return compile_errors_exit_code;
    }

    console.status("Finished",
                   std::format("{} in {}, in {}", what, seconds_since(start), to_utf8(output)));
    return 0;
}

/// Runs fb-bindgen, next to fb, with what `fb bindgen` was given, and gives its exit code.
int bindgen_command(const options_t& options) {
    const console_t console(options.color);
    std::filesystem::path tool = current_executable().parent_path() / "fb-bindgen";
    tool += executable_extension;
    std::error_code error;
    if (!std::filesystem::is_regular_file(tool, error)) {
        console.error(std::format("fb bindgen runs fb-bindgen, which isn't next to fb, at {}; it's "
                                  "built where CMake finds libclang",
                                  to_utf8(tool)));
        return compile_errors_exit_code;
    }

    // "--fb" first tells fb-bindgen it's run as `fb bindgen`, which its messages then say.
    std::vector<std::string> arguments{ "--fb" };
    arguments.insert(arguments.end(), options.passed.begin(), options.passed.end());
    const std::expected<int, std::string> ran = run_attached(tool, arguments);
    if (!ran) {
        console.error(ran.error());
        return compile_errors_exit_code;
    }

    return *ran;
}

} // namespace

int run_command(const options_t& options) {
    switch (options.command) {
    case command_t::HELP:
        console_t::print(help_text(options.help_for, console_t(options.color).out()));
        return 0;
    case command_t::VERSION:
        console_t::print(std::format("fb {} ({}, {})\n", build_config::version, build_commit,
                                     build_config::toolchain_target));
        return 0;
    case command_t::NEW:
        return new_command(options);
    case command_t::CHECK:
    case command_t::BUILD:
    case command_t::RUN:
        return compile_command(options);
    case command_t::TEST:
        return test_command(options);
    case command_t::BINDGEN:
        return bindgen_command(options);
    case command_t::FMT:
        return format_command(options);
    case command_t::DOC:
        return doc_command(options);
    }

    std::unreachable();
}

void report_usage_error(const usage_error_t& error) {
    // A --color before the mistake still says how it's shown.
    console_t(error.color).error(error.message);
    console_t::print_error("\nRun 'fb help' to see the commands and options.\n");
}

} // namespace fb
