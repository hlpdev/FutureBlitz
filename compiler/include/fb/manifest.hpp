#pragma once

#include "fb/diagnostic.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// A dependency a project.toml names: a local package, by its folder.
struct dependency_t {
    /// The name its modules are imported by, like `physics` in `Import physics.shapes`.
    std::string name;
    span_t name_span;
    /// Its folder, from the project.toml's folder, as written.
    std::string path;
    span_t path_span;
};

/// How a native library links.
enum class native_kind_t : std::uint8_t {
    /// An archive, linked into the program.
    STATIC,
    /// One the system's linker finds by its name, as -lname.
    SYSTEM,
    /// A dynamic library, which the program loads as it starts, from beside it.
    DYNAMIC,
};

/// A C library a project.toml's [native] names, which `Extern "C" Lib "name"` refers to.
struct native_t {
    /// The name Lib gives it, like "joltc".
    std::string name;
    span_t name_span;
    native_kind_t kind = native_kind_t::STATIC;
    /// For a static one, its archive from the project.toml's folder, as written, which may
    /// leave out its "lib" prefix and its extension: "libs/joltc" for libs/libjoltc.a. For a
    /// dynamic one, its file from there without them, "native/{target}/engine" for engine.dll
    /// or libengine.so. For a system one, the name the linker takes, like "m" for -lm. A
    /// static or dynamic one's "{target}" is the build's target.
    std::string path;
    span_t path_span;
};

/// What a package builds, as project.toml's `type` says.
enum class package_type_t : std::uint8_t {
    /// A program, from its main file.
    EXECUTABLE,
    /// Modules that programs use, built into them or beside them.
    LIBRARY,
};

/// A package's project.toml, read.
struct manifest_t {
    /// An identifier, which messages show and the package's build is named by.
    std::string name;
    span_t name_span;
    /// Three numbers, like "0.1.0".
    std::string version;
    /// What it builds, when `type` is written. Without it, a package with its main file is an
    /// executable, and one without is a library.
    std::optional<package_type_t> type;
    std::optional<span_t> type_span;
    /// The main file, from the project.toml's folder. A package without it is a library.
    std::string main = "main.fb";
    std::optional<span_t> main_span;
    std::vector<dependency_t> dependencies;
    /// Its [native] libraries, in the order the file has them.
    std::vector<native_t> natives;
};

/// Whether text is a name a native library can have, or a system library's name for the
/// linker: ASCII letters, digits, '_', '-', '.', and '+', not starting with '-'.
[[nodiscard]] bool is_library_name(std::string_view text);

/// Reads `file`, a project.toml: errors for what it can't use, at their lines, and warnings for
/// the keys it doesn't know. nullopt after an error.
[[nodiscard]] std::optional<manifest_t> read_manifest(const source_file_t& file,
                                                      diagnostic_bag_t& diagnostics);

} // namespace fb
