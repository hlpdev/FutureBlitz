#include "fb/link.hpp"

#include "fb/build_config.hpp"
#include "fb/process.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// What a link writes.
enum class output_kind_t : std::uint8_t {
    EXECUTABLE,
    LIBRARY,
};

#ifdef _WIN32
[[nodiscard]] bool is_ascii(const std::string_view text) {
    return std::ranges::all_of(text,
                               [](const char c) { return static_cast<unsigned char>(c) < 0x80; });
}
#endif

/// The linker's output, without the blank lines after it.
std::string linker_output(std::string output) {
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
    }

    return output;
}

/// A folder in the temporary folder for one link's files, removed when it goes.
class link_folder_t {
public:
    link_folder_t() = default;
    link_folder_t(const link_folder_t&) = delete;
    link_folder_t& operator=(const link_folder_t&) = delete;
    link_folder_t(link_folder_t&&) = delete;
    link_folder_t& operator=(link_folder_t&&) = delete;

    // Cleaning up is best effort.
    ~link_folder_t() {
        try {
            std::error_code ignored;
            std::filesystem::remove_all(m_path, ignored);
        } catch (...) {
            return;
        }
    }

    /// The folder, made the first time it's asked for.
    [[nodiscard]] const std::filesystem::path& path() {
        std::error_code ignored;
        if (m_path.empty()) {
            std::random_device random;
            m_path = std::filesystem::temp_directory_path(ignored) / "futureblitz" /
                     std::format("link-{:08x}", random());
            std::filesystem::create_directories(m_path, ignored);
        }

        return m_path;
    }

private:
    std::filesystem::path m_path;
};

/// `argument` in a response file, quoted as lld reads it: on Windows as a command line's words
/// are, where a backslash escapes only a quote or the backslashes before one, and elsewhere as a
/// POSIX shell's are.
std::string response_word(const std::string_view argument) {
    std::string text = "\"";
#ifdef _WIN32
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
        } else {
            if (c == '"') {
                text.append(backslashes + 1, '\\');
            }

            backslashes = 0;
        }

        text += c;
    }

    text.append(backslashes, '\\');
#else
    for (const char c : argument) {
        if (c == '"' || c == '\\') {
            text += '\\';
        }

        text += c;
    }
#endif
    text += '"';
    return text;
}

/// A file of the toolchain's target folder, as the linker takes it.
std::string piece(const std::string_view name) {
    return to_utf8(toolchain().libraries / path_from_utf8(name));
}

#ifdef __APPLE__
/// The macOS SDK's folder and version, from Apple's Command Line Tools, which a program links
/// against; they can't be shipped with fb.
struct apple_sdk_t {
    std::string path;
    std::string version;
};

std::expected<apple_sdk_t, std::string> apple_sdk() {
    static const std::expected<apple_sdk_t, std::string> found =
        []() -> std::expected<apple_sdk_t, std::string> {
        const auto ask = [](const std::string& option) -> std::optional<std::string> {
            const std::vector<std::string> arguments{ "--sdk", "macosx", option };
            const std::expected<process_result_t, std::string> result =
                run_process("/usr/bin/xcrun", arguments);
            if (!result || result->exit_code != 0) {
                return std::nullopt;
            }

            return result->output;
        };
        const std::optional<std::string> path_output = ask("--show-sdk-path");
        const std::optional<std::string> version_output = ask("--show-sdk-version");
        std::optional<std::string> path = path_output ? sdk_path_in(*path_output) : std::nullopt;
        std::optional<std::string> version =
            version_output ? sdk_version_in(*version_output) : std::nullopt;
        if (!path || !version) {
            return std::unexpected(
                "linking needs the macOS SDK from Apple's Command Line Tools, which aren't "
                "installed; install them with `xcode-select --install`");
        }

        return apple_sdk_t{ .path = std::move(*path), .version = std::move(*version) };
    }();
    return found;
}
#endif

/// What lld takes before the objects: what it writes and for which target, and the C runtime's
/// startup files.
std::expected<std::vector<std::string>, std::string> start_arguments(const output_kind_t kind) {
    const bool library = kind == output_kind_t::LIBRARY;
    std::vector<std::string> arguments;
#ifdef _WIN32
    // MinGW-w64's, with everything static.
    arguments = { "-m", "i386pep", "-Bstatic" };
    if (library) {
        arguments.insert(arguments.end(), {
                                              "--shared",
                                              "-e",
                                              "DllMainCRTStartup",
                                              "--enable-auto-image-base",
                                          });
    }

    arguments.push_back(piece(library ? "dllcrt2.o" : "crt2.o"));
    arguments.push_back(piece("crtbegin.o"));
#elifdef __APPLE__
    const std::expected<apple_sdk_t, std::string> sdk = apple_sdk();
    if (!sdk) {
        return std::unexpected(sdk.error());
    }

#ifdef __aarch64__
    arguments = { "-arch", "arm64" };
#else
    arguments = { "-arch", "x86_64" };
#endif
    arguments.insert(arguments.end(),
                     { "-platform_version", "macos", std::string(build_config::macos_minimum),
                       sdk->version, "-syslibroot", sdk->path, "-dead_strip" });
    if (library) {
        // What it takes from the executable is found as it loads.
        arguments.insert(arguments.end(), { "-dylib", "-undefined", "dynamic_lookup" });
    }
#else
#ifdef __aarch64__
    arguments = { "-m", "aarch64linux" };
    const std::string_view loader = "/lib/ld-linux-aarch64.so.1";
#else
    arguments = { "-m", "elf_x86_64" };
    const std::string_view loader = "/lib64/ld-linux-x86-64.so.2";
#endif
    arguments.insert(arguments.end(),
                     { "--build-id", "--eh-frame-hdr", "--hash-style=gnu", "--as-needed" });
    if (library) {
        arguments.insert(arguments.end(), { "-shared", "-z", "relro" });
    } else {
        arguments.insert(arguments.end(), { "-dynamic-linker", std::string(loader), "-pie", "-z",
                                            "now", "-z", "relro" });
        arguments.push_back(piece("Scrt1.o"));
    }

    arguments.push_back(piece("crti.o"));
    arguments.push_back(piece("crtbeginS.o"));
#endif
    arguments.push_back("-L" + to_utf8(toolchain().libraries));
    arguments.emplace_back("--error-limit=0");
    return arguments;
}

/// What lld takes after everything else: the C and C++ runtimes' libraries, the C++ runtime
/// static, and the C runtime's last files.
void finish_arguments(const output_kind_t kind, std::vector<std::string>& arguments) {
#ifdef _WIN32
    // As GCC links them, with UCRT, the current C runtime, which MinGW's static winpthread
    // needs.
    for (const std::string_view library : {
             "stdc++",
             "mingw32",
             "gcc",
             "gcc_eh",
             "mingwex",
             "ucrt",
             "kernel32",
             "pthread",
             "advapi32",
             "shell32",
             "user32",
             "kernel32",
             "mingw32",
             "gcc",
             "gcc_eh",
             "mingwex",
             "ucrt",
             "kernel32",
         }) {
        arguments.push_back(std::format("-l{}", library));
    }

    if (kind == output_kind_t::EXECUTABLE) {
        arguments.push_back(piece("default-manifest.o"));
    }

    arguments.push_back(piece("crtend.o"));
#elifdef __APPLE__
    (void)kind;
    arguments.insert(arguments.end(), { "-lc++", "-lSystem", piece("libclang_rt.osx.a") });
#else
    (void)kind;
    // glibc 2.28, which the release links against, has dlopen in libdl and shm_open in librt,
    // which newer ones have in libc; a program needs them only where it calls them.
    arguments.insert(arguments.end(), { "-Bstatic", "-lstdc++", "-Bdynamic", "-lm", "-lgcc",
                                        "-lgcc_eh", "--as-needed", "-ldl", "-lrt", "--no-as-needed",
                                        "-lpthread", "-lc", "-lgcc", "-lgcc_eh" });
    arguments.push_back(piece("crtendS.o"));
    arguments.push_back(piece("crtn.o"));
#endif
}

/// Runs lld with `arguments`, through a response file in the link's folder, writing `output`,
/// and with `debug_symbols`, on Windows, its PDB.
std::expected<void, std::string> run_linker(const output_kind_t kind,
                                            std::vector<std::string> arguments,
                                            const std::filesystem::path& output,
                                            const bool debug_symbols, link_folder_t& folder) {
    std::expected<std::vector<std::string>, std::string> all = start_arguments(kind);
    if (!all) {
        return std::unexpected(all.error());
    }

    std::ranges::move(arguments, std::back_inserter(*all));
    finish_arguments(kind, *all);
    all->emplace_back("-o");
    all->push_back(to_utf8(output));
#ifdef _WIN32
    // A build without one leaves no other build's beside it.
    std::filesystem::path pdb = output;
    pdb.replace_extension(".pdb");
    if (debug_symbols) {
        all->push_back("--pdb=" + to_utf8(pdb));
    } else {
        std::error_code ignored;
        std::filesystem::remove(pdb, ignored);
    }
#else
    (void)debug_symbols;
#endif

    const std::filesystem::path response = folder.path() / "arguments.rsp";
    {
        std::ofstream file(response, std::ios::binary);
        for (const std::string& argument : *all) {
            file << response_word(argument) << '\n';
        }

        if (!file) {
            return std::unexpected(
                std::format("cannot write the linker's arguments to '{}'", to_utf8(response)));
        }
    }

#ifdef __APPLE__
    std::vector<std::string> command{ "-flavor", "darwin" };
#else
    std::vector<std::string> command{ "-flavor", "gnu" };
#endif
    command.push_back("@" + to_utf8(response));
    const std::expected<process_result_t, std::string> result =
        run_process(toolchain().linker, command);
    if (!result) {
        return std::unexpected(result.error());
    }

    if (result->exit_code != 0) {
        return std::unexpected(std::format("linking failed:\n{}", linker_output(result->output)));
    }

    return {};
}

/// The arguments that link a library's dynamic natives, which it finds beside it.
void link_dynamic(const native_libraries_t& natives, link_folder_t& folder,
                  std::vector<std::string>& arguments) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)folder;
    for (const dynamic_native_t& library : natives.dynamic) {
        arguments.push_back(to_utf8(library.linked));
    }
#else
    // A shared object without a SONAME is recorded by the name the linker found it by, so each
    // goes by its name beside the executable, from a folder of the link's own. Each is recorded
    // even when only another uses it, since $ORIGIN, a DT_RUNPATH, finds only the executable's
    // own, and --as-needed would leave out those it doesn't call.
    if (!natives.dynamic.empty()) {
        const std::filesystem::path found = folder.path() / "dynamic";
        std::error_code ignored;
        std::filesystem::create_directories(found, ignored);
        arguments.push_back("-L" + to_utf8(found));
        arguments.insert(arguments.end(), { "--push-state", "--no-as-needed" });
        for (const dynamic_native_t& library : natives.dynamic) {
            std::filesystem::create_symlink(library.linked, found / path_from_utf8(library.beside),
                                            ignored);
            arguments.push_back("-l:" + library.beside);
        }

        arguments.emplace_back("--pop-state");
    }
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
/// The folders a Linux system keeps its libraries in, after the toolchain's own.
constexpr auto system_folders = std::to_array<std::string_view>({
#ifdef __aarch64__
    "/usr/local/lib",
    "/usr/lib/aarch64-linux-gnu",
    "/lib/aarch64-linux-gnu",
#else
    "/usr/local/lib",
    "/usr/lib/x86_64-linux-gnu",
    "/lib/x86_64-linux-gnu",
#endif
    "/usr/lib64",
    "/lib64",
    "/usr/lib",
    "/lib",
});

#endif

/// The arguments that link system libraries by their names, a framework of macOS's, like
/// "Security.framework", too.
void link_system(const native_libraries_t& natives, std::vector<std::string>& arguments) {
#if !defined(_WIN32) && !defined(__APPLE__)
    for (const std::string_view folder : system_folders) {
        arguments.push_back(std::format("-L{}", folder));
    }
#endif
    for (const std::string& library : natives.system) {
        if (const std::string_view name = library; name.ends_with(".framework")) {
            arguments.emplace_back("-framework");
            arguments.emplace_back(
                name.substr(0, name.size() - std::string_view(".framework").size()));
        } else {
#if !defined(_WIN32) && !defined(__APPLE__)
            std::vector<std::filesystem::path> folders{ toolchain().libraries };
            folders.insert(folders.end(), system_folders.begin(), system_folders.end());
            arguments.push_back(system_library_argument(library, folders));
#else
            arguments.push_back(std::format("-l{}", library));
#endif
        }
    }
}

/// The option by which the OS finds a library's dynamic natives beside it, where it has any.
/// Windows looks there anyway.
std::vector<std::string> find_beside(const native_libraries_t& natives) {
#ifdef _WIN32
    (void)natives;
    return {};
#else
    if (natives.dynamic.empty()) {
        return {};
    }

#ifdef __APPLE__
    return { "-rpath", "@loader_path" };
#else
    return { "-rpath", "$ORIGIN" };
#endif
#endif
}

/// The last line of `output` that `wanted` takes, without its line break or spaces around it.
template <typename Wanted>
std::optional<std::string> last_line(const std::string_view output, const Wanted wanted) {
    std::optional<std::string> found;
    std::size_t start = 0;
    while (start <= output.size()) {
        const std::size_t end = std::min(output.find('\n', start), output.size());
        std::string_view line = output.substr(start, end - start);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }

        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }

        if (!line.empty() && wanted(line)) {
            found = std::string(line);
        }

        start = end + 1;
    }

    return found;
}

} // namespace

std::optional<std::string> sdk_path_in(const std::string_view output) {
    return last_line(output, [](const std::string_view line) { return line.front() == '/'; });
}

std::optional<std::string> sdk_version_in(const std::string_view output) {
    return last_line(output, [](const std::string_view line) {
        return line.front() >= '0' && line.front() <= '9' &&
               std::ranges::all_of(line,
                                   [](const char c) { return (c >= '0' && c <= '9') || c == '.'; });
    });
}

std::string system_library_argument(const std::string_view name,
                                    const std::span<const std::filesystem::path> folders) {
    std::error_code ignored;
    for (const std::filesystem::path& folder : folders) {
        if (std::filesystem::exists(folder / std::format("lib{}.so", name), ignored) ||
            std::filesystem::exists(folder / std::format("lib{}.a", name), ignored)) {
            return std::format("-l{}", name);
        }
    }

    // The first folder's with one, its shortest name, which is the one the system loads it by,
    // like libGL.so.1 and not libGL.so.1.7.0, and of those its newest.
    const std::string prefix = std::format("lib{}.so.", name);
    for (const std::filesystem::path& folder : folders) {
        std::optional<std::string> best;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(folder, ignored)) {
            const std::string file = to_utf8(entry.path().filename());
            if (!entry.is_directory(ignored) && file.starts_with(prefix) &&
                (!best || file.size() < best->size() ||
                 (file.size() == best->size() && file > *best))) {
                best = file;
            }
        }

        if (best) {
            return "-l:" + *best;
        }
    }

    return std::format("-l{}", name);
}

/// The archives go after the objects, which use them, and the system libraries after the
/// runtime, which they could be used by too. lld reports every error, not its first 20.
std::expected<void, std::string> link_program(const std::span<const std::filesystem::path> objects,
                                              const native_libraries_t& natives,
                                              const std::filesystem::path& written,
                                              const exports_t* exports, const bool debug_symbols) {
    // On Windows an executable's name ends in .exe.
    std::filesystem::path output = written;
#ifdef _WIN32
    if (output.extension().empty()) {
        output += ".exe";
    }
#endif

    link_folder_t folder;
    std::vector<std::string> arguments;
    for (const std::filesystem::path& object : objects) {
        arguments.push_back(to_utf8(object));
    }

    for (const std::filesystem::path& archive : natives.archives) {
        arguments.push_back(to_utf8(archive));
    }

    link_dynamic(natives, folder, arguments);
    arguments.push_back(piece(build_config::runtime_library));
    for (const std::filesystem::path& archive : natives.bundled) {
        arguments.push_back(to_utf8(archive));
    }

    link_system(natives, arguments);
    std::ranges::move(find_beside(natives), std::back_inserter(arguments));
    // The libraries beside it take what they use from its own symbols, the runtime functions
    // they call among them, which it links for them.
    if (exports != nullptr) {
#ifdef _WIN32
        // On Windows they name it in their imports by its file's name.
        if (!is_ascii(to_utf8(output.filename()))) {
            return std::unexpected(std::format(
                "the executable's name, {}, has to be ASCII, since the libraries beside it "
                "take their code from it by that name",
                to_utf8(output.filename())));
        }

        arguments.emplace_back("--export-all-symbols");
        arguments.push_back("--out-implib=" + to_utf8(exports->import_library));
#elifdef __APPLE__
        arguments.emplace_back("-export_dynamic");
#else
        arguments.emplace_back("--export-dynamic");
#endif
        for (const std::string& symbol : exports->symbols) {
            arguments.emplace_back("-u");
#ifdef __APPLE__
            arguments.push_back("_" + symbol);
#else
            arguments.push_back(symbol);
#endif
        }
    }

    return run_linker(output_kind_t::EXECUTABLE, std::move(arguments), output, debug_symbols,
                      folder);
}

std::expected<void, std::string>
link_beside(const std::span<const std::filesystem::path> objects, const native_libraries_t& natives,
            const std::span<const std::filesystem::path> import_libraries,
            const std::filesystem::path& written, const std::filesystem::path& import_library,
            const bool debug_symbols) {
    link_folder_t folder;
    std::vector<std::string> arguments;
#ifdef _WIN32
    // It exports its own code: a name with ", " in it can't go as an /export directive.
    arguments.emplace_back("--export-all-symbols");
    arguments.push_back("--out-implib=" + to_utf8(import_library));
#elifndef __APPLE__
    // Its calls of its own code stay its own.
    arguments.emplace_back("-Bsymbolic");
#endif
    for (const std::filesystem::path& object : objects) {
        arguments.push_back(to_utf8(object));
    }

    for (const std::filesystem::path& archive : natives.archives) {
        arguments.push_back(to_utf8(archive));
    }

    link_dynamic(natives, folder, arguments);
#ifdef _WIN32
    for (const std::filesystem::path& library : import_libraries) {
        arguments.push_back(to_utf8(library));
    }
#else
    (void)import_libraries;
    (void)import_library;
#endif
    link_system(natives, arguments);
    std::ranges::move(find_beside(natives), std::back_inserter(arguments));
    return run_linker(output_kind_t::LIBRARY, std::move(arguments), written, debug_symbols, folder);
}

} // namespace fb
