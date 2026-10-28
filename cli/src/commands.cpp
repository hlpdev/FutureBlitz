#include "fb/commands.hpp"

#include "fb/build_config.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/driver.hpp"
#include "fb/dump.hpp"
#include "fb/interface.hpp"
#include "fb/lexer.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"
#include "fb/style.hpp"
#include "fb/terminal.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
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
    case command_t::CHECK:
        return "Check a program for errors, without building it";
    case command_t::BUILD:
        return "Compile a program into an executable";
    case command_t::RUN:
        return "Compile a program and run it";
    case command_t::BINDGEN:
        return "Turn a C header into a FutureBlitz module, with fb-bindgen";
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

    std::string out = command ? std::format("{}\n\n", summary(*command))
                              : std::format("FutureBlitz {}: build and run .fb programs\n\n",
                                            build_config::version);
    if (command) {
        out += std::format("{} fb {} [options] [file.fb | package folder]{}\n", heading("Usage:"),
                           command_name(*command),
                           command == command_t::RUN ? " [-- <arguments>]" : "");
        out +=
            "\nWithout a file or folder, it's the package the current folder is in, from it up.\n";
    } else {
        out += std::format("{} fb [--color <when>] <command> [options] [<file or folder>]\n",
                           heading("Usage:"));
    }

    if (command == command_t::RUN) {
        out += "What follows -- goes to the program.\n";
    }

    if (!command) {
        out += std::format("\n{}\n", heading("Commands:"));
        for (const command_t each :
             { command_t::CHECK, command_t::BUILD, command_t::RUN, command_t::BINDGEN }) {
            out += entry(command_name(each), summary(each));
        }

        out += entry("help <command>", "Show help for a command");
    }

    out += std::format("\n{}\n", heading("Options:"));
    if (!command || command == command_t::BUILD) {
        out += entry("-o, --output <path>",
                     "Where to write the executable (next to the source, or in build/)");
    }

    out += entry("    --release", "Optimize, and let integer overflow wrap instead of panicking");
    out += entry("    --show <stages>", "Print stages: tokens, ast, types, mir, llvm, or all");
    out += entry("    --color <when>", "Color the output: auto, always, or never");
    out += entry("-h, --help", "Show this help");
    if (!command) {
        out += entry("    --version", "Show fb's version");
    }

    return out;
}

/// "    Finished debug build in 0.31s", then each bundled C library the program links, whose
/// license notice the program ships: that's its author's job, and each dynamic library copied
/// beside it, which it needs.
void report_built(const console_t& console, const options_t& options,
                  const compilation_t& compilation, const mir_module_t& module,
                  const std::chrono::steady_clock::time_point start) {
    const std::string_view mode = options.mode == build_mode_t::RELEASE ? "release" : "debug";
    console.status("Finished", std::format("{} build in {}", mode, seconds_since(start)));
    for (const build_config::bundled_library_t* library : bundled_libraries(module)) {
        // A part of another library, as mbedTLS's TLS is, has its notices on that one's line.
        if (library->notice.empty()) {
            continue;
        }

        console.status("Bundled",
                       std::format("{} {}, under {}; ship its notices with the program: {}",
                                   library->name, library->version, library->license,
                                   library->notice));
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
            render_pretty(diagnostic, compilation.sources, { .color = console.err().color }));
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

    if (auto built = build_program(module, compilation, output, options.mode); !built) {
        report_build_error(console, compilation, name, built.error());
        return compile_errors_exit_code;
    }

    report_built(console, options, compilation, module, start);
    return 0;
}

/// `fb build` on a library: writes its interface file, build/<target>/<mode>/<name>.fbi in its
/// folder, which programs can compile against in place of its source.
int build_library(const console_t& console, const options_t& options,
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
    const std::optional<std::string> interface =
        write_interface(compilation, options.mode, diagnostics);
    for (const diagnostic_t& diagnostic : diagnostics.diagnostics()) {
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, { .color = console.err().color }));
    }

    if (!interface) {
        console.error(std::format("couldn't build {} because of {}", name,
                                  count(diagnostics.error_count(), "error")));
        return compile_errors_exit_code;
    }

    if (interface_modules(*interface).value_or(std::vector<library_file_t>{}).empty()) {
        console.error(
            std::format("{} has no .fb files, so it has no modules to build", package->name));
        return usage_exit_code;
    }

    const std::filesystem::path folder = package->root / "build" / path_from_utf8(build_target()) /
                                         path_from_utf8(mode_folder(options.mode));
    const std::filesystem::path path = folder / path_from_utf8(package->name + ".fbi");
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::ofstream file(path, std::ios::binary);
    file << *interface;
    file.close();
    if (error || !file) {
        console.error(std::format("can't write {}", to_utf8(path)));
        return compile_errors_exit_code;
    }

    console.status("Finished",
                   std::format("{} build in {}", mode_folder(options.mode), seconds_since(start)));
    return 0;
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

    const std::expected<void, std::string> built =
        build_program(module, compilation, program, options.mode);
    if (!built) {
        remove_temporary(folder);
        report_build_error(console, compilation, shown, built.error());
        return compile_errors_exit_code;
    }

    report_built(console, options, compilation, module, start);
    console.status("Running", name);
    const std::expected<int, std::string> exit_code =
        run_attached(program, options.program_arguments);

    // Cleaning up is best effort. A folder left in temp isn't worth an error.
    remove_temporary(folder);
    if (!exit_code) {
        console.error(exit_code.error());
        return compile_errors_exit_code;
    }

    // A program that crashes on Windows gives an NTSTATUS, like 0xC0000409, which shows in hex.
    if (*exit_code == panic_exit_code) {
        console.error(
            std::format("{} exited with code {}, which a panic gives", name, panic_exit_code));
    } else if (*exit_code < 0) {
        console.error(std::format("{} exited with code 0x{:08X}", name,
                                  static_cast<std::uint32_t>(*exit_code)));
    } else if (*exit_code != 0) {
        console.error(std::format("{} exited with code {}", name, *exit_code));
    }

    return *exit_code;
}

/// check, build, and run all compile and lower the program, printing the views asked for. Then
/// build and run go on to write an executable.
int compile_command(const options_t& options) {
    const auto start = std::chrono::steady_clock::now();
    const console_t console(options.color);
    const bool checking = options.command == command_t::CHECK;

    // This only fails when the file can't be read, or a folder isn't a package's, which counts as
    // bad usage. Without either, it's the package the current folder is in, as cargo finds one.
    std::filesystem::path input = options.input;
    if (input.empty()) {
        std::error_code error;
        const std::optional<std::filesystem::path> found =
            find_package(std::filesystem::current_path(error));
        if (!found) {
            console.error("there's no project.toml in this folder or above it; give a package's "
                          "folder, or a .fb file");
            return usage_exit_code;
        }

        input = *found;
    }

    const std::expected<compilation_t, std::string> compiled = compile_file(input, options.mode);
    if (!compiled) {
        console.error(compiled.error());
        return usage_exit_code;
    }

    // A package is named as cargo names a crate: "game v0.1.0 (C:/games/game)".
    const std::optional<package_info_t>& package = compiled->package;
    const std::string name = package ? std::format("{} v{} ({})", package->name, package->version,
                                                   to_utf8(package->root))
                                     : to_utf8(input);
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

    // A file with thousands of errors, like one that isn't source code, shows the first ones.
    const std::span<const diagnostic_t> diagnostics = compilation.diagnostics.diagnostics();
    for (const diagnostic_t& diagnostic : diagnostics | std::views::take(shown_diagnostics)) {
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, { .color = console.err().color }));
    }

    if (const std::size_t rest =
            diagnostics.size() - std::min(diagnostics.size(), shown_diagnostics);
        rest > 0) {
        console.note(rest == 1 ? std::string("1 more diagnostic isn't shown")
                               : std::format("{} more diagnostics aren't shown", rest));
    }

    if (const std::size_t errors = compilation.diagnostics.error_count(); errors > 0) {
        console.error(checking ? std::format("{} has {}", name, count(errors, "error"))
                               : std::format("couldn't build {} because of {}", name,
                                             count(errors, "error")));
        return compile_errors_exit_code;
    }

    // Lowering needs a program without errors, so the MIR and LLVM IR views only print from here.
    const mir_module_t module = lower(compilation.ast, compilation.analysis);
    if (const std::optional<mir_error_t>& error = module.error) {
        const diagnostic_t diagnostic{ .message = error->message, .span = error->span };
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, { .color = console.err().color }));
        console.error(checking ? std::format("{} has 1 error", name)
                               : std::format("couldn't build {} because of 1 error", name));
        return compile_errors_exit_code;
    }

    if (wants(options, stage_t::MIR)) {
        print_view(console, options, stage_t::MIR, dump(module, style));
    }

    if (wants(options, stage_t::LLVM)) {
        const auto ir =
            emit_ir(module, compilation.sources, compilation.file, { .mode = options.mode });
        if (!ir) {
            console.error(ir.error());
            return compile_errors_exit_code;
        }

        print_view(console, options, stage_t::LLVM, highlight_llvm(*ir, style));
    }

    switch (options.command) {
    case command_t::BUILD:
        return library ? build_library(console, options, compilation, name, start)
                       : build(console, options, compilation, name, module, start);
    case command_t::RUN:
        return run(console, options, compilation, name, module, start);
    default:
        console.status("Finished", std::format("check in {}", seconds_since(start)));
        return 0;
    }
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
        console_t::print(std::format("fb {}\n", build_config::version));
        return 0;
    case command_t::CHECK:
    case command_t::BUILD:
    case command_t::RUN:
        return compile_command(options);
    case command_t::BINDGEN:
        return bindgen_command(options);
    }

    std::unreachable();
}

void report_usage_error(const usage_error_t& error) {
    // A --color before the mistake still says how it's shown.
    console_t(error.color).error(error.message);
    console_t::print_error("\nRun 'fb help' to see the commands and options.\n");
}

} // namespace fb
