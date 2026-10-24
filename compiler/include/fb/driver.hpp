#pragma once

#include "fb/ast.hpp"
#include "fb/build_config.hpp"
#include "fb/codegen.hpp"
#include "fb/diagnostic.hpp"
#include "fb/link.hpp"
#include "fb/mir.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

#ifdef _WIN32
inline constexpr std::string_view executable_extension = ".exe";
#else
inline constexpr std::string_view executable_extension = "";
#endif

/// The package a program is, when a project.toml says: what `fb build` names its executable by
/// and writes it into, as <root>/build/<name>.
struct package_info_t {
    std::string name;
    std::string version;
    std::filesystem::path root;
    /// What its executable is named, without an extension: the package's name for its main file,
    /// or another main file's stem, as `fb run src/tool.fb` has.
    std::string program;
    /// It has no main file, so `fb check` checks its files, and it can't be built or run.
    bool library = false;
};

/// Everything the front end makes from a program: its main file, and the files it imports, which
/// come after it in `sources`. Each file is a module, which the AST's module table names. The
/// tokens are the main file's, and the AST and analysis cover every file. They refer to the files
/// in `sources`, which stay put when the compilation moves.
struct compilation_t {
    source_manager_t sources;
    /// The main file.
    file_id_t file{};
    /// Sorted by position.
    diagnostic_bag_t diagnostics;
    std::vector<token_t> tokens;
    ast_t ast;
    analysis_t analysis;
    /// The package, for a program a project.toml makes.
    std::optional<package_info_t> package;
    /// The [native] libraries of its packages, which it links.
    native_libraries_t natives;
    /// Where a project.toml names each of `natives`' system libraries, in their order.
    std::vector<span_t> system_spans;

    [[nodiscard]] const source_file_t& source() const {
        return sources.get(file);
    }
};

/// A file a program can import besides its main file, by its path in the program: one of the
/// standard library's, like "std/core/error.fb", which is std.core.error, or one of the
/// program's own, by its path from the main file's folder, like "ui/hud.fb". It only declares
/// things.
struct library_file_t {
    std::string path;
    std::string text;
};

/// Where an Import looks for its module, by paths in the program, like library_file_t's.
struct import_paths_t {
    /// The file it names: "std/collections.fb" for std.collections, or "ui/hud.fb" for
    /// "./ui/hud" in main.fb.
    std::string file;
    /// For a dotted name, the folder whose files it names when the file isn't there:
    /// "std/collections". Its subfolders' files aren't among them.
    std::optional<std::string> folder;
};

/// What an Import can reach from a file: its package's own files, from its folder, the
/// modules of the packages its project.toml depends on, and the standard library's.
struct import_from_t {
    /// The importing file's path in the program.
    std::string_view importer;
    /// What the paths of the importer's package start with in the program: its name and a '/',
    /// like "game/" or "physics/", "std/" for the standard library, or "" without a
    /// project.toml.
    std::string_view package;
    /// How messages name the folder its paths can't leave: "the main file's folder", "the
    /// package's folder", or "the physics package's folder". std's is "std/".
    std::string_view folder;
    /// The importer's package's dependencies, by the names its project.toml gives them, which
    /// their files' paths in the program start with.
    std::span<const std::string> dependencies;
};

/// Where an Import in a file looks for `target`: a dotted name, like "std.collections", which is
/// std/collections.fb or the folder std/collections/, or "physics.shapes", in one of the
/// importer's package's dependencies, or its own, or when `quoted`, a path from the importer's
/// folder, with ".fb" added. Fails, saying why, for a dotted name that names neither, or a path
/// that's absolute, leaves its package's folder, or without a project.toml, goes into a folder
/// called std.
[[nodiscard]] std::expected<import_paths_t, std::string>
resolve_import(std::string_view target, bool quoted, const import_from_t& from);

/// resolve_import() for a program without a project.toml, whose own files are paths from its
/// main file's folder.
[[nodiscard]] std::expected<import_paths_t, std::string>
resolve_import(std::string_view target, bool quoted, std::string_view importer);

/// The symbols an #If finds defined in a build of `mode`: DEBUG or RELEASE, and the platform's,
/// like PLATFORM_WINDOWS.
[[nodiscard]] std::vector<std::string_view> defined_symbols(build_mode_t mode);

/// A main file through the front end, with the standard library, for a debug build: every
/// stage runs, even after errors. It loads std/core/, std.collections.map, and what their
/// imports and the main file's reach, each once.
[[nodiscard]] compilation_t compile(std::string path, std::string text);

/// Like compile(), but its imports find their files among `library`, in place of the standard
/// library and the disk, for a build of `mode`, which #If can test.
[[nodiscard]] compilation_t compile(std::string path, std::string text,
                                    std::vector<library_file_t> library,
                                    build_mode_t mode = build_mode_t::DEBUG);

/// Like compile(), with the files on disk: the standard library's in std/, and the program's
/// own. `path` is a .fb file, whose folder the program's own paths start from, unless a
/// project.toml in its folder or above makes it part of that package, or a package's folder,
/// whose project.toml names its main file. A package's dependencies' files come from their
/// folders. Fails when the main file or the standard library can't be read, or a folder has no
/// project.toml; a project.toml's mistakes are the compilation's diagnostics.
[[nodiscard]] std::expected<compilation_t, std::string>
compile_file(const std::filesystem::path& path, build_mode_t mode = build_mode_t::DEBUG);

/// The folder of the package that `folder` is in: the nearest one from it up with a
/// project.toml. nullopt when there's none.
[[nodiscard]] std::optional<std::filesystem::path>
find_package(const std::filesystem::path& folder);

/// The standard library's files, from std/ in the source tree, sorted by path: its modules, and
/// the .proto files of protobuf's well-known types. Each is named like "std/core/error.fb". Fails
/// when one can't be read.
[[nodiscard]] std::expected<std::vector<library_file_t>, std::string> standard_library();

/// The C libraries std is built on that a program links, for the runtime's functions it calls
/// that use them: each whose prefix one of their names starts with.
[[nodiscard]] std::vector<const build_config::bundled_library_t*>
bundled_libraries(const mir_module_t& module);

/// Writes an executable for a module from a compilation without errors, creating the output's
/// folder if needed, linking the bundled C libraries it uses. The object file in between is
/// temporary.
[[nodiscard]] std::expected<void, std::string> build_program(const mir_module_t& module,
                                                             const compilation_t& compilation,
                                                             const std::filesystem::path& output,
                                                             build_mode_t mode);

/// What a failed link's errors, `failure` as build_program() gave it, are about, at the
/// program's lines.
struct link_errors_t {
    /// A C symbol that no library defines, at the first Extern Function that declares it, and a
    /// system library that the linker can't find, at the project.toml line that names it.
    std::vector<diagnostic_t> errors;
    /// Whether those are all the linker's errors.
    bool complete = false;
};

[[nodiscard]] link_errors_t link_errors(const compilation_t& compilation, std::string_view failure);

/// A new path in the system's temporary folder, like "futureblitz/hello-1a2b3c4d.exe".
[[nodiscard]] std::filesystem::path temporary_path(const std::filesystem::path& stem,
                                                   std::string_view extension);

/// Removes `path`, a file or a folder and what's in it, that temporary_path() named, and gives
/// whether it's gone. A program that just ended can hold its files for a moment while Windows,
/// or a scanner that read the new executable, lets go of them, so a removal that fails is tried
/// again for up to two seconds.
bool remove_temporary(const std::filesystem::path& path);

} // namespace fb
