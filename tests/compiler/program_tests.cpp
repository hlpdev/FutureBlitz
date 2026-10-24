#include "fb/driver.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <array>
#include <fcntl.h>
#include <unistd.h>
#endif

// End-to-end tests: every tests/programs/<name>.fb is compiled, linked, and run, and its output
// must match <name>.out. The modules it imports by path are in tests/programs/<name>/. A name
// starting with "panic_" must exit with a panic (code 101), one starting with "uncaught_" must
// let an error out of its top-level code (code 1), and one starting with "release_" is built in
// release mode.

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// `output` with each path in `folder` written from the folder, as "vmath/vmath.fb", so what a
/// panic in a dependency says doesn't depend on where the tests run.
std::string from_folder(std::string output, const std::filesystem::path& folder) {
    // Messages name a file under the current folder from there, and others by where they are.
    std::error_code error;
    const std::filesystem::path relative =
        folder.lexically_normal().lexically_relative(std::filesystem::current_path(error));
    for (const std::filesystem::path& shown : { folder, relative }) {
        if (shown.empty()) {
            continue;
        }

        const std::string prefix = (shown / "").generic_string();
        for (std::size_t at = output.find(prefix); at != std::string::npos;
             at = output.find(prefix, at)) {
            output.erase(at, prefix.size());
        }
    }

    return output;
}

/// The standard library, and the files in the folder named like the program, by their paths
/// from the program's folder, like "modules/shapes.fb".
std::vector<fb::library_file_t> program_files(const std::filesystem::path& source) {
    std::vector<fb::library_file_t> files =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    std::filesystem::path folder = source;
    folder.replace_extension();
    if (!std::filesystem::is_directory(folder)) {
        return files;
    }

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(folder)) {
        if (entry.path().extension() == ".fb") {
            files.push_back({
                .path = entry.path().lexically_relative(source.parent_path()).generic_string(),
                .text = read_file(entry.path()),
            });
        }
    }

    return files;
}

/// Fails with the first compile error, or when building or running the program fails. A
/// program with a .in file beside it reads that as its input.
std::expected<fb::process_result_t, std::string> build_and_run(const std::filesystem::path& source,
                                                               const fb::build_mode_t mode) {
    // Panics report paths from the program's folder, so the expected output doesn't depend on
    // the checkout.
    const fb::compilation_t compilation =
        fb::compile(source.filename().string(), read_file(source), program_files(source), mode);
    if (compilation.diagnostics.has_errors()) {
        const fb::diagnostic_t& first = compilation.diagnostics.diagnostics().front();
        return std::unexpected(std::format(
            "{}: {}", compilation.sources.get(first.span.file).slice(first.span), first.message));
    }

    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / source.stem();
    program += fb::executable_extension;
    std::filesystem::path input_file = source;
    input_file.replace_extension(".in");
    const std::optional<std::string> input =
        std::filesystem::exists(input_file) ? std::optional(read_file(input_file)) : std::nullopt;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    return fb::build_program(module, compilation, program, mode).and_then([&] {
        return fb::run_process(program, {}, input);
    });
}

/// Builds main.fb's `text` with the `library` modules and the standard library, which Print is
/// in, as the debug program `name`, and runs it. Fails with the first compile error.
std::expected<fb::process_result_t, std::string>
run_with_library(const std::string& name, std::string text,
                 std::vector<fb::library_file_t> library) {
    std::ranges::move(fb::standard_library().value_or(std::vector<fb::library_file_t>{}),
                      std::back_inserter(library));
    const fb::compilation_t compilation =
        fb::compile("main.fb", std::move(text), std::move(library));
    if (compilation.diagnostics.has_errors()) {
        const fb::diagnostic_t& first = compilation.diagnostics.diagnostics().front();
        return std::unexpected(std::format(
            "{}: {}", compilation.sources.get(first.span.file).slice(first.span), first.message));
    }

    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / name;
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    return fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG).and_then([&] {
        return fb::run_process(program, {});
    });
}

/// Runs `program` with a stdin that stays open, as a console's does until someone types, but
/// for no longer than `limit`, so a program that waits for it still ends. Gives what it printed,
/// or why it couldn't run, and how long it took.
std::pair<std::expected<fb::process_result_t, std::string>, std::chrono::steady_clock::duration>
run_with_open_stdin(const std::filesystem::path& program, const std::chrono::seconds limit) {
    // The program gets the pipe's read end as this process's stdin, and only this process has
    // the write end.
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    REQUIRE(CreatePipe(&read_end, &write_end, &security, 0) != 0);
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE before = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, read_end);
    const auto close_input = [&] { CloseHandle(write_end); };
#else
    std::array<int, 2> ends{};
    REQUIRE(pipe(ends.data()) == 0);
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);
    const int before = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    dup2(ends[0], STDIN_FILENO);
    const auto close_input = [&] { close(ends[1]); };
#endif

    std::mutex mutex;
    std::condition_variable ended;
    bool done = false;
    std::thread closer([&] {
        std::unique_lock lock(mutex);
        ended.wait_for(lock, limit, [&] { return done; });
        close_input();
    });
    const auto start = std::chrono::steady_clock::now();
    std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    {
        const std::scoped_lock lock(mutex);
        done = true;
    }

    ended.notify_all();
    closer.join();
#ifdef _WIN32
    SetStdHandle(STD_INPUT_HANDLE, before);
    CloseHandle(read_end);
#else
    dup2(before, STDIN_FILENO);
    close(before);
    close(ends[0]);
#endif
    return { std::move(result), took };
}

/// A copy of tests/packages in the build folder, so building writes nothing into the source
/// tree, with the vecmath C library in libs/ for each package whose project.toml links it.
std::filesystem::path staged_packages() {
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    std::filesystem::copy(FB_TEST_PACKAGES, packages, std::filesystem::copy_options::recursive);
    const std::filesystem::path archive(FB_TEST_VECMATH);
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(packages)) {
        if (read_file(entry.path() / "project.toml").contains("\"libs/vecmath\"")) {
            std::filesystem::create_directories(entry.path() / "libs");
            std::filesystem::copy_file(archive, entry.path() / "libs" / archive.filename());
        }
    }

    return packages;
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
            // A release_ program builds in release mode, and the rest of its name says how it
            // ends, as "release_panic_..." does.
            const std::string_view release_prefix = "release_";
            const bool release = name.starts_with(release_prefix);
            const fb::build_mode_t mode =
                release ? fb::build_mode_t::RELEASE : fb::build_mode_t::DEBUG;
            const std::expected<fb::process_result_t, std::string> result =
                build_and_run(entry.path(), mode);
            INFO((result ? result->output : result.error()));
            REQUIRE(result);

            std::filesystem::path expected = entry.path();
            expected.replace_extension(".out");
            CHECK(result->output == read_file(expected));
            const std::string_view ending =
                std::string_view(name).substr(release ? release_prefix.size() : 0);
            int code = 0;
            if (ending.starts_with("panic_")) {
                code = 101;
            } else if (ending.starts_with("uncaught_")) {
                code = 1;
            }

            CHECK(result->exit_code == code);
        }
    }
}

TEST_CASE("packages build and print what they should", "[programs]") {
    // Each folder of tests/packages with a project.toml is a package. One with a main file runs
    // and prints <name>.out, ending with a panic when its name starts with "panic_"; a library is
    // checked.
    const std::filesystem::path packages = staged_packages();
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(packages)) {
        if (!std::filesystem::is_regular_file(entry.path() / "project.toml")) {
            continue;
        }

        const std::string name = entry.path().filename().string();
        DYNAMIC_SECTION(name) {
            const std::expected<fb::compilation_t, std::string> compiled =
                fb::compile_file(entry.path());
            REQUIRE(compiled);
            for (const fb::diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
                FAIL_CHECK(std::format("{}: {}", compiled->sources.get(diagnostic.span.file).path(),
                                       diagnostic.message));
            }

            REQUIRE(compiled->package);
            if (compiled->package.value_or(fb::package_info_t{}).library) {
                continue;
            }

            std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / name;
            program += fb::executable_extension;
            const fb::mir_module_t module = fb::lower(compiled->ast, compiled->analysis);
            const std::expected<fb::process_result_t, std::string> result =
                fb::build_program(module, *compiled, program, fb::build_mode_t::DEBUG)
                    .and_then([&] { return fb::run_process(program, {}); });
            INFO((result ? result->output : result.error()));
            REQUIRE(result);
            CHECK(from_folder(result->output, packages) == read_file(packages / (name + ".out")));
            CHECK(result->exit_code == (name.starts_with("panic_") ? 101 : 0));
        }
    }
}

TEST_CASE("a panic in a library file says where in it", "[programs]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/check.fb",
            .text = "Public Function Check(n: Int)\n"
                    "    Assert(n > 0, \"{n} isn't positive\")\n"
                    "End Function\n",
        },
    };
    const fb::compilation_t compilation =
        fb::compile("main.fb", "Import \"./lib/check\"\nCheck(1)\nCheck(-2)", library);
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

TEST_CASE("two modules' Classes of one name stay apart, in the program and its leak report",
          "[programs]") {
    // Each library module has a Node and a Make of its own. Three Nodes in a cycle leak, and the
    // report shows each with its module, since their names are the same.
    const std::vector<fb::library_file_t> library{
        { .path = "lib/linked.fb", .text = "Public Interface Linked\nEnd Interface\n" },
        {
            .path = "lib/a.fb",
            .text = "Import \"./linked\"\n"
                    "Public Class Node Implements Linked\n"
                    "    Public Field next: Linked?\n"
                    "    Public Field label: String = \"a\"\n"
                    "End Class\n"
                    "Public Global count = 1\n"
                    "Public Function Make(): Node => Node()\n"
                    "Public Function MakeA(): Node => Make()\n"
                    "Public Function CountA(): Int => count\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./linked\"\n"
                    "Public Class Node Implements Linked\n"
                    "    Public Field next: Linked?\n"
                    "    Public Field label: String = \"b\"\n"
                    "End Class\n"
                    "Public Global count = 2\n"
                    "Public Function Make(): Node => Node()\n"
                    "Public Function MakeB(): Node => Make()\n"
                    "Public Function CountB(): Int => count\n",
        },
    };
    const std::expected<fb::process_result_t, std::string> result =
        run_with_library("module_names",
                         "Import std.core\n"
                         "Import \"./lib/linked\"\n"
                         "Import \"./lib/a\"\n"
                         "Import \"./lib/b\"\n"
                         "Class Node Implements Linked\n"
                         "    Field next: Linked?\n"
                         "End Class\n"
                         "Local a = MakeA()\n"
                         "Local b = MakeB()\n"
                         "Local m = Node()\n"
                         "a.next = b\n"
                         "b.next = m\n"
                         "m.next = a\n"
                         "Print(\"{a.label} {b.label} {CountA()} {CountB()}\")",
                         library);
    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output == "a b 1 2\n"
                            "leak: 1 lib.a.Node object was never released\n"
                            "leak: 1 lib.b.Node object was never released\n"
                            "leak: 1 main.Node object was never released\n"
                            "leak: a cycle of 3 objects: lib.a.Node.next -> lib.b.Node.next -> "
                            "main.Node.next -> the first lib.a.Node\n");
    CHECK(result->exit_code == 0);
}

TEST_CASE("the compiler's own Classes have no module in a leak report", "[programs]") {
    // A captured Local lives in a Cell<T>, which is the compiler's, so only its T has a module.
    const std::string timer = "Class Timer\n"
                              "    Public Field tick: (Func(): Int)? = Null\n"
                              "End Class\n"
                              "Function Leak()\n"
                              "    Local t = Timer()\n"
                              "    t.tick = Function(): Int\n"
                              "        Local u = t\n"
                              "        Return 1\n"
                              "    End Function\n"
                              "End Function\n";
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public " + timer.substr(0, timer.find("Function Leak")) + "Public " +
                    timer.substr(timer.find("Function Leak")),
        },
    };
    const std::expected<fb::process_result_t, std::string> result = run_with_library(
        "made_names", "Import \"./lib/a\" As A\n" + timer + "Leak()\nA.Leak()\n", library);
    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output.contains("leak: 1 Cell<lib.a.Timer> object was never released\n"));
    CHECK(result->output.contains("leak: 1 Cell<main.Timer> object was never released\n"));
    CHECK_FALSE(result->output.contains("main.Cell"));
}

TEST_CASE("a type panic tells two modules' types of one name apart", "[programs]") {
    // lib.a's Cat and Pet aren't lib.b's, so these conversions fail, and the panics say whose
    // each is, as the leak report does.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/animal.fb",
            .text = "Public Class Animal\n"
                    "End Class\n",
        },
        {
            .path = "lib/a.fb",
            .text = "Import \"./animal\"\n"
                    "Public Class Cat Extends Animal\n"
                    "End Class\n"
                    "Public Interface Pet\n"
                    "    Method Name(): String\n"
                    "End Interface\n"
                    "Public Function AsCat(x: Animal): Cat => x As Cat\n"
                    "Public Function AsPet(x: Animal): Pet => x As Pet\n",
        },
        {
            .path = "lib/b.fb",
            .text = "Import \"./animal\"\n"
                    "Public Class Cat Extends Animal\n"
                    "End Class\n"
                    "Public Class Pet Extends Animal\n"
                    "End Class\n"
                    "Public Function BCat(): Animal => Cat()\n"
                    "Public Function BPet(): Animal => Pet()\n",
        },
    };
    const std::string imports = "Import \"./lib/a\"\nImport \"./lib/b\"\n";

    const std::expected<fb::process_result_t, std::string> cat =
        run_with_library("module_cat", imports + "Local c = AsCat(BCat())", library);
    INFO((cat ? cat->output : cat.error()));
    REQUIRE(cat);
    CHECK(cat->output == "panic: expected lib.a.Cat, found lib.b.Cat at lib/a.fb:7:42\n");
    CHECK(cat->exit_code == 101);

    // An Interface and a Class whose names are the same.
    const std::expected<fb::process_result_t, std::string> pet =
        run_with_library("module_pet", imports + "Local p = AsPet(BPet())", library);
    INFO((pet ? pet->output : pet.error()));
    REQUIRE(pet);
    CHECK(pet->output == "panic: expected lib.a.Pet, found lib.b.Pet at lib/a.fb:8:42\n");
    CHECK(pet->exit_code == 101);

    // Names that differ stay as they are.
    const std::expected<fb::process_result_t, std::string> other =
        run_with_library("module_other", imports + "Local c = AsCat(BPet())", library);
    INFO((other ? other->output : other.error()));
    REQUIRE(other);
    CHECK(other->output == "panic: expected Cat, found Pet at lib/a.fb:7:42\n");
    CHECK(other->exit_code == 101);
}

TEST_CASE("panics and leak reports tell a generic Class from another of its name", "[programs]") {
    // lib.a's Box<T> and the main module's Box are two declarations, so the runtime shows each
    // with its module, as sema's messages do, though their names differ.
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Interface Held\n"
                    "End Interface\n"
                    "Public Class Box<T> Implements Held\n"
                    "    Public Field next: Held?\n"
                    "End Class\n"
                    "Public Function MakeBox(next: Held?): Held\n"
                    "    Local box = Box<Int>()\n"
                    "    box.next = next\n"
                    "    Return box\n"
                    "End Function\n",
        },
    };
    const std::string own = "Import \"./lib/a\"\n"
                            "Class Box Implements Held\n"
                            "    Field next: Held?\n"
                            "End Class\n";

    const std::expected<fb::process_result_t, std::string> panic = run_with_library(
        "generic_panic", own + "Local h = MakeBox(Null)\nLocal b = h As Box", library);
    INFO((panic ? panic->output : panic.error()));
    REQUIRE(panic);
    CHECK(panic->output == "panic: expected main.Box, found lib.a.Box<Int32> at main.fb:6:11\n");
    CHECK(panic->exit_code == 101);

    const std::expected<fb::process_result_t, std::string> leak = run_with_library(
        "generic_leak", own + "Local mine = Box()\nmine.next = MakeBox(mine)", library);
    INFO((leak ? leak->output : leak.error()));
    REQUIRE(leak);
    CHECK(leak->output == "leak: 1 main.Box object was never released\n"
                          "leak: 1 lib.a.Box<Int32> object was never released\n"
                          "leak: a cycle of 2 objects: main.Box.next -> lib.a.Box<Int32>.next -> "
                          "the first main.Box\n");
    CHECK(leak->exit_code == 0);
}

TEST_CASE("a leak report is one message, and shows a Class made of one alike another",
          "[programs]") {
    const std::vector<fb::library_file_t> library{
        {
            .path = "lib/a.fb",
            .text = "Public Interface Held\n"
                    "End Interface\n"
                    "Public Class Node Implements Held\n"
                    "    Public Field next: Held?\n"
                    "End Class\n"
                    "Public Class Box<T> Implements Held\n"
                    "    Public Field next: Held?\n"
                    "End Class\n"
                    "Public Class Pair<T> Implements Held\n"
                    "    Public Field next: Held?\n"
                    "End Class\n"
                    "Public Function LeakNode()\n"
                    "    Local node = Node()\n"
                    "    node.next = node\n"
                    "End Function\n"
                    "Public Function MakeBox(next: Held?): Held\n"
                    "    Local box = Box<Int>()\n"
                    "    box.next = next\n"
                    "    Return box\n"
                    "End Function\n",
        },
    };

    // Each cycle has one of the two Nodes, and shows it as the counts do.
    const std::expected<fb::process_result_t, std::string> cycles =
        run_with_library("module_cycles",
                         "Import \"./lib/a\"\n"
                         "Class Node Implements Held\n"
                         "    Field next: Held?\n"
                         "End Class\n"
                         "Local mine = Node()\n"
                         "mine.next = mine\n"
                         "LeakNode()",
                         library);
    INFO((cycles ? cycles->output : cycles.error()));
    REQUIRE(cycles);
    CHECK(cycles->output == "leak: 1 lib.a.Node object was never released\n"
                            "leak: 1 main.Node object was never released\n"
                            "leak: a cycle of 1 object: main.Node.next -> the first main.Node\n"
                            "leak: a cycle of 1 object: lib.a.Node.next -> the first lib.a.Node\n");
    CHECK(cycles->exit_code == 0);

    // lib.a's Pair of the main module's Box is made of a Box that isn't lib.a's Box<Int32>.
    const std::expected<fb::process_result_t, std::string> parts =
        run_with_library("module_parts",
                         "Import \"./lib/a\"\n"
                         "Class Box Implements Held\n"
                         "    Field next: Held?\n"
                         "End Class\n"
                         "Local mine = Box()\n"
                         "Local pair = Pair<Box>()\n"
                         "pair.next = MakeBox(mine)\n"
                         "mine.next = pair",
                         library);
    INFO((parts ? parts->output : parts.error()));
    REQUIRE(parts);
    CHECK(parts->output == "leak: 1 main.Box object was never released\n"
                           "leak: 1 lib.a.Pair<main.Box> object was never released\n"
                           "leak: 1 lib.a.Box<Int32> object was never released\n"
                           "leak: a cycle of 3 objects: main.Box.next -> lib.a.Pair<main.Box>.next "
                           "-> lib.a.Box<Int32>.next -> the first main.Box\n");
    CHECK(parts->exit_code == 0);
}

TEST_CASE("a request nothing waits for doesn't keep a stuck coroutine's program from ending",
          "[programs]") {
    // Inner waits on a Scheduler nothing ticks, so Outer never goes on, and the line InputAsync
    // asks for is nothing either waits for. Stdin stays open, as a console's does, so the
    // program ends only if it doesn't wait for the line.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Global paused = Scheduler()\n"
                    "Async Function Inner()\n"
                    "    Await NextFrame()\n"
                    "End Function\n"
                    "Async Function Outer(t: Task)\n"
                    "    Await t\n"
                    "End Function\n"
                    "Local inner = Inner()\n"
                    "paused.Run(inner)\n"
                    "Outer(inner)\n"
                    "Local pending = InputAsync()\n"
                    "Print(\"top done\")\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "stuck_open_input";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto [result, took] = run_with_open_stdin(program, std::chrono::seconds(20));
    REQUIRE(result);
    CHECK(result->output == "top done\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(10));
}

TEST_CASE("QueryParams' Sort is a merge sort, which a query of many pairs doesn't slow",
          "[programs]") {
    // An insertion sort of 20000 pairs in reverse order, comparing two names' UTF-16 units each
    // time, takes minutes; a merge sort of the names' order, worked out once, well under a
    // second.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Import std.net\n"
                    "Local params = QueryParams()\n"
                    "For i = 0 Until 20000\n"
                    "    params.Add(\"k{39999 - i}\", \"v\")\n"
                    "End For\n"
                    "params.Sort()\n"
                    "For pair In params\n"
                    "    Print(params.Count, pair.0)\n"
                    "    Break\n"
                    "End For\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "query_sort";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "20000 k20000\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(10));
}

TEST_CASE("an Event's Clear takes out all its handlers at once", "[programs]") {
    // Taking each of 100000 handlers out on its own goes over the rest each time, for minutes;
    // taking them all out at the end, as an Emit does, takes well under a second.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Import std.events\n"
                    "Const event = Event<Int>()\n"
                    "For i = 1 To 100000\n"
                    "    event.Subscribe(Function(n: Int) => Print(n))\n"
                    "End For\n"
                    "event.Clear()\n"
                    "Print(event.Count)\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "event_clear";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "0\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(10));
}

TEST_CASE("a ZipWriter takes time in proportion to the zip's size", "[programs]") {
    // A zip of 128 MiB, stored, grown 64 KiB at a time, copies what it has each time, for about
    // 20 seconds; grown as a vector does, it takes under one.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Import std.collections\n"
                    "Import std.compress\n"
                    "Local data: UInt8[] = []\n"
                    "data.Resize(128 * 1024 * 1024, 7)\n"
                    "Local writer = ZipWriter()\n"
                    "writer.Add(\"big.bin\", data, ZipMethod.STORE)\n"
                    "Print(writer.Finish().Count > data.Count)\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "zip_large";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "True\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(8));
}

TEST_CASE("a 7z's links in one solid folder take one decoding of it", "[programs]") {
    // compress_archives' links.7z: a folder of 64 MiB of zeros and the targets of 40 links to
    // them. Decoding the folder for each link took about 50 seconds in a debug build; once for
    // them all, about one.
    const std::filesystem::path programs = FB_TEST_PROGRAMS;
    std::vector<fb::library_file_t> library =
        fb::standard_library().value_or(std::vector<fb::library_file_t>{});
    library.push_back({
        .path = "compress_archives/data.fb",
        .text = read_file(programs / "compress_archives" / "data.fb"),
    });
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Import std.compress\n"
                    "Import \"./compress_archives/data\"\n"
                    "Local reader = Try SevenZip.Read(Links7z())\n"
                    "Print(reader.Count, reader.Entries[40].Target)\n",
                    std::move(library));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "seven_zip_links";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "41 zeros.bin\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(15));
}
