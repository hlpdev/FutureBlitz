#include "fb/toolchain.hpp"

#include "fb/build_config.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace fb {

toolchain_t find_toolchain(const std::filesystem::path& executable) {
#ifdef _WIN32
    constexpr std::string_view linker = "lld.exe";
#else
    constexpr std::string_view linker = "lld";
#endif
    if (!executable.empty()) {
        const std::filesystem::path bin = executable.parent_path();
        const std::filesystem::path root = bin.parent_path();
        toolchain_t release{
            .linker = bin / linker,
            .libraries = root / "lib" / path_from_utf8(build_config::toolchain_target),
            .notices = root / "notices",
            .std = root / "std",
            .clang_headers = root / "lib" / "clang",
        };
        std::error_code ignored;
        if (std::filesystem::is_regular_file(release.linker, ignored) &&
            std::filesystem::is_directory(release.libraries, ignored) &&
            std::filesystem::is_directory(release.std, ignored)) {
            if (std::filesystem::is_directory(release.libraries / "include", ignored)) {
                release.c_headers.push_back(release.libraries / "include");
            }

            return release;
        }
    }

    toolchain_t built{
        .linker = path_from_utf8(build_config::lld_directory) / linker,
        .libraries = path_from_utf8(build_config::library_directory),
        .notices = path_from_utf8(build_config::notice_directory),
        .std = path_from_utf8(build_config::std_directory),
        .clang_headers = path_from_utf8(build_config::clang_resource_directory),
    };
    for (const std::string_view folder : build_config::c_header_directories) {
        built.c_headers.push_back(path_from_utf8(folder));
    }

    return built;
}

namespace {

toolchain_t& chosen_toolchain() {
    static toolchain_t found = find_toolchain(current_executable());
    return found;
}

} // namespace

const toolchain_t& toolchain() {
    return chosen_toolchain();
}

void use_toolchain(toolchain_t chosen) {
    chosen_toolchain() = std::move(chosen);
}

} // namespace fb
