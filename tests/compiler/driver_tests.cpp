#include "fb/driver.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace {

/// "<file>: <source text>: message" for each diagnostic, in order.
std::vector<std::string> errors_of(const fb::compilation_t& compilation) {
    std::vector<std::string> errors;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        const fb::source_file_t& file = compilation.sources.get(diagnostic.span.file);
        errors.push_back(std::format("{}: {}: {}", file.path(), file.slice(diagnostic.span),
                                     diagnostic.message));
    }

    return errors;
}

} // namespace

TEST_CASE("compile runs every stage, even after errors", "[driver]") {
    // The first line has a type error, and the second still gets analyzed.
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Local x: Int = \"a\"\nPrint(\"{x}\")");
    CHECK(compilation.source().path() == "test.fb");
    CHECK(compilation.diagnostics.error_count() == 1);
    CHECK(compilation.tokens.back().kind == fb::token_kind_t::END_OF_FILE);
    // The standard library's items come first.
    CHECK(std::ranges::count_if(compilation.ast.items(), [&](const fb::stmt_id_t id) {
              return !compilation.ast.from_library(id);
          }) == 2);
    CHECK(compilation.analysis.exprs.size() == compilation.ast.expr_count());
}

TEST_CASE("compile adds the standard library after the main file", "[driver]") {
    const fb::compilation_t compilation = fb::compile("test.fb", "Throw Error(\"x\")");
    CHECK(compilation.diagnostics.diagnostics().empty());
    CHECK(compilation.file == fb::file_id_t{});
    CHECK(compilation.sources.get(fb::file_id_t{ 1 }).path() == "std/core/error.fb");
    CHECK(compilation.analysis.error_class != fb::type_id_t::INVALID);

    const std::expected<std::vector<fb::library_file_t>, std::string> library =
        fb::standard_library();
    REQUIRE(library);
    CHECK(std::ranges::is_sorted(*library, {}, &fb::library_file_t::path));
    CHECK(std::ranges::contains(*library, "std/core/error.fb", &fb::library_file_t::path));
}

TEST_CASE("library files share the main file's names", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Function Twice(x: Int): Int => Helper(x) * 2\n" },
        { .path = "lib/b.fb", .text = "Function Helper(x: Int): Int => x\nConst LIMIT = 10\n" },
    };
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Print(\"{Twice(LIMIT)}\")", library);
    CHECK(errors_of(compilation).empty());

    // A name the main file declares again is its error, not the library's.
    CHECK(errors_of(fb::compile("test.fb", "Function Helper(x: Int): Int => x", library)) ==
          std::vector<std::string>{
              "test.fb: Helper: 'Helper' is already declared with the same parameter types" });
}

TEST_CASE("a library file only declares things", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Global count = 1\nPrint(\"x\")\nLocal y = 2\n" },
    };
    CHECK(errors_of(fb::compile("test.fb", "Print(\"{count}\")", library)) ==
          std::vector<std::string>{
              "lib/a.fb: Print(\"x\"): a library file only declares things; code that runs goes "
              "in the main file",
              "lib/a.fb: Local y = 2: a library file only declares things; code that runs goes in "
              "the main file",
          });
}

TEST_CASE("a library file's errors point into it", "[driver]") {
    const std::vector<fb::library_file_t> library{
        { .path = "lib/a.fb", .text = "Function F(): Int => \"a\"\n" },
    };
    CHECK(errors_of(fb::compile("test.fb", "Local x = 1", library)) ==
          std::vector<std::string>{ "lib/a.fb: \"a\": expected Int32, found String" });
}

TEST_CASE("compile_file reports a file it can't read", "[driver]") {
    const std::expected<fb::compilation_t, std::string> compiled =
        fb::compile_file("no such file.fb");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().starts_with("cannot read 'no such file.fb'"));
}

TEST_CASE("temporary paths are new each time", "[driver]") {
    const std::filesystem::path first = fb::temporary_path("hello", ".exe");
    const std::filesystem::path second = fb::temporary_path("hello", ".exe");
    CHECK(first != second);
    CHECK(first.extension() == ".exe");
    CHECK(first.stem().string().starts_with("hello-"));
    CHECK(first.parent_path().filename() == "futureblitz");
    CHECK(std::filesystem::is_directory(first.parent_path()));
}
