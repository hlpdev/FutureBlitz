#include "fb/driver.hpp"

#include "fb/build_config.hpp"
#include "fb/lexer.hpp"
#include "fb/link.hpp"
#include "fb/manifest.hpp"
#include "fb/parser.hpp"
#include "fb/process.hpp"
#include "fb/proto.hpp"
#include "fb/types.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

/// One of a program's packages on disk: what its project.toml says, and its folder.
struct package_t {
    std::string name;
    std::string version;
    std::filesystem::path root;
    /// What its files' paths in the program start with: its name and a '/', like "physics/".
    std::string prefix;
    /// Its dependencies, by the names its project.toml gives them.
    std::vector<std::string> dependencies;
    /// Its main file on disk, which isn't one of the modules an Import of its folder brings in,
    /// however project.toml spells it. Empty for a library.
    std::filesystem::path main;
    /// Its [native] libraries' names, which `Lib` gives.
    std::vector<std::string> natives;
    /// What they link: its archives, found in its folder, and system libraries.
    native_libraries_t linked;
    /// Where its project.toml names each of `linked`'s system libraries.
    std::vector<span_t> system_spans;
};

/// Where a program's files come from, by their paths in the program: "std/core/error.fb" for
/// the standard library's, and in a package, its name and then a path from its folder, like
/// "game/ui/hud.fb" or "physics/shapes.fb". Without a project.toml, the program's own are paths
/// from the main file's folder, like "ui/hud.fb".
struct file_set_t {
    /// In memory, for compile(): every file there is, sorted by path.
    std::vector<library_file_t> files;
    /// On disk, for compile_file(): the folder the program's own paths start from without a
    /// project.toml, or the main package's. The standard library's files are in
    /// build_config::std_directory.
    std::optional<std::filesystem::path> root;
    /// On disk, when a project.toml makes the program a package: the packages, the main one
    /// first.
    std::vector<package_t> packages;
    /// Files loaded as modules of their own, though nothing imports them: a library's, for its
    /// check.
    std::vector<std::string> extra;
};

/// The package whose files `path`, a path in the program, is in, or whose folder it names: its
/// first part is the package's name. Null for std's, and without a project.toml.
const package_t* package_named(const file_set_t& files, const std::string_view path) {
    for (const package_t& package : files.packages) {
        if (path == package.name || path.starts_with(package.prefix)) {
            return &package;
        }
    }

    return nullptr;
}

/// Where a file or folder of the program is on disk.
std::filesystem::path disk_path(const file_set_t& files, const std::string_view path) {
    if (path.starts_with("std/")) {
        return path_from_utf8(build_config::std_directory) / path_from_utf8(path.substr(4));
    }

    if (const package_t* package = package_named(files, path)) {
        return path == package->name
                   ? package->root
                   : package->root / path_from_utf8(path.substr(package->prefix.size()));
    }

    return files.root.value_or(std::filesystem::path()) / path_from_utf8(path);
}

/// Whether `path` is `folder` or under it, both as real_path() gives them, in the disk's letter
/// case.
bool is_under(const std::filesystem::path& path, const std::filesystem::path& folder) {
    auto at = path.begin();
    for (const std::filesystem::path& part : folder) {
        if (part.empty()) {
            continue;
        }

        if (at == path.end() || *at != part) {
            return false;
        }

        ++at;
    }

    return true;
}

/// Whether an entry of a folder is a link or a junction, which leads somewhere other than its
/// own name in the folder.
bool is_link(const std::filesystem::path& entry) {
    const std::optional<std::filesystem::path> real = real_path(entry);
    const std::optional<std::filesystem::path> folder = real_path(entry.parent_path());
    return !real || !folder || *real != *folder / entry.filename();
}

/// A file on disk as messages name it: from the current folder when it's under it, or else
/// where it is.
std::filesystem::path shown_disk_path(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    std::filesystem::path normal = path.lexically_normal();
    if (error || !normal.is_absolute()) {
        return normal;
    }

    const std::filesystem::path relative = normal.lexically_relative(current);
    return relative.empty() || *relative.begin() == ".." ? normal : relative;
}

/// How messages name a file: by its path in the program, or for the program's own files on
/// disk, by where they are.
std::string shown_path(const file_set_t& files, const std::string_view path) {
    if (!files.root || path.starts_with("std/")) {
        return std::string(path);
    }

    return to_utf8(shown_disk_path(disk_path(files, path)));
}

/// `text` with ASCII letters in lower case, as a disk that ignores letter case compares names.
std::string folded(const std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return lower;
}

/// Splits a path at its '/'s, or '\'s, leaving out empty parts and ".".
std::vector<std::string_view> path_parts(const std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = std::min(path.find_first_of("/\\", begin), path.size());
        const std::string_view part = path.substr(begin, end - begin);
        if (!part.empty() && part != ".") {
            parts.push_back(part);
        }

        begin = end + 1;
    }

    return parts;
}

/// A file's text, or nullopt when there's no such file.
std::optional<std::string> read_file(const file_set_t& files, const std::string_view path) {
    if (!files.root) {
        const auto found = std::ranges::find(files.files, path, &library_file_t::path);
        return found != files.files.end() ? std::optional(found->text) : std::nullopt;
    }

    const std::filesystem::path where = disk_path(files, path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(where, error)) {
        return std::nullopt;
    }

    std::ifstream file(where, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    return std::string{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// The paths of the .fb files in a folder, without its subfolders', sorted. Empty when there's
/// no such folder.
std::vector<std::string> folder_files(const file_set_t& files, const std::string_view folder) {
    const std::string prefix = std::string(folder) + "/";
    std::vector<std::string> found;
    if (!files.root) {
        for (const library_file_t& file : files.files) {
            if (file.path.starts_with(prefix) && file.path.ends_with(".fb") &&
                file.path.find('/', prefix.size()) == std::string::npos) {
                found.push_back(file.path);
            }
        }

        return found;
    }

    std::error_code error;
    for (auto entry = std::filesystem::directory_iterator(disk_path(files, folder), error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        std::error_code ignored;
        if (entry->is_regular_file(ignored) && entry->path().extension() == ".fb") {
            found.push_back(prefix + to_utf8(entry->path().filename()));
        }
    }

    // The order the folder lists them in varies, and the program shouldn't.
    std::ranges::sort(found);
    return found;
}

/// Every standard library file the file set has, sorted.
std::vector<std::string> std_files(const file_set_t& files) {
    std::vector<std::string> found;
    if (!files.root) {
        for (const library_file_t& file : files.files) {
            if (file.path.starts_with("std/") && file.path.ends_with(".fb")) {
                found.push_back(file.path);
            }
        }

        return found;
    }

    const std::filesystem::path folder = path_from_utf8(build_config::std_directory);
    std::error_code error;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
         !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        std::error_code ignored;
        if (entry->is_regular_file(ignored) && entry->path().extension() == ".fb") {
            found.push_back("std/" + to_utf8(entry->path().lexically_relative(folder)));
        }
    }

    std::ranges::sort(found);
    return found;
}

/// What an Import names to bring in a standard library module's names: its folder's module, as
/// in "std.collections" for std/collections/map.fb, or its own for a file right in std/.
std::string std_import_name(const std::string_view path) {
    const std::string name = module_name(path);
    return std::ranges::count(path, '/') > 1 ? name.substr(0, name.rfind('.')) : name;
}

/// A .proto file the front end has read, and whether it, or a file it imports, has errors, which
/// leave its module empty.
struct proto_entry_t {
    proto_file_t file;
    bool failed = false;
};

/// One file the front end has loaded: its path in the program, and the files its imports name.
struct loaded_t {
    std::string path;
    file_id_t file{};
    /// For each of its Import and From lines, the loaded files it names, by index.
    std::vector<std::pair<stmt_id_t, std::vector<std::size_t>>> imports;
    /// Where the Import that first named it is. nullopt for the files every program loads.
    std::optional<span_t> imported_at;
    /// For the module of a .proto file, which fb makes from it, the file. Its Imports of the
    /// modules of the files it imports name their paths in the program.
    const proto_entry_t* proto = nullptr;
};

/// Loads a program's files into a compilation: the main file, std/core/, std.collections.map,
/// and what their imports reach, each file once. Then it puts them in the module table in the
/// order their Globals initialize: std/core/'s first, then each module after the ones it
/// imports, depth-first in the order of its Import lines, and the main module last.
class loader_t {
public:
    loader_t(compilation_t& compilation, file_set_t files, const build_mode_t mode)
        : m_compilation(compilation), m_files(std::move(files)), m_defined(defined_symbols(mode)) {
        for (std::size_t i = 0; i < m_files.packages.size(); ++i) {
            m_folders.push_back(
                i == 0 ? "the package's folder"
                       : std::format("the {} package's folder", m_files.packages[i].name));
        }
    }

    /// The main file is in the compilation already, at `path` in the program.
    void run(const std::string& path) {
        const source_file_t& main = m_compilation.source();
        m_loaded.push_back({ .path = path, .file = main.id() });
        m_keys.emplace(key_of(path), 0);
        parse_file(0, &m_compilation.tokens);

        // The language uses std/core/'s Error, Interfaces, and Iterator, and a Map literal needs
        // std.collections.map, whether or not anything imports them.
        for (const std::string& core : folder_files(m_files, "std/core")) {
            if (const std::optional<std::size_t> loaded = load(core)) {
                m_core.push_back(*loaded);
            }
        }

        m_map = load("std/collections/map.fb");
        for (const std::string& extra : m_files.extra) {
            if (const std::optional<std::size_t> loaded = load(extra)) {
                m_extra.push_back(*loaded);
            }
        }

        // Loading a file adds its imports' files, which come after it.
        for (std::size_t i = 0; i < m_loaded.size(); ++i) {
            resolve_imports(i);
        }

        add_modules();
        add_std_names();
    }

private:
    compilation_t& m_compilation;
    file_set_t m_files;
    std::vector<std::string_view> m_defined;
    std::vector<loaded_t> m_loaded;
    /// Each loaded file, by key_of() its path.
    std::map<std::string, std::size_t, std::less<>> m_keys;
    /// std/core/'s files, and std.collections.map's, by index in m_loaded.
    std::vector<std::size_t> m_core;
    std::optional<std::size_t> m_map;
    /// The file set's extra files, by index in m_loaded.
    std::vector<std::size_t> m_extra;
    /// For each loaded file, the AST items it added.
    std::vector<std::vector<stmt_id_t>> m_items;
    /// How messages name each package's folder.
    std::vector<std::string> m_folders;
    /// The .proto files read, which stay put while more are read.
    std::vector<std::unique_ptr<proto_entry_t>> m_protos;
    /// The keys of the .proto files being read, each importing the next, for a cycle's error.
    std::vector<std::string> m_reading;

    /// The package a file of the program is in, by index in the file set's packages, which is
    /// 0, the main one, for every file of a program without a project.toml. nullopt for the
    /// standard library's.
    [[nodiscard]] std::optional<std::size_t> package_of(const std::string_view path) const {
        if (path.starts_with("std/")) {
            return std::nullopt;
        }

        const package_t* package = package_named(m_files, path);
        return package != nullptr ? static_cast<std::size_t>(package - m_files.packages.data()) : 0;
    }

    /// The folder of another package that what an Import names is in, inside the folder of
    /// the package its path starts with, or the main file's: the nearest folder with a
    /// project.toml of its own between them. Its files are that package's, which only its name
    /// imports.
    [[nodiscard]] std::optional<std::filesystem::path>
    nested_package(const import_paths_t& paths) const {
        const std::string& path = paths.file.empty() ? paths.folder.value_or("") : paths.file;
        if (!m_files.root || path.empty() || path.starts_with("std/")) {
            return std::nullopt;
        }

        const package_t* package = package_named(m_files, path);
        const std::filesystem::path& root = package != nullptr ? package->root : *m_files.root;
        std::error_code ignored;
        const std::filesystem::path folder = std::filesystem::weakly_canonical(root, ignored);
        std::filesystem::path at =
            std::filesystem::weakly_canonical(disk_path(m_files, path), ignored);
        // A dotted name's folder of modules can be a package's folder itself.
        if (paths.folder && !std::filesystem::is_regular_file(at, ignored)) {
            at = std::filesystem::weakly_canonical(disk_path(m_files, *paths.folder), ignored);
        }

        // Up from what's imported to the package's folder, which the disk says when it's reached,
        // whatever the letter case of the names on the way.
        std::vector<std::filesystem::path> between;
        for (; !std::filesystem::equivalent(at, folder, ignored); at = at.parent_path()) {
            if (!at.has_relative_path()) {
                return std::nullopt;
            }

            between.push_back(at);
        }

        for (const std::filesystem::path& each : between) {
            if (std::filesystem::is_regular_file(each / "project.toml", ignored)) {
                return each;
            }
        }

        return std::nullopt;
    }

    /// What an Import in the file at `path` can reach.
    [[nodiscard]] import_from_t origin_of(const std::string_view path) const {
        if (path.starts_with("std/")) {
            return { .importer = path, .package = "std/", .folder = "std/" };
        }

        if (m_files.packages.empty()) {
            return { .importer = path, .folder = "the main file's folder" };
        }

        const std::size_t index = package_of(path).value_or(0);
        const package_t& package = m_files.packages[index];
        return {
            .importer = path,
            .package = package.prefix,
            .folder = m_folders[index],
            .dependencies = package.dependencies,
        };
    }

    /// What tells two paths apart: in memory, the path itself, and on disk, where the file
    /// really is, with links and junctions followed and in the disk's letter case, so a file
    /// reached by two paths loads once. One that isn't there is where its path leads.
    [[nodiscard]] std::string key_of(const std::string_view path) const {
        if (!m_files.root) {
            return std::string(path);
        }

        const std::filesystem::path where = disk_path(m_files, path);
        if (const std::optional<std::filesystem::path> real = real_path(where)) {
            return to_utf8(*real);
        }

        std::error_code ignored;
        return to_utf8(std::filesystem::weakly_canonical(where, ignored));
    }

    /// A loaded file that's the file at `path` on disk, though their keys differ: where the
    /// system gives a real path in the letter case it was asked for, on a disk that ignores
    /// letter case, "UI/Hud.fb" is "ui/hud.fb".
    [[nodiscard]] std::optional<std::size_t> same_file(const std::string_view path) const {
        if (!m_files.root) {
            return std::nullopt;
        }

        const std::string key = folded(key_of(path));
        const std::filesystem::path where = disk_path(m_files, path);
        for (const auto& [loaded_key, index] : m_keys) {
            std::error_code ignored;
            if (folded(loaded_key) == key &&
                std::filesystem::equivalent(where, disk_path(m_files, m_loaded[index].path),
                                            ignored)) {
                return index;
            }
        }

        return std::nullopt;
    }

    /// Loads and parses the file at `path`, once, for the Import at `imported_at` if any.
    /// nullopt when there's no such file.
    std::optional<std::size_t> load(const std::string& path,
                                    const std::optional<span_t> imported_at = std::nullopt) {
        if (const auto found = m_keys.find(key_of(path)); found != m_keys.end()) {
            return found->second;
        }

        if (const std::optional<std::size_t> same = same_file(path)) {
            m_keys.emplace(key_of(path), *same);
            return same;
        }

        std::optional<std::string> text = read_file(m_files, path);
        if (!text) {
            return std::nullopt;
        }

        const file_id_t file =
            m_compilation.sources.add(shown_path(m_files, path), std::move(*text));
        const std::size_t index = m_loaded.size();
        m_loaded.push_back({ .path = path, .file = file, .imported_at = imported_at });
        m_keys.emplace(key_of(path), index);
        parse_file(index, nullptr);
        return index;
    }

    /// Where std keeps protobuf's well-known types, like google/protobuf/timestamp.proto, which a
    /// .proto imports by that path.
    static constexpr std::string_view well_known_types = "std/data/proto/";

    /// Whether the program has a file at `path`.
    [[nodiscard]] bool has_file(const std::string_view path) const {
        if (!m_files.root) {
            return std::ranges::contains(m_files.files, path, &library_file_t::path);
        }

        std::error_code ignored;
        return std::filesystem::is_regular_file(disk_path(m_files, path), ignored);
    }

    /// The module of the .proto file an Import names: the file from the importing file's folder,
    /// or for a path in google/protobuf/ that isn't there, the well-known type std has.
    std::optional<std::size_t> load_imported_proto(const import_stmt_t& node,
                                                   const import_paths_t& paths) {
        if (has_file(paths.file) || !node.target.starts_with("google/protobuf/")) {
            return load_proto(paths.file, node.target_span);
        }

        const std::string standard = std::string(well_known_types) + node.target;
        return has_file(standard) ? load_proto(standard, node.target_span) : std::nullopt;
    }

    /// The file a .proto's import names, by its path in the program: the path from the importing
    /// file's folder, or else from each folder above it up to the folder its package's paths start
    /// from, the nearest first, as protoc finds a file in the first of its import paths that has
    /// it, or else one of protobuf's well-known types that std has. nullopt, after an error, when
    /// there's none.
    std::optional<std::string> find_proto_import(const std::string& importer,
                                                 const proto_import_t& imported) {
        const std::vector<std::string_view> written = path_parts(imported.path);
        const bool canonical =
            !imported.path.empty() && !imported.path.starts_with('/') &&
            !imported.path.contains('\\') &&
            std::ranges::none_of(written,
                                 [](const std::string_view part) { return part == ".."; }) &&
            written.size() == static_cast<std::size_t>(std::ranges::count(imported.path, '/') + 1);
        if (!canonical) {
            m_compilation.diagnostics.error(imported.span,
                                            "an import's path goes down from a folder, with '/' "
                                            "between folders and without '.' or '..', like "
                                            "\"shapes/circle.proto\"");
            return std::nullopt;
        }

        const std::string_view package = origin_of(importer).package;
        std::vector<std::string_view> folder = path_parts(importer);
        folder.pop_back();
        const std::size_t fixed = path_parts(package).size();
        while (true) {
            std::string candidate;
            for (const std::string_view part : folder) {
                candidate += std::string(part) + "/";
            }

            candidate += imported.path;
            if (has_file(candidate) && !linked_away(candidate)) {
                if (m_files.root) {
                    warn_respelled_proto(imported, candidate);
                }

                return candidate;
            }

            if (folder.size() <= fixed) {
                break;
            }

            folder.pop_back();
        }

        std::string standard = std::string(well_known_types) + imported.path;
        if (imported.path.starts_with("google/protobuf/") && has_file(standard)) {
            return standard;
        }

        m_compilation.diagnostics.error(imported.span,
                                        R"(there's no "{}" in this file's folder or the folders )"
                                        "above it, up to {}",
                                        imported.path, folder_of(importer));
        return std::nullopt;
    }

    /// Reads the .proto file at `path` in the program, once, with the .proto files it imports, and
    /// gives its module, which fb makes from it, for the Import or import line at `imported_at`.
    /// nullopt when there's no such file. Errors in it or a file it imports leave its module
    /// empty, past them.
    std::optional<std::size_t> load_proto(const std::string& path, const span_t imported_at) {
        const std::string key = key_of(path);
        if (const auto found = m_keys.find(key); found != m_keys.end()) {
            return found->second;
        }

        if (const auto reading = std::ranges::find(m_reading, key); reading != m_reading.end()) {
            std::string chain;
            for (const std::string& each : std::ranges::subrange(reading, m_reading.end())) {
                chain += std::format("{} -> ", to_utf8(path_from_utf8(each).filename()));
            }

            m_compilation.diagnostics.error(imported_at,
                                            ".proto files can't import each other in a cycle: "
                                            "{}{}",
                                            chain, to_utf8(path_from_utf8(path).filename()));
            return std::nullopt;
        }

        std::optional<std::string> text = read_file(m_files, path);
        if (!text) {
            return std::nullopt;
        }

        const std::string shown = shown_path(m_files, path);
        const file_id_t source = m_compilation.sources.add(shown, std::move(*text));
        const std::size_t errors = m_compilation.diagnostics.error_count();
        auto& entry = *m_protos.emplace_back(std::make_unique<proto_entry_t>(proto_entry_t{
            .file = parse_proto(m_compilation.sources.get(source), m_compilation.diagnostics),
        }));
        entry.file.path = path;
        m_reading.push_back(key);
        for (proto_import_t& imported : entry.file.imports) {
            const std::optional<std::string> found = find_proto_import(path, imported);
            const std::optional<std::size_t> module =
                found ? load_proto(*found, imported.span) : std::nullopt;
            if (module && m_loaded[*module].proto != nullptr) {
                imported.file = &m_loaded[*module].proto->file;
                entry.failed = entry.failed || m_loaded[*module].proto->failed;
            }
        }

        m_reading.pop_back();
        entry.failed = entry.failed || m_compilation.diagnostics.error_count() > errors;
        if (!entry.failed) {
            resolve_proto(entry.file, m_compilation.diagnostics);
            entry.failed = m_compilation.diagnostics.error_count() > errors;
        }

        std::string module =
            entry.failed ? failed_proto_module(entry.file, shown) : generate_proto(entry.file);
        const file_id_t file = m_compilation.sources.add(shown + ".fb", std::move(module));
        const std::size_t index = m_loaded.size();
        m_loaded.push_back(
            { .path = path, .file = file, .imported_at = imported_at, .proto = &entry });
        m_keys.emplace(key, index);
        parse_file(index, nullptr);
        return index;
    }

    /// The name in `folder` on disk that `name` spells, in its own letter cases, if any: one
    /// spelled the same way, or else one whose ASCII letters differ only in case, or else the
    /// one the disk finds by `name`, as it does for other letters in other cases, which isn't a
    /// link to it.
    [[nodiscard]] static std::optional<std::string> on_disk(const std::filesystem::path& folder,
                                                            const std::string_view name) {
        std::optional<std::string> found;
        std::error_code error;
        for (auto entry = std::filesystem::directory_iterator(folder, error);
             !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
            std::string entry_name = to_utf8(entry->path().filename());
            if (entry_name == name) {
                return entry_name;
            }

            if (!found && folded(entry_name) == folded(name)) {
                found = entry_name;
            }
        }

        const std::filesystem::path written = folder / path_from_utf8(name);
        if (found || !std::filesystem::exists(written, error)) {
            return found;
        }

        for (auto entry = std::filesystem::directory_iterator(folder, error);
             !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
            std::error_code ignored;
            if (!is_link(entry->path()) &&
                std::filesystem::equivalent(entry->path(), written, ignored)) {
                return to_utf8(entry->path().filename());
            }
        }

        return std::nullopt;
    }

    /// Warns where an Import spells a folder or file in other letter cases than the disk has
    /// it, which only a disk that ignores them finds, as Windows' and macOS's usually do. Only
    /// what the Import writes is compared: the parts of a quoted path, from the importing file's
    /// folder, or of a dotted name after std or the package's name.
    void warn_respelled(const import_stmt_t& node, const std::string& importer,
                        const import_paths_t& paths) {
        std::filesystem::path real;
        std::vector<std::string_view> parts;
        std::string written;
        if (node.quoted) {
            real = disk_path(m_files, importer).parent_path();
            parts = path_parts(node.target);
            written = node.target.starts_with("./") ? "." : "";
        } else {
            const std::string_view first =
                std::string_view(node.target).substr(0, std::string_view(node.target).find('.'));
            real = disk_path(m_files, first == "std" ? "std/" : first);
            std::string_view rest = node.target;
            rest = rest.size() > first.size() ? rest.substr(first.size() + 1) : "";
            for (std::size_t begin = 0; begin < rest.size();) {
                const std::size_t end = std::min(rest.find('.', begin), rest.size());
                parts.push_back(rest.substr(begin, end - begin));
                begin = end + 1;
            }

            written = first;
        }

        bool differs = false;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const std::string_view part = parts[i];
            const char* separator = node.quoted ? "/" : ".";
            if (part == "..") {
                real = real.parent_path();
                written += written.empty() ? ".." : std::format("{}..", separator);
                continue;
            }

            // The last part is a file, or for a dotted name, maybe a folder.
            const bool last = i + 1 == parts.size();
            std::optional<std::string> found = last && !paths.file.empty()
                                                   ? on_disk(real, std::format("{}.fb", part))
                                                   : std::nullopt;
            if (found && !found->ends_with(".fb")) {
                // A module's file ends in .fb, which only a disk that ignores letter cases finds
                // as another spelling of it.
                m_compilation.diagnostics.warning(
                    node.target_span,
                    "the disk spells its file {}, in other letter cases, which only a disk that "
                    "ignores them finds; a module's file ends in .fb",
                    *found);
                return;
            }

            if (found) {
                found = found->substr(0, found->size() - 3);
            } else {
                found = on_disk(real, part);
            }

            if (!found) {
                return; // nothing there, which the Import reports
            }

            differs = differs || *found != part;
            real /= path_from_utf8(*found);
            written += std::format("{}{}", written.empty() ? "" : separator, *found);
        }

        if (differs) {
            m_compilation.diagnostics.warning(
                node.target_span,
                "the disk spells it {}, in other letter cases, which only a disk that ignores "
                "them finds",
                node.quoted ? std::format("\"{}\"", written) : written);
        }
    }

    /// Warns where a .proto's import, found at `found` in the program, spells a folder or file in
    /// other letter cases than the disk has it, as warn_respelled does for an Import.
    void warn_respelled_proto(const proto_import_t& imported, const std::string& found) {
        std::filesystem::path real =
            disk_path(m_files, found.substr(0, found.size() - imported.path.size()));
        std::string written;
        bool differs = false;
        for (const std::string_view part : path_parts(imported.path)) {
            const std::optional<std::string> spelled = on_disk(real, part);
            if (!spelled) {
                return;
            }

            differs = differs || *spelled != part;
            real /= path_from_utf8(*spelled);
            written += std::format("{}{}", written.empty() ? "" : "/", *spelled);
        }

        if (differs) {
            m_compilation.diagnostics.warning(imported.span,
                                              "the disk spells it \"{}\", in other letter cases, "
                                              "which only a disk that ignores them finds",
                                              written);
        }
    }

    /// Lexes and parses a loaded file into the AST, keeping its tokens in `tokens` if given.
    void parse_file(const std::size_t index, std::vector<token_t>* tokens) {
        const source_file_t& file = m_compilation.sources.get(m_loaded[index].file);
        std::vector<token_t> lexed = lex(file, m_compilation.diagnostics, m_defined);
        const std::size_t before = m_compilation.ast.items().size();
        parse_into(file, lexed, m_compilation.diagnostics, m_compilation.ast);
        m_items.resize(m_loaded.size());
        m_items[index].assign(m_compilation.ast.items().begin() +
                                  static_cast<std::ptrdiff_t>(before),
                              m_compilation.ast.items().end());
        if (tokens != nullptr) {
            *tokens = std::move(lexed);
        }
    }

    /// Finds the files a loaded file's Import and From lines name, loading the new ones. Each
    /// that names nothing is reported at the line.
    void resolve_imports(const std::size_t index) {
        for (const stmt_id_t id : std::vector<stmt_id_t>(m_items[index])) {
            const auto* found = std::get_if<import_stmt_t>(&m_compilation.ast.get(id).node);
            if (found == nullptr) {
                continue;
            }

            // Loading a file adds to the AST, which can move its statements, so this is a copy.
            const import_stmt_t node = *found;
            std::vector<std::size_t> targets;
            const std::string& importer = m_loaded[index].path;
            // A .proto's module imports those of the files it imports by their paths, which are
            // loaded already.
            if (m_loaded[index].proto != nullptr && node.quoted) {
                if (const auto loaded = m_keys.find(key_of(node.target)); loaded != m_keys.end()) {
                    targets.push_back(loaded->second);
                }

                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            const std::expected<import_paths_t, std::string> paths =
                resolve_import(node.target, node.quoted, origin_of(importer));
            if (!paths) {
                m_compilation.diagnostics.error(node.target_span, "{}", paths.error());
                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            if (m_files.root) {
                warn_respelled(node, importer, *paths);
            }

            // An Import stays in its package's files, though another package's folder can be
            // in its own, and a link or junction there can lead anywhere.
            if (const std::optional<std::filesystem::path> nested = nested_package(*paths)) {
                report_nested(node, *paths, *nested);
                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            if (report_linked(node, *paths)) {
                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            // A folder whose files are all links away was reported for them, not as missing.
            bool linked = false;
            if (paths->file.ends_with(".proto")) {
                if (const std::optional<std::size_t> file = load_imported_proto(node, *paths)) {
                    targets.push_back(*file);
                }
            } else if (const std::optional<std::size_t> file =
                           paths->file.empty() ? std::nullopt
                                               : load(paths->file, node.target_span)) {
                targets.push_back(*file);
            } else if (paths->folder) {
                // A package's main file isn't one of the modules its folder gives, and neither
                // is the program's main module, which can be another file.
                const package_t* package = package_named(m_files, *paths->folder);
                for (const std::string& path : folder_files(m_files, *paths->folder)) {
                    std::error_code ignored;
                    if (package != nullptr && !package->main.empty() &&
                        std::filesystem::equivalent(disk_path(m_files, path), package->main,
                                                    ignored)) {
                        continue;
                    }

                    if (const std::optional<std::filesystem::path> away = linked_away(path)) {
                        m_compilation.diagnostics.error(
                            node.target_span,
                            "{} is a link to '{}', outside {}; an Import stays in its package's "
                            "files",
                            shown_path(m_files, path), to_utf8(shown_disk_path(*away)),
                            folder_of(path));
                        linked = true;
                        continue;
                    }

                    if (const std::optional<std::size_t> loaded = load(path, node.target_span);
                        loaded && *loaded != 0) {
                        targets.push_back(*loaded);
                    }
                }
            }

            if (targets.empty() && !linked) {
                report_missing(node, *paths);
            }

            m_loaded[index].imports.emplace_back(id, std::move(targets));
        }
    }

    /// Where what's at `path` on disk really is, when a link or junction takes it out of the
    /// folder of the package its path is in, the main file's for a program without one: no
    /// Import reaches another package's files, or any outside its own, by a path that looks like
    /// one of its own.
    [[nodiscard]] std::optional<std::filesystem::path> linked_away(const std::string& path) const {
        if (!m_files.root || path.empty() || path.starts_with("std/")) {
            return std::nullopt;
        }

        const std::optional<std::size_t> package = package_of(path);
        const std::filesystem::path& folder =
            package && !m_files.packages.empty() ? m_files.packages[*package].root : *m_files.root;
        std::optional<std::filesystem::path> real = real_path(disk_path(m_files, path));
        const std::optional<std::filesystem::path> real_folder = real_path(folder);
        if (!real || !real_folder || is_under(*real, *real_folder)) {
            return std::nullopt;
        }

        return real;
    }

    /// How messages name the folder of the package a file is in.
    [[nodiscard]] std::string folder_of(const std::string& path) const {
        if (m_files.packages.empty()) {
            return "the main file's folder";
        }

        return m_folders[package_of(path).value_or(0)];
    }

    /// Reports an Import whose file or folder a link or junction takes out of its package's
    /// folder.
    bool report_linked(const import_stmt_t& node, const import_paths_t& paths) {
        std::error_code ignored;
        const bool file =
            !paths.file.empty() && std::filesystem::exists(disk_path(m_files, paths.file), ignored);
        const std::string& path = file ? paths.file : paths.folder.value_or("");
        const std::optional<std::filesystem::path> away = linked_away(path);
        if (!away) {
            return false;
        }

        const std::string shown = node.quoted ? std::format("\"{}\"", node.target) : node.target;
        m_compilation.diagnostics.error(
            node.target_span,
            "{} is a link to '{}', outside {}; an Import stays in its package's files", shown,
            to_utf8(shown_disk_path(*away)), folder_of(path));
        return true;
    }

    /// Reports an Import that names a file of the package in `folder`, which is in its own
    /// package's folder.
    void report_nested(const import_stmt_t& node, const import_paths_t& paths,
                       const std::filesystem::path& folder) {
        const std::string shown = node.quoted ? std::format("\"{}\"", node.target) : node.target;
        std::error_code ignored;
        const bool file = !paths.file.empty() &&
                          std::filesystem::is_regular_file(disk_path(m_files, paths.file), ignored);
        const std::string_view what = file ? "a file" : "a folder";
        const auto named = std::ranges::find_if(m_files.packages, [&](const package_t& package) {
            return std::filesystem::equivalent(package.root, folder, ignored);
        });
        if (named != m_files.packages.end()) {
            m_compilation.diagnostics.error(
                node.target_span,
                "{} names {} of the {} package, in its folder; import its modules by its name",
                shown, what, named->name);
        } else if (m_files.packages.empty()) {
            m_compilation.diagnostics.error(
                node.target_span,
                "{} names {} of the package in '{}', which has a project.toml of its own; a "
                "program imports a package's modules by its name, once its own project.toml "
                "depends on it",
                shown, what, to_utf8(folder));
        } else {
            m_compilation.diagnostics.error(
                node.target_span,
                "{} names {} of the package in '{}', which has a project.toml of its own; make it "
                "a dependency, and import its modules by its name",
                shown, what, to_utf8(folder));
        }
    }

    /// "there's no module std.nothing: no std/nothing.fb, and no folder std/nothing/ of
    /// modules", or for a quoted path, the file it looked for.
    void report_missing(const import_stmt_t& node, const import_paths_t& paths) {
        if (!node.quoted && paths.file.empty()) {
            m_compilation.diagnostics.error(
                node.target_span, "the package {} has no modules in its folder, {}", node.target,
                shown_path(m_files, paths.folder.value_or("")));
            return;
        }

        // A folder whose files end in .FB, or .Fb, has none a module's file ends in, .fb.
        if (!node.quoted && paths.folder) {
            if (const std::optional<std::string> other = respelled_file(*paths.folder)) {
                m_compilation.diagnostics.error(
                    node.target_span,
                    "there's no module {}: {}/ has {}, but a module's file ends in .fb",
                    node.target, shown_path(m_files, *paths.folder), *other);
                return;
            }
        }

        if (!node.quoted) {
            m_compilation.diagnostics.error(
                node.target_span, "there's no module {}: no {}, and no folder {}/ of modules",
                node.target, shown_path(m_files, paths.file),
                shown_path(m_files, paths.folder.value_or("")));
            return;
        }

        if (node.target.ends_with(".fb")) {
            m_compilation.diagnostics.error(
                node.target_span, R"(there's no module "{}": an Import leaves out .fb, as in "{}")",
                node.target, node.target.substr(0, node.target.size() - 3));
            return;
        }

        m_compilation.diagnostics.error(node.target_span, "there's no module \"{}\": no {}",
                                        node.target, shown_path(m_files, paths.file));
    }

    /// The first file in `folder` on disk, by name, whose extension is .fb in other letter cases.
    [[nodiscard]] std::optional<std::string> respelled_file(const std::string& folder) const {
        if (!m_files.root) {
            return std::nullopt;
        }

        std::optional<std::string> first;
        std::error_code error;
        for (auto entry = std::filesystem::directory_iterator(disk_path(m_files, folder), error);
             !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
            const std::string name = to_utf8(entry->path().filename());
            if (folded(to_utf8(entry->path().extension())) == ".fb" && !name.ends_with(".fb") &&
                (!first || name < *first)) {
                first = name;
            }
        }

        return first;
    }

    /// The loaded files in the order their Globals initialize, by index.
    [[nodiscard]] std::vector<std::size_t> initialization_order() const {
        std::vector<std::size_t> order;
        std::vector<bool> visited(m_loaded.size());
        const std::function<void(std::size_t)> visit = [&](const std::size_t index) {
            if (visited[index]) {
                return;
            }

            visited[index] = true;
            for (const auto& [statement, targets] : m_loaded[index].imports) {
                for (const std::size_t target : targets) {
                    visit(target);
                }
            }

            order.push_back(index);
        };

        for (const std::size_t core : m_core) {
            visit(core);
        }

        if (m_map) {
            visit(*m_map);
        }

        for (const std::size_t extra : m_extra) {
            visit(extra);
        }

        // The main file is the first loaded, and every other is reached from these.
        visit(0);
        return order;
    }

    /// Puts each loaded file in the module table, in the order its Globals initialize, with what
    /// it imports, and the AST's items in that order too.
    void add_modules() {
        const std::vector<std::size_t> order = initialization_order();
        std::vector<module_id_t> ids(m_loaded.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            ids[order[i]] = static_cast<module_id_t>(i);
        }

        // The standard library is a package of its own, and so is each of the program's: its
        // project.toml's, or without one, the main file and what it imports by path.
        module_table_t& table = m_compilation.ast.modules();
        const std::string main_name = to_utf8(path_from_utf8(m_loaded[0].path).stem());
        const package_id_t std_package = table.add_package("std", true, std::vector<std::string>());
        std::vector<package_id_t> packages;
        if (m_files.packages.empty()) {
            packages.push_back(table.add_package(main_name));
        }

        for (const package_t& package : m_files.packages) {
            packages.push_back(table.add_package(package.name, false, package.natives));
        }

        for (std::size_t i = 0; i < m_files.packages.size(); ++i) {
            for (const std::string& dependency : m_files.packages[i].dependencies) {
                const auto on = std::ranges::find(m_files.packages, dependency, &package_t::name);
                if (on != m_files.packages.end()) {
                    table.add_dependency(packages[i],
                                         packages[static_cast<std::size_t>(
                                             std::distance(m_files.packages.begin(), on))]);
                }
            }
        }

        // Two files can't be one module, as a.b.fb and a/b.fb would be, since names in MIR start
        // with the module's.
        std::map<std::string, std::size_t, std::less<>> names;
        for (const std::size_t index : order) {
            const loaded_t& loaded = m_loaded[index];
            module_t module{
                .name =
                    index == 0 && m_files.packages.empty() ? main_name : module_name(loaded.path),
                .path = loaded.path,
                .file = loaded.file,
                .is_main = index == 0,
                .package = loaded.path.starts_with("std/")
                               ? std_package
                               : packages[package_of(loaded.path).value_or(0)],
            };
            if (const auto [taken, added] = names.emplace(module.name, index); !added) {
                m_compilation.diagnostics.error(
                    loaded.imported_at.value_or(span_t{ .file = loaded.file }),
                    "{}'s module is named {}, and so is {}'s; rename one of them",
                    shown_path(m_files, loaded.path), module.name,
                    shown_path(m_files, m_loaded[taken->second].path));
            }

            for (const auto& [statement, targets] : loaded.imports) {
                module_import_t imported{ .statement = statement };
                for (const std::size_t target : targets) {
                    imported.modules.push_back(ids[target]);
                }

                module.imports.push_back(std::move(imported));
            }

            table.add(std::move(module));
        }

        m_compilation.ast.sort_items();
    }

    /// What the standard library's modules that aren't loaded declare Public, for the error that
    /// a name isn't declared to say which module has it. Each is parsed on its own, without
    /// reporting anything.
    void add_std_names() {
        for (const std::string& path : std_files(m_files)) {
            if (m_keys.contains(key_of(path))) {
                continue;
            }

            std::optional<std::string> text = read_file(m_files, path);
            if (!text) {
                continue;
            }

            source_manager_t sources;
            diagnostic_bag_t ignored;
            const source_file_t& file = sources.get(sources.add(path, std::move(*text)));
            const ast_t ast = fb::parse(file, lex(file, ignored, m_defined), ignored);
            const std::string module = std_import_name(path);
            for (const stmt_id_t id : ast.items()) {
                for (const std_name_t& name : declared_names(ast, id, module)) {
                    m_compilation.ast.modules().add_std_name(name);
                }
            }
        }
    }

    /// The Public names a top-level statement of a standard library module declares, and the
    /// Public Methods and Properties it adds to T[] or a built-in type, like CString's ToString
    /// or an Int64's Seconds. Importing the module wouldn't bring in the rest.
    static std::vector<std_name_t> declared_names(const ast_t& ast, const stmt_id_t id,
                                                  const std::string& module) {
        const stmt_node_t& node = ast.get(id).node;
        const auto named = [&](const std::string_view name, const modifiers_t& modifiers,
                               const bool array_method = false,
                               const std::string_view extended = {}) {
            if (modifiers.visibility != visibility_t::PUBLIC) {
                return std::vector<std_name_t>{};
            }

            return std::vector<std_name_t>{
                {
                    .name = std::string(name),
                    .module = module,
                    .array_method = array_method,
                    .extended = std::string(extended),
                },
            };
        };

        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            return named(type->name.text, type->modifiers);
        }

        if (const auto* declared = std::get_if<enum_stmt_t>(&node)) {
            return named(declared->name.text, declared->modifiers);
        }

        if (const auto* function = std::get_if<function_stmt_t>(&node)) {
            return named(function->name.text, function->modifiers);
        }

        if (const auto* variable = std::get_if<variable_stmt_t>(&node)) {
            return named(variable->name.text, variable->modifiers);
        }

        const auto* extension = std::get_if<extend_stmt_t>(&node);
        if (extension == nullptr) {
            return {};
        }

        const bool array = std::holds_alternative<array_type_t>(ast.get(extension->type).node);
        const auto* built_in = std::get_if<named_type_t>(&ast.get(extension->type).node);
        const std::optional<type_id_t> type = built_in != nullptr && !built_in->qualifier
                                                  ? builtin_type(built_in->name)
                                                  : std::nullopt;
        if (!array && !type) {
            return {};
        }

        std::vector<std_name_t> members;
        const std::string_view extended = type ? builtin_name(*type) : std::string_view();
        for (const stmt_id_t member : extension->members) {
            if (const auto* method = std::get_if<function_stmt_t>(&ast.get(member).node)) {
                std::ranges::copy(named(method->name.text, method->modifiers, array, extended),
                                  std::back_inserter(members));
            } else if (const auto* property = std::get_if<property_stmt_t>(&ast.get(member).node)) {
                std::ranges::copy(named(property->name.text, property->modifiers, array, extended),
                                  std::back_inserter(members));
            }
        }

        return members;
    }
};

/// The main file is in `compilation` already, at `path` in the program, and the files it
/// imports come from `files`.
void run_front_end(compilation_t& compilation, file_set_t files, const std::string& path,
                   const build_mode_t mode) {
    loader_t(compilation, std::move(files), mode).run(path);
    compilation.analysis =
        analyze(compilation.sources, compilation.ast, compilation.diagnostics, mode);
    // Each stage adds its own diagnostics, so sorting puts them back in source order.
    compilation.diagnostics.sort();
}

/// Reads the packages a program is made of: the main one's project.toml, then those of the
/// packages it depends on, and theirs, each once. What's wrong goes in the compilation's
/// diagnostics, at the project.toml's lines, except a main project.toml that can't be read.
class package_loader_t {
public:
    explicit package_loader_t(compilation_t& compilation) : m_compilation(compilation) {}

    /// The packages, the main one, in `folder`, first. nullopt after an error.
    std::optional<std::vector<package_t>> run(const std::filesystem::path& folder) {
        std::vector<std::size_t> chain;
        if (!load(folder, nullptr, chain) || m_failed) {
            return std::nullopt;
        }

        return std::move(m_packages);
    }

    /// The main package's project.toml, once run() has read it.
    [[nodiscard]] const manifest_t& main() const {
        return m_manifests.front();
    }

    /// The main package's project.toml's file, once it's loaded.
    [[nodiscard]] std::optional<file_id_t> main_file() const {
        return m_main_file;
    }

    /// Why the main package's project.toml couldn't be read, if it couldn't.
    [[nodiscard]] const std::optional<std::string>& unreadable() const {
        return m_unreadable;
    }

private:
    compilation_t& m_compilation;
    std::vector<package_t> m_packages;
    std::vector<manifest_t> m_manifests;
    /// Each package's folder, as weakly_canonical() has it.
    std::vector<std::filesystem::path> m_folders;
    /// The folders whose project.toml has mistakes, which are reported once.
    std::vector<std::filesystem::path> m_broken;
    std::optional<file_id_t> m_main_file;
    std::optional<std::string> m_unreadable;
    bool m_failed = false;

    template <typename... Args>
    void fail(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        m_compilation.diagnostics.error(span, fmt, std::forward<Args>(args)...);
        m_failed = true;
    }

    /// The package in `folder`, which the dependency `named` names, if any, read with the ones it
    /// depends on. `chain` is the packages on the way to it, for a cycle's message.
    std::optional<std::size_t> load(const std::filesystem::path& folder, const dependency_t* named,
                                    std::vector<std::size_t>& chain) {
        std::error_code ignored;
        const std::filesystem::path root = std::filesystem::weakly_canonical(folder, ignored);
        // A folder read already, by another path or in other letter cases, is the same package,
        // which every dependency on it names alike.
        for (std::size_t index = 0; index < m_folders.size(); ++index) {
            if (!std::filesystem::equivalent(m_folders[index], root, ignored)) {
                continue;
            }

            if (named != nullptr && m_packages[index].name != named->name) {
                fail(named->name_span,
                     "'{}' names the package in '{}', which its project.toml names {}", named->name,
                     to_utf8(root), m_packages[index].name);
            } else if (named != nullptr && std::ranges::contains(chain, index)) {
                std::string cycle;
                for (const std::size_t link :
                     chain | std::views::drop(std::ranges::find(chain, index) - chain.begin())) {
                    cycle += m_packages[link].name + " -> ";
                }

                fail(named->name_span, "packages can't depend on each other in a cycle: {}{}",
                     cycle, m_packages[index].name);
            }

            return index;
        }

        if (std::ranges::any_of(m_broken, [&](const std::filesystem::path& broken) {
                return std::filesystem::equivalent(broken, root, ignored);
            })) {
            return std::nullopt;
        }

        const std::filesystem::path manifest_path = root / "project.toml";
        const bool there = std::filesystem::is_regular_file(manifest_path, ignored);
        std::string missing = std::format("there's no project.toml in '{}'", to_utf8(root));
        if (std::filesystem::is_regular_file(root, ignored)) {
            missing = std::format("'{}' is a file, not a package's folder", to_utf8(root));
        }

        const std::expected<file_id_t, std::string> file =
            there ? m_compilation.sources.load(shown_disk_path(manifest_path))
                  : std::unexpected(missing);
        if (!file) {
            if (named != nullptr) {
                fail(named->path_span, "{}, which {} names", file.error(), named->name);
            } else {
                m_unreadable = file.error();
            }

            return std::nullopt;
        }

        if (named == nullptr) {
            m_main_file = *file;
        }

        std::optional<manifest_t> manifest =
            read_manifest(m_compilation.sources.get(*file), m_compilation.diagnostics);
        if (!manifest) {
            m_failed = true;
            m_broken.push_back(root);
            return std::nullopt;
        }

        if (named != nullptr && manifest->name != named->name) {
            fail(named->name_span,
                 "'{}' names the package in '{}', which its project.toml names {}", named->name,
                 to_utf8(root), manifest->name);
            return std::nullopt;
        }

        if (const auto same = std::ranges::find(m_packages, manifest->name, &package_t::name);
            same != m_packages.end()) {
            fail(named != nullptr ? named->name_span : manifest->name_span,
                 "two packages are named {}: the one in '{}', and the one in '{}'", manifest->name,
                 to_utf8(same->root), to_utf8(root));
            return std::nullopt;
        }

        const std::size_t index = m_packages.size();
        package_t package{
            .name = manifest->name,
            .version = manifest->version,
            .root = root,
            .prefix = manifest->name + "/",
        };
        for (const dependency_t& dependency : manifest->dependencies) {
            package.dependencies.push_back(dependency.name);
        }

        // The main file project.toml names has to be there. Without 'main', a package without
        // main.fb is a library.
        const std::filesystem::path main = root / path_from_utf8(manifest->main);
        if (std::filesystem::is_regular_file(main, ignored)) {
            package.main = std::filesystem::weakly_canonical(main, ignored);
        } else if (manifest->main_span) {
            fail(*manifest->main_span, "there's no {} in the package's folder, which 'main' names",
                 manifest->main);
        }

        for (const native_t& native : manifest->natives) {
            package.natives.push_back(native.name);
            if (native.kind == native_kind_t::SYSTEM) {
                package.linked.system.push_back(native.path);
                package.system_spans.push_back(native.path_span);
            } else if (native.path.ends_with('/') || native.path.ends_with('\\')) {
                // A folder's path, whose archives would have names like "lib.a".
                fail(native.path_span, "\"{}\" is a folder, not a static library", native.path);
            } else if (const std::optional<std::filesystem::path> archive =
                           find_archive(root, native.path)) {
                if (is_archive(*archive)) {
                    package.linked.archives.push_back(*archive);
                } else {
                    fail(native.path_span,
                         "\"{}\" names {}, which isn't a static library: it doesn't start as an "
                         "archive does",
                         native.path, shown(native.path, *archive));
                }
            } else {
                report_no_archive(root, native);
            }
        }

        const std::vector<dependency_t> dependencies = manifest->dependencies;
        m_packages.push_back(std::move(package));
        m_manifests.push_back(std::move(*manifest));
        m_folders.push_back(root);
        chain.push_back(index);
        for (const dependency_t& dependency : dependencies) {
            (void)load(root / path_from_utf8(dependency.path), &dependency, chain);
        }

        chain.pop_back();
        return index;
    }

    /// Whether a file starts as an archive does, as a static library's .a or .lib.
    static bool is_archive(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::array<char, 8> start{};
        in.read(start.data(), start.size());
        const std::string_view read(start.data(), static_cast<std::size_t>(in.gcount()));
        return read == "!<arch>\n" || read == "!<thin>\n";
    }

    /// A file found for a static library's path, `written`, as written, with its name as found.
    static std::string shown(const std::string& written, const std::filesystem::path& found) {
        std::string text = to_utf8(path_from_utf8(written).parent_path() / found.filename());
        std::ranges::replace(text, '\\', '/');
        return text;
    }

    /// Whether `extension` is `name`, in any letter case.
    static bool extension_is(const std::filesystem::path& path, const std::string_view name) {
        const std::string extension = to_utf8(path.extension());
        return std::ranges::equal(extension, name, [](const char a, const char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        });
    }

    /// The archive a static library's path from `root` names, `written`: the file itself, or
    /// without its "lib" prefix and extension, libx.a, x.a, or x.lib in its folder.
    static std::optional<std::filesystem::path> find_archive(const std::filesystem::path& root,
                                                             const std::string& written) {
        std::error_code ignored;
        const std::filesystem::path path = root / path_from_utf8(written);
        for (const std::filesystem::path& candidate : archive_names(path)) {
            if (std::filesystem::is_regular_file(candidate, ignored)) {
                return std::filesystem::weakly_canonical(candidate, ignored);
            }
        }

        return std::nullopt;
    }

    /// The files a static library's path can name: the file itself, and unless it has an
    /// archive's extension, libx.a, x.a, and x.lib.
    static std::vector<std::filesystem::path> archive_names(const std::filesystem::path& path) {
        if (extension_is(path, ".a") || extension_is(path, ".lib")) {
            return { path };
        }

        const std::filesystem::path folder = path.parent_path();
        const std::string name = to_utf8(path.filename());
        return {
            path,
            folder / path_from_utf8("lib" + name + ".a"),
            folder / path_from_utf8(name + ".a"),
            folder / path_from_utf8(name + ".lib"),
        };
    }

    /// ""libs/x" names no static library: there's no libs/x, libs/libx.a, libs/x.a, or
    /// libs/x.lib, from the package's folder", by the paths as written, or that it's a folder,
    /// or a dynamic library.
    void report_no_archive(const std::filesystem::path& root, const native_t& native) {
        const std::filesystem::path written = path_from_utf8(native.path);
        std::error_code ignored;
        if (extension_is(written, ".so") || extension_is(written, ".dylib") ||
            extension_is(written, ".dll")) {
            fail(native.path_span,
                 "\"{}\" is a dynamic library, and [native] links those in M7; link a static "
                 "archive, like \"libs/{}\"",
                 native.path, to_utf8(written.stem()));
            return;
        }

        if (std::filesystem::is_directory(root / written, ignored)) {
            fail(native.path_span, "\"{}\" is a folder, not a static library", native.path);
            return;
        }

        std::vector<std::string> names;
        for (const std::filesystem::path& name : archive_names(written)) {
            std::string text = to_utf8(name);
            std::ranges::replace(text, '\\', '/');
            names.push_back(std::move(text));
        }

        std::string listed = names.front();
        for (std::size_t i = 1; i < names.size(); ++i) {
            listed += std::format("{}{}", i + 1 == names.size() ? ", or " : ", ", names[i]);
        }

        fail(native.path_span, "\"{}\" names no static library: there's no {}{}", native.path,
             listed, written.is_absolute() ? "" : ", from the package's folder");
    }
};

/// Puts each package in `finished` after every package it depends on, from `index`.
void finish(const std::vector<package_t>& packages, const std::size_t index,
            std::vector<bool>& seen, std::vector<std::size_t>& finished) {
    seen[index] = true;
    for (const std::string& name : std::views::reverse(packages[index].dependencies)) {
        const auto found = std::ranges::find(packages, name, &package_t::name);
        const auto at = static_cast<std::size_t>(found - packages.begin());
        if (found != packages.end() && !seen[at]) {
            finish(packages, at, seen, finished);
        }
    }

    finished.push_back(index);
}

/// The native libraries of a program's packages, as link_program() takes them: each package's
/// archives before those of the packages it depends on, the main package's first and then its
/// dependencies' in its project.toml's order where nothing else decides, and each system
/// library once, with where it's named in `spans`.
native_libraries_t natives_of(const std::vector<package_t>& packages, std::vector<span_t>& spans) {
    std::vector<bool> seen(packages.size());
    std::vector<std::size_t> finished;
    finish(packages, 0, seen, finished);
    native_libraries_t natives;
    for (const std::size_t index : std::views::reverse(finished)) {
        const native_libraries_t& linked = packages[index].linked;
        std::ranges::copy(linked.archives, std::back_inserter(natives.archives));
        for (std::size_t i = 0; i < linked.system.size(); ++i) {
            if (!std::ranges::contains(natives.system, linked.system[i])) {
                natives.system.push_back(linked.system[i]);
                spans.push_back(packages[index].system_spans[i]);
            }
        }
    }

    return natives;
}

/// `path`'s path from `root`, with '/'s, as the program's paths are.
std::string program_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::string joined;
    for (const std::filesystem::path& part : path.lexically_relative(root)) {
        joined += (joined.empty() ? "" : "/") + to_utf8(part);
    }

    return joined;
}

/// A library's files, which its check loads: each .fb file from its folder down, but not its
/// build/ folder, hidden folders, links, or the folders of other packages in it, which have
/// their own project.toml.
std::vector<std::string> library_files(const package_t& package) {
    std::vector<std::string> files;
    std::error_code error;
    for (auto entry = std::filesystem::recursive_directory_iterator(package.root, error);
         !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        const std::string name = to_utf8(entry->path().filename());
        std::error_code ignored;
        if (is_link(entry->path())) {
            entry.disable_recursion_pending();
        } else if (entry->is_directory(ignored)) {
            if (name.starts_with('.') || (entry.depth() == 0 && name == "build") ||
                std::filesystem::exists(entry->path() / "project.toml", ignored)) {
                entry.disable_recursion_pending();
            }
        } else if (entry->path().extension() == ".fb") {
            files.push_back(package.prefix + program_path(package.root, entry->path()));
        }
    }

    std::ranges::sort(files);
    return files;
}

/// A package's program: its main file, given or its project.toml's, or for a library, its
/// files, each a module of its own, so it's checked.
std::expected<compilation_t, std::string>
compile_package(const std::filesystem::path& folder,
                const std::optional<std::filesystem::path>& main, const build_mode_t mode) {
    compilation_t compilation;
    package_loader_t reader(compilation);
    std::optional<std::vector<package_t>> packages = reader.run(folder);
    if (const std::optional<std::string>& unreadable = reader.unreadable()) {
        return std::unexpected(*unreadable);
    }

    // The project.toml's mistakes are the compilation's, about the main project.toml.
    const auto stopped = [&] {
        compilation.file = reader.main_file().value_or(file_id_t{});
        compilation.diagnostics.sort();
        return std::move(compilation);
    };
    if (!packages) {
        return stopped();
    }

    const package_t& own = packages->front();
    const manifest_t& manifest = reader.main();
    compilation.natives = natives_of(*packages, compilation.system_spans);
    compilation.package = package_info_t{
        .name = own.name,
        .version = own.version,
        .root = own.root,
        .program = own.name,
    };
    std::error_code error;
    const std::filesystem::path written =
        std::filesystem::weakly_canonical(own.root / path_from_utf8(manifest.main), error);
    std::filesystem::path main_path = written;
    if (main) {
        main_path = std::filesystem::weakly_canonical(*main, error);
        if (!std::filesystem::equivalent(main_path, written, error)) {
            compilation.package->program = to_utf8(main_path.stem());
        }
    } else if (!std::filesystem::is_regular_file(written, error)) {
        // Without a main file, it's a library, whose files are each checked. The main file is
        // empty, and named unlike any of them.
        compilation.package->library = true;
        std::vector<std::string> extra = library_files(own);
        std::string name = "(library).fb";
        for (int i = 2; std::ranges::contains(extra, own.prefix + name); ++i) {
            name = std::format("(library {}).fb", i);
        }

        compilation.file =
            compilation.sources.add(to_utf8(shown_disk_path(own.root / name)), std::string());
        const std::filesystem::path root = own.root;
        const std::string path = own.prefix + name;
        run_front_end(compilation,
                      { .root = root, .packages = std::move(*packages), .extra = std::move(extra) },
                      path, mode);
        return compilation;
    }

    const std::expected<file_id_t, std::string> file =
        compilation.sources.load(shown_disk_path(main_path));
    if (!file) {
        return std::unexpected(file.error());
    }

    compilation.file = *file;
    const std::filesystem::path root = own.root;
    const std::string path = own.prefix + program_path(own.root, main_path);
    run_front_end(compilation, { .root = root, .packages = std::move(*packages) }, path, mode);
    return compilation;
}

} // namespace

std::vector<std::string_view> defined_symbols(const build_mode_t mode) {
    std::vector<std::string_view> defined{ mode == build_mode_t::RELEASE ? "RELEASE" : "DEBUG" };
#ifdef _WIN32
    defined.emplace_back("PLATFORM_WINDOWS");
#elifdef __APPLE__
    defined.emplace_back("PLATFORM_MACOS");
#elifdef __linux__
    defined.emplace_back("PLATFORM_LINUX");
#endif
    return defined;
}

std::expected<import_paths_t, std::string>
resolve_import(const std::string_view target, const bool quoted, const std::string_view importer) {
    return resolve_import(
        target, quoted,
        {
            .importer = importer,
            .package = importer.starts_with("std/") ? "std/" : "",
            .folder = importer.starts_with("std/") ? "std/" : "the main file's folder",
        });
}

std::expected<import_paths_t, std::string>
resolve_import(const std::string_view target, const bool quoted, const import_from_t& from) {
    if (target == "std") {
        return std::unexpected(
            "std is the standard library's folder; import one of its modules, like "
            "std.collections");
    }

    // A dotted name starts with std, or a dependency's name: "physics" is its folder's modules,
    // and "physics.shapes" is physics/shapes.fb or the folder physics/shapes/.
    if (!quoted) {
        const std::string_view first = target.substr(0, target.find('.'));
        const bool own = from.package.size() > 1 && from.package != "std/" &&
                         first == from.package.substr(0, from.package.size() - 1);
        if (first != "std" && !own && !std::ranges::contains(from.dependencies, first)) {
            std::string named;
            for (const std::string& dependency : from.dependencies) {
                named += std::format("{}{}", named.empty() ? " (" : ", ", dependency);
            }

            const std::string_view file = target.substr(target.rfind('.') + 1);
            if (from.package.empty() || from.package == "std/") {
                return std::unexpected(std::format(
                    "there's no module {}: a module's name starts with std, like "
                    "std.collections, or a dependency's name in project.toml{}, and the "
                    "program's own files are imported by their paths, like \"./{}\"",
                    target, named.empty() ? "" : named + ")", file));
            }

            return std::unexpected(std::format(
                "there's no module {}: a module's name starts with std, like std.collections, "
                "this package's name, {}, or a dependency's name in project.toml{}; its own files "
                "are imported by their paths too, like \"./{}\"",
                target, from.package.substr(0, from.package.size() - 1),
                named.empty() ? "" : named + ")", file));
        }

        std::string folder(target);
        std::ranges::replace(folder, '.', '/');
        if (first != "std" && target == first) {
            return import_paths_t{ .folder = folder };
        }

        return import_paths_t{ .file = folder + ".fb", .folder = folder };
    }

    const bool absolute = target.starts_with('/') || target.starts_with('\\') ||
                          (target.size() > 1 && target[1] == ':');
    if (target.empty() || absolute) {
        return std::unexpected(std::format(
            R"("{}" isn't a path from this file's folder, like "./hud" or "../ui/hud")", target));
    }

    // Its last part is a file's name, without ".fb".
    const std::string_view last = target.substr(target.find_last_of("/\\") + 1);
    if (last.empty() || last == "." || last == "..") {
        return std::unexpected(std::format(
            R"("{}" names a folder; a quoted path names a .fb file, without ".fb", like "./hud")",
            target));
    }

    // A path goes from the importing file's folder, and stays in its package's: std/ for std's
    // own files, a dependency's folder for its, or else the main package's.
    const std::size_t fixed = path_parts(from.package).size();
    std::vector<std::string_view> parts = path_parts(from.importer);
    if (!parts.empty()) {
        parts.pop_back();
    }

    for (const std::string_view part : path_parts(target)) {
        if (part != "..") {
            parts.push_back(part);
        } else if (parts.size() > fixed) {
            parts.pop_back();
        } else {
            return std::unexpected(std::format("\"{}\" leaves {}, where the paths of this "
                                               "program's modules start",
                                               target, from.folder));
        }
    }

    std::string file;
    for (const std::string_view part : parts) {
        file += (file.empty() ? "" : "/") + std::string(part);
    }

    // Without a project.toml, a program's own folders can't be named std, as its files' paths
    // start.
    if (from.package.empty() && parts.size() > 1 && parts.front() == "std") {
        return std::unexpected(std::format(
            "\"{}\" names a file in a folder called std, which is the standard library's name",
            target));
    }

    // A .proto file's path names it, extension and all.
    return import_paths_t{ .file = target.ends_with(".proto") ? file : file + ".fb" };
}

compilation_t compile(std::string path, std::string text) {
    std::expected<std::vector<library_file_t>, std::string> library = standard_library();
    compilation_t compilation =
        compile(std::move(path), std::move(text), library.value_or(std::vector<library_file_t>{}));
    if (!library) {
        compilation.diagnostics.error({ .file = compilation.file }, "{}", library.error());
    }

    return compilation;
}

compilation_t compile(std::string path, std::string text, std::vector<library_file_t> library,
                      const build_mode_t mode) {
    // A path given twice keeps its first text.
    file_set_t files{ .files = std::move(library) };
    std::ranges::stable_sort(files.files, {}, &library_file_t::path);

    // The main file's path in the program is its name, from its own folder.
    const std::string name = to_utf8(path_from_utf8(path).filename());
    compilation_t compilation;
    compilation.file = compilation.sources.add(std::move(path), std::move(text));
    run_front_end(compilation, std::move(files), name, mode);
    return compilation;
}

std::expected<compilation_t, std::string> compile_file(const std::filesystem::path& path,
                                                       const build_mode_t mode) {
    std::error_code error;
    // Every program loads std/core/ and std.collections.map.
    const std::filesystem::path std_folder = path_from_utf8(build_config::std_directory);
    if (!std::filesystem::is_regular_file(std_folder / "core" / "error.fb", error) ||
        !std::filesystem::is_regular_file(std_folder / "collections" / "map.fb", error)) {
        return std::unexpected(
            std::format("cannot read the standard library in '{}'", to_utf8(std_folder)));
    }
    if (std::filesystem::is_directory(path, error)) {
        if (!std::filesystem::is_regular_file(path / "project.toml", error)) {
            return std::unexpected(std::format(
                "there's no project.toml in '{}'; give a package's folder, or a .fb file",
                to_utf8(path)));
        }

        return compile_package(path, std::nullopt, mode);
    }

    if (std::filesystem::is_regular_file(path, error) &&
        folded(to_utf8(path.extension())) != ".fb") {
        return std::unexpected(std::format("'{}' isn't a .fb file; give a .fb file, or a package's "
                                           "folder",
                                           to_utf8(path)));
    }

    // A file in a package's folder, or under it, is that package's main file.
    if (std::filesystem::is_regular_file(path, error)) {
        const std::filesystem::path folder =
            path.has_parent_path() ? path.parent_path() : std::filesystem::current_path(error);
        if (const std::optional<std::filesystem::path> root = find_package(folder)) {
            return compile_package(*root, path, mode);
        }
    }

    compilation_t compilation;
    const std::expected<file_id_t, std::string> file = compilation.sources.load(path);
    if (!file) {
        return std::unexpected(file.error());
    }

    compilation.file = *file;
    const std::filesystem::path folder = path.has_parent_path() ? path.parent_path() : ".";
    run_front_end(compilation, { .root = folder }, to_utf8(path.filename()), mode);
    return compilation;
}

std::optional<std::filesystem::path> find_package(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::path at = std::filesystem::weakly_canonical(folder, error);
    while (!at.empty()) {
        if (std::filesystem::is_regular_file(at / "project.toml", error)) {
            return at;
        }

        const std::filesystem::path parent = at.parent_path();
        if (parent == at) {
            break;
        }

        at = parent;
    }

    return std::nullopt;
}

std::expected<std::vector<library_file_t>, std::string> standard_library() {
    const std::filesystem::path folder(build_config::std_directory);
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
         !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        if (entry->is_regular_file() &&
            (entry->path().extension() == ".fb" || entry->path().extension() == ".proto")) {
            paths.push_back(entry->path());
        }
    }

    if (error) {
        return std::unexpected(std::format("cannot read the standard library in '{}': {}",
                                           to_utf8(folder), error.message()));
    }

    // The order the folder lists them in varies, and the program shouldn't.
    std::ranges::sort(paths);
    std::vector<library_file_t> library;
    library.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        std::string name = "std/" + to_utf8(path.lexically_relative(folder));
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected(std::format("cannot open '{}'", name));
        }

        library.push_back({
            .path = std::move(name),
            .text = { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() },
        });
    }

    return library;
}

std::vector<const build_config::bundled_library_t*> bundled_libraries(const mir_module_t& module) {
    std::vector<const build_config::bundled_library_t*> used;
    for (const build_config::bundled_library_t& library : build_config::bundled_libraries) {
        const bool calls =
            std::ranges::any_of(module.functions, [&](const mir_function_t& function) {
                return function.external && !function.external->c_abi &&
                       function.external->symbol.starts_with(library.prefix);
            });
        if (calls) {
            used.push_back(&library);
        }
    }

    return used;
}

std::expected<void, std::string> build_program(const mir_module_t& module,
                                               const compilation_t& compilation,
                                               const std::filesystem::path& output,
                                               const build_mode_t mode) {
    // The output's folder may not exist yet, as with -o or fb run. If it can't be made, linking
    // reports that.
    std::error_code ignored;
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path(), ignored);
    }

    // A bundled library's system libraries come with it, after the packages' own.
    native_libraries_t natives = compilation.natives;
    for (const build_config::bundled_library_t* library : bundled_libraries(module)) {
        natives.bundled.push_back(path_from_utf8(library->runtime));
        for (const auto part : std::views::split(library->archives, ';')) {
            natives.bundled.push_back(path_from_utf8(std::string_view(part.begin(), part.end())));
        }

        for (const auto part : std::views::split(library->system, ' ')) {
            const std::string name(part.begin(), part.end());
            if (!name.empty() && !std::ranges::contains(natives.system, name)) {
                natives.system.push_back(name);
            }
        }
    }

    // The object file is only needed for linking, and is removed even when linking fails.
    const std::filesystem::path object = temporary_path(output.stem(), ".o");
    std::expected<void, std::string> built =
        emit_object(module, compilation.sources, compilation.file, { .mode = mode }, object)
            .and_then([&] { return link_program(std::array{ object }, natives, output); });
    std::filesystem::remove(object, ignored);
    return built;
}

namespace {

/// The first Extern Function that declares the C symbol `symbol`, in the order the program's
/// files come in. Apple's C symbols have an '_' before the name, which the linker shows.
std::optional<diagnostic_t> undefined_symbol(const compilation_t& compilation,
                                             const std::string_view symbol) {
    const bool apple = build_config::target_triple.contains("apple");
    for (const bool underscored : { false, true }) {
        if (underscored && !apple) {
            break;
        }

        for (const stmt_id_t id : compilation.ast.items()) {
            const auto* block = std::get_if<extern_stmt_t>(&compilation.ast.get(id).node);
            if (block == nullptr || block->convention == convention_t::RUNTIME) {
                continue;
            }

            for (const stmt_id_t function : block->functions) {
                const auto& declared =
                    std::get<function_stmt_t>(compilation.ast.get(function).node);
                const external_t external = declared.external.value_or(external_t{});
                const std::string name = external.symbol.value_or(std::string(declared.name.text));
                if ((underscored ? "_" + name : name) != symbol) {
                    continue;
                }

                return diagnostic_t{
                    .severity = severity_t::ERROR,
                    .message = std::format("the C symbol '{}' isn't in {}, or any library the "
                                           "program links",
                                           name, block->library.value_or("the C runtime")),
                    .span = external.symbol_span.value_or(declared.name.span),
                };
            }
        }
    }

    return std::nullopt;
}

/// The error a line of the linker's is about, at the program's line, if it's one of those.
std::optional<diagnostic_t> link_error(const compilation_t& compilation,
                                       const std::string_view line) {
    // A DLL's symbols have a prefix.
    constexpr std::string_view undefined = "undefined symbol: ";
    if (const std::size_t at = line.find(undefined); at != std::string_view::npos) {
        std::string_view symbol = line.substr(at + undefined.size());
        if (symbol.starts_with("__declspec(dllimport) ")) {
            symbol.remove_prefix(std::string_view("__declspec(dllimport) ").size());
        }

        return undefined_symbol(compilation, symbol);
    }

    // ld.lld and lld-link say one thing, and ld64.lld another.
    for (const std::string_view library :
         { "unable to find library -l", "library not found for -l" }) {
        const std::size_t at = line.find(library);
        if (at == std::string_view::npos) {
            continue;
        }

        const std::string_view name = line.substr(at + library.size());
        const auto found = std::ranges::find(compilation.natives.system, name);
        if (found != compilation.natives.system.end()) {
            return diagnostic_t{
                .severity = severity_t::ERROR,
                .message = std::format("the linker can't find the system library '{}'", name),
                .span = compilation.system_spans[static_cast<std::size_t>(
                    found - compilation.natives.system.begin())],
            };
        }
    }

    return std::nullopt;
}

} // namespace

link_errors_t link_errors(const compilation_t& compilation, const std::string_view failure) {
    // lld starts each error with "ld.lld: error: ", or another of its names, and shows where a
    // symbol is used on the lines after it. The C++ driver adds that the linker failed.
    link_errors_t found{ .complete = true };
    std::vector<std::string_view> seen;
    for (const auto part : std::views::split(failure, '\n')) {
        std::string_view line(part.begin(), part.end());
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        const std::size_t at = line.find(": error: ");
        if (at == std::string_view::npos || line.find("lld") > at ||
            std::ranges::contains(seen, line)) {
            continue;
        }

        seen.push_back(line);
        if (std::optional<diagnostic_t> error = link_error(compilation, line)) {
            found.errors.push_back(std::move(*error));
        } else {
            found.complete = false;
        }
    }

    if (found.errors.empty()) {
        found.complete = false;
    }

    std::ranges::stable_sort(found.errors, {}, [](const diagnostic_t& error) {
        return std::pair(error.span.file, error.span.begin);
    });
    return found;
}

std::filesystem::path temporary_path(const std::filesystem::path& stem,
                                     const std::string_view extension) {
    std::error_code ignored;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path(ignored) / "futureblitz";
    std::filesystem::create_directories(folder, ignored);

    // A random suffix keeps compilations that run at the same time, like parallel tests, apart.
    std::random_device random;
    std::filesystem::path name = stem;
    name += std::format("-{:08x}{}", random(), extension);
    return folder / name;
}

} // namespace fb
