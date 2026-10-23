#include "fb/codegen.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Const Functions running in the compiler, and the build mode their DebugAsserts follow.

namespace {

using errors_t = std::vector<std::string>;

/// Each error of `text`, with the standard library, in a build of `mode`, as "<source text>:
/// <message>", each followed by its labels, as "<label>: <source text>".
errors_t errors_of(const std::string_view text,
                   const fb::build_mode_t mode = fb::build_mode_t::DEBUG) {
    const fb::compilation_t compilation =
        fb::compile("test.fb", std::string(text),
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}), mode);
    const auto written = [&](const fb::span_t span) {
        return compilation.sources.get(span.file).slice(span);
    };
    errors_t errors;
    for (const fb::diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        errors.push_back(std::format("{}: {}", written(diagnostic.span), diagnostic.message));
        for (const fb::label_t& label : diagnostic.labels) {
            errors.push_back(std::format("{}: {}", label.message, written(label.span)));
        }
    }

    return errors;
}

} // namespace

TEST_CASE("a Const Function's DebugAssert runs in debug builds only", "[evaluate]") {
    const std::string_view half = "Const Function Half(n: Int): Int\n"
                                  "    DebugAssert(n > 0, \"n must be positive\")\n"
                                  "    Return n / 2\n"
                                  "End Function\n"
                                  "Const H = Half(-4)";
    CHECK(errors_of(half) ==
          errors_t{
              "Half(-4): 'Half' can't run at compile time: it panics: n must be positive",
              "here: DebugAssert(n > 0, \"n must be positive\")",
          });
    CHECK(errors_of(half, fb::build_mode_t::RELEASE).empty());
}
