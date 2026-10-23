#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
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
#include <csignal>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

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

/// A little-endian number at `at` in `bytes`, or 0 past their end.
template <typename Number>
Number number_at(const std::string& bytes, const std::uint64_t at) {
    if (at > bytes.size() || bytes.size() - at < sizeof(Number)) {
        return 0;
    }

    Number value = 0;
    for (std::size_t i = sizeof(Number); i-- > 0;) {
        value = static_cast<Number>(value << 8U) | static_cast<unsigned char>(bytes[at + i]);
    }

    return value;
}

/// The NUL-ended text at `at` in `bytes`.
std::string text_at(const std::string& bytes, const std::uint64_t at) {
    if (at >= bytes.size()) {
        return {};
    }

    return bytes.substr(at, bytes.find('\0', at) - at);
}

/// The DLLs a PE file imports, delay-loaded ones too.
std::vector<std::string> pe_imports(const std::string& bytes) {
    std::vector<std::string> names;
    const auto pe = number_at<std::uint32_t>(bytes, 0x3C);
    if (!bytes.starts_with("MZ") || bytes.compare(pe, 4, std::string("PE\0\0", 4)) != 0) {
        return names;
    }

    const auto sections = number_at<std::uint16_t>(bytes, pe + 6);
    const std::uint64_t optional = pe + 24;
    const std::uint64_t table = optional + number_at<std::uint16_t>(bytes, pe + 20);
    // PE32+ has 64-bit addresses before the data directories, PE32 32-bit ones.
    const bool wide = number_at<std::uint16_t>(bytes, optional) == 0x20B;
    const std::uint64_t directories = optional + (wide ? 112 : 96);
    const auto offset_of = [&](const std::uint32_t address) -> std::uint64_t {
        for (std::uint64_t i = 0; i < sections; ++i) {
            const std::uint64_t section = table + (i * 40);
            const auto start = number_at<std::uint32_t>(bytes, section + 12);
            const auto size = std::max(number_at<std::uint32_t>(bytes, section + 8),
                                       number_at<std::uint32_t>(bytes, section + 16));
            if (address >= start && address - start < size) {
                return address - start + number_at<std::uint32_t>(bytes, section + 20);
            }
        }

        return bytes.size();
    };

    // The import directory's entries are 20 bytes, each naming its DLL 12 bytes in, and the
    // delay-load directory's 32, naming it 4 bytes in. A 0 name ends each.
    for (const auto& [directory, size, name_at] :
         { std::tuple(1U, 20U, 12U), std::tuple(13U, 32U, 4U) }) {
        const auto address = number_at<std::uint32_t>(bytes, directories + (directory * 8ULL));
        if (address == 0) {
            continue;
        }

        for (std::uint64_t entry = offset_of(address);; entry += size) {
            const auto name = number_at<std::uint32_t>(bytes, entry + name_at);
            if (name == 0) {
                break;
            }

            names.push_back(text_at(bytes, offset_of(name)));
        }
    }

    return names;
}

/// The libraries a 64-bit, little-endian ELF file needs: its DT_NEEDED entries.
std::vector<std::string> elf_needed(const std::string& bytes) {
    std::vector<std::string> names;
    if (!bytes.starts_with("\x7F"
                           "ELF") ||
        bytes.size() < 6 || bytes[4] != 2 || bytes[5] != 1) {
        return names;
    }

    // What each loaded segment's addresses are in the file, and where the dynamic section is.
    struct segment_t {
        std::uint64_t address;
        std::uint64_t offset;
        std::uint64_t size;
    };
    std::vector<segment_t> loaded;
    std::uint64_t dynamic = 0;
    std::uint64_t dynamic_size = 0;
    const auto headers = number_at<std::uint64_t>(bytes, 0x20);
    const auto header_size = number_at<std::uint16_t>(bytes, 0x36);
    for (std::uint64_t i = 0; i < number_at<std::uint16_t>(bytes, 0x38); ++i) {
        const std::uint64_t header = headers + (i * header_size);
        const auto type = number_at<std::uint32_t>(bytes, header);
        if (type == 1) {
            loaded.push_back({
                .address = number_at<std::uint64_t>(bytes, header + 16),
                .offset = number_at<std::uint64_t>(bytes, header + 8),
                .size = number_at<std::uint64_t>(bytes, header + 32),
            });
        } else if (type == 2) {
            dynamic = number_at<std::uint64_t>(bytes, header + 8);
            dynamic_size = number_at<std::uint64_t>(bytes, header + 32);
        }
    }

    // DT_NEEDED (1) is a name's place in the string table, which DT_STRTAB (5) says the
    // address of; DT_NULL (0) ends them.
    std::vector<std::uint64_t> needed;
    std::uint64_t strings = 0;
    for (std::uint64_t entry = dynamic; entry + 16 <= dynamic + dynamic_size; entry += 16) {
        const auto tag = number_at<std::uint64_t>(bytes, entry);
        const auto value = number_at<std::uint64_t>(bytes, entry + 8);
        if (tag == 0) {
            break;
        }

        if (tag == 1) {
            needed.push_back(value);
        } else if (tag == 5) {
            strings = value;
        }
    }

    for (const segment_t& segment : loaded) {
        if (strings >= segment.address && strings - segment.address < segment.size) {
            for (const std::uint64_t name : needed) {
                names.push_back(text_at(bytes, strings - segment.address + segment.offset + name));
            }
        }
    }

    return names;
}

/// The libraries a 64-bit Mach-O file loads: its LC_LOAD_DYLIB commands, and the weak,
/// re-exported, lazy, and upward ones.
std::vector<std::string> mach_o_dylibs(const std::string& bytes) {
    std::vector<std::string> names;
    if (number_at<std::uint32_t>(bytes, 0) != 0xFEEDFACF) {
        return names;
    }

    std::uint64_t command = 32;
    for (std::uint32_t i = 0; i < number_at<std::uint32_t>(bytes, 16); ++i) {
        const auto kind = number_at<std::uint32_t>(bytes, command);
        const auto size = number_at<std::uint32_t>(bytes, command + 4);
        if (size == 0) {
            break;
        }

        if (kind == 0xC || kind == 0x80000018 || kind == 0x8000001F || kind == 0x20 ||
            kind == 0x80000023) {
            names.push_back(text_at(bytes, command + number_at<std::uint32_t>(bytes, command + 8)));
        }

        command += size;
    }

    return names;
}

/// The shared libraries an executable has the system load with it.
std::vector<std::string> loaded_libraries(const std::filesystem::path& executable) {
    std::ifstream file(executable, std::ios::binary);
    const std::string bytes{ std::istreambuf_iterator<char>(file), {} };
    std::vector<std::string> names = pe_imports(bytes);
    std::ranges::move(elf_needed(bytes), std::back_inserter(names));
    std::ranges::move(mach_o_dylibs(bytes), std::back_inserter(names));
    return names;
}

/// Whether a library is the system's own, which every machine the program runs on has.
bool is_system_library(std::string name) {
#ifdef _WIN32
    std::ranges::transform(name, name.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    // The Universal C Runtime is part of Windows since Windows 10, and its api-ms-win-crt-*
    // contracts with it.
    constexpr auto system = std::to_array<std::string_view>({
        "kernel32.dll",
        "ntdll.dll",
        "advapi32.dll",
        "ws2_32.dll",
        "mswsock.dll",
        "iphlpapi.dll",
        "userenv.dll",
        "shell32.dll",
        "user32.dll",
        "gdi32.dll",
        "ole32.dll",
        "oleaut32.dll",
        "dbghelp.dll",
        "bcrypt.dll",
        "crypt32.dll",
        "secur32.dll",
        "ucrtbase.dll",
        "msvcrt.dll",
    });
    return name.starts_with("api-ms-win-") || std::ranges::contains(system, name);
#elifdef __APPLE__
    // Nothing but Apple's is in /usr/lib, which the system keeps to itself.
    return name.starts_with("/usr/lib/") || name.starts_with("/System/Library/");
#else
    // glibc's, which every Linux the program runs on has.
    constexpr auto system = std::to_array<std::string_view>({
        "libc.so.6",
        "libm.so.6",
        "libpthread.so.0",
        "libdl.so.2",
        "librt.so.1",
        "libutil.so.1",
        "libresolv.so.2",
        "ld-linux-x86-64.so.2",
        "ld-linux-aarch64.so.1",
    });
    return std::ranges::contains(system, name);
#endif
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
    CHECK(result.output.contains("Usage: fb [--color <when>] <command> [options]"));
    CHECK(result.output.contains("--show <stages>"));

    CHECK(fb_run({ "help", "build" }).output.contains("Usage: fb build"));
    CHECK(result.output.contains(
        "bindgen              Turn a C header into a FutureBlitz module, with fb-bindgen"));
    CHECK(
        fb_run({ "bindgen", "--help" }).output.contains("Usage: fb bindgen <header.h> [options]"));
    const std::string run_help = fb_run({ "run", "--help" }).output;
    CHECK(run_help.contains("Usage: fb run"));
    CHECK_FALSE(run_help.contains("--output"));
}

TEST_CASE("bad usage exits with code 2", "[cli]") {
    check_usage_error({ "frobnicate" }, "error: unknown command 'frobnicate'");
    check_usage_error({ "check", "a.fb", "b.fb" }, "unexpected argument 'b.fb'");
    check_usage_error({ "check", "a.fb", "--nope" }, "unknown option '--nope'");
    check_usage_error({ "check", "a.fb", "-o", "a.exe" }, "'-o' only works with 'fb build'");
    check_usage_error({ "check", "a.fb", "--show", "bytes" }, "unknown stage 'bytes'");
    check_usage_error({ "check", "a.fb", "--show" }, "'--show' needs a value");
    check_usage_error({ "check", "a.fb", "--show=" }, "'--show' needs stages");
    check_usage_error({ "help", "check", "extra" }, "unexpected argument 'extra'");
    check_usage_error({ "--version", "extra" }, "unexpected argument 'extra'");
    check_usage_error({ "build", "a.fb", "-o", "" }, "'-o' takes the executable's file name");
    check_usage_error({ "build", "a.fb", "-o", "out/" }, "'-o' takes the executable's file name");
    check_usage_error({ "build", "a.fb", "--", "x" }, "only 'fb run' runs");
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
    const std::filesystem::path source =
        write_source("next_to.fb", "Import std.core\nPrint(\"here\")\n");
    std::filesystem::path executable = source;
    executable.replace_extension(fb::executable_extension);
    std::filesystem::remove(executable);

    const fb::process_result_t result = fb_run({ "build", fb::to_utf8(source), "--release" });
    INFO(result.output);
    CHECK(result.exit_code == 0);
    CHECK(result.output.contains("    Finished release build in "));
    CHECK(std::filesystem::exists(executable));
}

TEST_CASE("fb build names each bundled C library a program links, with its notice", "[cli]") {
    // A program that doesn't read input links none of libuv.
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "bundled";
    const fb::process_result_t hello =
        fb_run({ "build", program("hello.fb"), "-o", fb::to_utf8(folder / "hello") });
    INFO(hello.output);
    REQUIRE(hello.exit_code == 0);
    CHECK_FALSE(hello.output.contains("Bundled"));

    const std::filesystem::path source =
        write_source("bundled/reads.fb", "Import std.core\nPrint(\"[{Input() ?? \"none\"}]\")\n");
    std::filesystem::path executable = source;
    executable.replace_extension(fb::executable_extension);
    const fb::process_result_t reads = fb_run({ "build", fb::to_utf8(source) });
    INFO(reads.output);
    REQUIRE(reads.exit_code == 0);
    const fb::build_config::bundled_library_t& libuv = fb::build_config::bundled_libraries[0];
    CHECK(reads.output.contains(std::format("     Bundled libuv {}, under the MIT, BSD-2-Clause, "
                                            "and ISC licenses; ship its notices with the "
                                            "program: {}\n",
                                            libuv.version, libuv.notice)));
    // Its notices are libuv's own, and those of the code it has from others.
    std::ifstream file(fb::path_from_utf8(libuv.notice), std::ios::binary);
    const std::string notices{ std::istreambuf_iterator<char>(file), {} };
    CHECK(notices.contains("libuv project contributors"));
    CHECK(notices.contains("Joyent, Inc."));
    CHECK(notices.contains("Niels Provos"));
    CHECK(notices.contains("Internet Systems Consortium"));

    const std::expected<fb::process_result_t, std::string> ran =
        fb::run_process(executable, {}, "typed\n");
    REQUIRE(ran);
    CHECK(ran->output == "[typed]\n");

    // SQLite's notice is its dedication to the public domain, and its blessing.
    const std::filesystem::path stores = write_source(
        "bundled/stores.fb", "Import std.db\nLocal db = Try? Database.Open(\":memory:\")\n");
    const fb::process_result_t stored = fb_run({ "build", fb::to_utf8(stores) });
    INFO(stored.output);
    REQUIRE(stored.exit_code == 0);
    const auto sqlite = std::ranges::find(fb::build_config::bundled_libraries, "SQLite",
                                          &fb::build_config::bundled_library_t::name);
    REQUIRE(sqlite != fb::build_config::bundled_libraries.end());
    CHECK(stored.output.contains(std::format("     Bundled SQLite {}, under a public-domain "
                                             "dedication; ship its notices with the program: "
                                             "{}\n",
                                             sqlite->version, sqlite->notice)));
    std::ifstream dedication(fb::path_from_utf8(sqlite->notice), std::ios::binary);
    const std::string blessing{ std::istreambuf_iterator<char>(dedication), {} };
    CHECK(blessing.contains("The author disclaims copyright to this source code."));
    CHECK(blessing.contains("May you share freely, never taking more than you give."));
    CHECK_FALSE(blessing.contains("This header file defines the interface"));
}

TEST_CASE("std.text links utf8proc only where a program needs Unicode's data", "[cli]") {
    // Byte positions, whitespace, StringBuilder, and the encodings need none of it.
    const std::filesystem::path plain =
        write_source("bundled/plain_text.fb",
                     "Import std.core\nImport std.text\nImport std.text.encoding\n"
                     "Local words = \" é 😀 \".Trim().Split(\" \")\n"
                     "Print(StringBuilder().Append(words[0]), Percent.Encode(words[1]), "
                     "Base64.Encode(words[1].Bytes), words[0].IsBlank(), 'é'.IsWhitespace())\n");
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(plain) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    CHECK_FALSE(built.output.contains("Bundled"));

    // Case does.
    const std::filesystem::path cased = write_source(
        "bundled/cased_text.fb", "Import std.core\nImport std.text\nPrint(\"straße\".ToUpper())\n");
    const fb::process_result_t linked = fb_run({ "build", fb::to_utf8(cased) });
    INFO(linked.output);
    REQUIRE(linked.exit_code == 0);
    const auto utf8proc = std::ranges::find(fb::build_config::bundled_libraries, "utf8proc",
                                            &fb::build_config::bundled_library_t::name);
    REQUIRE(utf8proc != fb::build_config::bundled_libraries.end());
    CHECK(linked.output.contains(std::format("     Bundled utf8proc {}, under the MIT license and "
                                             "the Unicode license; ship its notices with the "
                                             "program: {}\n",
                                             utf8proc->version, utf8proc->notice)));
    std::ifstream file(fb::path_from_utf8(utf8proc->notice), std::ios::binary);
    const std::string notices{ std::istreambuf_iterator<char>(file), {} };
    CHECK(notices.contains("Steven G. Johnson"));
    CHECK(notices.contains("Unicode data license"));

    std::filesystem::path executable = cased;
    executable.replace_extension(fb::executable_extension);
    const std::expected<fb::process_result_t, std::string> ran =
        fb::run_process(executable, {}, "");
    REQUIRE(ran);
    CHECK(ran->output == "STRASSE\n");
}

TEST_CASE("a program fb builds loads only the system's own libraries", "[cli]") {
    // What std and the language bundle, like libuv for input, and SQLite and mbedTLS for a
    // database and a random Uuid, is linked in, so the program runs wherever it's copied, with
    // nothing beside it.
    const auto sources = std::to_array<std::pair<std::string_view, std::string_view>>({
        { "own_libraries/reads.fb", "Import std.core\nPrint(\"[{Input() ?? \"none\"}]\")\n" },
        {
            "own_libraries/database.fb",
            "Import std.core\nImport std.db\nImport std.uuid\nTry\n"
            "    Local db = Database.Open(\":memory:\")\n"
            "    Local text = db.Rows(\"SELECT ?\", Uuid.V4().ToString())[0].Text(0) ?? \"\"\n"
            "    Print(text.Bytes.Count)\n"
            "Catch e As DatabaseError\n    Print(e.Message)\nEnd Try\n",
        },
    });
    for (const auto& [name, text] : sources) {
        INFO(name);
        const std::filesystem::path source = write_source(name, text);
        std::filesystem::path executable = source;
        executable.replace_extension(fb::executable_extension);
        const fb::process_result_t built = fb_run({ "build", fb::to_utf8(source) });
        INFO(built.output);
        REQUIRE(built.exit_code == 0);

        // Every program loads the system's C library at least, so an empty list read nothing.
        const std::vector<std::string> libraries = loaded_libraries(executable);
        CHECK_FALSE(libraries.empty());
        for (const std::string& library : libraries) {
            INFO(library);
            CHECK(is_system_library(library));
        }

        const std::expected<fb::process_result_t, std::string> ran =
            fb::run_process(executable, {}, "");
        REQUIRE(ran);
        CHECK(ran->exit_code == 0);
        CHECK(ran->output == (name.ends_with("reads.fb") ? "[none]\n" : "36\n"));
    }
}

TEST_CASE("a program's database is a file it makes, opens again, and shares", "[cli]") {
    // Each run starts with only a file that isn't a database, which the programs find by its
    // whole path, with forward slashes, as SQLite takes them everywhere.
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "database";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "notes.db", std::ios::binary) << std::string(200, 'x');
    std::string at = fb::to_utf8(folder);
    std::ranges::replace(at, '\\', '/');
    // How long the last one ran.
    std::chrono::steady_clock::duration took{};
    const auto run = [&](const std::string_view name, const std::string_view text) {
        const std::filesystem::path source = write_source(
            std::format("database/{}.fb", name),
            std::format("Import std.core\nImport std.db\n\nConst folder = \"{}\"\n{}", at, text));
        std::filesystem::path executable = source;
        executable.replace_extension(fb::executable_extension);
        const fb::process_result_t built = fb_run({ "build", fb::to_utf8(source) });
        INFO(built.output);
        REQUIRE(built.exit_code == 0);
        const auto started = std::chrono::steady_clock::now();
        const std::expected<fb::process_result_t, std::string> ran =
            fb::run_process(executable, {});
        REQUIRE(ran);
        CHECK(ran->exit_code == 0);
        took = std::chrono::steady_clock::now() - started;
        return ran->output;
    };

    SECTION("files") {
        // A file is made, kept, and opened again only to read; one connection writing makes another
        // wait its 5 seconds, then throw BUSY; Close lets go of its file at once, though a
        // Statement of it is kept; and a file that isn't a database is CORRUPT.
        const std::string kinds = run("kinds", R"fb(
Function Report(e: DatabaseError)
    Print("{e.Kind}: {e.Message}")
End Function

Function Run() Throws DatabaseError
    Local made = Try Database.Open(folder + "/kinds.db")
    Try made.Execute("CREATE TABLE t (n INTEGER)")
    Try made.Execute("INSERT INTO t VALUES (?)", 1)
    made.Close()
    Local again = Try Database.Open("file:" + folder + "/kinds.db?mode=ro")
    Print((Try again.Rows("SELECT n FROM t"))[0].Integer(0) ?? -1)
    Try
        again.Execute("INSERT INTO t VALUES (2)")
    Catch e As DatabaseError
        Report(e)
    End Try

    Local first = Try Database.Open(folder + "/kinds.db")
    Local second = Try Database.Open(folder + "/kinds.db")
    Local transaction = Try first.Begin()
    Try first.Execute("INSERT INTO t VALUES (3)")
    Try
        second.Execute("INSERT INTO t VALUES (4)")
    Catch e As DatabaseError
        Report(e)
    End Try
    Try transaction.Commit()
    Print((Try second.Rows("SELECT count(*) FROM t"))[0].Integer(0) ?? -1)

    Local locked = Try Database.Open(folder + "/locked.db")
    Try locked.Execute("PRAGMA locking_mode = EXCLUSIVE; CREATE TABLE t (n INTEGER)")
    Local kept = Try locked.Prepare("SELECT count(*) FROM t")
    locked.Close()
    Local other = Try Database.Open(folder + "/locked.db")
    Try other.Execute("INSERT INTO t VALUES (1)")
    Print((Try other.Rows("SELECT count(*) FROM t"))[0].Integer(0) ?? -1, kept.Parameters)

    Try
        Local notes = Database.Open(folder + "/notes.db")
        notes.Rows("SELECT * FROM sqlite_schema")
    Catch e As DatabaseError
        Report(e)
    End Try
End Function

Try
    Run()
Catch e As DatabaseError
    Report(e)
End Try
)fb");
        CHECK(kinds == "1\n"
                       "OTHER: attempt to write a readonly database\n"
                       "BUSY: database is locked\n"
                       "2\n"
                       "1 0\n"
                       "CORRUPT: file is not a database\n");
        CHECK(took >= std::chrono::seconds(4));
    }

    SECTION("migrate") {
        // A Migrate that waits to begin while another connection migrates the database past its
        // last migration throws, as one that starts after would.
        const std::string migrated = run("migrated", R"fb(
Const first = "CREATE TABLE log (step TEXT); INSERT INTO log VALUES ('one')"
Const ready = Channel<Int>(1)

Function Older(): String
    Try
        Local db = Database.Open(folder + "/migrated.db")
        ready.Send(1)
        db.Migrate(first)
        Return "older migrated"
    Catch e As DatabaseError
        Return "older: {e.Kind} {e.Message}"
    End Try
End Function

Try
    Local db = Database.Open(folder + "/migrated.db")
    Local holding = db.Begin()
    Local older = Spawn(Function() => Older())
    ready.Receive()
    db.Rows("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 1000000) " +
            "SELECT count(*) FROM c")
    db.Execute(first)
    db.Execute("INSERT INTO log VALUES ('two'); PRAGMA user_version = 2")
    holding.Commit()
    Print(older.Wait())
    Print(db.Rows("SELECT count(*) FROM log")[0].Integer(0) ?? -1)
Catch e As DatabaseError
    Print("failed: {e.Kind} {e.Message}")
End Try
)fb");
        CHECK(migrated ==
              "older: OTHER the database is at version 2, and there are 1 migrations\n2\n");
    }
}

TEST_CASE("a program's input goes in while its output comes out, however much of each", "[cli]") {
    // Each is more than a pipe holds, so writing all the input before reading would wait
    // forever.
    const std::filesystem::path source =
        write_source("echo/echo.fb", "Import std.core\nLocal line = Input()\nWhile line != Null\n"
                                     "    Print(line)\n    line = Input()\nEnd While\n");
    std::filesystem::path executable = source;
    executable.replace_extension(fb::executable_extension);
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(source) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    std::string input;
    for (int i = 0; i < 20000; ++i) {
        input += std::format("line {} of the input\n", i);
    }

    const std::expected<fb::process_result_t, std::string> ran =
        fb::run_process(executable, {}, input);
    REQUIRE(ran);
    CHECK(ran->output == input);
}

#ifndef _WIN32
TEST_CASE("a program that reads stdin leaves it as it was, for what else reads it", "[cli]") {
    // A pipe's read end is the shell's too, which a non-blocking one would break. That holds
    // however the program ends: normally, with a panic, or with an error it lets out.
    for (const auto& [name, text, code] : {
             std::tuple("ask", "Print(Input() ?? \"none\")\n", 0),
             std::tuple("ask_panic",
                        "Local line = Input()\nLocal none: Int[] = []\nPrint(none[0])\n", 101),
             std::tuple("ask_error", "Local line = Input()\nThrow Error(\"after reading\")\n", 1),
         }) {
        INFO(name);
        const std::filesystem::path source = write_source(std::format("flags/{}.fb", name),
                                                          std::format("Import std.core\n{}", text));
        std::filesystem::path executable = source;
        executable.replace_extension(fb::executable_extension);
        const fb::process_result_t built = fb_run({ "build", fb::to_utf8(source) });
        INFO(built.output);
        REQUIRE(built.exit_code == 0);

        std::array<int, 2> ends{};
        REQUIRE(pipe(ends.data()) == 0);
        const int before = fcntl(ends[0], F_GETFL);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, ends[0], STDIN_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        const std::string path = executable.string();
        std::array<char*, 2> argv{ const_cast<char*>(path.c_str()), nullptr };
        pid_t pid = 0;
        const int spawned =
            posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        REQUIRE(spawned == 0);
        CHECK(write(ends[1], "first\n", 6) == 6);
        close(ends[1]);
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == code);
        CHECK(fcntl(ends[0], F_GETFL) == before);
        close(ends[0]);
    }
}
#endif

TEST_CASE("fb and the programs it builds give the console its code page back", "[cli]") {
#ifdef FB_TEST_CONSOLE_PROBE
    // The console is the shell's too, so the UTF-8 that fb and a program print in is only theirs
    // while they run, however they end. cmd's chcp sets the page, and the probe prints it.
    const std::filesystem::path hello =
        write_source("console/hello.fb", "Import std.core\nPrint(\"hi\")\n");
    const std::filesystem::path panics = write_source(
        "console/panics.fb", "Import std.core\nLocal none: Int[] = []\nPrint(none[0])\n");
    for (const std::filesystem::path& source : { hello, panics }) {
        const fb::process_result_t built = fb_run({ "build", fb::to_utf8(source) });
        INFO(built.output);
        REQUIRE(built.exit_code == 0);
    }

    const auto windows_path = [](std::filesystem::path path) {
        return path.replace_extension(fb::executable_extension).make_preferred().string();
    };
    const std::string probe =
        std::filesystem::path(FB_TEST_CONSOLE_PROBE).make_preferred().string();
    const std::filesystem::path script = hello.parent_path() / "pages.cmd";
    std::ofstream(script, std::ios::binary) << std::format(
        "@echo off\r\nchcp 437 >nul\r\n\"{0}\"\r\n\"{1}\" --version >nul\r\n\"{0}\"\r\n"
        "\"{2}\" >nul\r\n\"{0}\"\r\n\"{3}\" >nul 2>nul\r\n\"{0}\"\r\n",
        probe, std::filesystem::path(FB_CLI).make_preferred().string(), windows_path(hello),
        windows_path(panics));
    const std::vector<std::string> arguments{
        "/c",
        std::filesystem::path(script).make_preferred().string(),
    };
    const std::expected<fb::process_result_t, std::string> pages =
        fb::run_process("cmd.exe", arguments);
    REQUIRE(pages);
    INFO(pages->output);
    std::size_t found = 0;
    for (std::size_t at = pages->output.find("out=437"); at != std::string::npos;
         at = pages->output.find("out=437", at + 1)) {
        ++found;
    }

    CHECK(found == 4);
#else
    SKIP("only Windows' console has a code page that programs share");
#endif
}

TEST_CASE("Ctrl+C during fb run ends the program, and fb cleans up and gives its exit code",
          "[cli]") {
    // The terminal sends Ctrl+C to fb and the program alike. fb waits for the program to end,
    // deletes the folder it built it in, and ends with its exit code, as a shell does.
    const std::filesystem::path source = write_source(
        "interrupt/interrupted.fb", "Import std.core\nPrintError(\"started\")\nLocal n = 0\n"
                                    "While True\n    n = (n + 1) % 1000\nEnd While\n");
    // The temp folder is every run's, so a folder an earlier run that was stopped left there
    // isn't this one's.
    const std::filesystem::path temporary = fb::temporary_path("probe", ".none").parent_path();
    const auto folders = [&] {
        std::vector<std::filesystem::path> found;
        std::error_code ignored;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(temporary, ignored)) {
            if (fb::to_utf8(entry.path().filename()).starts_with("interrupted-")) {
                found.push_back(entry.path());
            }
        }

        return found;
    };
    const std::vector<std::filesystem::path> before = folders();
    const auto left_behind = [&] {
        return std::ranges::any_of(folders(), [&](const std::filesystem::path& folder) {
            return !std::ranges::contains(before, folder);
        });
    };
    std::string output;
#ifdef _WIN32
    // Ctrl+Break, the Ctrl+C that a process group can be sent, goes through the console they
    // share.
    if (GetConsoleCP() == 0) {
        SKIP("this process has no console to send Ctrl+Break through");
    }

    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    REQUIRE(CreatePipe(&read_end, &write_end, &security, 0) != 0);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    std::wstring command = std::format(L"\"{}\" run \"{}\"",
                                       std::filesystem::path(FB_CLI).wstring(), source.wstring());
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                           CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process) != 0);
    CloseHandle(write_end);
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (!output.contains("started") &&
           ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) !=
               0 &&
           count > 0) {
        output.append(buffer.data(), count);
    }

    const bool sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId) != 0;
    if (!sent) {
        TerminateProcess(process.hProcess, 1);
    }

    while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) !=
               0 &&
           count > 0) {
        output.append(buffer.data(), count);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(read_end);
    INFO(output);
    REQUIRE(sent);
    // The program ends as Windows ends one on Ctrl+C or Ctrl+Break.
    CHECK(exit_code == 0xC000013A);
    CHECK(output.contains("interrupted.exe exited with code 0xC000013A"));
#else
    // SIGINT, which Ctrl+C sends to the terminal's process group: fb's own here.
    std::array<int, 2> ends{};
    REQUIRE(pipe(ends.data()) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, static_cast<short>(POSIX_SPAWN_SETPGROUP));
    std::string fb_path = FB_CLI;
    std::string run_word = "run";
    std::string source_path = source.string();
    std::array<char*, 4> argv{ fb_path.data(), run_word.data(), source_path.data(), nullptr };
    pid_t pid = 0;
    const int spawned =
        posix_spawn(&pid, fb_path.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(ends[1]);
    REQUIRE(spawned == 0);
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while (!output.contains("started") &&
           (count = read(ends[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }

    kill(-pid, SIGINT);
    while ((count = read(ends[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }

    close(ends[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    INFO(output);
    REQUIRE(WIFEXITED(status));
    // The program ends by SIGINT, which a shell reports as 128 plus its number.
    CHECK(WEXITSTATUS(status) == 128 + SIGINT);
    CHECK(output.contains(std::format("interrupted exited with code {}", 128 + SIGINT)));
#endif
    CHECK_FALSE(left_behind());
}

TEST_CASE("a program fb starts gets SIGPIPE at its default, though fb ignores it", "[cli]") {
#ifdef _WIN32
    SKIP("Windows has no SIGPIPE");
#else
    // Writing a program's input has this process ignore SIGPIPE, which the programs it starts
    // after don't inherit, so a shell killed by one ends as one does.
    const std::vector<std::string> reads{ "-c", "cat > /dev/null" };
    const std::expected<fb::process_result_t, std::string> fed =
        fb::run_process("/bin/sh", reads, "input\n");
    REQUIRE(fed);
    const std::vector<std::string> arguments{ "-c", "kill -PIPE $$; echo survived" };
    const std::expected<fb::process_result_t, std::string> piped =
        fb::run_process("/bin/sh", arguments);
    REQUIRE(piped);
    CHECK(piped->exit_code == 128 + SIGPIPE);
    CHECK_FALSE(piped->output.contains("survived"));
#endif
}

TEST_CASE("fb check --release checks the release build's code", "[cli]") {
    const std::filesystem::path source = write_source(
        "release_check/branches.fb",
        "#If RELEASE\nLocal a = only_in_release\n#Else\nLocal b = only_in_debug\n#End If\n");
    const fb::process_result_t debug = fb_run({ "check", fb::to_utf8(source) });
    INFO(debug.output);
    CHECK(debug.exit_code == 1);
    CHECK(debug.output.contains("'only_in_debug' isn't declared"));
    CHECK_FALSE(debug.output.contains("only_in_release"));

    const fb::process_result_t release = fb_run({ "check", "--release", fb::to_utf8(source) });
    INFO(release.output);
    CHECK(release.exit_code == 1);
    CHECK(release.output.contains("'only_in_release' isn't declared"));
    CHECK_FALSE(release.output.contains("only_in_debug"));
}

TEST_CASE("fb build stops at compile errors", "[cli]") {
    const std::filesystem::path source =
        write_source("broken.fb", "Import std.core\nPrint(missing)\n");
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
    CHECK(panic.output.contains(
        std::format("error: panic_overflow{} exited with code 101, which a panic gives\n",
                    fb::executable_extension)));
}

TEST_CASE("fb --show prints the stages it names", "[cli]") {
    const auto show = [](const std::string& stages) {
        const fb::process_result_t result =
            fb_run({ "check", program("hello.fb"), "--show", stages });
        CHECK(result.exit_code == 0);
        return result.output;
    };

    CHECK(show("tokens").contains("3:1   identifier      Print\n"));
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

    // A usage error follows a --color given before the mistake.
    const fb::process_result_t unknown = fb_run({ "--color", "always", "nosuchcmd" });
    CHECK(unknown.exit_code == 2);
    CHECK(unknown.output.contains("\x1b[1;31merror\x1b[0m"));
    const fb::process_result_t option =
        fb_run({ "check", program("hello.fb"), "--color=always", "--nope" });
    CHECK(option.exit_code == 2);
    CHECK(option.output.contains("\x1b[1;31merror\x1b[0m"));
    CHECK_FALSE(fb_run({ "nosuchcmd" }).output.contains('\x1b'));
}

TEST_CASE("fb runs and builds a package from its folder", "[cli]") {
    // A copy, so building doesn't write into the source tree.
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "game", "physics" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    const std::string game = fb::to_utf8(packages / "game");
    const fb::process_result_t ran = fb_run({ "run", game });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("   Compiling game v0.1.0 ("));
    CHECK(ran.output.contains("     Running game"));
    CHECK(ran.output.contains("at 3, 1.5\narea 12\n2\n"));

    // fb build writes it to the package's build/ folder, named after the package.
    std::filesystem::path executable = packages / "game" / "build" / "game";
    executable += fb::executable_extension;
    const fb::process_result_t built = fb_run({ "build", game });
    INFO(built.output);
    CHECK(built.exit_code == 0);
    CHECK(std::filesystem::exists(executable));

    // A library is checked, but has nothing to run.
    const std::string physics = fb::to_utf8(packages / "physics");
    const fb::process_result_t checked = fb_run({ "check", physics });
    CHECK(checked.exit_code == 0);
    CHECK(checked.output.contains("    Checking physics v0.2.0 ("));
    // Its views are of its own files, not the placeholder a library's check has for a main file.
    const fb::process_result_t shown =
        fb_run({ "check", physics, "--show", "tokens,ast,types", "--color", "never" });
    CHECK(shown.exit_code == 0);
    CHECK(shown.output.contains("vector.fb\n"));
    CHECK(shown.output.contains("(struct Public Vec"));
    CHECK(shown.output.contains("(class Public Body"));
    CHECK_FALSE(shown.output.contains("(library)"));
    check_usage_error({ "check", physics, "--show", "mir" },
                      "physics is a library, with no program of its own to show as MIR or LLVM IR");
    const fb::process_result_t library = fb_run({ "run", physics });
    CHECK(library.exit_code == 2);
    CHECK(library.output.contains("error: physics is a library, with no main file to run"));

    // A folder that isn't a package's is bad usage.
    check_usage_error({ "check", fb::to_utf8(packages) }, "there's no project.toml in '");
}

TEST_CASE("fb runs a package that links a C library", "[cli]") {
    const std::filesystem::path packages =
        std::filesystem::path(FB_TEST_OUTPUT) / "native_packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "native_calls", "vmath" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    // The vmath package links tests/native's vecmath from its libs/ folder.
    const std::filesystem::path archive(FB_TEST_VECMATH);
    std::filesystem::create_directories(packages / "vmath" / "libs");
    std::filesystem::copy_file(archive, packages / "vmath" / "libs" / archive.filename());
    const fb::process_result_t ran = fb_run({ "run", fb::to_utf8(packages / "native_calls") });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("5 4000000005 6 2.5\nTrue False 2 0\n"));

    // Without it, project.toml's line says so.
    std::filesystem::remove_all(packages / "vmath" / "libs");
    const fb::process_result_t missing = fb_run({ "check", fb::to_utf8(packages / "vmath") });
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.contains(
        "project.toml:6:11: error: \"libs/vecmath\" names no static library: there's no "
        "libs/vecmath, libs/libvecmath.a, libs/vecmath.a, or libs/vecmath.lib, from the package's "
        "folder\n"));
}

TEST_CASE("fb bindgen runs fb-bindgen, which writes a C header's module", "[cli]") {
#ifdef FB_TEST_BINDGEN
    const std::filesystem::path header =
        write_source("bindgen/shapes.h", "int area(int width, int height);\n");
    const std::filesystem::path output = header.parent_path() / "shapes.fb";
    std::filesystem::remove(output);
    const fb::process_result_t made =
        fb_run({ "bindgen", fb::to_utf8(header), "-o", fb::to_utf8(output), "--lib", "shapes" });
    INFO(made.output);
    CHECK(made.exit_code == 0);
    std::ifstream file(output, std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(file),
                            std::istreambuf_iterator<char>() };
    CHECK(text.ends_with("Public Extern \"C\" Lib \"shapes\"\n"
                         "    Function area(width: Int32, height: Int32): Int32\n"
                         "End Extern\n"));

    // What goes wrong is fb-bindgen's to say, an error a line, as `fb bindgen`.
    const std::filesystem::path broken =
        write_source("bindgen/broken.h", "int broken(int x;\nint also(;\n");
    const fb::process_result_t failed =
        fb_run({ "bindgen", fb::to_utf8(broken), "-o", fb::to_utf8(output) });
    CHECK(failed.exit_code == 1);
    CHECK(failed.output.contains("broken.h:1:17: expected ')'\nerror: "));
    CHECK(failed.output.contains("broken.h:2:10: expected parameter declarator\n"));
    check_usage_error({ "bindgen" }, "error: fb bindgen takes a C header, like 'fb bindgen "
                                     "vecmath.h'");
    check_usage_error({ "bindgen", "a.h", "--nope" }, "error: unknown option '--nope'");
    check_usage_error({ "bindgen", "a.h", "--nope" },
                      "Run 'fb bindgen --help' to see the options.");

    // fb's own --color is fb's, and isn't passed on.
    const fb::process_result_t colored = fb_run({
        "bindgen",
        "--color",
        "never",
        fb::to_utf8(header),
        "--color=always",
        "-o",
        fb::to_utf8(output),
    });
    INFO(colored.output);
    CHECK(colored.exit_code == 0);
    check_usage_error({ "bindgen", "a.h", "--color" }, "error: '--color' needs a value");
    check_usage_error({ "bindgen", "a.h", "--color", "pink" },
                      "error: '--color' takes auto, always, or never, not 'pink'");

    // Empty values say nothing, -o goes apart from its value, -o can name a new folder, and the
    // library's name is one project.toml can have.
    check_usage_error({ "bindgen", "a.h", "-D", "" }, "'-D' needs a macro's name");
    check_usage_error({ "bindgen", "a.h", "--target", "" }, "'--target' needs a target");
    check_usage_error({ "bindgen", "a.h", "--std=" }, "'--std' needs a C standard");
    check_usage_error({ "bindgen", "-optimize", "a.h" }, "unknown option '-optimize'");
    const std::filesystem::path nested = header.parent_path() / "new" / "folder" / "shapes.fb";
    std::filesystem::remove_all(header.parent_path() / "new");
    CHECK(fb_run({ "bindgen", fb::to_utf8(header), "-o", fb::to_utf8(nested) }).exit_code == 0);
    CHECK(std::filesystem::exists(nested));
    const std::filesystem::path unnamed = write_source("bindgen/b\xC3\xA4r.h", "int f(int);\n");
    check_usage_error({ "bindgen", fb::to_utf8(unnamed) },
                      "isn't a name a [native] library can have");

    // -o takes the module's file name: not nothing, nor a folder's, which nothing is made for.
    check_usage_error({ "bindgen", fb::to_utf8(header), "-o", "" },
                      "'-o' takes the module's file name, like 'out/vecmath.fb'");
    const std::filesystem::path unmade = header.parent_path() / "unmade";
    std::filesystem::remove_all(unmade);
    check_usage_error({ "bindgen", fb::to_utf8(header), "-o", fb::to_utf8(unmade) + "/" },
                      "'-o' takes the module's file name, like 'out/vecmath.fb'");
    CHECK_FALSE(std::filesystem::exists(unmade));
    std::filesystem::create_directories(unmade);
    check_usage_error(
        { "bindgen", fb::to_utf8(header), "--output", fb::to_utf8(unmade) },
        std::format("'-o' names a folder, {}; give the module's file name", fb::to_utf8(unmade)));

    // The module never goes over the header, whichever path it's given by.
    check_usage_error(
        { "bindgen", fb::to_utf8(header), "-o", fb::to_utf8(header) },
        std::format("'-o' names the header, {}; give another path", fb::to_utf8(header)));
    // Without -o, the module goes in the current folder, named after the header.
    const std::filesystem::path module_named = write_source("bindgen/named.fb", "int g(int);\n");
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(module_named.parent_path());
    const fb::process_result_t over = fb_run({ "bindgen", "named.fb" });
    std::filesystem::current_path(before);
    INFO(over.output);
    CHECK(over.exit_code == 2);
    CHECK(over.output.contains("the module would be named.fb, the header; give its path with -o"));
    std::ifstream kept(header, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) ==
          "int area(int width, int height);\n");
#else
    SKIP("fb-bindgen is built only where CMake finds libclang");
#endif
}

TEST_CASE("fb build reports a C symbol that no library defines at its Extern Function", "[cli]") {
    const std::filesystem::path source =
        write_source("missing_c.fb", "Import std.core\n"
                                     "Extern \"C\"\n"
                                     "    Function no_such_c_symbol(): Int32\n"
                                     "End Extern\n"
                                     "Print(\"{no_such_c_symbol()}\")\n");
    std::filesystem::path executable = std::filesystem::path(FB_TEST_OUTPUT) / "missing_c";
    executable += fb::executable_extension;
    for (const std::string_view command : { "build", "run" }) {
        std::vector<std::string> arguments{ std::string(command), fb::to_utf8(source) };
        if (command == "build") {
            arguments.insert(arguments.end(), { "-o", fb::to_utf8(executable) });
        }

        const fb::process_result_t built = fb_run(arguments);
        INFO(built.output);
        CHECK(built.exit_code == 1);
        CHECK(built.output.contains("missing_c.fb:3:14: error: the C symbol 'no_such_c_symbol' "
                                    "isn't in the C runtime, or any library the program links\n"));
        CHECK(built.output.contains("missing_c.fb because of 1 error\n"));
        CHECK(built.output.contains("error: couldn't link "));
        CHECK_FALSE(built.output.contains("ld.lld"));
    }
}

TEST_CASE("fb's help, --color, and -- go where people put them", "[cli]") {
    // "fb help help" is fb's own help, "fb --help check" check's, and --color goes before the
    // command too.
    CHECK(fb_run({ "help", "help" }).output.contains("Commands:"));
    CHECK(fb_run({ "--help", "check" }).output.contains("Usage: fb check"));
    const fb::process_result_t colored =
        fb_run({ "--color", "never", "check", program("hello.fb") });
    CHECK(colored.exit_code == 0);
    CHECK_FALSE(colored.output.contains('\x1b'));

    // What follows -- goes to the program.
    const fb::process_result_t ran = fb_run({ "run", program("hello.fb"), "--", "a", "--b" });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("hello\n"));
}

TEST_CASE("fb without a file or folder uses the package the current folder is in", "[cli]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "no_input";
    std::filesystem::remove_all(root);
    write_source("no_input/app/project.toml", "[project]\nname = \"app\"\nversion = \"1.0.0\"\n");
    write_source("no_input/app/main.fb", "Import std.core\nPrint(\"from app\")\n");
    std::filesystem::create_directories(root / "app" / "ui" / "deep");
    std::filesystem::create_directories(root / "alone");
    const std::filesystem::path before = std::filesystem::current_path();

    // From a folder in the package, however deep, it's the package.
    std::filesystem::current_path(root / "app" / "ui" / "deep");
    const fb::process_result_t ran = fb_run({ "run" });
    // Outside every package, it's bad usage.
    std::filesystem::current_path(root / "alone");
    const fb::process_result_t alone = fb_run({ "check" });
    std::filesystem::current_path(before);

    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("from app\n"));
    INFO(alone.output);
    CHECK(alone.exit_code == 2);
    CHECK(alone.output.contains("there's no project.toml in this folder or above it"));
}

TEST_CASE("fb build's -o names a new file, not a folder or the program's own", "[cli]") {
    const std::filesystem::path source =
        write_source("output/hi.fb", "Import std.core\nPrint(\"hi\")\n");
    const std::filesystem::path folder = source.parent_path() / "made";
    std::filesystem::create_directories(folder);
    check_usage_error({ "build", fb::to_utf8(source), "-o", fb::to_utf8(folder) },
                      "'-o' names a folder");
    check_usage_error({ "build", fb::to_utf8(source), "-o", fb::to_utf8(source) },
                      "a file of the program; give another path");
    std::ifstream kept(source, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) ==
          "Import std.core\nPrint(\"hi\")\n");

    // Without -o, the executable's path can be a folder's too, as game/ is beside game.fb on
    // Linux and macOS, and that's said without blaming a -o nobody gave.
    const std::filesystem::path game =
        write_source("output/game.fb", "Import std.core\nPrint(\"game\")\n");
    std::filesystem::path beside = game;
    beside.replace_extension(fb::executable_extension);
    std::filesystem::create_directories(beside);
    check_usage_error({ "build", fb::to_utf8(game) },
                      std::format("the executable would be {}, which is a folder; give its path "
                                  "with -o",
                                  fb::to_utf8(beside)));

    // A path that isn't ASCII, without an extension, gets the one the target's executables have.
    const std::filesystem::path wide = source.parent_path() / fb::path_from_utf8("日本") / "out";
    std::filesystem::path written = wide;
    written += fb::executable_extension;
    std::filesystem::remove(written);
    const fb::process_result_t built =
        fb_run({ "build", fb::to_utf8(source), "-o", fb::to_utf8(wide) });
    INFO(built.output);
    CHECK(built.exit_code == 0);
    CHECK(std::filesystem::exists(written));
}

TEST_CASE("fb builds where the temporary folder's name isn't in the ANSI code page", "[cli]") {
#ifdef _WIN32
    // The C++ driver that links reads TMP and TEMP in the ANSI code page, which has no snowman.
    const std::filesystem::path temporary =
        std::filesystem::path(FB_TEST_OUTPUT) / fb::path_from_utf8("tmp \xE2\x98\x83 x");
    std::filesystem::create_directories(temporary);
    std::wstring old_tmp;
    std::wstring old_temp;
    if (const wchar_t* const value = _wgetenv(L"TMP")) {
        old_tmp = value;
    }

    if (const wchar_t* const value = _wgetenv(L"TEMP")) {
        old_temp = value;
    }

    _wputenv_s(L"TMP", temporary.wstring().c_str());
    _wputenv_s(L"TEMP", temporary.wstring().c_str());
    const fb::process_result_t ran = fb_run({ "run", program("hello.fb") });
    _wputenv_s(L"TMP", old_tmp.c_str());
    _wputenv_s(L"TEMP", old_temp.c_str());
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("hello\n"));
#else
    SKIP("only Windows reads TMP in a code page");
#endif
}

TEST_CASE("fb shows the first hundred diagnostics of a file with many", "[cli]") {
    std::string text;
    for (int i = 0; i < 150; ++i) {
        text += std::format("Local x{}: Int = \"a\"\n", i);
    }

    const std::filesystem::path source = write_source("many_errors.fb", text);
    const fb::process_result_t checked =
        fb_run({ "check", fb::to_utf8(source), "--color", "never" });
    CHECK(checked.exit_code == 1);
    CHECK(checked.output.contains("x99: Int"));
    CHECK_FALSE(checked.output.contains("x100: Int"));
    CHECK(checked.output.contains("note: 50 more diagnostics aren't shown\n"));
    CHECK(checked.output.contains("many_errors.fb has 150 errors\n"));
}

TEST_CASE("fb takes a .fb file's extension in any letter case", "[cli]") {
    const std::filesystem::path source =
        write_source("SHOUT.FB", "Import std.core\nPrint(\"loud\")\n");
    const fb::process_result_t checked = fb_run({ "check", fb::to_utf8(source) });
    INFO(checked.output);
    CHECK(checked.exit_code == 0);
}
