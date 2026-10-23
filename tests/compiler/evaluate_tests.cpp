#include "fb/codegen.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Const Functions running in the compiler: their limits, the place in the code that each limit's
// error points at, and the build mode their DebugAsserts follow.

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

TEST_CASE("a Const Function past a limit points at where it got to", "[evaluate]") {
    CHECK(errors_of("Const Function Forever(): Int\n"
                    "    Local x = 0\n"
                    "    While True\n"
                    "        x += 1\n"
                    "        If x == -5 Then Break\n"
                    "    End While\n"
                    "    Return x\n"
                    "End Function\n"
                    "Const X = Forever()") ==
          errors_t{
              "Forever(): 'Forever' can't run at compile time: it runs more than 10000000 steps",
              "here: x += 1",
          });
    CHECK(errors_of("Const Function Deep(n: Int): Int => n == 0 ? 0 : 1 + Deep(n - 1)\n"
                    "Const X = Deep(20000)") ==
          errors_t{
              "Deep(20000): 'Deep' can't run at compile time: its calls go more than 10000 deep",
              "here: Deep(n - 1)",
          });
    CHECK(errors_of("Const Function Big(n: Int): String\n"
                    "    Local s = \"x\"\n"
                    "    For i = 1 To n\n"
                    "        s = s + s\n"
                    "    End For\n"
                    "    Return s\n"
                    "End Function\n"
                    "Const X = Big(40)") ==
          errors_t{
              "Big(40): 'Big' can't run at compile time: it holds more than 256 MiB",
              "here: s + s",
          });
}

TEST_CASE("calls just short of the depth limit run", "[evaluate]") {
    CHECK(errors_of("Const Function Deep(n: Int): Int => n == 0 ? 0 : 1 + Deep(n - 1)\n"
                    "Const X = Deep(9000)")
              .empty());
}

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
