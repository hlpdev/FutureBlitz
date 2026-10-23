#include "fb/driver.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

// std's arithmetic at counts no test program could hold in memory, through its Package helpers,
// which only std's own modules can call: a module in std/extra/ calls them, and a debug program,
// in which an overflow panics, prints what they give.
// And how long std's work takes as its input grows, which no test program can see.

namespace {

/// Builds main.fb's `text` as the debug program `name`, with the standard library and `module`
/// as std/extra/checks.fb, and runs it. Fails with the first compile error.
std::expected<fb::process_result_t, std::string>
run_with_std_module(const std::string& name, std::string text, std::string module) {
    std::vector<fb::library_file_t> library =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    library.push_back({ .path = "std/extra/checks.fb", .text = std::move(module) });
    const fb::compilation_t compilation =
        fb::compile("main.fb", std::move(text), std::move(library));
    if (compilation.diagnostics.has_errors()) {
        const fb::diagnostic_t& first = compilation.diagnostics.diagnostics().front();
        return std::unexpected(std::format(
            "{}: {}", compilation.sources.get(first.span.file).slice(first.span), first.message));
    }

    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / name;
    program += fb::executable_extension;
    const fb::mir_module_t module_code = fb::lower(compilation.ast, compilation.analysis);
    return fb::build_program(module_code, compilation, program, fb::build_mode_t::DEBUG)
        .and_then([&] { return fb::run_process(program, {}); });
}

} // namespace

TEST_CASE("std's counts and indexes don't overflow near the most an Int holds", "[programs]") {
    // A Map's buckets stop at 2^30; a heap's last parent is before count / 2; a Queue drops its
    // emptied slots at half; a RingBuffer's slot goes round without adding past its capacity;
    // and a Deque's balance takes the larger half of one side, with room for as many values as
    // an Int counts.
    const std::string module = "Import std.core\n"
                               "Import std.collections.array\n"
                               "Import std.collections.deque\n"
                               "Import std.collections.map\n"
                               "Import std.collections.queue\n"
                               "Import std.collections.ring_buffer\n"
                               "Public Function Check()\n"
                               "    For count In [0, 8, 9, 1073741823, 1073741824, 1073741825, "
                               "2147483647]\n"
                               "        Print(count, BucketsFor(count))\n"
                               "    End For\n"
                               "    Print(HeapChild(1073741822, 2147483647) ?? -1,\n"
                               "          HeapChild(1073741823, 2147483647) ?? -1,\n"
                               "          HeapChild(1073741824, 2147483647) ?? -1,\n"
                               "          HeapChild(2147483646, 2147483647) ?? -1, "
                               "HeapChild(0, 1) ?? -1, HeapChild(0, 2) ?? -1)\n"
                               "    Print(HalfEmptied(1073741823, 2147483647), "
                               "HalfEmptied(1073741824, 2147483647),\n"
                               "          HalfEmptied(1, 3), HalfEmptied(1, 2))\n"
                               "    Print(SlotFrom(2147483646, 0, 2147483647), "
                               "SlotFrom(2147483646, 1, 2147483647),\n"
                               "          SlotFrom(2147483646, 2147483646, 2147483647),\n"
                               "          SlotFrom(1073741824, 1073741823, 2147483647), "
                               "SlotFrom(2, 2, 3), SlotFrom(0, 2, 3))\n"
                               "    Print(HalfUp(2147483647), HalfUp(2147483646), HalfUp(1), "
                               "HalfUp(2), HalfUp(3))\n"
                               "    Print(HasRoom(2147483646, 0), HasRoom(2147483647, 0), "
                               "HasRoom(0, 2147483647),\n"
                               "          HasRoom(1073741824, 1073741823), "
                               "HasRoom(1073741823, 1073741823))\n"
                               "End Function\n";
    const std::expected<fb::process_result_t, std::string> result =
        run_with_std_module("std_limits", "Import std.extra.checks\nCheck()", module);
    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output == "0 (8, 61)\n"
                            "8 (8, 61)\n"
                            "9 (16, 60)\n"
                            "1073741823 (1073741824, 34)\n"
                            "1073741824 (1073741824, 34)\n"
                            "1073741825 (1073741824, 34)\n"
                            "2147483647 (1073741824, 34)\n"
                            "2147483645 -1 -1 -1 -1 1\n"
                            "False True False True\n"
                            "2147483646 0 2147483645 0 1 2\n"
                            "1073741824 1073741823 1 1 2\n"
                            "True False False False True\n");
    CHECK(result->exit_code == 0);
}

TEST_CASE("std.concurrent's counts stop at the most an Int holds", "[programs]") {
    // A ConcurrentQueue's ring doubles up to that, and it and a ConcurrentMap count up to it.
    const std::string module = "Import std.core\n"
                               "Import std.concurrent\n"
                               "Public Function Check()\n"
                               "    Print(GrownRing(0), GrownRing(16), GrownRing(1073741823),\n"
                               "          GrownRing(1073741824), GrownRing(2147483646))\n"
                               "    CheckQueueRoom(2147483646)\n"
                               "    CheckMapRoom(2147483646)\n"
                               "    Print(\"room\")\n"
                               "End Function\n"
                               "Public Function FullQueue()\n"
                               "    CheckQueueRoom(2147483647)\n"
                               "End Function\n"
                               "Public Function FullMap()\n"
                               "    CheckMapRoom(2147483647)\n"
                               "End Function\n";
    const std::expected<fb::process_result_t, std::string> result =
        run_with_std_module("concurrent_limits", "Import std.extra.checks\nCheck()", module);
    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output == "16 32 2147483646 2147483647 2147483647\nroom\n");
    CHECK(result->exit_code == 0);

    const std::expected<fb::process_result_t, std::string> queue = run_with_std_module(
        "concurrent_full_queue", "Import std.extra.checks\nFullQueue()", module);
    INFO((queue ? queue->output : queue.error()));
    REQUIRE(queue);
    CHECK(queue->output.starts_with(
        "panic: a ConcurrentQueue holds no more values than an Int counts at "));
    CHECK(queue->exit_code == 101);

    const std::expected<fb::process_result_t, std::string> map =
        run_with_std_module("concurrent_full_map", "Import std.extra.checks\nFullMap()", module);
    INFO((map ? map->output : map.error()));
    REQUIRE(map);
    CHECK(map->output.starts_with(
        "panic: a ConcurrentMap holds no more entries than an Int counts at "));
    CHECK(map->exit_code == 101);
}

TEST_CASE("a ConcurrentMap's keys spread over its parts", "[programs]") {
    // 64 hashes that differ only in their low bits, as an Id's often do, in their middle ones, or
    // in their high ones, go in many of the 16 parts, so threads changing them seldom wait.
    const std::string module = "Import std.core\n"
                               "Import std.concurrent\n"
                               "Function Parts(shift: UInt64): Int\n"
                               "    Local used: Bool[16]\n"
                               "    For i = 0 Until 64\n"
                               "        used[PartIndex((i As UInt64) << shift)] = True\n"
                               "    End For\n"
                               "    Local count = 0\n"
                               "    For i = 0 Until 16\n"
                               "        If used[i] Then count += 1\n"
                               "    End For\n"
                               "    Return count\n"
                               "End Function\n"
                               "Public Function Check()\n"
                               "    Print(Parts(0), Parts(26), Parts(58))\n"
                               "End Function\n";
    const std::expected<fb::process_result_t, std::string> result =
        run_with_std_module("concurrent_parts", "Import std.extra.checks\nCheck()", module);
    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output == "16 16 16\n");
    CHECK(result->exit_code == 0);
}

TEST_CASE("an archive writer's Finish panics at its caller for an archive past its limit",
          "[programs]") {
    // Finish's limit is the most a UInt8[] holds, which a test can't fill, so a smaller one
    // stands for it.
    const std::string module = "Import std.core\n"
                               "Import std.compress\n"
                               "Public Function Tar(limit: Int)\n"
                               "    Local writer = TarWriter()\n"
                               "    writer.Add(\"a.txt\", \"hello\".Bytes)\n"
                               "    Print(writer.FinishedWithin(limit).Count)\n"
                               "End Function\n"
                               "Public Function Zip(limit: Int)\n"
                               "    Local writer = ZipWriter()\n"
                               "    writer.Add(\"a.txt\", \"hello\".Bytes)\n"
                               "    Print(writer.FinishedWithin(limit).Count)\n"
                               "End Function\n";
    const std::expected<fb::process_result_t, std::string> fits =
        run_with_std_module("archive_fits", "Import std.extra.checks\nTar(2048)\nZip(200)", module);
    INFO((fits ? fits->output : fits.error()));
    REQUIRE(fits);
    CHECK(fits->output.starts_with("2048\n"));
    CHECK(fits->exit_code == 0);

    const std::expected<fb::process_result_t, std::string> tar =
        run_with_std_module("archive_tar_past", "Import std.extra.checks\nTar(2047)", module);
    INFO((tar ? tar->output : tar.error()));
    REQUIRE(tar);
    CHECK(tar->output.starts_with("panic: the tar's bytes are more than a UInt8[] holds at "));
    CHECK(tar->output.contains("checks.fb:6:"));
    CHECK(tar->exit_code == 101);

    const std::expected<fb::process_result_t, std::string> zip =
        run_with_std_module("archive_zip_past", "Import std.extra.checks\nZip(20)", module);
    INFO((zip ? zip->output : zip.error()));
    REQUIRE(zip);
    CHECK(zip->output.starts_with("panic: the zip's bytes are more than a UInt8[] holds at "));
    CHECK(zip->output.contains("checks.fb:11:"));
    CHECK(zip->exit_code == 101);
}

TEST_CASE("Html.Unescape takes time that grows with its text alone", "[programs]") {
    // 1.6 MB of bare "&" and an entity at the end: each "&" used to look all the way to the ";",
    // which took over 30 seconds.
    const fb::compilation_t compilation =
        fb::compile("unescape.fb",
                    "Import std.core\n"
                    "Import std.text\n"
                    "Import std.text.encoding\n"
                    "Print(Html.Unescape(\"&b=2\".Repeated(400000) + \"&amp;\").Length)\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "std_unescape";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::RELEASE));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "1600001\n");
    CHECK(took < std::chrono::seconds(10));
}

TEST_CASE("Repeated makes an empty String without counting to its count", "[programs]") {
    // "" 2147483647 times used to append nothing that many times, which took most of a minute.
    const fb::compilation_t compilation =
        fb::compile("repeat.fb",
                    "Import std.core\n"
                    "Import std.text\n"
                    "Local empty = \"\"\n"
                    "Print(\"[{empty.Repeated(2147483647)}]\")\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "std_repeat";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "[]\n");
    CHECK(took < std::chrono::seconds(10));
}
