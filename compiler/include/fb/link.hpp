#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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

/// What an executable with libraries beside it does for them: it exports what they take from it,
/// and links each of `symbols`, the runtime's functions they call, though it doesn't call them
/// itself. On Windows it writes its import library to `import_library`, which they link
/// against.
struct exports_t {
    std::vector<std::string> symbols;
    std::filesystem::path import_library;
};

/// How lld finds the system library `name` on Linux, in `folders`: by -l where one has its
/// development link or archive, or else by the file the system loads it by, like libGL.so.1,
/// which needs no development package.
[[nodiscard]] std::string system_library_argument(std::string_view name,
                                                  std::span<const std::filesystem::path> folders);

/// Links object files, the native libraries, the runtime library, and the bundled ones into an
/// executable at `written`, with fb's own lld and the C runtime's pieces in its toolchain, which
/// finds the dynamic ones beside it. With `exports`, it's one that libraries beside it take
/// what they use from. Fails when the linker can't run or reports errors.
[[nodiscard]] std::expected<void, std::string>
link_program(std::span<const std::filesystem::path> objects, const native_libraries_t& natives,
             const std::filesystem::path& written, const exports_t* exports = nullptr);

/// Links a library beside an executable, its objects and its own native libraries, into the
/// OS's dynamic library at `written`, which takes the rest from the executable and the other
/// libraries beside it: on Windows through `import_libraries`, theirs and the executable's, and
/// writing its own to `import_library`, and elsewhere as it loads. Its own dynamic natives are
/// found beside it.
[[nodiscard]] std::expected<void, std::string>
link_beside(std::span<const std::filesystem::path> objects, const native_libraries_t& natives,
            std::span<const std::filesystem::path> import_libraries,
            const std::filesystem::path& written, const std::filesystem::path& import_library);

} // namespace fb
