#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/interface.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

/// Sets an environment variable for the processes a test starts, while it lives, then puts it
/// back.
class environment_variable_t {
public:
    environment_variable_t(const std::string_view name, const std::string_view value)
        : m_name(name) {
#ifdef _WIN32
        const std::wstring wide = fb::path_from_utf8(name).wstring();
        if (const wchar_t* const old = _wgetenv(wide.c_str())) {
            m_old = fb::to_utf8(std::filesystem::path(old));
        }

        _wputenv_s(wide.c_str(), fb::path_from_utf8(value).wstring().c_str());
#else
        if (const char* const old = std::getenv(m_name.c_str())) {
            m_old = old;
        }

        setenv(m_name.c_str(), std::string(value).c_str(), 1);
#endif
    }

    environment_variable_t(const environment_variable_t&) = delete;
    environment_variable_t& operator=(const environment_variable_t&) = delete;
    environment_variable_t(environment_variable_t&&) = delete;
    environment_variable_t& operator=(environment_variable_t&&) = delete;

    // Putting it back is best effort.
    ~environment_variable_t() {
        try {
#ifdef _WIN32
            _wputenv_s(fb::path_from_utf8(m_name).wstring().c_str(),
                       fb::path_from_utf8(m_old.value_or("")).wstring().c_str());
#else
            if (m_old) {
                setenv(m_name.c_str(), m_old->c_str(), 1);
            } else {
                unsetenv(m_name.c_str());
            }
#endif
        } catch (...) {
            return;
        }
    }

private:
    std::string m_name;
    std::optional<std::string> m_old;
};

/// The file of a bundled library's notices, in the build folder.
std::filesystem::path notice_file(const fb::build_config::bundled_library_t& library) {
    return fb::path_from_utf8(fb::build_config::notice_directory) /
           fb::path_from_utf8(library.notice);
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

/// A file's bytes.
std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
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

#ifdef __linux__
/// The symbols a 64-bit, little-endian ELF file exports: its .dynsym's global and weak ones that
/// it defines.
std::vector<std::string> elf_exports(const std::string& bytes) {
    std::vector<std::string> names;
    if (!bytes.starts_with("\x7F"
                           "ELF") ||
        bytes.size() < 64) {
        return names;
    }

    // SHT_DYNSYM (11) is the symbols, and its sh_link the section of their names.
    const auto sections = number_at<std::uint64_t>(bytes, 0x28);
    const auto section_size = number_at<std::uint16_t>(bytes, 0x3A);
    const auto count = number_at<std::uint16_t>(bytes, 0x3C);
    const auto section = [&](const std::uint64_t index) {
        return sections + (index * section_size);
    };
    for (std::uint64_t i = 0; i < count; ++i) {
        if (number_at<std::uint32_t>(bytes, section(i) + 4) != 11) {
            continue;
        }

        const auto symbols = number_at<std::uint64_t>(bytes, section(i) + 24);
        const auto size = number_at<std::uint64_t>(bytes, section(i) + 32);
        const auto strings = number_at<std::uint64_t>(
            bytes, section(number_at<std::uint32_t>(bytes, section(i) + 40)) + 24);
        // Each symbol is 24 bytes: its name's place, its binding in the info's high bits, and
        // its section, 0 where it isn't defined.
        for (std::uint64_t symbol = symbols; symbol + 24 <= symbols + size; symbol += 24) {
            const auto binding =
                static_cast<unsigned>(static_cast<unsigned char>(bytes[symbol + 4])) >> 4U;
            if ((binding == 1 || binding == 2) &&
                number_at<std::uint16_t>(bytes, symbol + 6) != 0) {
                names.push_back(text_at(bytes, strings + number_at<std::uint32_t>(bytes, symbol)));
            }
        }
    }

    return names;
}
#endif

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

TEST_CASE("fb --version prints the version, the commit, and the target", "[cli]") {
    const fb::process_result_t result = fb_run({ "--version" });
    CHECK(result.exit_code == 0);
    // "fb 0.1.0 (abc1234, windows-x64)", or "unknown" where git didn't say.
    const std::string start = std::format("fb {} (", fb::build_config::version);
    const std::string end = std::format(", {})\n", fb::build_config::toolchain_target);
    INFO(result.output);
    REQUIRE(result.output.starts_with(start));
    REQUIRE(result.output.ends_with(end));
    const std::string commit =
        result.output.substr(start.size(), result.output.size() - start.size() - end.size());
    CHECK((commit == "unknown" ||
           (commit.size() >= 7 && std::ranges::all_of(commit, [](const char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))));
}

TEST_CASE("fb new makes a package that fb runs", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "new";
    std::filesystem::remove_all(folder);
    const std::filesystem::path package = folder / "games" / "hello";
    const fb::process_result_t made = fb_run({ "new", fb::to_utf8(package) });
    INFO(made.output);
    REQUIRE(made.exit_code == 0);
    CHECK(made.output == std::format("     Created package hello in {}\n", fb::to_utf8(package)));
    CHECK(read_file(package / "project.toml") ==
          "[project]\nname = \"hello\"\nversion = \"0.1.0\"\n");
    CHECK(read_file(package / "main.fb") == "Import std.core\n\nPrint(\"Hello, world!\")\n");
    CHECK(read_file(package / ".gitignore") == "build/\n");

    const fb::process_result_t ran = fb_run({ "run", fb::to_utf8(package) });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.ends_with("Hello, world!\n"));

    // An empty folder is fine, and "game/" is the folder "game".
    std::filesystem::create_directories(folder / "game");
    const fb::process_result_t empty = fb_run({ "new", fb::to_utf8(folder) + "/game/" });
    INFO(empty.output);
    CHECK(empty.exit_code == 0);
    CHECK(read_file(folder / "game" / "project.toml").contains("name = \"game\""));

    // A folder is named by its own name, however it's written: "dotted/." and "slashed/./" are
    // the empty folders dotted and slashed, as `fb new .` is the one it runs in.
    for (const auto& [written, name] : {
             std::pair{ "dotted/.", "dotted" },
             std::pair{ "slashed/./", "slashed" },
         }) {
        INFO(written);
        std::filesystem::create_directories(folder / name);
        const fb::process_result_t made_there =
            fb_run({ "new", fb::to_utf8(folder) + "/" + written });
        INFO(made_there.output);
        CHECK(made_there.exit_code == 0);
        CHECK(made_there.output.contains(std::format("Created package {} in", name)));
        CHECK(
            read_file(folder / name / "project.toml").contains(std::format("name = \"{}\"", name)));
    }
}

TEST_CASE("fb new refuses a folder that isn't empty, and a name a package can't have", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "new_refused";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "full");
    std::ofstream(folder / "full" / "notes.txt", std::ios::binary) << "mine\n";
    std::ofstream(folder / "file", std::ios::binary) << "a file\n";

    const fb::process_result_t full = fb_run({ "new", fb::to_utf8(folder / "full") });
    CHECK(full.exit_code == 1);
    CHECK(full.output.contains(
        std::format("error: {} is there already, and isn't empty", fb::to_utf8(folder / "full"))));
    CHECK(read_file(folder / "full" / "notes.txt") == "mine\n");
    CHECK_FALSE(std::filesystem::exists(folder / "full" / "project.toml"));

    const fb::process_result_t file = fb_run({ "new", fb::to_utf8(folder / "file") });
    CHECK(file.exit_code == 1);
    CHECK(file.output.contains("is a file, not a folder"));

    // A folder under a file can't be made, which fb new says, with why.
    const fb::process_result_t under = fb_run({ "new", fb::to_utf8(folder / "file" / "game") });
    INFO(under.output);
    CHECK(under.exit_code == 1);
    CHECK(under.output.contains(std::format("error: couldn't make the folder {}: ",
                                            fb::to_utf8(folder / "file" / "game"))));

    // A root has no name for the package to take.
    const fb::process_result_t root = fb_run({ "new", fb::to_utf8(folder.root_path()) });
    CHECK(root.exit_code == 2);
    CHECK(root.output.contains("has no name, which the package would take"));

#ifdef _WIN32
    // Windows' devices' names are in every folder, though no file is.
    for (const std::string_view device : { "con", "NUL", "Com1", "lpt9" }) {
        INFO(device);
        const fb::process_result_t made =
            fb_run({ "new", std::format("{}/{}", fb::to_utf8(folder), device) });
        CHECK(made.exit_code == 2);
        CHECK(made.output.contains(std::format(
            "{} is the name of a device on Windows, which a folder can't have", device)));
    }
#endif

    // Names are checked as project.toml's are.
    for (const auto& [name, message] : {
             std::pair{ "9lives", "a package's name is an identifier, like game or my_physics, not "
                                  "\"9lives\"; it's the folder's name" },
             std::pair{ "my-game", "not \"my-game\"" },
             std::pair{ "If", "If is a keyword, which a package's name can't be; it's the "
                              "folder's name" },
             std::pair{ "Self", "Self is a keyword, which a package's name can't be" },
             std::pair{ "std", "std is the standard library's name" },
         }) {
        INFO(name);
        const fb::process_result_t bad = fb_run({ "new", fb::to_utf8(folder / name) });
        CHECK(bad.exit_code == 2);
        CHECK(bad.output.contains(message));
        CHECK_FALSE(std::filesystem::exists(folder / name));
    }

    check_usage_error({ "new" }, "'fb new' needs the new package's folder, like 'fb new game'");
    check_usage_error({ "new", "a", "b" },
                      "unexpected argument 'b'; 'fb new' takes the new package's folder");
    check_usage_error({ "new", "a", "--release" },
                      "'--release' doesn't work with 'fb new', whose only option is --color");
    CHECK(fb_run({ "help", "new" }).output.contains("Usage: fb new [options] <folder>"));
    CHECK(fb_run({}).output.contains("new                  Make a package in a new folder"));
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

TEST_CASE("FB_COLUMNS=utf-16 counts diagnostics' columns in UTF-16 code units", "[cli]") {
    // Two U+1F600s before the error take a code point each, and two UTF-16 units each.
    const std::filesystem::path source =
        write_source("columns.fb", "Local s = \"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\" + y\n");
    const fb::process_result_t points = fb_run({ "check", fb::to_utf8(source) });
    INFO(points.output);
    CHECK(points.output.contains("columns.fb:1:18: error: 'y' isn't declared\n"));

#ifdef _WIN32
    _putenv_s("FB_COLUMNS", "utf-16");
#else
    setenv("FB_COLUMNS", "utf-16", 1);
#endif
    const fb::process_result_t units = fb_run({ "check", fb::to_utf8(source) });
    // fb doc's warnings too.
    const std::filesystem::path documented = std::filesystem::path(FB_TEST_OUTPUT) / "columns";
    std::filesystem::remove_all(documented);
    std::filesystem::create_directories(documented);
    std::ofstream(documented / "project.toml", std::ios::binary)
        << "[project]\nname = \"columns\"\nversion = \"0.1.0\"\ntype = \"library\"\n";
    std::ofstream(documented / "a.fb", std::ios::binary)
        << "/// A \xF0\x9F\x98\x80 thing, see [Missing].\nPublic Function F(): Int => 1\n";
    const fb::process_result_t warned = fb_run({ "doc", fb::to_utf8(documented), "-o", "-" });
#ifdef _WIN32
    _putenv_s("FB_COLUMNS", "");
#else
    unsetenv("FB_COLUMNS");
#endif
    INFO(units.output);
    CHECK(units.output.contains("columns.fb:1:20: error: 'y' isn't declared\n"));
    INFO(warned.output);
    CHECK(warned.output.contains("a.fb:1:21: warning: 'Missing' names nothing"));
    // The snippet's markers still go under the span by the columns the terminal shows.
    CHECK(units.output.contains("1 | Local s = \"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\" + y\n"));
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
    const std::filesystem::path notice = notice_file(libuv);
    CHECK(reads.output.contains(std::format("     Bundled libuv {}, under the MIT, BSD-2-Clause, "
                                            "and ISC licenses; ship its notices with the "
                                            "program: {}\n",
                                            libuv.version, fb::to_utf8(notice))));
    // Its notices are libuv's own, and those of the code it has from others.
    std::ifstream file(notice, std::ios::binary);
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
                                             sqlite->version, fb::to_utf8(notice_file(*sqlite)))));
    std::ifstream dedication(notice_file(*sqlite), std::ios::binary);
    const std::string blessing{ std::istreambuf_iterator<char>(dedication), {} };
    CHECK(blessing.contains("The author disclaims copyright to this source code."));
    CHECK(blessing.contains("May you share freely, never taking more than you give."));
    CHECK_FALSE(blessing.contains("This header file defines the interface"));

    // A part of another library, as mbedTLS's TLS is, has no line of its own: mbedTLS's says it.
    const std::filesystem::path secures =
        write_source("bundled/secures.fb", "Import std.net.tls\nLocal client = TlsClient()\n");
    const fb::process_result_t secured = fb_run({ "build", fb::to_utf8(secures) });
    INFO(secured.output);
    REQUIRE(secured.exit_code == 0);
    CHECK(secured.output.contains("     Bundled mbedTLS "));
    CHECK_FALSE(secured.output.contains("TLS's"));
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
                                             utf8proc->version,
                                             fb::to_utf8(notice_file(*utf8proc)))));
    std::ifstream file(notice_file(*utf8proc), std::ios::binary);
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
    // however the program ends: normally, with a panic, with an error it lets out, or by a
    // signal, like Ctrl+C's, which a negative code says it's sent once it has read.
    constexpr std::string_view loops = "Local line = Input()\nLocal n = 0\nWhile True\n"
                                       "    n = (n + 1) % 9\nEnd While\n";
    for (const auto& [name, text, code] : {
             std::tuple("ask", "Print(Input() ?? \"none\")\n", 0),
             std::tuple("ask_panic",
                        "Local line = Input()\nLocal none: Int[] = []\nPrint(none[0])\n", 101),
             std::tuple("ask_error", "Local line = Input()\nThrow Error(\"after reading\")\n", 1),
             std::tuple("ask_interrupted", loops.data(), -SIGINT),
             std::tuple("ask_terminated", loops.data(), -SIGTERM),
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
        if (code < 0) {
            // libuv makes the pipe non-blocking as it reads it.
            for (int i = 0; i < 1000 && fcntl(ends[0], F_GETFL) == before; ++i) {
                usleep(10000);
            }

            CHECK(fcntl(ends[0], F_GETFL) != before);
            kill(pid, -code);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        if (code < 0) {
            REQUIRE(WIFSIGNALED(status));
            CHECK(WTERMSIG(status) == -code);
        } else {
            REQUIRE(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) == code);
        }

        CHECK(fcntl(ends[0], F_GETFL) == before);
        close(ends[0]);
    }
}
#endif

TEST_CASE("fb and the programs it builds give the console its code page and mode back", "[cli]") {
#ifdef FB_TEST_CONSOLE_PROBE
    // The console is the shell's too, so the UTF-8 that fb and a program print in, and the
    // escape codes they color with, are only theirs while they run, however they end: normally,
    // with a panic, or with Ctrl+Break. cmd's chcp sets the page, and the probe prints it.
    const std::filesystem::path hello =
        write_source("console/hello.fb", "Import std.core\nPrint(\"hi\")\n");
    const std::filesystem::path panics = write_source(
        "console/panics.fb", "Import std.core\nLocal none: Int[] = []\nPrint(none[0])\n");
    const std::filesystem::path loops =
        write_source("console/loops.fb",
                     "Import std.core\nLocal n = 0\nWhile True\n    n = (n + 1) % 9\nEnd While\n");
    for (const std::filesystem::path& source : { hello, panics, loops }) {
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
    // "run" and "break" run each with the console as its stdout and stderr, from a mode without
    // escape codes, which fb and the programs turn on for their own. "break" ends a program,
    // and fb, as soon as it has changed the console, which is before it's done starting, five
    // times each: Ctrl+Break then used to end them before they could put it back.
    const std::string fb = std::filesystem::path(FB_CLI).make_preferred().string();
    std::string breaks;
    for (int i = 0; i < 5; ++i) {
        breaks += std::format("\"{0}\" break \"{1}\"\r\n\"{0}\" break \"{2}\" check \"{3}\"\r\n",
                              probe, windows_path(loops), fb,
                              std::filesystem::path(hello).make_preferred().string());
    }

    std::ofstream(script, std::ios::binary) << std::format(
        "@echo off\r\nchcp 437 >nul\r\n\"{0}\"\r\n\"{1}\" --version >nul\r\n\"{0}\"\r\n"
        "\"{2}\" >nul\r\n\"{0}\"\r\n\"{3}\" >nul 2>nul\r\n\"{0}\"\r\n"
        "\"{0}\" run \"{1}\" --version\r\n\"{0}\" run \"{2}\"\r\n\"{0}\" run \"{3}\"\r\n{4}",
        probe, fb, windows_path(hello), windows_path(panics), breaks);
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

    // Each one that ran on the console left it as it was before.
    std::size_t runs = 0;
    for (std::size_t at = pages->output.find("before="); at != std::string::npos;
         at = pages->output.find("before=", at + 1)) {
        const std::size_t end = pages->output.find('\n', at);
        std::string line = pages->output.substr(at, end - at);
        if (line.ends_with('\r')) {
            line.pop_back();
        }

        const std::size_t after = line.find(" after=");
        INFO(line);
        REQUIRE(after != std::string::npos);
        CHECK(line.substr(7, after - 7) == line.substr(after + 7));
        ++runs;
    }

    CHECK(runs == 13);
#else
    SKIP("only Windows' console has a code page that programs share");
#endif
}

TEST_CASE("Ctrl+C during fb run ends the program, and fb cleans up and gives its exit code",
          "[cli]") {
    // The terminal sends Ctrl+C to fb and the program alike. fb waits for the program to end,
    // deletes the folder it built it in, and ends with its exit code, as a shell does. The
    // program is named for this process, so copies of this test that run at once each look for
    // their own folder.
#ifdef _WIN32
    const auto own = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto own = static_cast<unsigned long>(getpid());
#endif
    const std::string name = std::format("interrupted_{}", own);
    const std::filesystem::path source =
        write_source(std::format("interrupt/{}.fb", name),
                     "Import std.core\nPrintError(\"started\")\nLocal n = 0\n"
                     "While True\n    n = (n + 1) % 1000\nEnd While\n");
    // The temp folder is every run's, so a folder an earlier run that was stopped left there
    // isn't this one's.
    const std::filesystem::path temporary = fb::temporary_path("probe", ".none").parent_path();
    const auto folders = [&] {
        std::vector<std::filesystem::path> found;
        std::error_code ignored;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(temporary, ignored)) {
            if (fb::to_utf8(entry.path().filename()).starts_with(name + "-")) {
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
    CHECK(output.contains(std::format("{}.exe exited with code 0xC000013A", name)));
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
    CHECK(output.contains(std::format("{} exited with code {}", name, 128 + SIGINT)));
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

TEST_CASE("a program started while another runs doesn't keep the other's pipes", "[cli]") {
#ifdef _WIN32
    SKIP("this checks the descriptors posix_spawn passes on");
#else
    // Two programs started at once each kept the other's, its stdin's write end among them, so
    // each waited for the end of input the other held, as the HTTP test servers of a test did.
    const std::vector<std::string> lists{ "-c", "ls /dev/fd" };
    const std::expected<fb::process_result_t, std::string> alone =
        fb::run_process("/bin/sh", lists);
    REQUIRE(alone);
    std::thread other([] {
        const std::vector<std::string> sleeps{ "-c", "sleep 1" };
        std::ignore = fb::run_process("/bin/sh", sleeps, "");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::expected<fb::process_result_t, std::string> beside =
        fb::run_process("/bin/sh", lists);
    other.join();
    REQUIRE(beside);
    CHECK(beside->output == alone->output);
#endif
}

namespace {

/// What a program that drive() runs is sent: an interrupt, which is Ctrl+Break on Windows and
/// SIGINT elsewhere, or SIGTERM or SIGHUP, which Windows has none of to send.
enum class sent_t : std::uint8_t {
    INTERRUPT,
    TERMINATE,
    HANG_UP,
};

/// Once the program has printed `after`, past what the step before waited for, it's sent
/// `signal`, `times` times.
struct step_t {
    std::string_view after;
    sent_t signal = sent_t::INTERRUPT;
    int times = 1;
};

/// How a program that drive() ran ended: what it printed, stdout and stderr together, and its
/// exit code, or on Linux and macOS minus the signal that ended it.
struct driven_t {
    std::string output;
    std::int64_t status = 0;
    /// It ran for a minute, and was ended.
    bool stuck = false;
};

/// Builds `source` as the program signals/`name` and runs it in a process group of its own,
/// sending it signals as `steps` say. On Linux and macOS, it can start with SIGHUP ignored, as
/// nohup starts one. A program that runs for a minute is ended.
driven_t drive(const std::string_view name, const std::string_view source,
               const std::vector<step_t>& steps, const bool ignore_hang_up = false) {
    const std::filesystem::path written = write_source(std::format("signals/{}.fb", name), source);
    std::filesystem::path executable = written;
    executable.replace_extension();
    executable += fb::executable_extension;
    std::filesystem::remove(executable);
    const fb::process_result_t built =
        fb_run({ "build", fb::to_utf8(written), "-o", fb::to_utf8(executable) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    std::mutex mutex;
    std::condition_variable ended;
    bool done = false;
    driven_t driven;
    const auto watch = [&](const auto& end) {
        return std::thread([&, end] {
            std::unique_lock lock(mutex);
            if (!ended.wait_for(lock, std::chrono::minutes(1), [&] { return done; })) {
                driven.stuck = true;
                end();
            }
        });
    };
    // Once the output has ended, so has the program, or the watcher has ended it.
    const auto stop = [&](std::thread& watcher) {
        {
            const std::scoped_lock lock(mutex);
            done = true;
        }

        ended.notify_all();
        watcher.join();
    };
    std::size_t from = 0;
#ifdef _WIN32
    // Ctrl+Break is the Ctrl+C that a process group can be sent, through the console it shares.
    REQUIRE(!ignore_hang_up);
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
    std::wstring command = std::format(L"\"{}\"", executable.wstring());
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                           CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process) != 0);
    CloseHandle(write_end);
    std::thread watcher = watch([&process] { TerminateProcess(process.hProcess, 1); });
    const auto read_until = [&](const std::string_view text) {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (driven.output.find(text, from) == std::string::npos &&
               ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                        nullptr) != 0 &&
               count > 0) {
            driven.output.append(buffer.data(), count);
        }
    };
    for (const step_t& step : steps) {
        read_until(step.after);
        REQUIRE(driven.output.find(step.after, from) != std::string::npos);
        from = driven.output.find(step.after, from) + step.after.size();
        REQUIRE(step.signal == sent_t::INTERRUPT);
        for (int i = 0; i < step.times; ++i) {
            REQUIRE(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId) != 0);
        }
    }

    read_until("the end it never prints");
    stop(watcher);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    driven.status = exit_code;
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(read_end);
#else
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
    std::string path = executable.string();
    std::array<char*, 2> argv{ path.data(), nullptr };
    // A signal ignored is ignored in what's started, as nohup has it.
    struct sigaction before{};
    if (ignore_hang_up) {
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        sigaction(SIGHUP, &ignored, &before);
    }

    pid_t pid = 0;
    const int spawned =
        posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ);
    if (ignore_hang_up) {
        sigaction(SIGHUP, &before, nullptr);
    }

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(ends[1]);
    REQUIRE(spawned == 0);
    std::thread watcher = watch([pid] { kill(pid, SIGKILL); });
    const auto read_until = [&](const std::string_view text) {
        std::array<char, 4096> buffer{};
        ssize_t count = 0;
        while (driven.output.find(text, from) == std::string::npos &&
               (count = read(ends[0], buffer.data(), buffer.size())) > 0) {
            driven.output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    };
    for (const step_t& step : steps) {
        read_until(step.after);
        REQUIRE(driven.output.find(step.after, from) != std::string::npos);
        from = driven.output.find(step.after, from) + step.after.size();
        const int sent = step.signal == sent_t::INTERRUPT   ? SIGINT
                         : step.signal == sent_t::TERMINATE ? SIGTERM
                                                            : SIGHUP;
        for (int i = 0; i < step.times; ++i) {
            REQUIRE(kill(pid, sent) == 0);
        }
    }

    read_until("the end it never prints");
    stop(watcher);
    close(ends[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    driven.status = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
#endif
    return driven;
}

/// How many times `text` is in `output`.
std::size_t count_of(const std::string_view output, const std::string_view text) {
    std::size_t count = 0;
    for (std::size_t at = output.find(text); at != std::string_view::npos;
         at = output.find(text, at + text.size())) {
        ++count;
    }

    return count;
}

/// The status of a program that an interrupt ended: Windows' for Ctrl+C and Ctrl+Break, or SIGINT.
#ifdef _WIN32
constexpr std::int64_t interrupted_status = 0xC000013A;
#else
constexpr std::int64_t interrupted_status = -SIGINT;
#endif

} // namespace

TEST_CASE("OnInterrupt and OnTerminate handle a program's signals until its code is done",
          "[cli]") {
#ifdef _WIN32
    if (GetConsoleCP() == 0) {
        SKIP("this process has no console to send Ctrl+Break through");
    }

    // Windows has no SIGTERM to send, so it's another Ctrl+Break.
    const sent_t second = sent_t::INTERRUPT;
#else
    const sent_t second = sent_t::TERMINATE;
#endif
    // Each handler sends what it saw to the program's code, which prints it, asks for the next,
    // and ends, with the threads the handlers wait on.
    const driven_t driven =
        drive("handled",
              "Import std.core\nImport std.os\nConst seen = Channel<String>(4)\n"
              "Process.OnInterrupt(Function()\n    seen.Send(\"interrupted\")\nEnd Function)\n"
              "Process.OnTerminate(Function()\n    seen.Send(\"terminated\")\nEnd Function)\n"
              "PrintError(\"started\")\nPrint(seen.Receive() ?? \"closed\")\n"
              "PrintError(\"again\")\nPrint(seen.Receive() ?? \"closed\")\n",
              { { .after = "started" }, { .after = "again", .signal = second } });
    INFO(driven.output);
    CHECK_FALSE(driven.stuck);
    CHECK(driven.status == 0);
#ifdef _WIN32
    CHECK(count_of(driven.output, "interrupted\n") == 2);
#else
    CHECK(count_of(driven.output, "interrupted\n") == 1);
    CHECK(count_of(driven.output, "terminated\n") == 1);
#endif
}

TEST_CASE("a signal ends a program again once its code is done, while a thread runs on", "[cli]") {
#ifdef _WIN32
    if (GetConsoleCP() == 0) {
        SKIP("this process has no console to send Ctrl+Break through");
    }
#endif
    const driven_t driven =
        drive("after_end",
              "Import std.core\nImport std.os\nImport std.time\n"
              "Process.OnInterrupt(Function()\n    Print(\"interrupted\")\nEnd Function)\n"
              "Thread.Start(Function()\n    Sleep(300.Milliseconds)\n    PrintError(\"send\")\n"
              "    Sleep(5.Seconds)\n    Print(\"the thread ran to its end\")\nEnd Function)\n"
              "Print(\"done\")\n",
              { { .after = "send" } });
    INFO(driven.output);
    CHECK_FALSE(driven.stuck);
    CHECK(driven.status == interrupted_status);
    CHECK_FALSE(driven.output.contains("interrupted"));
    CHECK_FALSE(driven.output.contains("the thread ran to its end"));
}

TEST_CASE("a handler runs once more for the signals that come while it runs", "[cli]") {
#ifdef _WIN32
    if (GetConsoleCP() == 0) {
        SKIP("this process has no console to send Ctrl+Break through");
    }
#endif
    // Three signals during the first run make one more run, and no third comes.
    const driven_t driven =
        drive("coalesced",
              "Import std.core\nImport std.os\nImport std.time\nConst ran = Channel<Int>(8)\n"
              "Process.OnInterrupt(Function()\n    PrintError(\"running\")\n    Sleep(1.Seconds)\n"
              "    ran.Send(1)\nEnd Function)\n"
              "PrintError(\"started\")\nran.Receive()\nran.Receive()\nSleep(1500.Milliseconds)\n"
              "Local runs = 2\nLocal more = 0\nWhile ran.TryReceive(ByRef more)\n    runs += 1\n"
              "End While\nPrint(\"runs: {runs}\")\n",
              { { .after = "started" }, { .after = "running", .times = 3 } });
    INFO(driven.output);
    CHECK_FALSE(driven.stuck);
    CHECK(driven.status == 0);
    CHECK(driven.output.contains("runs: 2\n"));
}

TEST_CASE("a coroutine waiting for what a handler gives waits for the signal", "[cli]") {
#ifdef _WIN32
    if (GetConsoleCP() == 0) {
        SKIP("this process has no console to send Ctrl+Break through");
    }
#endif
    // The handler's thread isn't work that could finish the coroutine's Task, but the TaskSource
    // it gives is.
    const driven_t driven =
        drive("source",
              "Import std.core\nImport std.concurrent\nImport std.os\n"
              "Const stop = TaskSource<Bool>()\n"
              "Process.OnInterrupt(Function()\n    stop.TryFinish(True)\nEnd Function)\n"
              "Async Function Run()\n    PrintError(\"waiting\")\n    Await stop.Task\n"
              "    Print(\"stopping\")\nEnd Function\nRun()\n",
              { { .after = "waiting" } });
    INFO(driven.output);
    CHECK_FALSE(driven.stuck);
    CHECK(driven.status == 0);
    CHECK(driven.output.contains("stopping\n"));
}

TEST_CASE("a signal a program started out ignoring stays ignored, with a handler or after",
          "[cli]") {
#ifdef _WIN32
    SKIP("Windows has no signals to ignore");
#else
    // Started as nohup starts it, the program gets SIGHUP while its code runs and once it's
    // done, while a thread runs on, and both are ignored.
    const driven_t driven = drive(
        "ignored",
        "Import std.core\nImport std.os\nImport std.time\n"
        "Process.OnTerminate(Function()\n    Print(\"terminated\")\nEnd Function)\n"
        "PrintError(\"watching\")\nSleep(1.Seconds)\n"
        "Thread.Start(Function()\n    Sleep(300.Milliseconds)\n    PrintError(\"code done\")\n"
        "    Sleep(1.Seconds)\n    Print(\"the thread ran to its end\")\nEnd Function)\n",
        { { .after = "watching", .signal = sent_t::HANG_UP },
          { .after = "code done", .signal = sent_t::HANG_UP } },
        true);
    INFO(driven.output);
    CHECK_FALSE(driven.stuck);
    CHECK(driven.status == 0);
    CHECK_FALSE(driven.output.contains("terminated"));
    CHECK(driven.output.contains("the thread ran to its end\n"));
#endif
}

namespace {

/// Builds `source` as the program `name` beside it, and gives its path.
std::filesystem::path build_program(const std::string_view name, const std::string_view source) {
    const std::filesystem::path written = write_source(std::format("{}.fb", name), source);
    std::filesystem::path executable = written;
    executable.replace_extension();
    executable += fb::executable_extension;
    std::filesystem::remove(executable);
    const fb::process_result_t built =
        fb_run({ "build", fb::to_utf8(written), "-o", fb::to_utf8(executable) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    return executable;
}

} // namespace

TEST_CASE("a .bat or .cmd file gets each argument as cmd.exe can carry it, and nothing runs",
          "[cli]") {
#ifndef _WIN32
    SKIP("only Windows runs .bat and .cmd files, in cmd.exe");
#else
    // show.bat says what each argument came as: one that ran something would add a line, and an
    // expanded %PATH% would show.
    const std::filesystem::path script = write_source(
        "batch/show.bat", "@echo off\r\necho 1=[%1]\r\necho 2=[%2]\r\necho 3=[%3]\r\n");
    std::filesystem::copy_file(script, script.parent_path() / "show.cmd",
                               std::filesystem::copy_options::overwrite_existing);
    const std::string folder = script.parent_path().generic_string();
    const std::filesystem::path runs = build_program("batch/runs", std::format(R"(Import std.core
Import std.os
For name In ["show.bat", "SHOW.CMD", "show.bat. ."]
    Local command = Command("{0}/" + name)
    command.Args(["safe&echo,INJECTED", "x\" & echo INJECTED", "%PATH%"])
    Local output = Try command.Output()
    Write(output.Text)
    Print(output.Status)
End For
Try
    Try Command("{0}/show.bat").Arg("two\nlines").Output()
Catch e As IOError
    Print(e.Kind)
End Try
)",
                                                                               folder));
    const std::expected<fb::process_result_t, std::string> ran = fb::run_process(runs, {});
    REQUIRE(ran);
    INFO(ran->output);
    std::string expected;
    for (int i = 0; i < 3; ++i) {
        expected += "1=[\"safe&echo,INJECTED\"]\r\n2=[\"x\"\" & echo INJECTED\"]\r\n"
                    "3=[\"%PATH%\"]\r\nexit code 0\n";
    }

    CHECK(ran->output == expected + "OTHER\n");
    CHECK(ran->exit_code == 0);
#endif
}

TEST_CASE("a program's name is found on PATH, never in the folder it runs in", "[cli]") {
    // whoami, in the folder a Command runs in, is a program that says so: only a path runs it.
    const std::filesystem::path planted =
        build_program("planted/planted", "Import std.core\nPrint(\"planted\")\n");
    const std::filesystem::path folder = planted.parent_path() / "folder";
    std::filesystem::create_directories(folder);
    std::filesystem::path whoami = folder / "whoami";
    whoami += fb::executable_extension;
    std::filesystem::copy_file(planted, whoami, std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path runner =
        build_program("planted/runner", std::format(R"(Import std.core
Import std.os
// Windows looks in the current folder first, unless this is set, as some shells set it.
Env.Unset("NoDefaultCurrentDirectoryInExePath")
Local named = Try Command("whoami").Folder("{0}").Output()
Print(named.Status, named.Text == "planted\n")
Local pathed = Try Command("./whoami").Folder("{0}").Output()
Print(pathed.Status, pathed.Text == "planted\n")
)",
                                                    folder.generic_string()));
    const std::expected<fb::process_result_t, std::string> ran = fb::run_process(runner, {});
    REQUIRE(ran);
    CHECK(ran->output == "exit code 0 False\nexit code 0 True\n");
    CHECK(ran->exit_code == 0);
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

    // fb build writes a library's interface file, for its build's target and mode.
    const fb::process_result_t interface = fb_run({ "build", physics, "--release" });
    INFO(interface.output);
    CHECK(interface.exit_code == 0);
    CHECK(interface.output.contains("    Finished release build in "));
    const std::filesystem::path written = packages / "physics" / "build" /
                                          fb::path_from_utf8(fb::build_target()) / "release" /
                                          "physics.fbi";
    std::ifstream file(written, std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(file),
                            std::istreambuf_iterator<char>() };
    CHECK(text.starts_with("// fbi physics 0.2.0 "));
    CHECK(text.contains("\n// module physics/vector.fb "));
    check_usage_error({ "build", physics, "-o", "x" },
                      "physics is a library, whose build goes in its build/ folder; '-o' names an "
                      "executable");

    // A prebuilt library is checked from its interface file, and has no source to build.
    const std::filesystem::path prebuilt = packages / "prebuilt";
    std::filesystem::create_directories(prebuilt / "lib" / fb::path_from_utf8(fb::build_target()) /
                                        "release");
    std::filesystem::copy_file(packages / "physics" / "project.toml", prebuilt / "project.toml");
    std::filesystem::copy_file(written, prebuilt / "lib" / fb::path_from_utf8(fb::build_target()) /
                                            "release" / "physics.fbi");
    const fb::process_result_t prechecked = fb_run({ "check", fb::to_utf8(prebuilt), "--release" });
    INFO(prechecked.output);
    CHECK(prechecked.exit_code == 0);
    check_usage_error({ "build", fb::to_utf8(prebuilt), "--release" }, "physics is prebuilt, ");

    // A library needs modules to build.
    const std::filesystem::path empty = packages / "empty";
    std::filesystem::create_directories(empty);
    std::ofstream(empty / "project.toml") << "[project]\nname = \"empty\"\nversion = \"0.1.0\"\n";
    check_usage_error({ "build", fb::to_utf8(empty) },
                      "empty has no .fb files, so it has no modules to build");

    // A folder that isn't a package's is bad usage.
    check_usage_error({ "check", fb::to_utf8(packages) }, "there's no project.toml in '");
}

TEST_CASE("fb test runs a package's tests, filters them, and reports each", "[cli]") {
    // A copy, so its tests' program isn't built into the source tree.
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "tested";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "tested", "physics" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    const std::string tested = fb::to_utf8(packages / "tested");
    const fb::process_result_t ran = fb_run({ "test", tested, "--color", "never" });
    INFO(ran.output);
    CHECK(ran.exit_code == 1);
    CHECK(ran.output.contains("   Compiling tested v0.1.0 ("));
    CHECK(ran.output.contains("     Running 11 tests\n"));
    // Each one's result, in the order its run ends: a test that ends its program ends a run.
    for (const std::string_view line : {
             "test tested.counting.Counts ... ok\n",
             "test tested.counting.UsesAGlobal ... ok\n",
             "test tested.counting.Breaks ... FAILED\n",
             "test tested.counting.Parses ... ok\n",
             "test tested.counting.LetsAnErrorOut ... FAILED\n",
             "test tested.counting.Skipped ... skipped: there's nothing to count\n",
             "test tested.trouble.Panics ... FAILED\n",
             "test tested.trouble.Asserts ... FAILED\n",
             "test tested.trouble.Exits ... FAILED\n",
             "test tested.trouble.Leaks ... FAILED\n",
             "test tested.tests.outside.CountsFromOutside ... ok\n",
         }) {
        CHECK(ran.output.contains(line));
    }

    // Then what each failure said, in the tests' order, and how they all did.
    // Places show a file by its path from the current folder, with '/'s, as messages do.
    const auto shown = [&](const std::string_view file) {
        std::error_code ignored;
        const std::filesystem::path path =
            std::filesystem::weakly_canonical(packages / "tested" / file, ignored);
        const std::filesystem::path relative =
            path.lexically_relative(std::filesystem::current_path(ignored));
        std::string text =
            fb::to_utf8(relative.empty() || *relative.begin() == ".." ? path : relative);
        std::ranges::replace(text, '\\', '/');
        return text;
    };
    const std::string counting = shown("counting.fb");
    const std::string trouble = shown("trouble.fb");
    CHECK(ran.output.contains(std::format(
        "\nfailures:\n"
        "\n---- tested.counting.Breaks ----\n"
        "expect failed: counter.count + 1 == 3 at {0}:49:5\n"
        "  left: 1\n"
        "  right: 3\n"
        "expect failed: Twice(2) > 5 at {0}:50:5\n"
        "  left: 4\n"
        "  right: 5\n"
        "  two twice\n"
        "failed: on purpose at {0}:51:5\n"
        "expect failed: counter.Parse(\"2\") didn't throw at {0}:52:12\n"
        "expect failed: counter.Parse(\"two\") didn't throw OtherError at {0}:53:12, but it "
        "threw CountError: 'two' isn't a count at {0}:21:32\n"
        "\n---- tested.counting.LetsAnErrorOut ----\n"
        "threw CountError: 'none' isn't a count at {0}:21:32\n"
        "\n---- tested.trouble.Panics ----\n"
        "panic: index 5 is out of range for 2 elements at {1}:14:11\n"
        "\n---- tested.trouble.Asserts ----\n"
        "panic: arithmetic broke at {1}:19:5\n"
        "\n---- tested.trouble.Exits ----\n"
        "the tests' program exited with code 3, in this test\n"
        "\n---- tested.trouble.Leaks ----\n"
        "leak: 1 Node object was never released\n"
        "\ntest result: FAILED. 4 passed; 6 failed; 1 skipped; 0 filtered out; finished in ",
        counting, trouble)));

    // A filter runs the tests whose names have it in them, and those that pass pass.
    const fb::process_result_t filtered =
        fb_run({ "test", tested, "--filter", "Count", "--color", "never" });
    INFO(filtered.output);
    CHECK(filtered.exit_code == 0);
    CHECK(filtered.output.contains("     Running 2 tests\n"
                                   "test tested.counting.Counts ... ok\n"
                                   "test tested.tests.outside.CountsFromOutside ... ok\n"
                                   "\ntest result: ok. 2 passed; 0 failed; 0 skipped; 9 filtered "
                                   "out; finished in "));

    // A file in the package tests the package.
    const fb::process_result_t file = fb_run({
        "test",
        fb::to_utf8(packages / "tested" / "counting.fb"),
        "--filter",
        "counting.Counts",
        "--color",
        "never",
    });
    INFO(file.output);
    CHECK(file.exit_code == 0);
    CHECK(file.output.contains("test tested.counting.Counts ... ok\n"));
    CHECK(file.output.contains("10 filtered out"));

    // Its program and report are in a temporary folder, whose path can be beyond the ANSI code
    // page, as a user's name can.
    const std::filesystem::path temporary =
        packages / fb::path_from_utf8("t\xC3\xABmp \xE6\x97\xA5");
    std::filesystem::create_directories(temporary);
#ifdef _WIN32
    const std::wstring before = _wgetenv(L"TMP") != nullptr ? _wgetenv(L"TMP") : L"";
    _wputenv_s(L"TMP", temporary.c_str());
#else
    const char* const outer = std::getenv("TMPDIR");
    const std::string before = outer != nullptr ? outer : "";
    setenv("TMPDIR", temporary.c_str(), 1);
#endif
    const fb::process_result_t unusual =
        fb_run({ "test", tested, "--filter", "counting.Counts", "--color", "never" });
#ifdef _WIN32
    _wputenv_s(L"TMP", before.c_str());
#else
    if (outer != nullptr) {
        setenv("TMPDIR", before.c_str(), 1);
    } else {
        unsetenv("TMPDIR");
    }
#endif
    INFO(unusual.output);
    CHECK(unusual.exit_code == 0);
    CHECK(unusual.output.contains("test tested.counting.Counts ... ok\n"));

    // A release build has no leak report, so a leak doesn't fail a test there.
    const fb::process_result_t released =
        fb_run({ "test", tested, "--filter", "trouble.Leaks", "--release", "--color", "never" });
    INFO(released.output);
    CHECK(released.exit_code == 0);
    CHECK(released.output.contains("test tested.trouble.Leaks ... ok\n"));

    // Errors stop it before it runs anything.
    std::ofstream(packages / "tested" / "broken.fb", std::ios::binary)
        << "Import std.test\n@Test Function Broken(n: Int)\nEnd Function\n";
    const fb::process_result_t broken = fb_run({ "test", tested, "--color", "never" });
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.contains("a @Test Function takes nothing"));
    CHECK(broken.output.contains("error: couldn't build tested v0.1.0 ("));
    CHECK_FALSE(broken.output.contains("Running"));

    check_usage_error({ "build", tested, "--filter", "x" }, "'--filter' only works with 'fb test'");
    check_usage_error({ "test", tested, "--show", "mir" },
                      "'--show' only works with 'fb check', 'fb build', and 'fb run'");
    CHECK(
        fb_run({}).output.contains("test                 Compile a package's tests and run them"));
    const std::string help = fb_run({ "test", "--help" }).output;
    CHECK(help.contains("Usage: fb test"));
    CHECK(help.contains("--filter <name>"));
    CHECK_FALSE(help.contains("--show"));
}

TEST_CASE("fb test gives a test the work it starts, and fails a run that fails outside its tests",
          "[cli]") {
    const auto run = [](const std::filesystem::path& file,
                        const std::vector<std::string>& more = {}) {
        std::vector<std::string> arguments{ "test", fb::to_utf8(file), "--color", "never" };
        arguments.insert(arguments.end(), more.begin(), more.end());
        return fb_run(arguments);
    };
    const std::string_view nodes = "Import std.core\n"
                                   "Import std.test\n"
                                   "Import std.time\n"
                                   "Class Node\n"
                                   "    Field next: Node?\n"
                                   "End Class\n"
                                   "Function MakeCycle(): Int\n"
                                   "    Local node = Node()\n"
                                   "    node.next = node\n"
                                   "    Return 1\n"
                                   "End Function\n";

    // Work a test starts is its own wherever the pool runs it: its leaks, its Expects, and its
    // panic, though the test doesn't wait for it, every run.
    const std::filesystem::path work = write_source(
        "testing/work.fb", std::string(nodes) +
                               "Function Later(): Int\n"
                               "    Sleep(300.Milliseconds)\n"
                               "    Panic(\"the work broke\")\n"
                               "End Function\n"
                               "@Test Function LeaksInSpawn()\n"
                               "    Expect(Spawn(Function() => MakeCycle()).Wait() == 1)\n"
                               "End Function\n"
                               "@Test Function ExpectsInParallel()\n"
                               "    Parallel For i = 0 Until 4\n"
                               "        Expect(i < 0)\n"
                               "    End For\n"
                               "End Function\n"
                               "@Test Function StartsWorkThatPanics()\n"
                               "    Local later = Spawn(Function() => Later())\n"
                               "End Function\n"
                               "@Test Function Innocent()\n"
                               "    Expect(1 + 1 == 2)\n"
                               "End Function\n");
    for (int i = 0; i < 3; ++i) {
        const fb::process_result_t ran = run(work);
        INFO(ran.output);
        CHECK(ran.exit_code == 1);
        CHECK(ran.output.contains("test work.LeaksInSpawn ... FAILED\n"));
        CHECK(ran.output.contains("\n---- work.LeaksInSpawn ----\n"
                                  "leak: 1 Node object was never released\n"));
        CHECK(ran.output.contains("test work.ExpectsInParallel ... FAILED\n"));
        std::size_t expects = 0;
        for (std::size_t at = ran.output.find("expect failed: i < 0 at "); at != std::string::npos;
             at = ran.output.find("expect failed: i < 0 at ", at + 1)) {
            ++expects;
        }

        CHECK(expects == 4);
        CHECK_FALSE(ran.output.contains("only a test's own code"));
        CHECK(ran.output.contains("test work.StartsWorkThatPanics ... FAILED\n"));
        CHECK(ran.output.contains(
            "\n---- work.StartsWorkThatPanics ----\npanic: the work broke at "));
        CHECK(ran.output.contains("test work.Innocent ... ok\n"));
        CHECK(ran.output.contains("test result: FAILED. 1 passed; 3 failed; 0 skipped"));
    }

    // A panic once the tests are done, in a Global's Destroy, fails the run, whose finished
    // tests' leaks it didn't check, and so does a leak no test made.
    const std::filesystem::path destroy =
        write_source("testing/destroy.fb", "Import std.core\n"
                                           "Import std.test\n"
                                           "Class Holder\n"
                                           "    Method Destroy()\n"
                                           "        Panic(\"Destroy broke\")\n"
                                           "    End Method\n"
                                           "End Class\n"
                                           "Global holder = Holder()\n"
                                           "@Test Function Fine()\n"
                                           "    Expect(1 == 1)\n"
                                           "End Function\n");
    const fb::process_result_t destroyed = run(destroy);
    INFO(destroyed.output);
    CHECK(destroyed.exit_code == 1);
    CHECK(destroyed.output.contains("test destroy.Fine ... ok\n"));
    CHECK(destroyed.output.contains("\n---- the tests' program ----\npanic: Destroy broke at "));
    CHECK(destroyed.output.contains(
        "\nso the leaks of the tests that had finished weren't checked\n"));
    CHECK(destroyed.output.contains("test result: FAILED. 1 passed; 0 failed"));
    const std::filesystem::path unowned =
        write_source("testing/unowned.fb", std::string(nodes) + "Global made = MakeCycle()\n"
                                                                "@Test Function Fine()\n"
                                                                "End Function\n");
    const fb::process_result_t left = run(unowned);
    INFO(left.output);
    CHECK(left.exit_code == 1);
    CHECK(left.output.contains("test unowned.Fine ... ok\n"));
    CHECK(left.output.contains(
        "\n---- the tests' program ----\nleak: 1 Node object was never released\n"));

    // A panic before any test runs, in a Global's initializer, says what panicked.
    const std::filesystem::path early = write_source("testing/early.fb", "Import std.test\n"
                                                                         "Function Start(): Int\n"
                                                                         "    Local counts = [1]\n"
                                                                         "    Return counts[2]\n"
                                                                         "End Function\n"
                                                                         "Global start = Start()\n"
                                                                         "@Test Function A()\n"
                                                                         "End Function\n");
    const fb::process_result_t panicked = run(early);
    INFO(panicked.output);
    CHECK(panicked.exit_code == 1);
    CHECK(panicked.output.contains("\n---- early.A ----\n"
                                   "panic: index 2 is out of range for 1 element at "));
    CHECK(panicked.output.contains("\nbefore any test ran\n"));

    // In a debug build, a run that panics runs its finished tests again, for their leaks.
    const std::filesystem::path twice =
        write_source("testing/twice.fb", "Import std.core\n"
                                         "Import std.test\n"
                                         "Import std.time\n"
                                         "Global runs = 0\n"
                                         "@Test Function Quick()\n"
                                         "    runs += 1\n"
                                         "    Print(\"Quick ran\")\n"
                                         "End Function\n"
                                         "@Test Function PanicsLater()\n"
                                         "    Sleep(500.Milliseconds)\n"
                                         "    Panic(\"later\")\n"
                                         "End Function\n");
    for (const bool release : { false, true }) {
        const fb::process_result_t ran = release ? run(twice, { "--release" }) : run(twice);
        INFO(ran.output);
        std::size_t quick = 0;
        for (std::size_t at = ran.output.find("Quick ran\n"); at != std::string::npos;
             at = ran.output.find("Quick ran\n", at + 1)) {
            ++quick;
        }

        CHECK(quick == (release ? 1U : 2U));
        CHECK(ran.output.contains("test twice.Quick ... ok\n"));
        CHECK(ran.output.contains("test twice.PanicsLater ... FAILED\n"));
    }

    // Ctrl+C stops fb test, which exits as the tests' program did.
    const std::filesystem::path interrupt =
        write_source("testing/interrupt.fb", "Import std.os\n"
                                             "Import std.test\n"
                                             "@Test Function Interrupted()\n"
                                             "#If PLATFORM_WINDOWS\n"
                                             "    Process.Exit(-1073741510)\n"
                                             "#Else\n"
                                             "    Process.Exit(130)\n"
                                             "#End If\n"
                                             "End Function\n"
                                             "@Test Function Other()\n"
                                             "End Function\n");
    const fb::process_result_t stopped = run(interrupt);
    INFO(stopped.output);
#ifdef _WIN32
    CHECK(static_cast<std::uint32_t>(stopped.exit_code) == 0xC000013AU);
#else
    CHECK(stopped.exit_code == 130);
#endif
    CHECK(stopped.output.contains("error: the tests were interrupted, and exited with code "));
    CHECK_FALSE(stopped.output.contains("test result:"));
}

namespace {

/// A copy of tests/packages/<name> in the test output folder, so what fb writes into a package's
/// build/ folder isn't in the source tree.
std::filesystem::path copied_package(const std::string_view name) {
    const std::filesystem::path copy = std::filesystem::path(FB_TEST_OUTPUT) / "copied" / name;
    std::filesystem::remove_all(copy);
    std::filesystem::create_directories(copy.parent_path());
    std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, copy,
                          std::filesystem::copy_options::recursive);
    return copy;
}

/// A file as messages show it: by its path from the current folder, with '/'s.
std::string shown_file(const std::filesystem::path& file) {
    std::error_code ignored;
    const std::filesystem::path path = std::filesystem::weakly_canonical(file, ignored);
    const std::filesystem::path relative =
        path.lexically_relative(std::filesystem::current_path(ignored));
    std::string text = fb::to_utf8(relative.empty() || *relative.begin() == ".." ? path : relative);
    std::ranges::replace(text, '\\', '/');
    return text;
}

} // namespace

TEST_CASE("fb test runs a package's examples, each a test named after its declaration", "[cli]") {
    const std::filesystem::path documented = copied_package("documented");
    const fb::process_result_t ran =
        fb_run({ "test", fb::to_utf8(documented), "--color", "never" });
    INFO(ran.output);
    CHECK(ran.exit_code == 1);
    CHECK(ran.output.contains("     Running 5 tests\n"));
    for (const std::string_view line : {
             "test documented.shapes.Circle (example) ... ok\n",
             "test documented.shapes.Circle.Grow (example) ... ok\n",
             "test documented.shapes.Circle.Grow (example 2) ... ok\n",
             "test documented.shapes.Circle.Grow (example 3) ... FAILED\n",
             "test documented.shapes.TotalArea (example) ... FAILED\n",
         }) {
        CHECK(ran.output.contains(line));
    }

    // What fails says where, in the doc comment, as a panic at its caller does too.
    const std::string shapes = shown_file(documented / "shapes.fb");
    CHECK(ran.output.contains(std::format(
        "\n---- documented.shapes.Circle.Grow (example 3) ----\n"
        "panic: a circle can't grow by less than 0 at {0}:61:9\n"
        "\n---- documented.shapes.TotalArea (example) ----\n"
        "expect failed: TotalArea(shapes) == 3 at {0}:110:5\n"
        "  left: 2\n"
        "  right: 3\n"
        "\ntest result: FAILED. 3 passed; 2 failed; 0 skipped; 0 filtered out; finished in ",
        shapes)));

    // An example's test is filtered by its name.
    const fb::process_result_t filtered = fb_run(
        { "test", fb::to_utf8(documented), "--filter", "Grow (example 2)", "--color", "never" });
    INFO(filtered.output);
    CHECK(filtered.exit_code == 0);
    CHECK(filtered.output.contains("test documented.shapes.Circle.Grow (example 2) ... ok\n"));
    CHECK(filtered.output.contains("4 filtered out"));

    // An example's mistake is shown at its line in the doc comment, and stops the tests.
    std::string text = read_file(documented / "shapes.fb");
    text.replace(text.find("/// Expect(circle.Radius == 2)"), 30,
                 "/// Expect(circle.Radius == \"2\")");
    std::ofstream(documented / "shapes.fb", std::ios::binary) << text;
    const fb::process_result_t broken =
        fb_run({ "test", fb::to_utf8(documented), "--color", "never" });
    INFO(broken.output);
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.contains(
        std::format("{}:25:12: error: '==' can't combine Float32 and String\n", shapes)));
    CHECK(broken.output.contains("25 | /// Expect(circle.Radius == \"2\")\n"));
    CHECK_FALSE(broken.output.contains("Running"));

    // A file outside a package runs its examples too, which import it by its path, and leaves its
    // top-level code out.
    const std::filesystem::path lone =
        write_source("examples/twice.fb", "Import std.core\n"
                                          "\n"
                                          "/// Twice `n`.\n"
                                          "///\n"
                                          "/// # Examples\n"
                                          "///\n"
                                          "/// ```\n"
                                          "/// Expect(Twice(2) == 4)\n"
                                          "/// Expect(Twice(3) == 5)\n"
                                          "/// ```\n"
                                          "Public Function Twice(n: Int): Int => n * 2\n"
                                          "\n"
                                          "Print(\"top-level code\")\n");
    const fb::process_result_t alone = fb_run({ "test", fb::to_utf8(lone), "--color", "never" });
    INFO(alone.output);
    CHECK(alone.exit_code == 1);
    CHECK(alone.output.contains("     Running 1 test\ntest twice.Twice (example) ... FAILED\n"));
    CHECK(alone.output.contains(
        std::format("expect failed: Twice(3) == 5 at {}:9:5\n", shown_file(lone))));
    CHECK_FALSE(alone.output.contains("top-level code"));

    // A stack trace calls an example's code by its test's name.
    const std::filesystem::path panics =
        write_source("examples/half.fb", "Import std.core\n"
                                         "\n"
                                         "/// Half of `n`.\n"
                                         "///\n"
                                         "/// # Examples\n"
                                         "///\n"
                                         "/// ```\n"
                                         "/// Local h = Half(3)\n"
                                         "/// ```\n"
                                         "Public Function Half(n: Int): Int\n"
                                         "    Local parts = [n]\n"
                                         "    Return parts[n]\n"
                                         "End Function\n");
    const fb::process_result_t traced = fb_run({ "test", fb::to_utf8(panics), "--color", "never" });
    INFO(traced.output);
    CHECK(traced.exit_code == 1);
    CHECK(traced.output.contains(
        std::format("    at half.Half (example) ({}:8)\n", shown_file(panics))));
    CHECK_FALSE(traced.output.contains("DocTest__Example"));
}

TEST_CASE("fb doc writes a package's public API as JSON, and warns about its doc comments",
          "[cli]") {
    const std::filesystem::path documented = copied_package("documented");
    const fb::process_result_t ran = fb_run({ "doc", fb::to_utf8(documented), "--color", "never" });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains(" Documenting documented v0.3.0 ("));
    const std::string shapes = shown_file(documented / "shapes.fb");
    CHECK(ran.output.contains(
        std::format("{}:19:22: warning: 'Missing' names nothing that code here sees\n", shapes)));
    CHECK(ran.output.contains(std::format(
        "{}:142:1: warning: this doc comment documents nothing: a /// comment goes on the lines "
        "right before a declaration\n",
        shapes)));
    const std::filesystem::path written = documented / "build" / "documented.json";
    CHECK(ran.output.contains(std::format("    Finished 1 module and 11 declarations in ")));
    CHECK(ran.output.contains(fb::to_utf8(written)));

    const std::string json = read_file(written);
    CHECK(json.starts_with("{\n"
                           "  \"format\": 1,\n"
                           "  \"package\": \"documented\",\n"
                           "  \"version\": \"0.3.0\",\n"
                           "  \"modules\": [\n"
                           "    {\n"
                           "      \"name\": \"documented.shapes\",\n"
                           "      \"file\": \"shapes.fb\",\n"
                           "      \"doc\": {\n"
                           "        \"summary\": \"Shapes, and what they cover: see [Shape] and "
                           "[Circle].\",\n"));
    // A member as M9's reference pages read it, with the doc it has from its Interface's.
    CHECK(
        json.contains("            {\n"
                      "              \"kind\": \"method\",\n"
                      "              \"name\": \"Area\",\n"
                      "              \"path\": \"documented.shapes.Square.Area\",\n"
                      "              \"signature\": \"Public Method Area(): Float\",\n"
                      "              \"attributes\": [],\n"
                      "              \"visibility\": \"Public\",\n"
                      "              \"file\": \"shapes.fb\",\n"
                      "              \"line\": 90,\n"
                      "              \"doc\": {\n"
                      "                \"summary\": \"How much of the plane it covers, in square "
                      "units.\",\n"
                      "                \"markdown\": \"How much of the plane it covers, in square "
                      "units.\",\n"
                      "                \"sections\": [\n"
                      "                  {\n"
                      "                    \"heading\": \"\",\n"
                      "                    \"markdown\": \"How much of the plane it covers, in "
                      "square units.\"\n"
                      "                  }\n"
                      "                ],\n"
                      "                \"links\": [],\n"
                      "                \"examples\": []\n"
                      "              },\n"
                      "              \"doc_from\": \"documented.shapes.Shape.Area\"\n"
                      "            }\n"));
    // An example, without its hidden lines, and a link, found.
    CHECK(json.contains("\"code\": \"circle.Grow(3)\\nExpect(circle.Radius == 3)\\n\",\n"
                        "                    \"line\": 46,\n"
                        "                    \"runs\": true\n"));
    CHECK(json.contains("\"text\": \"stacks.Stack\",\n"
                        "                \"path\": \"std.collections.stack.Stack\",\n"
                        "                \"builtin\": false\n"));

    // -o names the file, and - writes to stdout; a file of the package documents it.
    const std::filesystem::path elsewhere = documented / "out" / "api.json";
    const fb::process_result_t named = fb_run({
        "doc",
        fb::to_utf8(documented / "shapes.fb"),
        "-o",
        fb::to_utf8(elsewhere),
        "--color",
        "never",
    });
    INFO(named.output);
    CHECK(named.exit_code == 0);
    CHECK(read_file(elsewhere) == json);
    const fb::process_result_t printed =
        fb_run({ "doc", fb::to_utf8(documented), "-o", "-", "--color", "never" });
    CHECK(printed.exit_code == 0);
    CHECK(printed.output.contains(json));

    // A .fb file outside a package, and a folder without a project.toml, aren't packages.
    const std::filesystem::path lone =
        write_source("doc/lone.fb", "Public Function F()\nEnd Function\n");
    const fb::process_result_t outside = fb_run({ "doc", fb::to_utf8(lone), "--color", "never" });
    CHECK(outside.exit_code == 2);
    CHECK(outside.output.contains("isn't in a package, which a project.toml makes; fb doc "
                                  "documents a package, or std"));
    const fb::process_result_t folder =
        fb_run({ "doc", fb::to_utf8(lone.parent_path()), "--color", "never" });
    CHECK(folder.exit_code == 2);
    CHECK(folder.output.contains("there's no project.toml in"));

    // A path that isn't there can't be read, and with none, outside a package, fb doc says what
    // it takes.
    const std::string nope = fb::to_utf8(lone.parent_path() / "nope.fb");
    const fb::process_result_t missing = fb_run({ "doc", nope, "--color", "never" });
    CHECK(missing.exit_code == 2);
    CHECK(missing.output.contains(std::format("error: cannot read '{}': ", nope)));
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(lone.parent_path());
    const fb::process_result_t none = fb_run({ "doc", "--color", "never" });
    std::filesystem::current_path(before);
    CHECK(none.exit_code == 2);
    CHECK(none.output.contains("there's no project.toml in this folder or above it; give a "
                               "package's folder, one of its .fb files, or std"));

    // A package with errors isn't documented.
    std::ofstream(documented / "broken.fb", std::ios::binary)
        << "Public Function F(): Int\nEnd Function\n";
    const fb::process_result_t broken =
        fb_run({ "doc", fb::to_utf8(documented), "--color", "never" });
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.contains("error: couldn't document documented v0.3.0 ("));

    check_usage_error({ "doc", fb::to_utf8(documented), "--show", "ast" },
                      "'--show' only works with 'fb check', 'fb build', and 'fb run'");
    check_usage_error({ "doc", fb::to_utf8(documented), "--filter", "x" },
                      "'--filter' only works with 'fb test'");
    check_usage_error({ "doc", "-o", "out/" },
                      "'-o' takes the JSON's file name, like 'out/game.json', or '-' for stdout");
    check_usage_error({ "check", "-o", "x" }, "'-o' only works with 'fb build' and 'fb doc'");
    CHECK(fb_run({}).output.contains(
        "doc                  Write a package's public API as JSON, docs and all"));
    const std::string help = fb_run({ "doc", "--help" }).output;
    CHECK(help.contains("Usage: fb doc [options] [file.fb | package folder | std]"));
    CHECK(help.contains("With std, it's the standard library's API."));
    CHECK_FALSE(help.contains("--show"));
}

TEST_CASE("fb doc std writes the standard library's API, and fb test std runs its examples",
          "[cli]") {
    const std::filesystem::path written =
        std::filesystem::path(FB_TEST_OUTPUT) / "doc" / "std.json";
    std::filesystem::remove(written);
    const fb::process_result_t documented =
        fb_run({ "doc", "std", "-o", fb::to_utf8(written), "--color", "never" });
    INFO(documented.output);
    CHECK(documented.exit_code == 0);
    CHECK(documented.output.contains(" Documenting std\n"));
    // std's doc comments each document something, and each of their links names something.
    CHECK_FALSE(documented.output.contains("warning:"));
    const std::string json = read_file(written);
    CHECK(json.contains("\"package\": \"std\",\n"));
    CHECK(json.contains("\"path\": \"std.collections.stack.Stack.Push\",\n"));

    const fb::process_result_t tested = fb_run({ "test", "std", "--color", "never" });
    INFO(tested.output);
    CHECK(tested.exit_code == 0);
    CHECK(tested.output.contains("   Compiling std\n"));
    CHECK(tested.output.contains("test std.collections.stack.Stack (example) ... ok\n"));
    CHECK(tested.output.contains("\ntest result: ok. "));
    CHECK(fb_run({ "test", "--help" })
              .output.contains("With std, it runs the examples in the standard library's docs."));
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

TEST_CASE("fb copies a package's dynamic library beside the program, which loads it from there",
          "[cli]") {
    // tests/native's dyncalc, as its package has it, and as the program records it: by its
    // SONAME on Linux.
#ifdef _WIN32
    constexpr std::string_view file = "dyncalc.dll";
    constexpr std::string_view beside = "dyncalc.dll";
#elifdef __APPLE__
    constexpr std::string_view file = "libdyncalc.dylib";
    constexpr std::string_view beside = "@rpath/libdyncalc.dylib";
#else
    constexpr std::string_view file = "libdyncalc.so";
    constexpr std::string_view beside = "libdyncalc.so.1";
#endif
    const std::filesystem::path packages =
        std::filesystem::path(FB_TEST_OUTPUT) / "dynamic_packages";
    std::filesystem::remove_all(packages);
    const auto write = [&](const std::string_view path, const std::string_view text) {
        std::filesystem::create_directories((packages / path).parent_path());
        std::ofstream(packages / path, std::ios::binary) << text;
    };
    write("calc/project.toml", "[project]\nname = \"calc\"\nversion = \"0.1.0\"\n\n"
                               "[native]\ndyncalc = { dynamic = 'native\\{target}\\dyncalc' }\n");
    write("calc/calc.fb", "Package Extern \"C\" Lib \"dyncalc\"\n"
                          "    Function dc_add(a: Int32, b: Int32): Int32\n"
                          "    Function dc_count(): Int64\n"
                          "End Extern\n"
                          "Public Function Add(a: Int32, b: Int32): Int32 => dc_add(a, b)\n"
                          "Public Function Count(): Int64 => dc_count()\n");
    write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                              "[dependencies]\ncalc = { path = \"../calc\" }\n");
    write("app/main.fb",
          "Import std.core\nImport calc\nPrint(\"{Add(2, 3)} {Count()} {Count()}\")\n");
    const std::filesystem::path native =
        packages / "calc/native" / fb::path_from_utf8(fb::build_target());
    std::filesystem::create_directories(native);
    std::filesystem::copy_file(std::filesystem::path(FB_TEST_DYNCALC),
                               native / fb::path_from_utf8(file));
#ifdef _WIN32
    const std::filesystem::path import(FB_TEST_DYNCALC_LINKED);
    std::filesystem::copy_file(import, native / import.filename());
#endif

    // fb build says what it copied, which is all the program loads but the system's own.
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(packages / "app") });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    const std::string copied = std::string(beside.substr(beside.rfind('/') + 1));
    CHECK(built.output.contains(
        std::format("Copied {} beside the executable, which needs it\n", copied)));
    std::filesystem::path executable = packages / "app/build/app";
    executable += fb::executable_extension;
    std::vector<std::string> libraries = loaded_libraries(executable);
    CHECK(std::ranges::contains(libraries, beside));
    std::erase(libraries, std::string(beside));
    CHECK(std::ranges::all_of(libraries, is_system_library));

    // The program runs from its build folder, wherever it's started from.
    const std::expected<fb::process_result_t, std::string> ran =
        fb::run_process(executable, {}, "");
    REQUIRE(ran);
    CHECK(ran->exit_code == 0);
    CHECK(ran->output == "5 1 2\n");

    // And from fb run's folder.
    const fb::process_result_t run = fb_run({ "run", fb::to_utf8(packages / "app") });
    INFO(run.output);
    CHECK(run.exit_code == 0);
    CHECK(run.output.contains("5 1 2\n"));

    // A program that calls none of it has it beside it too, since its package needs it. Only a
    // Windows executable leaves out what it doesn't call.
    write("quiet/project.toml", "[project]\nname = \"quiet\"\nversion = \"0.1.0\"\n\n"
                                "[dependencies]\ncalc = { path = \"../calc\" }\n");
    write("quiet/main.fb", "Import std.core\nPrint(\"quiet\")\n");
    const fb::process_result_t quiet = fb_run({ "build", fb::to_utf8(packages / "quiet") });
    INFO(quiet.output);
    REQUIRE(quiet.exit_code == 0);
    CHECK(quiet.output.contains(std::format("Copied {} beside the executable", copied)));
    CHECK(std::filesystem::is_regular_file(packages / "quiet/build" / fb::path_from_utf8(copied)));
#ifndef _WIN32
    std::filesystem::path silent = packages / "quiet/build/quiet";
    CHECK(std::ranges::contains(loaded_libraries(silent), beside));
#endif
}

TEST_CASE("a game uses the stand-in engine, a C++ DLL its package brings, from its build folder",
          "[cli]") {
    // The engine needs nothing but the OS's own libraries, its C++ runtime linked into it.
    const std::filesystem::path library(FB_TEST_ENGINE);
    const std::vector<std::string> needs = loaded_libraries(library);
    CHECK_FALSE(needs.empty());
    for (const std::string& name : needs) {
        INFO(name);
        CHECK(is_system_library(name));
    }

    // On Linux it exports only its C API, not the C++ runtime inside it.
#ifdef __linux__
    std::ifstream image(library, std::ios::binary);
    const std::vector<std::string> exports =
        elf_exports(std::string{ std::istreambuf_iterator<char>(image), {} });
    CHECK(std::ranges::contains(exports, "engine_world_new"));
    for (const std::string& name : exports) {
        INFO(name);
        CHECK(name.starts_with("engine_"));
    }
#endif

    // The engine package has it in native/<target>/, as a package ships it.
    const std::filesystem::path packages =
        std::filesystem::path(FB_TEST_OUTPUT) / "engine_packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "engine", "engine_game" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    const std::filesystem::path native =
        packages / "engine/native" / fb::path_from_utf8(fb::build_target());
    std::filesystem::create_directories(native);
    const std::array<std::filesystem::path, 2> files{ library, FB_TEST_ENGINE_LINKED };
    for (const std::filesystem::path& file : files) {
        std::filesystem::copy_file(file, native / file.filename(),
                                   std::filesystem::copy_options::skip_existing);
    }

    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(packages / "engine_game") });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    std::filesystem::path game = packages / "engine_game/build/engine_game";
    game += fb::executable_extension;
    CHECK(std::filesystem::is_regular_file(game.parent_path() / library.filename()));
    const std::expected<fb::process_result_t, std::string> ran = fb::run_process(game, {}, "");
    REQUIRE(ran);
    CHECK(ran->exit_code == 0);
    std::ifstream file(std::filesystem::path(FB_TEST_PACKAGES) / "engine_game.out",
                       std::ios::binary);
    const std::string expected{ std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>() };
    CHECK(ran->output == expected);
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

    // Nor over a header it reads, which is lost as the header would be.
    const std::filesystem::path types =
        write_source("bindgen/vec_types.h", "typedef struct { float x, y; } vec2;\n");
    const std::filesystem::path vec =
        write_source("bindgen/vec.h", "#include \"vec_types.h\"\nfloat vec_length(vec2 v);\n");
    check_usage_error({ "bindgen", fb::to_utf8(vec), "-o", fb::to_utf8(types) },
                      std::format("'-o' names {}, a header {} reads; give another path",
                                  fb::to_utf8(types), fb::to_utf8(vec)));
    std::ifstream untouched(types, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(untouched), {}) ==
          "typedef struct { float x, y; } vec2;\n");
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

    // A file the program embeds is one of its own too.
    const std::filesystem::path data = write_source("output/data.txt", "data");
    const std::filesystem::path embeds =
        write_source("output/embeds.fb", "Import std.core\nPrint(EmbedText(\"data.txt\"))\n");
    check_usage_error({ "build", fb::to_utf8(embeds), "-o", fb::to_utf8(data) },
                      "a file of the program; give another path");
    std::ifstream data_kept(data, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(data_kept), {}) == "data");

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

TEST_CASE("fb links with its own lld, with no C++ compiler on PATH", "[cli]") {
    // PATH is an empty folder's, so the linker and the C runtime's pieces come from fb's
    // toolchain. The program uses libuv, and its system libraries, through std.io.
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "no_tools";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "empty");
    const std::filesystem::path text = folder / "linked.txt";
    const std::filesystem::path source =
        write_source("no_tools/files.fb",
                     std::format("Import std.core\nImport std.io\n"
                                 "Try File.WriteText(Path(\"{0}\"), \"linked\")\n"
                                 "Print(Try File.ReadText(Path(\"{0}\")))\n",
                                 fb::to_utf8(std::filesystem::path(text.generic_string()))));
    // Outside Windows, whose names can't have them, the linker's arguments keep a quote and a
    // backslash.
#ifdef _WIN32
    std::filesystem::path executable = folder / "out folder" / "files";
#else
    std::filesystem::path executable = folder / "out \"quoted\" back\\slash" / "files";
#endif
    std::filesystem::create_directories(executable.parent_path());
    const environment_variable_t path("PATH", fb::to_utf8(folder / "empty"));
    const fb::process_result_t built =
        fb_run({ "build", fb::to_utf8(source), "-o", fb::to_utf8(executable) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    executable += fb::executable_extension;
    const std::vector<std::string> none;
    CHECK(fb::run_process(executable, none).value_or(fb::process_result_t{}).output == "linked\n");
}

TEST_CASE("on Windows a debug build writes a PDB beside each binary, and a release build none",
          "[cli]") {
#ifdef _WIN32
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "pdb";
    std::filesystem::remove_all(packages);
    const auto write = [&](const std::string_view path, const std::string_view text) {
        const std::filesystem::path file = packages / fb::path_from_utf8(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };
    write("lib/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("lib/lib.fb", "Public Function Twice(a: Int): Int => a * 2\n");
    write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                              "lib = { path = \"../lib\", link = \"beside\" }\n");
    write("app/main.fb", "Import std.core\nImport lib\nFunction Shown(): Int => Twice(2)\n"
                         "Print(\"pdb {Shown()}\")\n");

    // Each is an MSF file, which names the program's Functions.
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string{ std::istreambuf_iterator<char>(file), {} };
    };
    const fb::process_result_t debug = fb_run({ "build", fb::to_utf8(packages / "app") });
    INFO(debug.output);
    REQUIRE(debug.exit_code == 0);
    const std::vector<std::string> none;
    CHECK(fb::run_process(packages / "app/build/app.exe", none)
              .value_or(fb::process_result_t{})
              .output == "pdb 4\n");
    const std::string app = read(packages / "app/build/app.pdb");
    CHECK(app.starts_with("Microsoft C/C++ MSF 7.00"));
    CHECK(app.contains("Shown"));
    const std::string lib = read(packages / "app/build/lib.pdb");
    CHECK(lib.starts_with("Microsoft C/C++ MSF 7.00"));
    CHECK(lib.contains("Twice"));

    const fb::process_result_t release =
        fb_run({ "build", fb::to_utf8(packages / "app"), "--release" });
    INFO(release.output);
    REQUIRE(release.exit_code == 0);
    CHECK_FALSE(std::filesystem::exists(packages / "app/build/app.pdb"));
    CHECK_FALSE(std::filesystem::exists(packages / "app/build/lib.pdb"));
#else
    SKIP("only Windows' debuggers read a PDB");
#endif
}

TEST_CASE("fb builds where the temporary folder's name isn't in the ANSI code page", "[cli]") {
#ifdef _WIN32
    // Linking writes its files in the temporary folder, whose name here has a snowman, which the
    // ANSI code page doesn't.
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
    // A library beside the executable links with its import library there too.
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "tmp_beside";
    std::filesystem::remove_all(packages);
    const auto write = [&](const std::string_view path, const std::string_view text) {
        const std::filesystem::path file = packages / fb::path_from_utf8(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };
    write("lib/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("lib/lib.fb", "Public Function F(a: Int, b: Int): Int => a + b\n");
    write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                              "lib = { path = \"../lib\", link = \"beside\" }\n");
    write("app/main.fb", "Import std.core\nImport lib\nPrint(\"beside {F(1, 2)}\")\n");
    const fb::process_result_t beside = fb_run({ "run", fb::to_utf8(packages / "app") });
    _wputenv_s(L"TMP", old_tmp.c_str());
    _wputenv_s(L"TEMP", old_temp.c_str());
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.contains("hello\n"));
    INFO(beside.output);
    CHECK(beside.output.contains("beside 3\n"));
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

TEST_CASE("fb fmt formats a package's files in place, and --check lists the ones it would",
          "[cli]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "fmt_package";
    std::filesystem::remove_all(root);
    write_source("fmt_package/project.toml",
                 "[project]\nname = \"app\"\nversion = \"1.0.0\"\n\n[format]\nwidth = 40\n");
    write_source("fmt_package/main.fb", "Import std.core\nIf 1<2\nPrint( \"yes\" )\nEnd If\n");
    write_source("fmt_package/ui/hud.fb", "Function Shown(a: Int, b: Int, c: Int, d: Int): Int\n"
                                          "    Return a + b + c + d\nEnd Function\n");
    write_source("fmt_package/ui/done.fb", "Print(1)\n");
    // A package's build/ folder, and hidden ones, aren't its code.
    write_source("fmt_package/build/old.fb", "Print( 1 )\n");
    write_source("fmt_package/.hidden/old.fb", "Print( 1 )\n");
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(root / "ui");
    const fb::process_result_t checked = fb_run({ "fmt", "--check" });
    std::filesystem::current_path(before);

    INFO(checked.output);
    CHECK(checked.exit_code == 1);
    CHECK(checked.output.contains("main.fb\n"));
    CHECK(checked.output.contains("hud.fb\n"));
    CHECK_FALSE(checked.output.contains("done.fb"));
    CHECK_FALSE(checked.output.contains("old.fb"));
    CHECK(checked.output.contains("2 files aren't formatted; fb fmt formats them"));
    CHECK(read_file(root / "main.fb") == "Import std.core\nIf 1<2\nPrint( \"yes\" )\nEnd If\n");

    const fb::process_result_t formatted = fb_run({ "fmt", fb::to_utf8(root) });
    INFO(formatted.output);
    CHECK(formatted.exit_code == 0);
    CHECK(formatted.output.contains("Formatted"));
    CHECK(read_file(root / "main.fb") == "Import std.core\nIf 1 < 2\n    Print(\"yes\")\nEnd If\n");
    // project.toml's width is 40.
    CHECK(read_file(root / "ui" / "hud.fb") == "Function Shown(a: Int, b: Int, c: Int,\n"
                                               "               d: Int): Int\n"
                                               "    Return a + b + c + d\nEnd Function\n");
    CHECK(read_file(root / "build" / "old.fb") == "Print( 1 )\n");

    const fb::process_result_t again =
        fb_run({ "fmt", "--check", fb::to_utf8(root / "main.fb"), fb::to_utf8(root / "ui") });
    INFO(again.output);
    CHECK(again.exit_code == 0);
    CHECK(again.output.empty());
}

TEST_CASE("fb fmt leaves a file that doesn't parse, with its errors", "[cli]") {
    const std::filesystem::path broken = write_source("fmt_broken.fb", "Print( 1\n");
    const fb::process_result_t formatted = fb_run({ "fmt", fb::to_utf8(broken) });
    INFO(formatted.output);
    CHECK(formatted.exit_code == 1);
    CHECK(formatted.output.contains("fmt_broken.fb:1:9: error: expected ')'"));
    CHECK(formatted.output.contains("couldn't format 1 file"));
    CHECK(read_file(broken) == "Print( 1\n");
}

TEST_CASE("fb fmt takes a .fb file's extension in any letter case", "[cli]") {
    const std::filesystem::path loud = write_source("FMT_LOUD.FB", "Print( 2 )\n");
    const fb::process_result_t formatted = fb_run({ "fmt", fb::to_utf8(loud) });
    INFO(formatted.output);
    CHECK(formatted.exit_code == 0);
    CHECK(read_file(loud) == "Print(2)\n");
}

TEST_CASE("fb fmt formats a file it's given twice, by two paths, once", "[cli]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "fmt_twice";
    std::filesystem::remove_all(root);
    write_source("fmt_twice/src/main.fb", "Print( 1 )\n");
    const fb::process_result_t checked = fb_run(
        { "fmt", "--check", fb::to_utf8(root), fb::to_utf8(root / "." / "src" / "main.fb") });
    INFO(checked.output);
    CHECK(checked.exit_code == 1);
    CHECK(checked.output.contains("1 file isn't formatted"));
    std::size_t listed = 0;
    for (std::size_t at = checked.output.find("main.fb"); at != std::string::npos;
         at = checked.output.find("main.fb", at + 1)) {
        ++listed;
    }

    CHECK(listed == 1);
}

TEST_CASE("fb fmt shows project.toml's warnings, as a misspelled width", "[cli]") {
    const std::filesystem::path root = std::filesystem::path(FB_TEST_OUTPUT) / "fmt_typo";
    std::filesystem::remove_all(root);
    write_source("fmt_typo/project.toml",
                 "[project]\nname = \"app\"\nversion = \"1.0.0\"\n\n[format]\nwidht = 40\n");
    write_source("fmt_typo/main.fb", "Print(1)\n");
    write_source("fmt_typo/other.fb", "Print(2)\n");
    const fb::process_result_t checked = fb_run({ "fmt", "--check", fb::to_utf8(root) });
    INFO(checked.output);
    CHECK(checked.exit_code == 0);
    // Once for the package, not for each file.
    const std::size_t first = checked.output.find("warning: [format] doesn't use 'widht'");
    CHECK(first != std::string::npos);
    CHECK(checked.output.find("warning: [format] doesn't use 'widht'", first + 1) ==
          std::string::npos);
}

TEST_CASE("fb fmt's usage mistakes", "[cli]") {
    check_usage_error({ "fmt", fb::to_utf8(std::filesystem::path(FB_TEST_OUTPUT) / "nowhere") },
                      "there's no file or folder");
    check_usage_error({ "fmt", fb::to_utf8(write_source("fmt_text.txt", "text")) },
                      "fmt_text.txt isn't a .fb file");
    check_usage_error({ "fmt", "--release" },
                      "'--release' doesn't work with 'fb fmt', whose options are --check and "
                      "--color");
    check_usage_error({ "fmt", "--check=yes" }, "'--check' takes no value");
    check_usage_error({ "build", "--check" }, "'--check' only works with 'fb fmt'");

    const fb::process_result_t help = fb_run({ "help", "fmt" });
    CHECK(help.exit_code == 0);
    CHECK(help.output.contains("fb fmt [options] [files and folders]"));
    CHECK(help.output.contains("--check"));
    CHECK(fb_run({ "help" }).output.contains("Format .fb files in place"));
}

TEST_CASE("fb builds a program with a prebuilt library's binary in it", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "prebuilt";
    std::filesystem::remove_all(folder);
    const auto write = [&](const std::string_view path, const std::string_view text) {
        const std::filesystem::path file = folder / fb::path_from_utf8(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };

    // A library's Classes, generics, Globals, and errors, which a program uses from its binary.
    write("geo/project.toml",
          "[project]\nname = \"geo\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    // Its code calls xxHash's, which a program links for it.
    write("geo/shapes.fb", "Import std.core\n"
                           "Import std.crypto\n"
                           "Public Class Shape\n"
                           "    Public Field Name: String = \"shape\"\n"
                           "    Public Method Area(): Float64 => 0.0\n"
                           "    Public Method Describe(): String => \"{Name} {Area()}\"\n"
                           "End Class\n"
                           "Public Class Circle Extends Shape\n"
                           "    Public Field R: Float64\n"
                           "    Public New(r: Float64)\n"
                           "        R = r\n"
                           "        Name = \"circle\"\n"
                           "    End New\n"
                           "    Override Method Area(): Float64 => 3.0 * R * R\n"
                           "End Class\n"
                           "Public Class GeoError Extends Error\n"
                           "    Public New(message: String)\n"
                           "        Super.New(message)\n"
                           "    End New\n"
                           "End Class\n"
                           "Global made: Int = 0\n"
                           "Global names: String[] = [\"start\"]\n"
                           "Public Global Total = 10\n"
                           "Public Function Make(r: Float64): Shape Throws GeoError\n"
                           "    If r < 0.0 Then Throw GeoError(\"negative\")\n"
                           "    made += 1\n"
                           "    names.Add(\"made\")\n"
                           "    Return Circle(r)\n"
                           "End Function\n"
                           "Public Function Made(): String => \"{made} {names.Count}\"\n"
                           "Public Function Digest(text: String): UInt64 => XxHash.Hash(text)\n"
                           "Public Function Largest<T: Comparable>(items: T[]): T\n"
                           "    Local best = items[0]\n"
                           "    For each In items\n"
                           "        If each > best Then best = each\n"
                           "    End For\n"
                           "    Return best\n"
                           "End Function\n");
    write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                              "[dependencies]\ngeo = { path = \"../geo-prebuilt\" }\n");
    write("app/main.fb",
          "Import std.core\n"
          "Import geo\n"
          "Class Square Extends Shape\n"
          "    Field S: Float64 = 2.0\n"
          "    Override Method Area(): Float64 => S * S\n"
          "End Class\n"
          "Local c = Try Make(1.0)\n"
          "Print(c.Describe())\n"
          "Print(Square().Describe())\n"
          "Local s: Shape = Square()\n"
          "Print(\"{c Is Circle} {s Is Circle} {Made()} {Total}\")\n"
          "Print(\"{Largest([3, 9, 4])} {Largest([\"a\", \"c\", \"b\"])}\")\n"
          "Print(\"{Digest(\"abc\") == Digest(\"abc\")} {Digest(\"a\") != Digest(\"b\")}\")\n"
          "Try\n"
          "    Local bad = Try Make(-1.0)\n"
          "Catch e As GeoError\n"
          "    Print(\"caught {e.Message}\")\n"
          "End Try\n");
    const std::string geo = fb::to_utf8(folder / "geo");
    const std::string app = fb::to_utf8(folder / "app");
    const std::filesystem::path target = fb::path_from_utf8(fb::build_target());
    for (const bool release : { false, true }) {
        const std::string mode = release ? "release" : "debug";
        std::vector<std::string> arguments{ "build", geo };
        if (release) {
            arguments.emplace_back("--release");
        }

        const fb::process_result_t built = fb_run(arguments);
        INFO(built.output);
        REQUIRE(built.exit_code == 0);
        const std::filesystem::path from = folder / "geo" / "build" / target / mode;
        const std::filesystem::path to = folder / "geo-prebuilt" / "lib" / target / mode;
        std::filesystem::create_directories(to);
        for (const std::string_view file : { "geo.fbi", "geo.fblib" }) {
            CHECK(std::filesystem::exists(from / file));
            std::filesystem::copy_file(from / file, to / file,
                                       std::filesystem::copy_options::overwrite_existing);
        }
    }

    std::filesystem::copy_file(folder / "geo" / "project.toml",
                               folder / "geo-prebuilt" / "project.toml");
    const std::string printed = "circle 3\n"
                                "shape 4\n"
                                "True False 1 2 10\n"
                                "9 c\n"
                                "True True\n"
                                "caught negative\n";
    for (const bool release : { false, true }) {
        std::vector<std::string> arguments{ "run", app };
        if (release) {
            arguments.emplace_back("--release");
        }

        const fb::process_result_t ran = fb_run(arguments);
        INFO(ran.output);
        CHECK(ran.exit_code == 0);
        CHECK(ran.output.ends_with(printed));
    }

    // A program is built against one build of a library: its binary has to be its interface
    // file's.
    const std::filesystem::path debug = folder / "geo-prebuilt" / "lib" / target / "debug";
    std::filesystem::copy_file(folder / "geo-prebuilt" / "lib" / target / "release" / "geo.fblib",
                               debug / "geo.fblib",
                               std::filesystem::copy_options::overwrite_existing);
    const fb::process_result_t mismatched = fb_run({ "build", app });
    INFO(mismatched.output);
    CHECK(mismatched.exit_code == 1);
    CHECK(mismatched.output.contains("geo.fblib is the build geo v0.1.0 ("));
    CHECK(mismatched.output.contains("), but its interface file, "));
    CHECK(mismatched.output.contains("; a program is built against one build of a library"));

    std::ofstream(debug / "geo.fblib", std::ios::binary) << "not a binary";
    const fb::process_result_t broken = fb_run({ "build", app });
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.contains("geo.fblib isn't a library's binary that fb writes"));

    std::filesystem::remove(debug / "geo.fblib");
    const fb::process_result_t missing = fb_run({ "build", app });
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.contains(
        "geo.fblib, the prebuilt library's binary beside its interface file"));
}

namespace {

/// Writes files under `folder` by their paths in it.
struct folder_writer_t {
    std::filesystem::path folder;

    void operator()(const std::string_view path, const std::string_view text) const {
        const std::filesystem::path file = folder / fb::path_from_utf8(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
};

/// Builds the library in `folder`/`name` in debug, and copies its build into
/// `folder`/`name`-prebuilt, with its project.toml.
void prebuild(const std::filesystem::path& folder, const std::string_view name) {
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(folder / name) });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    const std::filesystem::path target = fb::path_from_utf8(fb::build_target());
    const std::filesystem::path from = folder / name / "build" / target / "debug";
    const std::filesystem::path prebuilt = folder / std::format("{}-prebuilt", name);
    std::filesystem::create_directories(prebuilt / "lib" / target / "debug");
    std::filesystem::copy_file(folder / name / "project.toml", prebuilt / "project.toml",
                               std::filesystem::copy_options::overwrite_existing);
    for (const std::string extension : { ".fbi", ".fblib" }) {
        const std::string file = std::string(name) + extension;
        std::filesystem::copy_file(from / file, prebuilt / "lib" / target / "debug" / file,
                                   std::filesystem::copy_options::overwrite_existing);
    }
}

/// A library whose docs have an example, a module doc, members, and blank lines and bodies
/// that its interface file leaves out.
void write_documented_geo(const folder_writer_t& write) {
    write("geo/project.toml",
          "[project]\nname = \"geo\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("geo/shapes.fb", "//! Shapes.\n"
                           "\n"
                           "Import std.core\n"
                           "\n"
                           "/// A circle.\n"
                           "Public Class Circle\n"
                           "    /// The radius.\n"
                           "    Public Field Radius: Float\n"
                           "\n"
                           "    /// Makes one.\n"
                           "    Public New(r: Float)\n"
                           "        Radius = r\n"
                           "    End New\n"
                           "\n"
                           "    /// Grows it.\n"
                           "    ///\n"
                           "    /// # Examples\n"
                           "    ///\n"
                           "    /// ```\n"
                           "    /// Local c = Circle(1)\n"
                           "    /// c.Grow(1)\n"
                           "    /// Expect(c.Radius == 2)\n"
                           "    /// ```\n"
                           "    @Inline\n"
                           "    Public Method Grow(by: Float)\n"
                           "        Radius += by\n"
                           "    End Method\n"
                           "End Class\n"
                           "\n"
                           "/// Twice `n`.\n"
                           "Public Function Twice(n: Int): Int\n"
                           "    Return n + n\n"
                           "End Function\n");
}

} // namespace

TEST_CASE("fb doc gives a prebuilt library's items the lines of its source", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "doc-prebuilt";
    std::filesystem::remove_all(folder);
    write_documented_geo({ .folder = folder });
    prebuild(folder, "geo");
    const fb::process_result_t source = fb_run({ "doc", fb::to_utf8(folder / "geo"), "-o", "-" });
    INFO(source.output);
    REQUIRE(source.exit_code == 0);
    CHECK(source.output.contains(R"("name": "Grow")"));
    const fb::process_result_t prebuilt =
        fb_run({ "doc", fb::to_utf8(folder / "geo-prebuilt"), "-o", "-" });
    INFO(prebuilt.output);
    REQUIRE(prebuilt.exit_code == 0);
    // The same JSON, between the lines that name the folder it documents.
    const auto json = [](const std::string& output) {
        const std::size_t begin = output.find('{');
        return output.substr(begin, output.rfind('}') + 1 - begin);
    };
    CHECK(json(prebuilt.output) == json(source.output));
}

TEST_CASE("fb test runs a prebuilt library's tests and examples against its binary", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "test-prebuilt";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ .folder = folder };
    write_documented_geo(write);
    prebuild(folder, "geo");
    write("geo-prebuilt/tests/grows.fb", "Import std.test\n"
                                         "Import geo.shapes\n"
                                         "@Test Function Twice2()\n"
                                         "    Expect(Twice(2) == 4)\n"
                                         "End Function\n");
    const fb::process_result_t tested =
        fb_run({ "test", fb::to_utf8(folder / "geo-prebuilt"), "--color", "never" });
    INFO(tested.output);
    CHECK(tested.exit_code == 0);
    CHECK(tested.output.contains("test geo.tests.grows.Twice2 ... ok"));
    CHECK(tested.output.contains("test geo.shapes.Circle.Grow (example) ... ok"));
    CHECK(tested.output.contains("2 passed; 0 failed"));
}

TEST_CASE("a prebuilt library's binary runs its code as its source would", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "prebuilt_runs";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    // A module with nothing the interface file shows still sets its Globals; a module can start
    // with a Class; a ThreadLocal and a Const whose values are worked out run as the source's do;
    // and debug forms show the library's Classes and the program's.
    write("base/project.toml",
          "[project]\nname = \"base\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("base/base.fb", "Global names: String[] = [\"b1\", \"b2\"]\n"
                          "Public Function BName(): String => names[1]\n");
    prebuild(folder, "base");
    write("lib/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n\n"
          "[dependencies]\nbase = { path = \"../base-prebuilt\" }\n");
    write("lib/a.fb", "Import std.core\n"
                      "Import lib.b\n"
                      "Import lib.c\n"
                      "Import base\n"
                      "Public Class Thing\n"
                      "    Public Field N: Int\n"
                      "    Method Destroy()\n"
                      "        Print(\"destroy {N}\")\n"
                      "    End Method\n"
                      "End Class\n"
                      "Global made: Int = 0\n"
                      "Function Make(): Thing\n"
                      "    made += 1\n"
                      "    Print(\"making {made}\")\n"
                      "    Local thing = Thing()\n"
                      "    thing.N = made\n"
                      "    Return thing\n"
                      "End Function\n"
                      "Public Const First: Thing = Make()\n"
                      "Function Compute(): Int => 40 + 2\n"
                      "Public ThreadLocal Global Seed: Int = Compute()\n"
                      "Public Function Get(): String => Helper() + BName() + \"{Held()}\"\n"
                      "Public Function Shown(thing: Thing): String => \"{thing:?}\"\n");
    write("lib/b.fb", "Global names: String[] = [\"start\", \"two\"]\n"
                      "Package Function Helper(): String => names[1]\n");
    write("lib/c.fb", "Public Class Holder\n"
                      "    Public Field V: Int = 5\n"
                      "End Class\n"
                      "Global held = Holder()\n"
                      "Package Function Held(): Int => held.V\n");
    prebuild(folder, "lib");
    write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                              "lib = { path = \"../lib-prebuilt\" }\n"
                              "base = { path = \"../base-prebuilt\" }\n");
    write("app/main.fb", "Import std.core\n"
                         "Import lib\n"
                         "Interface Counted\n"
                         "    Method Count(): Int\n"
                         "End Interface\n"
                         "Extend Thing Implements Counted\n"
                         "    Method Count(): Int => N * 10\n"
                         "End Extend\n"
                         "Global classes = 3\n"
                         "Print(\"{Get()} {First.N} {Seed} {classes}\")\n"
                         "Print(\"{Spawn(Function() => Seed).Wait()}\")\n"
                         "Local counted: Counted = First\n"
                         "Print(\"{counted.Count()} {Shown(First)} {First:?}\")\n");
    const fb::process_result_t ran = fb_run({ "run", fb::to_utf8(folder / "app") });
    INFO(ran.output);
    CHECK(ran.exit_code == 0);
    CHECK(ran.output.ends_with("making 1\n"
                               "twob25 1 42 3\n"
                               "42\n"
                               "10 Thing { N: 1 } Thing { N: 1 }\n"
                               "destroy 1\n"));

    // Its view of the LLVM IR is of the program with the library's binary.
    const fb::process_result_t shown =
        fb_run({ "check", fb::to_utf8(folder / "app"), "--show", "llvm" });
    INFO(shown.output);
    CHECK(shown.exit_code == 0);
    CHECK(shown.output.contains("lib.a.Get()"));

    // A library's build is the same wherever it's built.
    std::filesystem::create_directories(folder / "elsewhere");
    std::filesystem::copy(folder / "lib", folder / "elsewhere" / "lib",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy(folder / "base-prebuilt", folder / "elsewhere" / "base-prebuilt",
                          std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(folder / "elsewhere" / "lib" / "build");
    prebuild(folder / "elsewhere", "lib");
    const auto first_line = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::string line;
        std::getline(in, line);
        return line;
    };
    const std::filesystem::path interface =
        fb::path_from_utf8(std::format("lib/build/{}/debug/lib.fbi", fb::build_target()));
    CHECK(first_line(folder / interface) == first_line(folder / "elsewhere" / interface));

    // A .fblib whose code was changed isn't the build its interface file names.
    const std::filesystem::path binary = folder / "lib-prebuilt" / "lib" /
                                         fb::path_from_utf8(fb::build_target()) / "debug" /
                                         "lib.fblib";
    std::ifstream in(binary, std::ios::binary);
    std::string header;
    std::string bundled;
    std::getline(in, header);
    std::getline(in, bundled);
    in.close();
    std::ofstream(binary, std::ios::binary) << header << "\n" << bundled << "\nnot bitcode";
    const fb::process_result_t broken = fb_run({ "build", fb::to_utf8(folder / "app") });
    INFO(broken.output);
    CHECK(broken.exit_code == 1);
    CHECK(broken.output.contains("lib.fbi isn't the build its first line names, lib v0.1.0 ("));

    // A Class's record, and a T[]'s, is one in both binaries, so a leak report counts each type
    // once.
    write("nodes/project.toml",
          "[project]\nname = \"nodes\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("nodes/nodes.fb", "Public Class Node\n"
                            "    Public Field Items: Int[] = []\n"
                            "    Public Field Next: Node?\n"
                            "End Class\n"
                            "Public Function Leak()\n"
                            "    Local node = Node()\n"
                            "    node.Next = node\n"
                            "End Function\n");
    prebuild(folder, "nodes");
    write("leaky/project.toml", "[project]\nname = \"leaky\"\nversion = \"0.1.0\"\n\n"
                                "[dependencies]\nnodes = { path = \"../nodes-prebuilt\" }\n");
    write("leaky/main.fb", "Import nodes\n"
                           "Leak()\n"
                           "Local node = Node()\n"
                           "node.Next = node\n");
    const fb::process_result_t leaked = fb_run({ "run", fb::to_utf8(folder / "leaky") });
    INFO(leaked.output);
    CHECK(leaked.output.contains("leak: 2 Node objects were never released"));
    CHECK(leaked.output.contains("leak: 2 Int32[] objects were never released"));

    // Two things one name names can't cross a binary.
    write("alike/project.toml",
          "[project]\nname = \"alike\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("alike/a.fb", "Public Class b\n    Public Static Method Z(): Int => 1\nEnd Class\n");
    write("alike/a/b.fb", "Public Function Z(): Int => 2\n");
    const fb::process_result_t alike = fb_run({ "build", fb::to_utf8(folder / "alike") });
    INFO(alike.output);
    CHECK(alike.exit_code == 1);
    CHECK(alike.output.contains("'alike.a.b.Z()' names two things, which a library's binary and a "
                                "program that links it can't tell apart; rename one"));
}

TEST_CASE("a library beside the executable shares one runtime with it", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "beside";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    // Objects, Classes a program extends, generic instances, Tasks, ThreadLocals, Globals, and
    // errors cross between them, and a leak in either is counted once.
    write("engine/project.toml",
          "[project]\nname = \"engine\"\nversion = \"0.3.0\"\ntype = \"library\"\n");
    write("engine/engine.fb", "Import std.core\n"
                              "Public Class Body\n"
                              "    Public Field Name: String = \"body\"\n"
                              "    Public Field Items: Int[] = []\n"
                              "    Public Field Next: Body?\n"
                              "    Public Method Mass(): Int => 1\n"
                              "    Public Method Describe(): String => \"{Name} {Mass()}\"\n"
                              "End Class\n"
                              "Public Class EngineError Extends Error\n"
                              "    Public New(message: String)\n"
                              "        Super.New(message)\n"
                              "    End New\n"
                              "End Class\n"
                              "Global steps: Int = 0\n"
                              "Public Global Gravity = 10\n"
                              "Function Seeded(): Int => 40 + 2\n"
                              "Public ThreadLocal Global Seed: Int = Seeded()\n"
                              "Public Function Step(body: Body): Body Throws EngineError\n"
                              "    If body.Mass() < 0 Then Throw EngineError(\"negative\")\n"
                              "    steps += 1\n"
                              "    Return body\n"
                              "End Function\n"
                              "Public Function Steps(): Int => steps\n"
                              "Public Function Pair<T>(a: T, b: T): T[] => [a, b]\n"
                              "Public Function Count(items: Int[]): Int => items.Count\n"
                              "Async Function Slowly(n: Int): Int\n"
                              "    Return Await Spawn(Function() => n * 2)\n"
                              "End Function\n"
                              "Public Async Function Simulate(n: Int): Int\n"
                              "    Return Await Slowly(n) + 1\n"
                              "End Function\n"
                              "Public Function Leak()\n"
                              "    Local body = Body()\n"
                              "    body.Next = body\n"
                              "End Function\n");
    write("game/project.toml", "[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                               "engine = { path = \"../engine\", link = \"beside\" }\n");
    write("game/main.fb", "Import std.core\n"
                          "Import engine\n"
                          "Class Heavy Extends Body\n"
                          "    Override Method Mass(): Int => 5\n"
                          "End Class\n"
                          "Async Function Run(): Int\n"
                          "    Return Await Simulate(20)\n"
                          "End Function\n"
                          "Local heavy: Body = Try Step(Heavy())\n"
                          "Print(\"{heavy.Describe()} {heavy Is Heavy} {Steps()} {Gravity}\")\n"
                          "Print(\"{Count(Pair(1, 2))} {Pair(\"a\", \"b\").Count} {Seed}\")\n"
                          "Print(\"{Spawn(Function() => Seed).Wait()} {Run().Wait()}\")\n"
                          "Class Light Extends Body\n"
                          "    Override Method Mass(): Int => -1\n"
                          "End Class\n"
                          "Try\n"
                          "    Local light = Try Step(Light())\n"
                          "Catch e As EngineError\n"
                          "    Print(\"caught {e.Message}\")\n"
                          "End Try\n"
                          "Leak()\n"
                          "Local mine = Body()\n"
                          "mine.Next = mine\n");
    const std::string printed = "body 5 True 1 10\n"
                                "2 2 42\n"
                                "42 41\n"
                                "caught negative\n";
    for (const bool release : { false, true }) {
        std::vector<std::string> arguments{ "run", fb::to_utf8(folder / "game") };
        if (release) {
            arguments.emplace_back("--release");
        }

        const fb::process_result_t ran = fb_run(arguments);
        INFO(ran.output);
        CHECK(ran.output.contains(printed));
        if (!release) {
            CHECK(ran.output.contains("leak: 2 Body objects were never released"));
            CHECK(ran.output.contains("leak: 2 Int32[] objects were never released"));
        }
    }

    // fb build puts it beside the executable, as the OS's own kind of library.
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(folder / "game") });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    std::filesystem::path executable = folder / "game" / "build" / "game";
    executable += fb::executable_extension;
#ifdef _WIN32
    const std::filesystem::path library = folder / "game" / "build" / "engine.dll";
#elifdef __APPLE__
    const std::filesystem::path library = folder / "game" / "build" / "libengine.dylib";
#else
    const std::filesystem::path library = folder / "game" / "build" / "libengine.so";
#endif
    REQUIRE(std::filesystem::exists(library));
    const std::vector<std::string> none;
    const fb::process_result_t direct =
        fb::run_process(executable, none).value_or(fb::process_result_t{ .exit_code = -1 });
    CHECK(direct.output.contains(printed));

    // A file that's missing, or another build, stops the program before its code runs.
    std::filesystem::rename(library, folder / "kept");
    const fb::process_result_t missing =
        fb::run_process(executable, none).value_or(fb::process_result_t{ .exit_code = -1 });
    CHECK(missing.exit_code == 1);
    CHECK(missing.output.contains(
        std::format("{} isn't there, or can't be loaded; the program needs it beside it",
                    fb::to_utf8(library.filename()))));
    CHECK_FALSE(missing.output.contains("body"));
    std::filesystem::rename(folder / "kept", library);
    write("engine/more.fb", "Public Function More(): Int => 1\n");
    const fb::process_result_t rebuilt = fb_run(
        { "build", fb::to_utf8(folder / "game"), "-o", fb::to_utf8(folder / "other" / "game") });
    REQUIRE(rebuilt.exit_code == 0);
    std::filesystem::copy_file(folder / "other" / library.filename(), library,
                               std::filesystem::copy_options::overwrite_existing);
    const fb::process_result_t mismatched =
        fb::run_process(executable, none).value_or(fb::process_result_t{ .exit_code = -1 });
    INFO(mismatched.output);
    CHECK(mismatched.exit_code == 1);
    CHECK(mismatched.output.contains(
        std::format("{} is the build engine 0.3.0 ", fb::to_utf8(library.filename()))));
    CHECK(mismatched.output.contains(", but the program was built against engine 0.3.0 "));
#ifdef _WIN32
    // A Windows executable keeps the name it was built with, which its libraries take its code by.
    std::filesystem::copy_file(folder / "other" / "game.exe", folder / "other" / "renamed.exe");
    const fb::process_result_t renamed = fb::run_process(folder / "other" / "renamed.exe", none)
                                             .value_or(fb::process_result_t{ .exit_code = -1 });
    CHECK(renamed.exit_code == 1);
    CHECK(renamed.output.contains("renamed.exe was built as game.exe, which the library "
                                  "engine.dll beside it takes its code from; give it that name "
                                  "again"));
#endif

    // A prebuilt library goes beside it too.
    prebuild(folder, "engine");
    write("prebuilt/project.toml", "[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n"
                                   "[dependencies]\nengine = { path = \"../engine-prebuilt\", "
                                   "link = \"beside\" }\n");
    write("prebuilt/main.fb", "Import std.core\n"
                              "Import engine\n"
                              "Print(\"{Body().Describe()} {Gravity} {Simulate(3).Wait()}\")\n");
    const fb::process_result_t prebuilt = fb_run({ "run", fb::to_utf8(folder / "prebuilt") });
    INFO(prebuilt.output);
    CHECK(prebuilt.output.contains("body 1 10 7\n"));

    // Only the program says how a library links, and its file can't take another's name.
    write("engine/project.toml",
          "[project]\nname = \"engine\"\nversion = \"0.3.0\"\ntype = "
          "\"library\"\n\n[dependencies]\nkernel32 = { path = \"../kernel32\", "
          "link = \"beside\" }\n");
    write("kernel32/project.toml",
          "[project]\nname = \"kernel32\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("kernel32/k.fb", "Public Function K(): Int => 1\n");
    const fb::process_result_t ignored = fb_run({ "check", fb::to_utf8(folder / "engine") });
    CHECK(ignored.output.contains(
        "only a program's own project.toml says how a library links, so this 'link' is ignored"));
#ifdef _WIN32
    write("clash/project.toml", "[project]\nname = \"clash\"\nversion = \"0.1.0\"\n\n"
                                "[dependencies]\nkernel32 = { path = \"../kernel32\", link = "
                                "\"beside\" }\n");
    write("clash/main.fb", "Import std.core\nImport kernel32\nPrint(\"{K()}\")\n");
    const fb::process_result_t clash = fb_run({ "build", fb::to_utf8(folder / "clash") });
    CHECK(clash.exit_code == 1);
    CHECK(clash.output.contains("the library kernel32 would go beside the executable as "
                                "kernel32.dll, which is the name of kernel32, a library of the "
                                "OS's; rename the package"));
#endif
}

TEST_CASE("a beside library's own dependencies go in the executable or beside it too", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "beside_deps";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    write("util/project.toml",
          "[project]\nname = \"util\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("util/util.fb", "Global calls: Int = 0\n"
                          "Public Function Twice(n: Int): Int\n"
                          "    calls += 1\n"
                          "    Return n * 2\n"
                          "End Function\n"
                          "Public Function Calls(): Int => calls\n"
                          "Public Class Thing\n"
                          "End Class\n"
                          "Public Class Box Extends Thing\n"
                          "End Class\n");
    write("engine/project.toml", "[project]\nname = \"engine\"\nversion = \"0.1.0\"\ntype = "
                                 "\"library\"\n\n[dependencies]\nutil = { path = \"../util\" }\n");
    write("engine/engine.fb", "Import util\n"
                              "Public Function Run(): Int => Twice(5)\n"
                              "Public Function MakeBox(): Thing => Box()\n");
    write("game/main.fb", "Import std.core\n"
                          "Import engine\n"
                          "Import util\n"
                          "Print(\"{Run()} {Twice(1)} {Calls()} {MakeBox() Is Box}\")\n");
    // The one copy of util is the executable's, or its own file's, so its Global counts both
    // calls.
    for (const std::string_view link : { "embed", "beside" }) {
        write("game/project.toml",
              std::format("[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                          "engine = {{ path = \"../engine\", link = \"beside\" }}\n"
                          "util = {{ path = \"../util\", link = \"{}\" }}\n",
                          link));
        for (const bool release : { false, true }) {
            std::vector<std::string> arguments{ "run", fb::to_utf8(folder / "game") };
            if (release) {
                arguments.emplace_back("--release");
            }

            const fb::process_result_t ran = fb_run(arguments);
            INFO(link << ' ' << release << ' ' << ran.output);
            CHECK(ran.output.contains("10 2 2 True\n"));
        }
    }
}

TEST_CASE("a generic instance made on both sides of a beside library is one type", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "beside_edge";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    write("lib/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("lib/lib.fb", "Import std.collections\n"
                        "Public Interface Shape\n"
                        "    Method Area(): Int\n"
                        "End Interface\n"
                        "Public Interface Boxed\n"
                        "End Interface\n"
                        "Public Class Box<T> Implements Boxed\n"
                        "    Public Field Value: T\n"
                        "    Public New(value: T)\n"
                        "        Self.Value = value\n"
                        "    End New\n"
                        "End Class\n"
                        "Public Function IntBox(): Box<Int> => Box<Int>(4)\n"
                        "Public Function StringBox(): Box<String> => Box<String>(\"s\")\n"
                        "Public Function IsInts(b: Boxed): Bool => b Is Box<Int>\n"
                        "Public Function Names(): Deque<String>\n"
                        "    Local names = Deque<String>()\n"
                        "    names.Add(\"a\")\n"
                        "    Return names\n"
                        "End Function\n"
                        "Public Function Total(items: Deque<String>): Int => items.Count\n"
                        "Public Function AreaOf(shape: Shape): Int => shape.Area()\n");
    write("app/project.toml", "[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                              "lib = { path = \"../lib\", link = \"beside\" }\n");
    // Box<Int>, Box<String>, and Deque<String> are each made on both sides, and an Interface
    // that one side declares the other implements.
    write("app/main.fb", "Import std.core\n"
                         "Import std.collections\n"
                         "Import lib\n"
                         "Class Square Implements Shape\n"
                         "    Method Area(): Int => 9\n"
                         "End Class\n"
                         "Local mine: Boxed = Box<Int>(2)\n"
                         "Local theirs: Boxed = IntBox()\n"
                         "Local strings: Boxed = StringBox()\n"
                         "Print(\"{IsInts(mine)} {IsInts(strings)} {theirs Is Box<Int>} "
                         "{theirs Is Box<String>} {strings Is Box<String>}\")\n"
                         "Local names = Names()\n"
                         "names.Add(\"b\")\n"
                         "Local more = Deque<String>()\n"
                         "more.Add(\"c\")\n"
                         "Print(\"{Total(names)} {Total(more)} {AreaOf(Square())}\")\n"
                         "Print(\"{IntBox():?} {names:?}\")\n");
    for (const bool release : { false, true }) {
        std::vector<std::string> arguments{ "run", fb::to_utf8(folder / "app") };
        if (release) {
            arguments.emplace_back("--release");
        }

        const fb::process_result_t ran = fb_run(arguments);
        INFO(ran.output);
        CHECK(ran.output.contains("True False True False True\n"
                                  "2 1 9\n"
                                  "Box<Int32> { Value: 4 } Deque<String> [\"a\", \"b\"]\n"));
        CHECK_FALSE(ran.output.contains("leak"));
    }
}

namespace {

/// The file a library beside the executable is, by the OS's names for one.
std::string library_file(const std::string_view name) {
#ifdef _WIN32
    return std::format("{}.dll", name);
#elifdef __APPLE__
    return std::format("lib{}.dylib", name);
#else
    return std::format("lib{}.so", name);
#endif
}

} // namespace

TEST_CASE("a library's code runs beside the executable as it does embedded", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "beside_runs";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const folder_writer_t write{ folder };
    std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / "vmath", folder / "vmath",
                          std::filesystem::copy_options::recursive);
    const std::filesystem::path archive(FB_TEST_VECMATH);
    std::filesystem::create_directories(folder / "vmath" / "libs");
    std::filesystem::copy_file(archive, folder / "vmath" / "libs" / archive.filename());
    write("ut/project.toml", "[project]\nname = \"ut\"\nversion = \"0.1.0\"\ntype = "
                             "\"library\"\n\n[native]\nm = { system = \"m\" }\n");
    write("ut/ut.fb", "Function Seed(): Int => 40 + 2\n"
                      "Public ThreadLocal Global Tl: Int = Seed()\n"
                      "Public Extern \"C\" Lib \"m\"\n"
                      "    Function ceil(x: Float64): Float64\n"
                      "End Extern\n");
    // Functions of two parameters, Methods, and Operators, large results, a Struct Global's
    // Fields, a tuple Global, a Static Field, a ThreadLocal of a package and of std's that only
    // the library uses, a static library of a package it uses, system libraries' Externs that
    // the program and the library call, and a Function's Callback.
    write("lib/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n\n"
          "[dependencies]\nut = { path = \"../ut\" }\nvmath = { path = \"../vmath\" }\n\n"
          "[native]\nm = { system = \"m\" }\n");
    write("lib/lib.fb", "Import std.math\n"
                        "Import ut\n"
                        "Import vmath\n"
                        "Public Struct Big\n"
                        "    Public Field A: Int\n"
                        "    Public Field B: Int\n"
                        "    Public Field C: Int\n"
                        "    Public Field D: Int\n"
                        "    Public Field E: Int\n"
                        "End Struct\n"
                        "Public Global Origin: Big = Big(1, 2, 3, 4, 5)\n"
                        "Public Global Pair: (Int, Int) = (3, 4)\n"
                        "Public Class Counted\n"
                        "    Public Static Field Count: Int = 9\n"
                        "End Class\n"
                        "Function Twice(a: Int, b: Int): Int => (a + b) * 2\n"
                        "Public Function Both(): Int => Twice(1, 2)\n"
                        "Public Function Up(): Float64 => ceil(1.5)\n"
                        "Public Function Order(a: Ptr<Void>, b: Ptr<Void>): Int32 => 0\n"
                        "Public Function Ordering(): Callback(Ptr<Void>, Ptr<Void>): Int32 => "
                        "Order\n"
                        "Public Extern \"C\" Lib \"m\"\n"
                        "    Function floor(x: Float64): Float64\n"
                        "End Extern\n"
                        "Public Function Root<T>(x: T): Float64 => floor(3.5)\n"
                        "Public Function Sum(a: Int32, b: Int32): Int32 => Add(a, b)\n"
                        "Public Function Quad(n: Int): (Int, Int, Int, Int) => (n, n, n, n)\n"
                        "Public Function Make(n: Int): Big => Big(n, n, n, n, n)\n"
                        "Public Class Box\n"
                        "    Public Field N: Int = 1\n"
                        "    Public Method Total(a: Int, b: Int): Int => a + b + N\n"
                        "    Operator +(a: Box, b: Box): Int => a.N + b.N\n"
                        "End Class\n"
                        "Public Function Roll(): Bool => Rand() != Rand()\n"
                        "Public Function Run(): Int\n"
                        "    Tl += 10\n"
                        "    Return Tl\n"
                        "End Function\n");
    write("game/main.fb",
          "Import std.core\n"
          "Import lib\n"
          "Origin.B = 20\n"
          "Print(\"{Sum(2, 3)} {Quad(3).1} {Make(7).E} {Box().Total(1, 2)} "
          "{Box() + Box()} {Origin.B} {Origin.E}\")\n"
          "Print(\"{Roll()} {Run()} {Run()} {Spawn(Function() => Run()).Wait()}\")\n"
          "Print(\"{floor(2.5)} {Root(1)}\")\n"
          "Counted.Count += 1\n"
          "Print(\"{Pair.1} {Counted.Count} {Both()} {Up()}\")\n"
          "Unsafe\n"
          "    Print(\"{Ordering() == Order}\")\n"
          "End Unsafe\n");
    const std::string printed = "5 3 7 4 2 20 5\n"
                                "True 52 62 52\n"
                                "2 3\n"
                                "4 10 6 2\n"
                                "True\n";
    prebuild(folder, "lib");
    for (const std::string_view from : { "lib", "lib-prebuilt" }) {
        for (const std::string_view link : { "embed", "beside" }) {
            write("game/project.toml",
                  std::format("[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n"
                              "[dependencies]\nlib = {{ path = \"../{}\", link = \"{}\" }}\n",
                              from, link));
            // The prebuilt library is a debug build.
            for (const bool release : { false, true }) {
                if (release && from == "lib-prebuilt") {
                    continue;
                }

                std::vector<std::string> arguments{ "run", fb::to_utf8(folder / "game") };
                if (release) {
                    arguments.emplace_back("--release");
                }

                const fb::process_result_t ran = fb_run(arguments);
                INFO(from << ' ' << link << ' ' << release << ' ' << ran.output);
                CHECK(ran.output.contains(printed));
                CHECK_FALSE(ran.output.contains("leak"));
            }
        }
    }

    // fb build says which files go beside the executable, and the bundled libraries only a
    // library beside it calls. Its LLVM IR shows the library's functions as declarations.
    write("hash/project.toml",
          "[project]\nname = \"hash\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("hash/hash.fb", "Import std.crypto.checksums\n"
                          "Public Function H(): UInt64 => XxHash.Hash(\"abc\".Bytes)\n");
    write("hashed/project.toml",
          "[project]\nname = \"hashed\"\nversion = \"0.1.0\"\n\n"
          "[dependencies]\nhash = { path = \"../hash\", link = \"beside\" }\n");
    write("hashed/main.fb", "Import std.core\nImport hash\nPrint(\"{H() > 0}\")\n");
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(folder / "hashed") });
    INFO(built.output);
    CHECK(built.exit_code == 0);
    CHECK(built.output.contains(
        std::format("Built {} beside the executable, which needs it\n", library_file("hash"))));
    CHECK(built.output.contains("Bundled xxHash "));
    const fb::process_result_t shown =
        fb_run({ "build", fb::to_utf8(folder / "hashed"), "--show", "llvm" });
    INFO(shown.output);
    CHECK(shown.exit_code == 0);
    CHECK(shown.output.contains("declare i64 @\"fb.hash.hash.H()\"()"));
}

TEST_CASE("libraries beside the executable use each other's code and Globals", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "beside_both";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    write("util/project.toml",
          "[project]\nname = \"util\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("util/util.fb", "Import std.core\n"
                          "Function Make(): Int\n"
                          "    Print(\"util init\")\n"
                          "    Return 7\n"
                          "End Function\n"
                          "Public Global Base: Int = Make()\n"
                          "Global hidden: Int = Make() + 1\n"
                          "Public ThreadLocal Global Count: Int = Make() * 2\n"
                          "Public Function Hidden(): Int => hidden\n"
                          "Public Function Times(a: Int, b: Int): Int => a * b\n");
    write("engine/project.toml", "[project]\nname = \"engine\"\nversion = \"0.1.0\"\ntype = "
                                 "\"library\"\n\n[dependencies]\nutil = { path = \"../util\" }\n");
    write("engine/engine.fb", "Import std.core\n"
                              "Import util\n"
                              "Global scaled: Int = Times(Base, 10)\n"
                              "Public Function Run(): Int\n"
                              "    Count += 1\n"
                              "    Return scaled + Hidden() + Count\n"
                              "End Function\n");
    write("game/main.fb", "Import std.core\n"
                          "Import engine\n"
                          "Import util\n"
                          "Base += 1\n"
                          "Print(\"main {Run()} {Run()} {Base} {Count}\")\n");
    // util's Globals are one copy, set once, wherever each library is listed.
    for (const std::string_view order : { "util engine", "engine util" }) {
        for (const std::string_view util : { "beside", "embed" }) {
            const std::string engine_line =
                "engine = { path = \"../engine\", link = \"beside\" }\n";
            const std::string util_line =
                std::format("util = {{ path = \"../util\", link = \"{}\" }}\n", util);
            write("game/project.toml",
                  "[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n[dependencies]\n" +
                      (order == "util engine" ? util_line + engine_line : engine_line + util_line));
            for (const bool release : { false, true }) {
                std::vector<std::string> arguments{ "run", fb::to_utf8(folder / "game") };
                if (release) {
                    arguments.emplace_back("--release");
                }

                const fb::process_result_t ran = fb_run(arguments);
                INFO(order << ' ' << util << ' ' << release << ' ' << ran.output);
                CHECK(ran.output.contains("util init\nutil init\nutil init\nmain 93 94 8 16\n"));
            }
        }
    }
}

TEST_CASE("fb names what keeps a library beside the executable from building or loading", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "beside_names";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    write("lib/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("lib/lib.fb", "Public Function F(n: Int): Int => n + 1\n");
    const std::string uses_lib = "[dependencies]\nlib = { path = \"../lib\", link = \"beside\" }\n";

    // Two of one name but for letter case, which on Windows can't be two folders either.
    write("upper/project.toml",
          "[project]\nname = \"Lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("upper/lib.fb", "Public Function G(): Int => 1\n");
    write("cased/project.toml", "[project]\nname = \"cased\"\nversion = \"0.1.0\"\n\n" + uses_lib +
                                    "Lib = { path = \"../upper\", link = \"beside\" }\n");
    write("cased/main.fb", "Import std.core\nImport lib\nImport Lib\nPrint(\"{F(1)} {G()}\")\n");
    const fb::process_result_t cased = fb_run({ "build", fb::to_utf8(folder / "cased") });
    CHECK(cased.exit_code == 1);
    CHECK(cased.output.contains(std::format(
        "error: the libraries Lib and lib would both go beside the executable as {}; rename one",
        library_file("lib"))));

    // A library named after a system library of its own.
#ifdef _WIN32
    const std::string_view system = "dwmapi";
#else
    const std::string_view system = "z";
#endif
    write(std::format("{}/project.toml", system),
          std::format("[project]\nname = \"{0}\"\nversion = \"0.1.0\"\ntype = \"library\"\n\n"
                      "[native]\n{0} = {{ system = \"{0}\" }}\n",
                      system));
    write(std::format("{}/k.fb", system), "Public Function K(): Int => 1\n");
    write("taken/project.toml",
          std::format("[project]\nname = \"taken\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                      "{0} = {{ path = \"../{0}\", link = \"beside\" }}\n",
                      system));
    write("taken/main.fb", std::format("Import std.core\nImport {}\nPrint(\"{{K()}}\")\n", system));
    const fb::process_result_t taken = fb_run({ "build", fb::to_utf8(folder / "taken") });
    INFO(taken.output);
    CHECK(taken.exit_code == 1);
    CHECK(taken.output.contains(
        std::format("the library {0} would go beside the executable as {1}, which is the name of "
                    "{0}, a library of the OS's; rename the package",
                    system, library_file(system))));

    // A program built from a main file given to it, in a package with none of its own.
    write("given/project.toml", "[project]\nname = \"given\"\nversion = \"0.1.0\"\n\n" + uses_lib);
    write("given/app.fb", "Import std.core\nImport lib\nPrint(\"app {F(1)}\")\n");
    const fb::process_result_t given = fb_run({ "build", fb::to_utf8(folder / "given/app.fb") });
    INFO(given.output);
    CHECK(given.exit_code == 0);
    CHECK_FALSE(given.output.contains("'link' is ignored"));
    CHECK(std::filesystem::exists(folder / "given/build" / library_file("lib")));

    // An error in a library's build is at its line, which fb check shows too.
    write("emb/project.toml",
          "[project]\nname = \"emb\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("emb/data.txt", "data");
    write("emb/emb.fb", "Public Function Text<T>(x: T): String => EmbedText(\"data.txt\")\n"
                        "Public Function More<T>(x: T): String => EmbedText(\"data.txt\")\n");
    write("embeds/project.toml",
          "[project]\nname = \"embeds\"\nversion = \"0.1.0\"\n\n"
          "[dependencies]\nemb = { path = \"../emb\", link = \"beside\" }\n");
    write("embeds/main.fb", "Import std.core\nImport emb\nPrint(Text(1))\n");
    const fb::process_result_t embeds = fb_run({ "run", fb::to_utf8(folder / "embeds") });
    INFO(embeds.output);
    CHECK(embeds.exit_code == 1);
    CHECK(embeds.output.contains("error: emb, the library beside the program, can't be built: "));
    CHECK(embeds.output.contains("emb.fb:1:42: the library's interface file keeps this code"));
    CHECK(embeds.output.contains("in the library; it has 1 more, which `fb check "));
    CHECK_FALSE(embeds.output.contains("\n\n"));
    const fb::process_result_t checked = fb_run({ "check", fb::to_utf8(folder / "emb") });
    CHECK(checked.exit_code == 1);
    CHECK(checked.output.contains("emb.fb:1:42: error: the library's interface file keeps this "
                                  "code"));

    // A library's file that can't be written leaves no executable that would need it.
    write("blocked/project.toml",
          "[project]\nname = \"blocked\"\nversion = \"0.1.0\"\n\n" + uses_lib);
    write("blocked/main.fb", "Import std.core\nImport lib\nPrint(\"{F(1)}\")\n");
    std::filesystem::create_directories(folder / "blocked/build" / library_file("lib"));
    const fb::process_result_t blocked = fb_run({ "build", fb::to_utf8(folder / "blocked") });
    INFO(blocked.output);
    CHECK(blocked.exit_code == 1);
    std::filesystem::path unneeded = folder / "blocked/build/blocked";
    unneeded += fb::executable_extension;
    CHECK_FALSE(std::filesystem::exists(unneeded));

    // Two programs of one package share its build folder, and a library built for the other
    // stops the first by name, on Windows, where it takes its code from it by its name.
    write("two/project.toml", "[project]\nname = \"two\"\nversion = \"0.1.0\"\n\n" + uses_lib);
    write("two/main.fb", "Import std.core\nImport lib\nPrint(\"two {F(1)}\")\n");
    write("two/tool.fb", "Import std.core\nImport lib\nPrint(\"tool {F(2)}\")\n");
    REQUIRE(fb_run({ "build", fb::to_utf8(folder / "two") }).exit_code == 0);
    REQUIRE(fb_run({ "build", fb::to_utf8(folder / "two/tool.fb") }).exit_code == 0);
    std::filesystem::path two = folder / "two/build/two";
    two += fb::executable_extension;
    std::filesystem::path tool = folder / "two/build/tool";
    tool += fb::executable_extension;
    const std::vector<std::string> none;
    CHECK(fb::run_process(tool, none).value_or(fb::process_result_t{}).output == "tool 3\n");
    const fb::process_result_t first =
        fb::run_process(two, none).value_or(fb::process_result_t{ .exit_code = -1 });
#ifdef _WIN32
    CHECK(first.exit_code == 1);
    CHECK(first.output.contains("lib.dll was built for tool.exe, which it takes its code from, "
                                "not two.exe; build two.exe again\n"));
    // It says so when that executable is gone too, though then it can't be loaded.
    std::filesystem::remove(tool);
    const fb::process_result_t gone =
        fb::run_process(two, none).value_or(fb::process_result_t{ .exit_code = -1 });
    CHECK(gone.exit_code == 1);
    CHECK(gone.output.contains("lib.dll was built for tool.exe"));
    REQUIRE(fb_run({ "build", fb::to_utf8(folder / "two/tool.fb") }).exit_code == 0);
#else
    CHECK(first.output == "two 2\n");
#endif

    // A file of another build that takes what this executable doesn't have still says it's that
    // build, though it can't be bound.
    write("swap/project.toml", "[project]\nname = \"swap\"\nversion = \"0.1.0\"\n\n" + uses_lib);
    write("swap/main.fb", "Import std.core\nImport lib\nPrint(\"{F(1)}\")\n");
    REQUIRE(fb_run({ "build", fb::to_utf8(folder / "swap") }).exit_code == 0);
    write("newer/project.toml",
          "[project]\nname = \"lib\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("newer/lib.fb", "Import std.text\n"
                          "Public Function F(n: Int): Int => n + 1\n"
                          "Public Function G(s: String): String => s.ToUpper()\n");
    write("swapped/project.toml",
          "[project]\nname = \"swap\"\nversion = \"0.1.0\"\n\n"
          "[dependencies]\nlib = { path = \"../newer\", link = \"beside\" }\n");
    write("swapped/main.fb", "Import std.core\nImport lib\nPrint(\"{F(1)} {G(\"a\")}\")\n");
    REQUIRE(fb_run({ "build", fb::to_utf8(folder / "swapped") }).exit_code == 0);
    std::filesystem::copy_file(folder / "swapped/build" / library_file("lib"),
                               folder / "swap/build" / library_file("lib"),
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::path swap = folder / "swap/build/swap";
    swap += fb::executable_extension;
    const fb::process_result_t swapped =
        fb::run_process(swap, none).value_or(fb::process_result_t{ .exit_code = -1 });
    INFO(swapped.output);
    CHECK(swapped.exit_code == 1);
    CHECK(swapped.output.contains(std::format("{} is the build lib 0.1.0 ", library_file("lib"))));

    // A library beside it that the program doesn't use is loaded and checked all the same.
    write("unused/project.toml",
          "[project]\nname = \"unused\"\nversion = \"0.1.0\"\n\n" + uses_lib);
    write("unused/main.fb", "Import std.core\nPrint(\"unused\")\n");
    const fb::process_result_t unused = fb_run({ "run", fb::to_utf8(folder / "unused") });
    INFO(unused.output);
    CHECK(unused.output.contains("unused\n"));

#ifdef _WIN32
    // A folder whose name isn't ASCII, or isn't in the ANSI code page, holds the program, and the
    // build's temporary files, and its executable's name has to be ASCII.
    const std::filesystem::path game = folder / fb::path_from_utf8("игра");
    write("игра/project.toml", "[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n" + uses_lib);
    write("игра/main.fb", "Import std.core\nImport lib\nPrint(\"game {F(1)}\")\n");
    const fb::process_result_t away = fb_run({ "build", fb::to_utf8(game) });
    INFO(away.output);
    REQUIRE(away.exit_code == 0);
    CHECK(fb::run_process(game / "build/game.exe", none).value_or(fb::process_result_t{}).output ==
          "game 2\n");
    const fb::process_result_t named = fb_run(
        { "build", fb::to_utf8(game), "-o", fb::to_utf8(folder / fb::path_from_utf8("out/игра")) });
    CHECK(named.exit_code == 1);
    CHECK(named.output.contains("the executable's name, игра.exe, has to be ASCII, since the "
                                "libraries beside it take their code from it by that name"));

    // Started by its short name, it still has its own.
    std::wstring shorter(MAX_PATH, L'\0');
    const std::wstring full = (game / "build/game.exe").wstring();
    shorter.resize(GetShortPathNameW(full.c_str(), shorter.data(), MAX_PATH));
    if (!shorter.empty() && shorter != full) {
        CHECK(fb::run_process(std::filesystem::path(shorter), none)
                  .value_or(fb::process_result_t{})
                  .output == "game 2\n");
    }
#endif

    // Started through a link, of another name too, it's its own file, beside its library. On
    // Windows only an administrator, or developer mode, can make one.
    std::filesystem::create_directories(folder / "linked");
    std::filesystem::path link = folder / "linked/run";
    link += fb::executable_extension;
    std::error_code unlinked;
    std::filesystem::create_symlink(tool, link, unlinked);
    if (!unlinked) {
        CHECK(fb::run_process(link, none).value_or(fb::process_result_t{}).output == "tool 3\n");
    }
}

TEST_CASE("M7's program embeds one prebuilt library and has another beside it, over a C++ DLL",
          "[cli]") {
    // tests/packages' physics and arena, prebuilt, and the engine package with its DLL.
    const std::filesystem::path packages = std::filesystem::path(FB_TEST_OUTPUT) / "m7_packages";
    std::filesystem::remove_all(packages);
    std::filesystem::create_directories(packages);
    for (const std::string_view name : { "physics", "engine", "arena", "space" }) {
        std::filesystem::copy(std::filesystem::path(FB_TEST_PACKAGES) / name, packages / name,
                              std::filesystem::copy_options::recursive);
    }

    const std::filesystem::path native =
        packages / "engine/native" / fb::path_from_utf8(fb::build_target());
    std::filesystem::create_directories(native);
    const std::array<std::filesystem::path, 2> files{ FB_TEST_ENGINE, FB_TEST_ENGINE_LINKED };
    for (const std::filesystem::path& file : files) {
        std::filesystem::copy_file(file, native / file.filename(),
                                   std::filesystem::copy_options::skip_existing);
    }

    prebuild(packages, "physics");
    prebuild(packages, "arena");
    const fb::process_result_t built = fb_run({ "build", fb::to_utf8(packages / "space") });
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    CHECK(built.output.contains(
        std::format("Built {} beside the executable, which needs it\n", library_file("arena"))));

    // It needs only its two files beside it and the OS's own libraries, and runs from its build
    // folder.
    std::filesystem::path space = packages / "space/build/space";
    space += fb::executable_extension;
    const std::filesystem::path engine(FB_TEST_ENGINE);
    CHECK(std::filesystem::is_regular_file(space.parent_path() / library_file("arena")));
    CHECK(std::filesystem::is_regular_file(space.parent_path() / engine.filename()));
    std::vector<std::string> libraries = loaded_libraries(space);
    std::erase_if(libraries, [](const std::string& name) { return name.contains("engine"); });
    CHECK(std::ranges::all_of(libraries, is_system_library));
    const std::expected<fb::process_result_t, std::string> ran = fb::run_process(space, {}, "");
    REQUIRE(ran);
    INFO(ran->output);
    CHECK(ran->exit_code == 0);
    std::ifstream file(std::filesystem::path(FB_TEST_PACKAGES) / "space.out", std::ios::binary);
    const std::string expected{ std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>() };
    CHECK(ran->output == expected);
}

TEST_CASE("a prebuilt library is the build it says, built with the program's builds", "[cli]") {
    const std::filesystem::path folder = std::filesystem::path(FB_TEST_OUTPUT) / "prebuilt_builds";
    std::filesystem::remove_all(folder);
    const folder_writer_t write{ folder };
    write("base/project.toml",
          "[project]\nname = \"base\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("base/base.fb", "Public Class A\n    Public Field n: Int = 1\nEnd Class\n");
    write("mid/project.toml",
          "[project]\nname = \"mid\"\nversion = \"0.1.0\"\ntype = \"library\"\n\n"
          "[dependencies]\nbase = { path = \"../base-prebuilt\" }\n");
    write("mid/mid.fb", "Import base\n"
                        "Public Class B Extends A\n"
                        "    Public Method Who(): String => \"B{n}\"\n"
                        "End Class\n"
                        "Public Function MakeB(): B => B()\n");
    write("app/main.fb", "Import std.core\nImport mid\nPrint(MakeB().Who())\n");
    prebuild(folder, "base");
    prebuild(folder, "mid");
    std::ifstream fbi(folder / "mid-prebuilt/lib" / fb::path_from_utf8(fb::build_target()) /
                          "debug/mid.fbi",
                      std::ios::binary);
    std::string line;
    std::getline(fbi, line);
    std::getline(fbi, line);
    CHECK(line.starts_with("// uses base 0.1.0 "));

    // A dependency built again, and the library not, is an error naming both of its builds, as
    // is a dependency from source that's changed, embedded or beside.
    for (const std::string_view link : { "embed", "beside" }) {
        write("app/project.toml",
              std::format("[project]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                          "base = {{ path = \"../base-prebuilt\" }}\n"
                          "mid = {{ path = \"../mid-prebuilt\", link = \"{}\" }}\n",
                          link));
        write("base/base.fb", "Public Class A\n    Public Field n: Int = 1\nEnd Class\n");
        prebuild(folder, "base");
        const fb::process_result_t same = fb_run({ "run", fb::to_utf8(folder / "app") });
        INFO(same.output);
        CHECK(same.output.contains("B1\n"));

        write("base/base.fb", "Public Class A\n"
                              "    Public Field label: String = \"L\"\n"
                              "    Public Field n: Int = 1\n"
                              "End Class\n");
        prebuild(folder, "base");
        const fb::process_result_t stale = fb_run({ "run", fb::to_utf8(folder / "app") });
        INFO(stale.output);
        CHECK(stale.exit_code == 1);
        CHECK(stale.output.contains("error: mid v0.1.0 was built with base v0.1.0 ("));
        CHECK(stale.output.contains("), but the program has base v0.1.0 ("));
        CHECK(stale.output.contains("); build mid again with it"));
    }

    // A debug build in the release folder, or an interface file that was changed, isn't the
    // build its first line names.
    const std::filesystem::path lib =
        folder / "base-prebuilt/lib" / fb::path_from_utf8(fb::build_target());
    std::filesystem::create_directories(lib / "release");
    for (const std::string_view file : { "base.fbi", "base.fblib" }) {
        std::filesystem::copy_file(lib / "debug" / file, lib / "release" / file,
                                   std::filesystem::copy_options::overwrite_existing);
    }

    write("user/project.toml", "[project]\nname = \"user\"\nversion = \"0.1.0\"\n\n"
                               "[dependencies]\nbase = { path = \"../base-prebuilt\" }\n");
    write("user/main.fb", "Import std.core\nImport base\nPrint(\"{A().n}\")\n");
    const fb::process_result_t moded = fb_run({ "run", fb::to_utf8(folder / "user"), "--release" });
    INFO(moded.output);
    CHECK(moded.exit_code == 1);
    CHECK(moded.output.contains("isn't the build its first line names, base v0.1.0 ("));
    CHECK(moded.output.contains("in release: it's another fb's, target's, or mode's build, or its "
                                "files were changed; build base again"));
    {
        std::ifstream in(lib / "debug/base.fbi", std::ios::binary);
        std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        text.replace(text.find("Public Field n: Int = 1"), 23, "Public Field n: Int = 2");
        std::ofstream(lib / "debug/base.fbi", std::ios::binary) << text;
    }

    const fb::process_result_t edited = fb_run({ "run", fb::to_utf8(folder / "user") });
    INFO(edited.output);
    CHECK(edited.exit_code == 1);
    CHECK(edited.output.contains("isn't the build its first line names"));

    // A library's binary that's the build it says but isn't bitcode is an error naming it,
    // embedded or beside.
    const std::string body = "// module fake/fake.fb 1\nPublic Function F(): Int => ...\n";
    const std::string hash = fb::abi_hash(body, "not bitcode", fb::build_mode_t::DEBUG);
    write("fake/project.toml",
          "[project]\nname = \"fake\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    const std::string debug = std::format("fake/lib/{}/debug/", fb::build_target());
    write(debug + "fake.fbi", std::format("// fbi fake 0.1.0 {}\n{}", hash, body));
    write(debug + "fake.fblib",
          std::format("// fblib fake 0.1.0 {}\n// bundled\nnot bitcode", hash));
    write("faked/main.fb", "Import std.core\nImport fake\nPrint(\"{F()}\")\n");
    for (const std::string_view link : { "embed", "beside" }) {
        write("faked/project.toml",
              std::format("[project]\nname = \"faked\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                          "fake = {{ path = \"../fake\", link = \"{}\" }}\n",
                          link));
        const fb::process_result_t bad = fb_run({ "build", fb::to_utf8(folder / "faked") });
        INFO(link << ' ' << bad.output);
        CHECK(bad.exit_code == 1);
        CHECK(bad.output.contains("fake.fblib's code isn't LLVM bitcode that this fb reads"));
    }

    // A library prebuilt with a dependency's source records its files, which a program with
    // changed ones doesn't have.
    write("src/project.toml",
          "[project]\nname = \"src\"\nversion = \"0.1.0\"\ntype = \"library\"\n");
    write("src/src.fb", "Public Struct P\n    Public Field x: Int\nEnd Struct\n");
    write("top/project.toml",
          "[project]\nname = \"top\"\nversion = \"0.1.0\"\ntype = \"library\"\n\n"
          "[dependencies]\nsrc = { path = \"../src\" }\n");
    write("top/top.fb", "Import src\nPublic Function Make(): P => P(3)\n");
    prebuild(folder, "top");
    write("over/project.toml",
          "[project]\nname = \"over\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
          "src = { path = \"../src\" }\ntop = { path = \"../top-prebuilt\" }\n");
    write("over/main.fb", "Import std.core\nImport top\nPrint(\"{Make().x}\")\n");
    CHECK(fb_run({ "run", fb::to_utf8(folder / "over") }).output.contains("3\n"));
    write("src/src.fb",
          "Public Struct P\n    Public Field y: Int\n    Public Field x: Int\nEnd Struct\n");
    const fb::process_result_t changed = fb_run({ "run", fb::to_utf8(folder / "over") });
    INFO(changed.output);
    CHECK(changed.exit_code == 1);
    CHECK(changed.output.contains("error: top v0.1.0 was built with src v0.1.0 (source:"));
}
