#include "fb/driver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <string>

TEST_CASE("compile runs every stage, even after errors", "[driver]") {
    // The first line has a type error, and the second still gets analyzed.
    const fb::compilation_t compilation =
        fb::compile("test.fb", "Local x: Int = \"a\"\nPrint(\"{x}\")");
    CHECK(compilation.source().path() == "test.fb");
    CHECK(compilation.diagnostics.error_count() == 1);
    CHECK(compilation.tokens.back().kind == fb::token_kind_t::END_OF_FILE);
    CHECK(compilation.ast.items().size() == 2);
    CHECK(compilation.analysis.exprs.size() == compilation.ast.expr_count());
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
