#include "fb/commands.hpp"

#include "fb/build_config.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/driver.hpp"
#include "fb/dump.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"
#include "fb/style.hpp"
#include "fb/terminal.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace fb {
namespace {

/// What a program exits with when it panics. The runtime's fb_panic uses the same code.
constexpr int panic_exit_code = 101;

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
    default:
        return "";
    }
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

    std::string out = command ? std::format("{}\n\n", summary(*command))
                              : std::format("FutureBlitz {}: build and run .fb programs\n\n",
                                            build_config::version);
    const std::string name =
        command ? std::format("fb {}", command_name(*command)) : "fb <command>";
    out += std::format("{} {} [options] <file.fb>\n", heading("Usage:"), name);
    if (!command) {
        out += std::format("\n{}\n", heading("Commands:"));
        for (const command_t each : { command_t::CHECK, command_t::BUILD, command_t::RUN }) {
            out += entry(command_name(each), summary(each));
        }

        out += entry("help <command>", "Show help for a command");
    }

    out += std::format("\n{}\n", heading("Options:"));
    if (!command || command == command_t::BUILD) {
        out += entry("-o, --output <path>", "Where to write the executable (next to the source)");
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

/// "    Finished debug build in 0.31s"
void report_built(const console_t& console, const options_t& options,
                  const std::chrono::steady_clock::time_point start) {
    const std::string_view mode = options.mode == build_mode_t::RELEASE ? "release" : "debug";
    console.status("Finished", std::format("{} build in {}", mode, seconds_since(start)));
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

/// `fb build`: writes the executable to the `-o` path, or next to the source.
int build(const console_t& console, const options_t& options, const compilation_t& compilation,
          const mir_module_t& module, const std::chrono::steady_clock::time_point start) {
    const std::filesystem::path output = options.output.value_or(
        std::filesystem::path(options.input).replace_extension(executable_extension));
    if (auto built = build_program(module, compilation.source(), output, options.mode); !built) {
        console.error(built.error());
        return compile_errors_exit_code;
    }

    report_built(console, options, start);
    return 0;
}

/// `fb run`: builds the program in a temporary folder, runs it in this terminal, and deletes the
/// folder. Returns the program's exit code, or 1 when it can't be built or started.
int run(const console_t& console, const options_t& options, const compilation_t& compilation,
        const mir_module_t& module, const std::chrono::steady_clock::time_point start) {
    // A folder of its own, so the program keeps its name.
    const std::filesystem::path folder = temporary_path(options.input.stem(), "");
    std::filesystem::path program = folder / options.input.stem();
    program += executable_extension;
    const std::string name = to_utf8(program.filename());

    const std::expected<int, std::string> exit_code =
        build_program(module, compilation.source(), program, options.mode).and_then([&] {
            report_built(console, options, start);
            console.status("Running", name);
            return run_attached(program, {});
        });

    // Cleaning up is best effort. A folder left in temp isn't worth an error.
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    if (!exit_code) {
        console.error(exit_code.error());
        return compile_errors_exit_code;
    }

    if (*exit_code == panic_exit_code) {
        console.error(std::format("{} panicked (exit code {})", name, panic_exit_code));
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
    const std::string name = to_utf8(options.input);
    const bool checking = options.command == command_t::CHECK;
    console.status(checking ? "Checking" : "Compiling", name);

    // This only fails when the file can't be read, which counts as bad usage.
    const std::expected<compilation_t, std::string> compiled = compile_file(options.input);
    if (!compiled) {
        console.error(compiled.error());
        return usage_exit_code;
    }

    // The front-end views print even when there are errors, so they can help find them.
    const compilation_t& compilation = *compiled;
    const style_t& style = console.out();
    if (wants(options, stage_t::TOKENS)) {
        print_view(console, options, stage_t::TOKENS,
                   dump_tokens(compilation.source(), compilation.tokens, style));
    }

    if (wants(options, stage_t::AST)) {
        print_view(console, options, stage_t::AST, dump_tree(compilation.ast, style));
    }

    if (wants(options, stage_t::TYPES)) {
        print_view(console, options, stage_t::TYPES,
                   dump_types(compilation.ast, compilation.analysis, style));
    }

    for (const diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        console_t::print_error(
            render_pretty(diagnostic, compilation.sources, { .color = console.err().color }));
    }

    if (const std::size_t errors = compilation.diagnostics.error_count(); errors > 0) {
        console.error(checking ? std::format("{} has {}", name, count(errors, "error"))
                               : std::format("couldn't build {} because of {}", name,
                                             count(errors, "error")));
        return compile_errors_exit_code;
    }

    // Lowering needs a program without errors, so the MIR and LLVM IR views only print from here.
    const mir_module_t module = lower(compilation.ast, compilation.analysis);
    if (wants(options, stage_t::MIR)) {
        print_view(console, options, stage_t::MIR, dump(module, style));
    }

    if (wants(options, stage_t::LLVM)) {
        const auto ir = emit_ir(module, compilation.source(), { .mode = options.mode });
        if (!ir) {
            console.error(ir.error());
            return compile_errors_exit_code;
        }

        print_view(console, options, stage_t::LLVM, highlight_llvm(*ir, style));
    }

    switch (options.command) {
    case command_t::BUILD:
        return build(console, options, compilation, module, start);
    case command_t::RUN:
        return run(console, options, compilation, module, start);
    default:
        console.status("Finished", std::format("check in {}", seconds_since(start)));
        return 0;
    }
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
    }

    std::unreachable();
}

void report_usage_error(const std::string_view message) {
    // The options didn't parse, so there's no `--color` to follow.
    console_t(color_t::AUTO).error(message);
    console_t::print_error("\nRun 'fb help' to see the commands and options.\n");
}

} // namespace fb
