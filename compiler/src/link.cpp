#include "fb/link.hpp"

#include "fb/build_config.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
#endif

namespace fb {
namespace {

#ifdef _WIN32
// Only Windows' C++ driver reads paths in a code page that isn't UTF-8.
[[nodiscard]] bool is_ascii(const std::string_view text) {
    return std::ranges::all_of(text,
                               [](const char c) { return static_cast<unsigned char>(c) < 0x80; });
}
#endif

/// Where the C++ driver can make temporary files. It reads TMP and TEMP in the ANSI code page,
/// so on Windows, where a user's name can put the temporary folder outside it, that's the
/// folder by its short name, or else, when it isn't ASCII either, a folder of ProgramData's or
/// the system's that is.
std::filesystem::path linker_temporary_folder() {
    std::error_code ignored;
    std::filesystem::path folder = std::filesystem::temp_directory_path(ignored);
#ifdef _WIN32
    if (is_ascii(to_utf8(folder))) {
        return folder;
    }

    const std::wstring wide = folder.wstring();
    if (const DWORD size = GetShortPathNameW(wide.c_str(), nullptr, 0); size != 0) {
        std::wstring shorter(size, L'\0');
        shorter.resize(GetShortPathNameW(wide.c_str(), shorter.data(), size));
        if (!shorter.empty() && is_ascii(to_utf8(std::filesystem::path(shorter)))) {
            return shorter;
        }
    }

    for (const wchar_t* const variable : { L"ProgramData", L"SystemRoot" }) {
        const wchar_t* const value = _wgetenv(variable);
        if (value == nullptr || !is_ascii(to_utf8(std::filesystem::path(value)))) {
            continue;
        }

        const std::filesystem::path candidate =
            std::filesystem::path(value) / (std::wstring(variable) == L"SystemRoot"
                                                ? std::filesystem::path("Temp")
                                                : std::filesystem::path("FutureBlitz") / "tmp");
        std::filesystem::create_directories(candidate, ignored);
        if (std::filesystem::is_directory(candidate, ignored)) {
            return candidate;
        }
    }
#endif
    return folder;
}

#ifdef _WIN32
/// Sets TMP and TEMP for the processes this one starts, while it lives, then puts them back.
class temporary_environment_t {
public:
    explicit temporary_environment_t(const std::filesystem::path& folder) {
        for (std::size_t i = 0; i < m_names.size(); ++i) {
            if (const DWORD size = GetEnvironmentVariableW(m_names[i], nullptr, 0); size != 0) {
                std::wstring value(size, L'\0');
                value.resize(GetEnvironmentVariableW(m_names[i], value.data(), size));
                m_old[i] = std::move(value);
            }

            SetEnvironmentVariableW(m_names[i], folder.wstring().c_str());
        }
    }

    temporary_environment_t(const temporary_environment_t&) = delete;
    temporary_environment_t& operator=(const temporary_environment_t&) = delete;
    temporary_environment_t(temporary_environment_t&&) = delete;
    temporary_environment_t& operator=(temporary_environment_t&&) = delete;

    ~temporary_environment_t() {
        for (std::size_t i = 0; i < m_names.size(); ++i) {
            const std::optional<std::wstring>& old = m_old[i];
            SetEnvironmentVariableW(m_names[i], old.has_value() ? old->c_str() : nullptr);
        }
    }

private:
    std::array<const wchar_t*, 2> m_names{ L"TMP", L"TEMP" };
    std::array<std::optional<std::wstring>, 2> m_old;
};
#endif

/// The linker's output, without the C++ driver's last line, which only says the linker failed,
/// and the blank lines after it.
std::string linker_output(std::string output) {
    for (const std::string_view noise :
         { "collect2.exe: error: ld returned", "collect2: error: ld returned" }) {
        if (const std::size_t at = output.find(noise); at != std::string::npos) {
            const std::size_t line_end = output.find('\n', at);
            output.erase(at, line_end == std::string::npos ? std::string::npos : line_end - at + 1);
        }
    }

    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
    }

    return output;
}

/// A folder in the temporary folder the C++ driver can use for one link's files, removed when
/// it goes.
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

    /// A new file's path in the folder, with `extension`, which the folder makes first.
    [[nodiscard]] std::filesystem::path file(const std::filesystem::path& extension) {
        std::filesystem::path name = std::format("{}", m_files++);
        name += extension;
        return path() / name;
    }

    /// The folder, made the first time it's asked for.
    [[nodiscard]] const std::filesystem::path& path() {
        std::error_code ignored;
        if (m_path.empty()) {
            std::random_device random;
            m_path =
                linker_temporary_folder() / "futureblitz" / std::format("link-{:08x}", random());
            std::filesystem::create_directories(m_path, ignored);
        }

        return m_path;
    }

private:
    std::filesystem::path m_path;
    unsigned m_files = 0;
};

/// A path as the linker can read it. The C++ driver reads its arguments in the ANSI code page,
/// which can't spell every name, so on Windows a file whose path isn't ASCII goes by its short
/// name, or else as a copy in an ASCII folder.
std::string linker_path(const std::filesystem::path& path, link_folder_t& folder) {
    std::string text = to_utf8(path);
#ifdef _WIN32
    if (is_ascii(text)) {
        return text;
    }

    const std::wstring wide = path.wstring();
    if (const DWORD size = GetShortPathNameW(wide.c_str(), nullptr, 0); size != 0) {
        std::wstring shorter(size, L'\0');
        shorter.resize(GetShortPathNameW(wide.c_str(), shorter.data(), size));
        if (std::string short_text = to_utf8(std::filesystem::path(shorter));
            !shorter.empty() && is_ascii(short_text)) {
            return short_text;
        }
    }

    const std::filesystem::path copy = folder.file(path.extension());
    std::error_code ignored;
    if (std::filesystem::copy_file(path, copy, ignored)) {
        return to_utf8(copy);
    }
#else
    (void)folder;
#endif
    return text;
}

/// Moves the linked executable from where the linker wrote it to `output`.
std::expected<void, std::string> move_output(const std::filesystem::path& linked,
                                             const std::filesystem::path& output) {
    std::error_code error;
    std::filesystem::rename(linked, output, error);
    if (!error) {
        return {};
    }

    // Another disk's folder takes a copy.
    std::filesystem::copy_file(linked, output, std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        return std::unexpected(
            std::format("cannot write '{}': {}", to_utf8(output), error.message()));
    }

    return {};
}

} // namespace

/// The C++ driver that built the runtime knows where the C and C++ runtime libraries are; -B
/// points it at lld. The archives go after the objects, which use them, and the system libraries
/// after the runtime, which they could be used by too. lld reports every error, not its first 20.
std::expected<void, std::string> link_program(const std::span<const std::filesystem::path> objects,
                                              const native_libraries_t& natives,
                                              const std::filesystem::path& written) {
    // On Windows, the C++ driver adds .exe to a name without an extension, so the path the
    // executable is moved to has it too.
    std::filesystem::path output = written;
#ifdef _WIN32
    if (output.extension().empty()) {
        output += ".exe";
    }
#endif

    link_folder_t folder;
    std::vector<std::string> arguments;
    for (const std::filesystem::path& object : objects) {
        arguments.push_back(linker_path(object, folder));
    }

    for (const std::filesystem::path& archive : natives.archives) {
        arguments.push_back(linker_path(archive, folder));
    }

    arguments.push_back(linker_path(path_from_utf8(build_config::runtime_library), folder));
    for (const std::filesystem::path& archive : natives.bundled) {
        arguments.push_back(linker_path(archive, folder));
    }

    // A framework of macOS's, like "Security.framework", links by its name.
    for (const std::string& library : natives.system) {
        if (const std::string_view name = library; name.ends_with(".framework")) {
            arguments.emplace_back("-framework");
            arguments.emplace_back(
                name.substr(0, name.size() - std::string_view(".framework").size()));
        } else {
            arguments.push_back(std::format("-l{}", library));
        }
    }

    // An executable whose path the linker can't read is written in the folder, then moved.
    std::optional<std::filesystem::path> linked;
#ifdef _WIN32
    if (!is_ascii(to_utf8(output))) {
        linked = folder.file(output.extension());
    }
#endif
    arguments.emplace_back("-o");
    arguments.push_back(to_utf8(linked.value_or(output)));
    arguments.emplace_back("-fuse-ld=lld");
    arguments.push_back(std::format("-B{}/", build_config::lld_directory));
    arguments.emplace_back("-Wl,--error-limit=0");
    // Per-platform options from compiler/CMakeLists.txt, like static linking on MinGW.
    for (const std::string_view option : build_config::link_options) {
        arguments.emplace_back(option);
    }

#ifdef _WIN32
    const temporary_environment_t environment(folder.path());
#endif
    const std::expected<process_result_t, std::string> result =
        run_process(build_config::linker_driver, arguments);
    if (!result) {
        return std::unexpected(result.error());
    }

    if (result->exit_code != 0) {
        return std::unexpected(std::format("linking failed:\n{}", linker_output(result->output)));
    }

    if (linked) {
        return move_output(*linked, output);
    }

    return {};
}

} // namespace fb
