#include "fb/driver.hpp"
#include "fb/interface.hpp"
#include "fb/lower.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
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
#include <fcntl.h>
#include <unistd.h>
#endif

// End-to-end tests: every tests/programs/<name>.fb is compiled, linked, and run, and its output
// must match <name>.out. The modules it imports by path are in tests/programs/<name>/. A name
// starting with "panic_" must exit with a panic (code 101), one starting with "uncaught_" must
// let an error out of its top-level code (code 1), and one starting with "release_" is built in
// release mode. One starting with "http_", after any "release_", runs under tests/native's HTTP
// test server, whose URLs and test CA are the first lines of its input. A panic's or uncaught
// error's stack trace is left out of what's compared, but where the rest of the name, after
// those, starts with "trace", as "panic_trace_calls" does.

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

/// `output` without the stack trace after its panic or uncaught error, the lines that start
/// "    at ", which only the programs whose ending names "trace" keep, as "panic_trace_calls"
/// does: the others' traces would change with every line std gains. A panic's or error's line
/// can start with what the program wrote without a line break, as a terminal's escapes.
std::string without_trace(const std::string& output) {
    std::string kept;
    bool reported = false;
    for (std::size_t at = 0; at < output.size();) {
        const std::size_t end = output.find('\n', at);
        const std::size_t next = end == std::string::npos ? output.size() : end + 1;
        const std::string_view line = std::string_view(output).substr(at, next - at);
        if (line.contains("panic: ") || line.contains("uncaught ")) {
            reported = true;
        } else if (reported && line.starts_with("    at ")) {
            at = next;
            continue;
        }

        kept += line;
        at = next;
    }

    return kept;
}

/// Whether a program's or package's name keeps its trace: one whose ending, after "release_"
/// and "panic_" or "uncaught_", starts with "trace".
bool keeps_trace(std::string_view name) {
    for (const std::string_view prefix : { "release_", "panic_", "uncaught_" }) {
        if (name.starts_with(prefix)) {
            name.remove_prefix(prefix.size());
        }
    }

    return name.starts_with("trace");
}

/// The standard library, and the files in the folder named like the program, by their paths
/// from the program's folder, like "modules/shapes.fb": its modules, the .proto files it
/// imports, and what it embeds.
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
        if (entry.is_regular_file()) {
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
    const std::string name = source.stem().string();
    const std::string_view release = "release_";
    const bool http = std::string_view(name)
                          .substr(name.starts_with(release) ? release.size() : 0)
                          .starts_with("http_");
    return fb::build_program(module, compilation, program, mode).and_then([&] {
        if (http) {
            const std::array arguments{ program.string() };
            return fb::run_process(FB_TEST_HTTP_SERVER, arguments, input.value_or(""));
        }

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

/// The tests/programs that this run checks, in order: every one, or with FB_PROGRAM_SHARD set to
/// "<i>/<n>", every n-th from the i-th, a shard of n that ctest runs beside the others.
std::vector<std::filesystem::path> shard_of_programs() {
    std::vector<std::filesystem::path> sources;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(FB_TEST_PROGRAMS)) {
        if (entry.path().extension() == ".fb") {
            sources.push_back(entry.path());
        }
    }

    std::ranges::sort(sources);
    std::size_t shard = 0;
    std::size_t shards = 1;
    if (const char* setting = std::getenv("FB_PROGRAM_SHARD")) {
        const std::string_view text(setting);
        const std::size_t slash = text.find('/');
        REQUIRE(slash != std::string_view::npos);
        shard = std::stoul(std::string(text.substr(0, slash)));
        shards = std::stoul(std::string(text.substr(slash + 1)));
        REQUIRE(shard < shards);
    }

    std::vector<std::filesystem::path> chosen;
    for (std::size_t i = shard; i < sources.size(); i += shards) {
        chosen.push_back(sources[i]);
    }

    return chosen;
}

/// A copy of tests/packages in the build folder, so building writes nothing into the source
/// tree, with the vecmath C library in libs/ for each package whose project.toml links it, and
/// the stand-in engine's DLL in native/<target>/ for each whose project.toml loads it.
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

        if (read_file(entry.path() / "project.toml").contains("\"native/{target}/engine\"")) {
            const std::filesystem::path native =
                entry.path() / "native" / fb::path_from_utf8(fb::build_target());
            std::filesystem::create_directories(native);
            // On Windows its import library goes beside it.
            const std::filesystem::path library(FB_TEST_ENGINE);
            const std::array<std::filesystem::path, 2> files{ library, FB_TEST_ENGINE_LINKED };
            for (const std::filesystem::path& file : files) {
                std::filesystem::copy_file(file, native / file.filename(),
                                           std::filesystem::copy_options::skip_existing);
            }
        }
    }

    // A library that a project.toml names as "../<name>-prebuilt" is built, in debug, into
    // <name>-prebuilt, with its project.toml, as a prebuilt package ships.
    std::string manifests;
    std::vector<std::filesystem::path> folders;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(packages)) {
        manifests += read_file(entry.path() / "project.toml");
        folders.push_back(entry.path());
    }

    for (const std::filesystem::path& folder : folders) {
        const std::string name = fb::to_utf8(folder.filename());
        if (!manifests.contains(std::format("\"../{}-prebuilt\"", name))) {
            continue;
        }

        const std::expected<fb::compilation_t, std::string> compiled = fb::compile_file(folder);
        REQUIRE(compiled);
        fb::diagnostic_bag_t diagnostics;
        const std::expected<fb::library_build_t, std::string> built =
            fb::build_library(*compiled, fb::build_mode_t::DEBUG, diagnostics);
        INFO((built ? std::string() : built.error()));
        REQUIRE(built);
        const std::filesystem::path prebuilt = packages / std::format("{}-prebuilt", name);
        const std::filesystem::path lib =
            prebuilt / "lib" / fb::path_from_utf8(fb::build_target()) / "debug";
        std::filesystem::create_directories(lib);
        std::filesystem::copy_file(folder / "project.toml", prebuilt / "project.toml");
        std::ofstream(lib / (name + ".fbi"), std::ios::binary) << built->interface;
        std::ofstream(lib / (name + ".fblib"), std::ios::binary) << built->binary;
    }

    return packages;
}

} // namespace

// Hidden, as ctest runs it in shards that tests/CMakeLists.txt registers, each a ctest test.
TEST_CASE("programs print what they should", "[programs][.]") {
    for (const std::filesystem::path& source : shard_of_programs()) {
        const std::string name = source.stem().string();
        DYNAMIC_SECTION(name) {
            // A release_ program builds in release mode, and the rest of its name says how it
            // ends, as "release_panic_..." does.
            const std::string_view release_prefix = "release_";
            const bool release = name.starts_with(release_prefix);
            const fb::build_mode_t mode =
                release ? fb::build_mode_t::RELEASE : fb::build_mode_t::DEBUG;
            const std::expected<fb::process_result_t, std::string> result =
                build_and_run(source, mode);
            INFO((result ? result->output : result.error()));
            REQUIRE(result);

            std::filesystem::path expected = source;
            expected.replace_extension(".out");
            CHECK((keeps_trace(name) ? result->output : without_trace(result->output)) ==
                  read_file(expected));
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
            const std::string output = from_folder(result->output, packages);
            CHECK((keeps_trace(name) ? output : without_trace(output)) ==
                  read_file(packages / (name + ".out")));
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
    CHECK(result->output == "panic: -2 isn't positive at lib/check.fb:2:5\n"
                            "    at lib.check.Check (lib/check.fb:2)\n"
                            "    at top-level code (main.fb:3)\n");
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
    CHECK(cat->output == "panic: expected lib.a.Cat, found lib.b.Cat at lib/a.fb:7:42\n"
                         "    at lib.a.AsCat (lib/a.fb:7)\n"
                         "    at top-level code (main.fb:3)\n");
    CHECK(cat->exit_code == 101);

    // An Interface and a Class whose names are the same.
    const std::expected<fb::process_result_t, std::string> pet =
        run_with_library("module_pet", imports + "Local p = AsPet(BPet())", library);
    INFO((pet ? pet->output : pet.error()));
    REQUIRE(pet);
    CHECK(pet->output == "panic: expected lib.a.Pet, found lib.b.Pet at lib/a.fb:8:42\n"
                         "    at lib.a.AsPet (lib/a.fb:8)\n"
                         "    at top-level code (main.fb:3)\n");
    CHECK(pet->exit_code == 101);

    // Names that differ stay as they are.
    const std::expected<fb::process_result_t, std::string> other =
        run_with_library("module_other", imports + "Local c = AsCat(BPet())", library);
    INFO((other ? other->output : other.error()));
    REQUIRE(other);
    CHECK(other->output == "panic: expected Cat, found Pet at lib/a.fb:7:42\n"
                           "    at lib.a.AsCat (lib/a.fb:7)\n"
                           "    at top-level code (main.fb:3)\n");
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
    CHECK(panic->output == "panic: expected main.Box, found lib.a.Box<Int32> at main.fb:6:11\n"
                           "    at top-level code (main.fb:6)\n");
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

TEST_CASE("Process.Exit ends the program at once with its code", "[programs]") {
    // What the program printed shows, and nothing runs after: not the Defer, the Destroy, the
    // code after the call, or the leak report the object it never let go of would get.
    const std::expected<fb::process_result_t, std::string> result =
        run_with_library("process_exit",
                         "Import std.core\n"
                         "Import std.os\n"
                         "Class Noisy\n"
                         "    Method Destroy()\n"
                         "        Print(\"destroyed\")\n"
                         "    End Method\n"
                         "End Class\n"
                         "Function Leave()\n"
                         "    Defer Print(\"deferred\")\n"
                         "    Local kept = Noisy()\n"
                         "    Print(\"leaving\", Process.Id > 0)\n"
                         "    Process.Exit(3)\n"
                         "End Function\n"
                         "Write(\"before, \")\n"
                         "Leave()\n"
                         "Print(\"after\")\n",
                         {});
    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output == "before, leaving True\n");
    CHECK(result->exit_code == 3);
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

TEST_CASE("TOML's array of tables reads in time in proportion to its length", "[programs]") {
    // Each key under an [[array]] went into its last table through a copy of the whole array, so
    // 20000 tables of four keys took about 6 seconds in release; going into it in place, they
    // take well under one.
    const fb::compilation_t compilation = fb::compile(
        "main.fb",
        "Import std.core\n"
        "Import std.collections\n"
        "Import std.data.toml\n"
        "Local parts: String[] = []\n"
        "For i = 1 To 20000\n"
        "    parts.Add(\"[[package]]\\nname = \\\"p{i}\\\"\\nversion = \\\"1.0.{i}\\\"\\n\")\n"
        "    parts.Add(\"source = \\\"registry\\\"\\nchecksum = \\\"abc\\\"\\n\")\n"
        "End For\n"
        "Local read = Try Toml.Read(parts.Join(\"\"))\n"
        "Print((read[\"package\"] ?? Value()).Count)\n",
        fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "toml_array_tables";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "20000\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(10));
}

TEST_CASE("a Channel's change doesn't wake every idle worker of the pool", "[programs]") {
    // Each Send and Receive woke every idle worker, which all took the pool's lock, so 320,000 of
    // them took about 11 seconds in release with the workers idle; waking only the threads
    // waiting on Channels, and a worker for each job, they take well under one.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Spawn(Function() => 1).Wait()\n"
                    "Const ch = Channel<Int>(2000)\n"
                    "Local total = 0\n"
                    "For round = 1 To 100\n"
                    "    For i = 1 To 1600\n"
                    "        ch.TrySend(i)\n"
                    "    End For\n"
                    "    Local value = 0\n"
                    "    While ch.TryReceive(ByRef value)\n"
                    "        total += 1\n"
                    "    End While\n"
                    "End For\n"
                    "Print(total)\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "channel_idle_pool";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto start = std::chrono::steady_clock::now();
    const std::expected<fb::process_result_t, std::string> result = fb::run_process(program, {});
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
    REQUIRE(result);
    CHECK(result->output == "160000\n");
    CHECK(result->exit_code == 0);
    CHECK(took < std::chrono::seconds(5));
}

TEST_CASE("the HTTP test server's program has no proxy of the environment's", "[programs]") {
    // As on a machine behind a proxy, which a client with no Proxy of its own would send the
    // test server's URLs to; this one refuses them.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Import std.net.http\n"
                    "Local base = Input() ?? \"\"\n"
                    "Print((Try? HttpClient().Get(\"{base}/hello\"))?.Text ?? \"failed\")\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "http_no_proxy";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const auto set = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    };
    const auto unset = [](const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    };
    for (const char* name : { "http_proxy", "HTTPS_PROXY", "all_proxy" }) {
        set(name, "http://127.0.0.1:9");
    }

    const std::array arguments{ program.string() };
    const std::expected<fb::process_result_t, std::string> result =
        fb::run_process(FB_TEST_HTTP_SERVER, arguments, "");
    for (const char* name : { "http_proxy", "HTTPS_PROXY", "all_proxy" }) {
        unset(name);
    }

    INFO((result ? result->output : result.error()));
    REQUIRE(result);
    CHECK(result->output == "hello\n");
    CHECK(result->exit_code == 0);
}

TEST_CASE("the HTTP test servers of tests that run at once share a trace, each line its server's",
          "[programs]") {
    // A program that waits 300 ms, then makes three requests, each through a client of its own.
    const fb::compilation_t compilation =
        fb::compile("main.fb",
                    "Import std.core\n"
                    "Import std.net.http\n"
                    "Import std.time\n"
                    "Local base = Input() ?? \"\"\n"
                    "Sleep(300.Milliseconds)\n"
                    "For i = 1 To 3\n"
                    "    Print((Try? HttpClient().Get(\"{base}/hello\"))?.Text ?? \"failed\")\n"
                    "End For\n",
                    fb::standard_library().value_or(std::vector<fb::library_file_t>{}));
    REQUIRE_FALSE(compilation.diagnostics.has_errors());
    std::filesystem::path program = std::filesystem::path(FB_TEST_OUTPUT) / "http_traced";
    program += fb::executable_extension;
    const fb::mir_module_t module = fb::lower(compilation.ast, compilation.analysis);
    REQUIRE(fb::build_program(module, compilation, program, fb::build_mode_t::DEBUG));

    const std::filesystem::path log = std::filesystem::path(FB_TEST_OUTPUT) / "http_traced.log";
    std::filesystem::remove(log);
#ifdef _WIN32
    _putenv_s("FB_HTTP_TEST_LOG", log.string().c_str());
#else
    setenv("FB_HTTP_TEST_LOG", log.string().c_str(), 1);
#endif
    const std::array arguments{ program.string() };
    std::array<std::expected<fb::process_result_t, std::string>, 2> results{
        std::unexpected(std::string("didn't run")),
        std::unexpected(std::string("didn't run")),
    };
    {
        // std::thread, not jthread, which Apple's libc++ doesn't have.
        std::thread first(
            [&] { results[0] = fb::run_process(FB_TEST_HTTP_SERVER, arguments, ""); });
        std::thread second(
            [&] { results[1] = fb::run_process(FB_TEST_HTTP_SERVER, arguments, ""); });
        first.join();
        second.join();
    }
#ifdef _WIN32
    _putenv_s("FB_HTTP_TEST_LOG", "");
#else
    unsetenv("FB_HTTP_TEST_LOG");
#endif

    for (const auto& result : results) {
        INFO((result ? result->output : result.error()));
        REQUIRE(result);
        CHECK(result->output == "hello\nhello\nhello\n");
    }

    // Each server's lines are all there, by its process: its accepts, their threads, and their
    // requests, the first of them after the program's wait, as the times count from the start.
    struct seen_t {
        int accepts = 0;
        int serves = 0;
        int requests = 0;
        long long first = -1;
    };
    std::map<std::string, seen_t> servers;
    std::ifstream file(log);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        long long since = 0;
        std::string process;
        std::string thread;
        std::string what;
        fields >> since >> process >> thread >> what;
        INFO(line);
        REQUIRE_FALSE(fields.fail());
        seen_t& seen = servers[process];
        if (seen.first < 0) {
            seen.first = since;
        }

        seen.accepts += what == "accept" ? 1 : 0;
        seen.serves += what == "serve" ? 1 : 0;
        seen.requests += what == "request" ? 1 : 0;
    }

    REQUIRE(servers.size() == 2);
    for (const auto& [process, seen] : servers) {
        INFO(process);
        CHECK(seen.accepts == 3);
        CHECK(seen.serves == 3);
        CHECK(seen.requests == 3);
        CHECK(seen.first >= 300);
    }
}
