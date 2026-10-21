#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace fb {

/// What a program links besides its objects and the runtime: its packages' [native] libraries.
struct native_libraries_t {
    /// Static archives, each package's in its project.toml's order, a package's before those of
    /// the packages it depends on.
    std::vector<std::filesystem::path> archives;
    /// System libraries by the names the linker takes, like "m" for -lm, each once.
    std::vector<std::string> system;
};

/// Links object files, the native libraries, and the runtime library into an executable at
/// `written`, using lld through the C++ compiler that built the runtime. Fails when the linker
/// can't run or reports errors.
[[nodiscard]] std::expected<void, std::string>
link_program(std::span<const std::filesystem::path> objects, const native_libraries_t& natives,
             const std::filesystem::path& written);

} // namespace fb
