#include "fb/driver.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// End-to-end tests: every tests/programs/<name>.fb is compiled, linked, and run, and its output
// must match <name>.out. A name starting with "panic_" must exit with a panic (code 101), one
// starting with "uncaught_" must let an error out of its top-level code (code 1), and one
// starting with "release_" is built in release mode.

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// Fails with the first compile error, or when building or running the program fails.
std::expected<fb::process_result_t, std::string> build_and_run(const std::filesystem::path& source,
                                                               const fb::build_mode_t mode) {
    // Panics report just the file name, so the expected output doesn't depend on the checkout.
    const fb::compilation_t compilation =
        fb::compile(source.filename().string(), read_file(source));
    if (compilation.diagnostics.has_errors()) {
        const fb::diagnostic_t& first = compilation.diagnostics.diagnostics().front();
        return std::unexpected(std::format(
            "{}: {}", compilation.sources.get(first.span.file).slice(first.span), first.message));
    }

    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / source.stem();
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    return fb::build_program(module, compilation, program, mode).and_then([&] {
        return fb::run_process(program, {});
    });
}

} // namespace

TEST_CASE("programs print what they should", "[programs]") {
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(FB_TEST_PROGRAMS)) {
        if (entry.path().extension() != ".fb") {
            continue;
        }

        const std::string name = entry.path().stem().string();
        DYNAMIC_SECTION(name) {
            const fb::build_mode_t mode =
                name.starts_with("release_") ? fb::build_mode_t::RELEASE : fb::build_mode_t::DEBUG;
            const std::expected<fb::process_result_t, std::string> result =
                build_and_run(entry.path(), mode);
            INFO((result ? result->output : result.error()));
            REQUIRE(result);

            std::filesystem::path expected = entry.path();
            expected.replace_extension(".out");
            CHECK(result->output == read_file(expected));
            int code = 0;
            if (name.starts_with("panic_")) {
                code = 101;
            } else if (name.starts_with("uncaught_")) {
                code = 1;
            }

            CHECK(result->exit_code == code);
        }
    }
}

TEST_CASE("a panic in a library file says where in it", "[programs]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/check.fb",
            .text = "Function Check(n: Int)\n"
                    "    Assert(n > 0, \"{n} isn't positive\")\n"
                    "End Function\n",
        },
    };
    const fb::compilation_t compilation = fb::compile("main.fb", "Check(1)\nCheck(-2)", library);
    REQUIRE_FALSE(compilation.diagnostics.has_errors());

    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "library_panic";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    const std::expected<fb::process_result_t, std::string> result =
        fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG).and_then([&] {
            return fb::run_process(program, {});
        });
    REQUIRE(result);
    CHECK(result->output == "panic: -2 isn't positive at lib/check.fb:2:5\n");
    CHECK(result->exit_code == 101);
}
