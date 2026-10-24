#include "fb/os_process.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What starting a child takes apart from the OS; tests/programs' os_process and tests/cli check
// what each OS does with it.

namespace {

/// What goes after cmd.exe's /c for C:\x\show.bat with `arguments`.
std::optional<std::string> batch(const std::vector<std::string>& arguments) {
    return fb::os::batch_arguments(R"(C:\x\show.bat)", arguments);
}

} // namespace

TEST_CASE("a .bat or .cmd file is one whatever its case and the dots and spaces after it",
          "[runtime]") {
    CHECK(fb::os::is_batch_file(R"(C:\x\show.bat)"));
    CHECK(fb::os::is_batch_file("SHOW.CMD"));
    CHECK(fb::os::is_batch_file("show.Bat."));
    CHECK(fb::os::is_batch_file("show.bat . ."));
    CHECK(!fb::os::is_batch_file("show.exe"));
    CHECK(!fb::os::is_batch_file("show.batch"));
    CHECK(!fb::os::is_batch_file("bat"));
    CHECK(!fb::os::is_batch_file(""));
}

TEST_CASE("a batch file's arguments are quoted and escaped so cmd.exe runs none of them",
          "[runtime]") {
    CHECK(batch({}) == R"(""C:\x\show.bat"")");
    CHECK(batch({ "plain", R"(a-b_c.d/e:f?g@h#$*+\x)" }) ==
          R"(""C:\x\show.bat" plain a-b_c.d/e:f?g@h#$*+\x")");
    // What cmd.exe reads as more than text goes in quotes, and a quote in it is doubled.
    CHECK(batch({ "safe&echo,INJECTED" }) == R"(""C:\x\show.bat" "safe&echo,INJECTED"")");
    CHECK(batch({ R"(x" & echo INJECTED)" }) == R"(""C:\x\show.bat" "x"" & echo INJECTED"")");
    CHECK(batch({ "a b", "", "^|<>()!" }) == R"(""C:\x\show.bat" "a b" "" "^|<>()!"")");
    // No % starts a variable's name.
    CHECK(batch({ "%PATH%" }) == R"(""C:\x\show.bat" "%%cd:~,%PATH%%cd:~,%"")");
    // Backslashes are doubled only before a quote, the closing one too.
    CHECK(batch({ R"(back\)", R"(a\"b)", R"(c\\d)" }) ==
          R"(""C:\x\show.bat" "back\\" "a\\""b" c\\d")");
    // UTF-8 goes as it is, but for C1 controls.
    CHECK(batch({ "µ", "\xC2\x85" }) == "\"\"C:\\x\\show.bat\" µ \"\xC2\x85\"\"");
    CHECK(fb::os::batch_arguments(R"(C:\100%\show.bat)", {}) == R"(""C:\100%%cd:~,%\show.bat"")");
}

TEST_CASE("a batch file can't take an argument with a line break, or a path no file has",
          "[runtime]") {
    CHECK(batch({ "fine", "two\nlines" }) == std::nullopt);
    CHECK(batch({ "return\r" }) == std::nullopt);
    CHECK(fb::os::batch_arguments(R"(C:\x"y\show.bat)", {}) == std::nullopt);
    CHECK(fb::os::batch_arguments(R"(C:\x\)", {}) == std::nullopt);
}

TEST_CASE("a Windows PATH lists folders, maybe in quotes, and never an empty one", "[runtime]") {
    using entries_t = std::vector<std::string>;
    CHECK(fb::os::path_entries(R"(C:\a;"C:\b;c";;'D:\e';C:\f\)") ==
          entries_t{ R"(C:\a)", R"(C:\b;c)", R"(D:\e)", R"(C:\f\)" });
    CHECK(fb::os::path_entries(R"(;C:\a)") == entries_t{ R"(C:\a)" });
    CHECK(fb::os::path_entries(R"(""; ;'')") == entries_t{ " " });
    CHECK(fb::os::path_entries("").empty());
}

TEST_CASE("a child's held bytes come out in order, however they're taken", "[runtime]") {
    fb::os::byte_queue_t queue;
    std::string expected;
    std::string taken;
    std::array<std::uint8_t, 7> into{};
    for (int i = 0; i < 1000; ++i) {
        const std::string bytes(static_cast<std::size_t>(i % 13),
                                static_cast<char>('a' + (i % 26)));
        queue.append(bytes);
        expected += bytes;
        const std::size_t count =
            queue.take(std::span(into).first(static_cast<std::size_t>(i % 8)));
        taken.append(reinterpret_cast<const char*>(into.data()), count);
        CHECK(queue.size() == expected.size() - taken.size());
    }

    while (!queue.empty()) {
        const std::size_t count = queue.take(into);
        REQUIRE(count > 0);
        taken.append(reinterpret_cast<const char*>(into.data()), count);
    }

    CHECK(taken == expected);
    CHECK(queue.take(into) == 0);
}

TEST_CASE("taking a child's held bytes takes time in proportion to them", "[runtime]") {
    // 128 MiB taken 64 KiB at a time: moving what's left each time moved 128 GiB, for many
    // seconds; moving it now and then moves less than is taken.
    fb::os::byte_queue_t queue;
    queue.append(std::string(std::size_t{ 128 } << 20U, 'x'));
    std::vector<std::uint8_t> into(std::size_t{ 64 } << 10U);
    const auto start = std::chrono::steady_clock::now();
    std::size_t taken = 0;
    while (!queue.empty()) {
        taken += queue.take(into);
    }

    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    CHECK(taken == std::size_t{ 128 } << 20U);
    CHECK(took < std::chrono::seconds(2));
}
