#pragma once

#include "fb/diagnostic.hpp"
#include "fb/semver.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// A dependency a project.toml names: a package from a registry, by a range of its versions, or
/// a local package, by its folder.
struct dependency_t {
    /// The name its modules are imported by, like `physics` in `Import physics.shapes`.
    std::string name;
    span_t name_span;
    /// For a local package, its folder, from the project.toml's folder, as written. Empty for one
    /// from a registry.
    std::string path;
    /// Where its path or its range is written.
    span_t path_span;
    /// For a package from a registry, the versions it takes, like "1.2".
    std::optional<version_range_t> range;
    /// The registry it's from, as [registries] names it; nullopt for futureblitz.dev's.
    std::optional<std::string> registry;
    std::optional<span_t> registry_span;
    /// `link = "beside"`: a library built into a file of its own beside the executable, rather
    /// than into it, which only the program's own project.toml decides.
    bool beside = false;
    /// Where `link` is written, if it is.
    std::optional<span_t> link_span;
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

/// A registry [registries] names, by its index: an http:// or https:// URL, a file:// one, or a
/// folder, from the project.toml's folder.
struct registry_entry_t {
    std::string name;
    std::string index;
    span_t index_span;
};

/// The name of futureblitz.dev's registry, which a dependency is from unless it says otherwise,
/// and which [registries] can name again, for a mirror.
inline constexpr std::string_view default_registry = "futureblitz";

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
    /// Semver, like "0.1.0" or "1.0.0-beta.2".
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
    /// `[format] width`: the line width `fb fmt` formats its code to.
    std::optional<std::uint32_t> format_width;
    /// What a registry shows of it, which `fb publish` needs some of: a line about it, its
    /// license as an SPDX expression, the files of its license and its readme, when they aren't
    /// LICENSE and README.md, its repository's, home page's, and documentation's URLs, and its
    /// keywords.
    std::optional<std::string> description;
    std::optional<std::string> license;
    std::optional<std::string> license_file;
    std::optional<std::string> readme;
    std::optional<std::string> repository;
    std::optional<std::string> homepage;
    std::optional<std::string> documentation;
    std::vector<std::string> keywords;
    /// `[project] fb`: the versions of fb it builds with, which a registry's resolver checks.
    std::optional<version_range_t> fb;
    /// [registries], in the order the file has them.
    std::vector<registry_entry_t> registries;
};

/// Why `text` isn't an SPDX license expression, like "MIT OR Apache-2.0", or nullopt when it is
/// one. Only its form is checked; the registry checks its licenses' names against SPDX's list.
[[nodiscard]] std::optional<std::string> license_error(std::string_view text);

/// Whether `index`, as [registries] writes it, is a folder or a file:// URL: a registry on disk.
[[nodiscard]] bool is_local_index(std::string_view index);

/// Whether `url` is an http:// URL whose host is this machine's: localhost or a name in it,
/// 127.0.0.1, or [::1], the only ones a registry can be at without TLS.
[[nodiscard]] bool is_loopback_url(std::string_view url);

/// Whether text is a name a native library can have, or a system library's name for the
/// linker: ASCII letters, digits, '_', '-', '.', and '+', not starting with '-'.
[[nodiscard]] bool is_library_name(std::string_view text);

/// Why text can't be a package's name, which is an identifier, but not std, or nullopt when it
/// can be.
[[nodiscard]] std::optional<std::string> package_name_error(std::string_view text);

/// Reads `file`, a project.toml: errors for what it can't use, at their lines, and warnings for
/// the keys it doesn't know. nullopt after an error.
[[nodiscard]] std::optional<manifest_t> read_manifest(const source_file_t& file,
                                                      diagnostic_bag_t& diagnostics);

} // namespace fb
