#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace fb {

/// A [native] dynamic library a program loads as it starts, which its build copies beside it.
struct dynamic_native_t {
    /// What the linker takes: on Windows its import library, and elsewhere the library itself.
    std::filesystem::path linked;
    /// The library.
    std::filesystem::path file;
    /// The name the executable records for it, which it's copied beside it under: on Windows
    /// its file's, on Linux its SONAME, or its file's without one, and on macOS its install
    /// name's last part.
    std::string beside;
};

/// What a program links besides its objects and the runtime: its packages' [native] libraries,
/// and the C libraries std is built on that the runtime's functions it calls use.
struct native_libraries_t {
    /// Static archives, each package's in its project.toml's order, a package's before those of
    /// the packages it depends on.
    std::vector<std::filesystem::path> archives;
    /// System libraries by the names the linker takes, like "m" for -lm, each once.
    std::vector<std::string> system;
    /// Dynamic libraries, in the order the archives are, each with its own name beside the
    /// executable.
    std::vector<dynamic_native_t> dynamic;
    /// The bundled C libraries' archives, each after the runtime's archive of the functions
    /// that use it, which go after the runtime.
    std::vector<std::filesystem::path> bundled;
};

/// Links object files, the native libraries, the runtime library, and the bundled ones into an
/// executable at `written`, using lld through the C++ compiler that built the runtime, which
/// finds the dynamic ones beside it. Fails when the linker can't run or reports errors.
[[nodiscard]] std::expected<void, std::string>
link_program(std::span<const std::filesystem::path> objects, const native_libraries_t& natives,
             const std::filesystem::path& written);

} // namespace fb
