#include "fb/toolchain.hpp"

#include "fb/build_config.hpp"
#include "fb/process.hpp"
#include "fb/utf8.hpp"

#include <filesystem>
#include <string_view>
#include <system_error>

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
        };
        std::error_code ignored;
        if (std::filesystem::is_regular_file(release.linker, ignored) &&
            std::filesystem::is_directory(release.libraries, ignored) &&
            std::filesystem::is_directory(release.std, ignored)) {
            return release;
        }
    }

    return {
        .linker = path_from_utf8(build_config::lld_directory) / linker,
        .libraries = path_from_utf8(build_config::library_directory),
        .notices = path_from_utf8(build_config::notice_directory),
        .std = path_from_utf8(build_config::std_directory),
    };
}

const toolchain_t& toolchain() {
    static const toolchain_t found = find_toolchain(current_executable());
    return found;
}

} // namespace fb
