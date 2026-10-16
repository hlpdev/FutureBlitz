#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// These run the real fb, with its output going to a pipe like in a script.

namespace {

/// Runs fb and collects its exit code and its stdout and stderr together.
fb::process_result_t fb_run(const std::vector<std::string>& arguments) {
    const std::expected<fb::process_result_t, std::string> result =
        fb::run_process(FB_CLI, arguments);
    REQUIRE(result);
    return *result;
}

/// tests/programs/<name>
std::string program(const std::string_view name) {
    return fb::to_utf8(std::filesystem::path(FB_TEST_PROGRAMS) / name);
}

/// Writes a source file into the test output folder.
std::filesystem::path write_source(const std::string_view name, const std::string_view text) {
    const std::filesystem::path path = std::filesystem::path(FB_TEST_OUTPUT) / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

void check_usage_error(const std::vector<std::string>& arguments, const std::string_view message) {
    const fb::process_result_t result = fb_run(arguments);
    INFO(result.output);
    CHECK(result.exit_code == 2);
    CHECK(result.output.contains(message));
}

} // namespace

TEST_CASE("fb --version prints the version", "[cli]") {
    const fb::process_result_t result = fb_run({ "--version" });
    CHECK(result.exit_code == 0);
    CHECK(result.output == std::format("fb {}\n", fb::build_config::version));
}

TEST_CASE("fb shows help", "[cli]") {
    const fb::process_result_t result = fb_run({});
    CHECK(result.exit_code == 0);
    CHECK(result.output.contains("Usage: fb <command> [options] <file.fb>"));
    CHECK(result.output.contains("--show <stages>"));

    CHECK(fb_run({ "help", "build" }).output.contains("Usage: fb build"));
    const std::string run_help = fb_run({ "run", "--help" }).output;
    CHECK(run_help.contains("Usage: fb run"));
    CHECK_FALSE(run_help.contains("--output"));
}

TEST_CASE("bad usage exits with code 2", "[cli]") {
    check_usage_error({ "frobnicate" }, "error: unknown command 'frobnicate'");
    check_usage_error({ "build" }, "error: 'fb build' needs a .fb file");
    check_usage_error({ "check", "a.fb", "b.fb" }, "unexpected argument 'b.fb'");
    check_usage_error({ "check", "a.fb", "--nope" }, "unknown option '--nope'");
    check_usage_error({ "check", "a.fb", "-o", "a.exe" }, "'-o' only works with 'fb build'");
    check_usage_error({ "check", "a.fb", "--show", "bytes" }, "unknown stage 'bytes'");
    check_usage_error({ "check", "a.fb", "--show" }, "'--show' needs a value");
    check_usage_error({ "check", "a.fb", "--color=sometimes" },
                      "'--color' takes auto, always, or never, not 'sometimes'");
    // A file that can't be read is bad usage too, not a compile error.
    check_usage_error({ "check", "missing.fb" }, "error: cannot read 'missing.fb'");
}

TEST_CASE("fb check reports errors with exit code 1", "[cli]") {
    const fb::process_result_t good = fb_run({ "check", program("hello.fb") });
    CHECK(good.exit_code == 0);
    // Status verbs are right-aligned in 12 columns.
    CHECK(good.output.contains("    Checking "));
    CHECK(good.output.contains("    Finished check in "));

    const std::filesystem::path source = write_source("bad.fb", "Local x: Int = \"a\"\n");
    const fb::process_result_t bad = fb_run({ "check", fb::to_utf8(source) });
    CHECK(bad.exit_code == 1);
    // Int is Int32, and 1:16 is where "a" starts.
    CHECK(bad.output.contains("bad.fb:1:16: error: expected Int32, found String\n"));
    CHECK(bad.output.contains("bad.fb has 1 error\n"));
}

TEST_CASE("fb build writes an executable that runs", "[cli]") {
    std::filesystem::path executable = std::filesystem::path(FB_TEST_OUTPUT) / "built" / "hello";
    executable += fb::executable_extension;
    std::filesystem::remove(executable);

    const fb::process_result_t built =
        fb_run({ "build", program("hello.fb"), "-o", fb::to_utf8(executable) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    CHECK(built.output.contains("   Compiling "));
    CHECK(built.output.contains("    Finished debug build in "));

    const std::expected<fb::process_result_t, std::string> ran = fb::run_process(executable, {});
    REQUIRE(ran);
    CHECK(ran->output == "hello\n");
}

TEST_CASE("fb build puts the executable next to the source", "[cli]") {
    const std::filesystem::path source = write_source("next_to.fb", "Print(\"here\")\n");
    std::filesystem::path executable = source;
    executable.replace_extension(fb::executable_extension);
    std::filesystem::remove(executable);

    const fb::process_result_t result = fb_run({ "build", fb::to_utf8(source), "--release" });
    INFO(result.output);
    CHECK(result.exit_code == 0);
    CHECK(result.output.contains("    Finished release build in "));
    CHECK(std::filesystem::exists(executable));
}

TEST_CASE("fb build stops at compile errors", "[cli]") {
    const std::filesystem::path source = write_source("broken.fb", "Print(missing)\n");
    const fb::process_result_t result = fb_run({ "build", fb::to_utf8(source) });
    CHECK(result.exit_code == 1);
    CHECK(result.output.contains("error: couldn't build "));
    CHECK(result.output.contains("broken.fb because of 1 error\n"));
}

TEST_CASE("fb run runs the program and passes its exit code on", "[cli]") {
    const fb::process_result_t hello = fb_run({ "run", program("hello.fb") });
    CHECK(hello.exit_code == 0);
    CHECK(hello.output.contains(
        std::format("     Running hello{}\nhello\n", fb::executable_extension)));

    // The program prints "before", then overflows an Int. Panics exit with code 101.
    const fb::process_result_t panic = fb_run({ "run", program("panic_overflow.fb") });
    CHECK(panic.exit_code == 101);
    CHECK(panic.output.contains("before\npanic: integer overflow in '+' at "));
    CHECK(panic.output.contains(std::format("error: panic_overflow{} panicked (exit code 101)\n",
                                            fb::executable_extension)));
}

TEST_CASE("fb --show prints the stages it names", "[cli]") {
    const auto show = [](const std::string& stages) {
        const fb::process_result_t result =
            fb_run({ "check", program("hello.fb"), "--show", stages });
        CHECK(result.exit_code == 0);
        return result.output;
    };

    CHECK(show("tokens").contains("1:1   identifier      Print\n"));
    CHECK(show("ast").contains("\n(call Print \"hello\")\n"));
    CHECK(show("types").contains("\n(call Print \"hello\": String)\n"));
    CHECK(show("mir").contains("\nfunction main\n"));
    CHECK(show("llvm").contains("\ndefine "));
    // One stage prints without a header.
    CHECK_FALSE(show("ast").contains("──"));

    // Several stages get headers, once each and in pipeline order, however they're given.
    const std::string both =
        fb_run({ "check", program("hello.fb"), "--show", "mir", "--show=ast,mir" }).output;
    const std::size_t ast = both.find("── ast ──\n");
    const std::size_t mir = both.find("── mir ──\n");
    CHECK(ast != std::string::npos);
    CHECK(mir != std::string::npos);
    CHECK(ast < mir);
    CHECK(both.find("── mir ──", mir + 1) == std::string::npos);
}

TEST_CASE("fb --show still prints the front end when there are errors", "[cli]") {
    const std::filesystem::path source = write_source("show_errors.fb", "Local x: Int = \"a\"\n");
    const fb::process_result_t result = fb_run({ "check", fb::to_utf8(source), "--show", "all" });
    CHECK(result.exit_code == 1);
    // Tokens, the tree, and types print. MIR needs a program without errors, so it doesn't.
    CHECK(result.output.contains("── tokens ──\n"));
    CHECK(result.output.contains("(local x: Int = \"a\")\n"));
    CHECK(result.output.contains("(local x: Int = \"a\": String)\n"));
    CHECK_FALSE(result.output.contains("── mir ──"));
}

TEST_CASE("fb --color turns ANSI colors on and off", "[cli]") {
    const std::vector<std::string> check_hello = { "check", program("hello.fb"), "--show", "ast" };
    std::vector<std::string> always = check_hello;
    always.emplace_back("--color=always");
    const std::string colored = fb_run(always).output;
    // A bold green status verb, then a magenta head and a cyan name in the tree.
    CHECK(colored.contains("\x1b[1;32m    Checking\x1b[0m "));
    CHECK(colored.contains("(\x1b[35mcall\x1b[0m \x1b[36mPrint\x1b[0m"));

    std::vector<std::string> never = check_hello;
    never.insert(never.end(), { "--color", "never" });
    CHECK_FALSE(fb_run(never).output.contains('\x1b'));

    // A pipe isn't a terminal, so the default is no color here.
    CHECK_FALSE(fb_run(check_hello).output.contains('\x1b'));
}
