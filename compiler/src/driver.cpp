#include "fb/driver.hpp"

#include "fb/build_config.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/doc.hpp"
#include "fb/dynamic_library.hpp"
#include "fb/interface.hpp"
#include "fb/lexer.hpp"
#include "fb/link.hpp"
#include "fb/lockfile.hpp"
#include "fb/lower.hpp"
#include "fb/manifest.hpp"
#include "fb/packages.hpp"
#include "fb/parser.hpp"
#include "fb/process.hpp"
#include "fb/proto.hpp"
#include "fb/registry.hpp"
#include "fb/toolchain.hpp"
#include "fb/types.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utf8proc.h>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

/// The most bytes one Embed, EmbedText, or EmbedFolder puts in the program. A larger file is
/// better read when the program runs, and the compiler holds every embedded file at once.
constexpr std::uint64_t max_embedded = std::uint64_t{ 64 } * 1024 * 1024;

/// A file's name as an OS that ignores letter case compares it: case-folded, and composed, as
/// NFC, or as it is when it isn't UTF-8.
std::string folded_name(const std::string& name) {
    constexpr auto options = static_cast<utf8proc_option_t>(
        static_cast<unsigned>(UTF8PROC_STABLE) | static_cast<unsigned>(UTF8PROC_COMPOSE) |
        static_cast<unsigned>(UTF8PROC_CASEFOLD));
    utf8proc_uint8_t* mapped = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): utf8proc's bytes are chars
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(name.data());
    const utf8proc_ssize_t size =
        utf8proc_map(bytes, static_cast<utf8proc_ssize_t>(name.size()), &mapped, options);
    if (size < 0) {
        return name;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): utf8proc's bytes are chars
    std::string text(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(size));
    std::free(mapped); // NOLINT(cppcoreguidelines-no-malloc): utf8proc mallocs it
    return text;
}

/// One of a program's packages on disk: what its project.toml says, and its folder.
struct package_t {
    std::string name;
    std::string version;
    std::filesystem::path root;
    /// A library the program has beside its executable, in a file of its own.
    bool beside = false;
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
    /// For a prebuilt library: its interface file, and the modules in it, by their paths in the
    /// program, which it has in place of .fb files.
    std::filesystem::path interface;
    std::vector<library_file_t> modules;
};

/// A file the compiler makes, in memory: a module of a doc comment's example, which messages show
/// where its lines come from.
struct generated_file_t {
    /// Its path in the program.
    std::string path;
    std::string text;
    /// How messages name it: as they name the file its lines come from.
    std::string shown;
    line_origin_t origin;
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
    /// toolchain().std.
    std::optional<std::filesystem::path> root;
    /// On disk, when a project.toml makes the program a package: the packages, the main one
    /// first.
    std::vector<package_t> packages;
    /// Files loaded as modules of their own, though nothing imports them: a library's, for its
    /// check.
    std::vector<std::string> extra;
    /// For fb test's build, whose main package's other files with top-level code are other main
    /// files, whose code it leaves out.
    bool testing = false;
    /// On disk, the texts of files that aren't saved, for the LSP.
    source_reader_t read;
    /// The std names that checks share, for the LSP.
    std_names_cache_t* cache = nullptr;
    /// In memory, whether or not the rest is: the modules fb test makes of doc comments'
    /// examples.
    std::vector<generated_file_t> generated;
};

/// Notes that `id`'s file is at `path` on disk.
void note_disk_path(compilation_t& compilation, const file_id_t id, std::filesystem::path path) {
    const auto index = static_cast<std::size_t>(std::to_underlying(id));
    if (compilation.on_disk.size() <= index) {
        compilation.on_disk.resize(index + 1);
    }

    compilation.on_disk[index] = std::move(path);
}

/// A file's text on disk, or `read`'s in place of it, when it has one. nullopt when it can't be
/// read.
std::optional<std::string> read_disk(const std::filesystem::path& where,
                                     const source_reader_t& read) {
    if (read) {
        if (std::optional<std::string> text = read(where)) {
            return text;
        }
    }

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

/// The generated file at `path`, a path in the program, if there's one.
const generated_file_t* generated_file(const file_set_t& files, const std::string_view path) {
    const auto found = std::ranges::find(files.generated, path, &generated_file_t::path);
    return found != files.generated.end() ? &*found : nullptr;
}

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

/// The prebuilt library whose files `path`, a path in the program, is in, or whose folder it
/// names. Null for any other.
const package_t* prebuilt_package(const file_set_t& files, const std::string_view path) {
    const package_t* package = package_named(files, path);
    return package != nullptr && !package->interface.empty() ? package : nullptr;
}

/// A prebuilt library's module at `path`, if it has one.
const library_file_t* prebuilt_module(const package_t& package, const std::string_view path) {
    const auto found = std::ranges::find(package.modules, path, &library_file_t::path);
    return found != package.modules.end() ? &*found : nullptr;
}

/// Where a file or folder of the program is on disk.
std::filesystem::path disk_path(const file_set_t& files, const std::string_view path) {
    if (path.starts_with("std/")) {
        return toolchain().std / path_from_utf8(path.substr(4));
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
/// where it is. FB_PATHS=absolute asks for where it is always, as an editor with no folder open
/// does, whose problems can't be found from the folder fb runs in.
std::filesystem::path shown_disk_path(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    std::filesystem::path normal = path.lexically_normal();
    if (const char* paths = std::getenv("FB_PATHS");
        paths != nullptr && std::string_view(paths) == "absolute") {
        return error ? normal : (current / normal).lexically_normal();
    }

    if (error || !normal.is_absolute()) {
        return normal;
    }

    const std::filesystem::path relative = normal.lexically_relative(current);
    return relative.empty() || *relative.begin() == ".." ? normal : relative;
}

/// How messages name a file: by its path in the program, or for the program's own files on
/// disk, by where they are.
std::string shown_path(const file_set_t& files, const std::string_view path) {
    if (const generated_file_t* generated = generated_file(files, path)) {
        return generated->shown;
    }

    if (!files.root || path.starts_with("std/")) {
        return std::string(path);
    }

    // A prebuilt library's modules are in its interface file, at their own lines.
    if (const package_t* package = prebuilt_package(files, path)) {
        const std::string interface = to_utf8(shown_disk_path(package->interface));
        return prebuilt_module(*package, path) != nullptr ? interface : std::format("{} in {}", path, interface);
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
    if (const generated_file_t* generated = generated_file(files, path)) {
        return generated->text;
    }

    if (!files.root) {
        const auto found = std::ranges::find(files.files, path, &library_file_t::path);
        return found != files.files.end() ? std::optional(found->text) : std::nullopt;
    }

    if (const package_t* package = prebuilt_package(files, path)) {
        const library_file_t* module = prebuilt_module(*package, path);
        return module != nullptr ? std::optional(module->text) : std::nullopt;
    }

    return read_disk(disk_path(files, path), files.read);
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

    if (const package_t* package = prebuilt_package(files, folder)) {
        for (const library_file_t& module : package->modules) {
            if (module.path.starts_with(prefix) && module.path.ends_with(".fb") &&
                module.path.find('/', prefix.size()) == std::string::npos) {
                found.push_back(module.path);
            }
        }

        std::ranges::sort(found);
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

    const std::filesystem::path& folder = toolchain().std;
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
        : m_compilation(compilation), m_files(std::move(files)), m_mode(mode),
          m_defined(defined_symbols(mode)) {
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
        // The main file, which fb test's extras can name again, stays the last to initialize.
        for (const std::string& extra : m_files.extra) {
            if (const std::optional<std::size_t> loaded = load(extra); loaded && *loaded != 0) {
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

    /// The bytes of the file that Embed or EmbedText names with `target`, written at `at`.
    std::expected<std::shared_ptr<const std::string>, std::string>
    embed_file(const span_t at, const std::string_view target) {
        const std::expected<std::string, std::string> path = embedded_path(at.file, target, false);
        if (!path) {
            return std::unexpected(path.error());
        }

        warn_respelled_embed(at, target);

        if (is_folder(*path)) {
            return std::unexpected(std::format(
                R"("{}" is a folder; EmbedFolder puts a folder's files in the program)", target));
        }

        std::uint64_t size = 0;
        if (!file_size(*path, size)) {
            return std::unexpected(std::format(R"("{}" isn't a file: there's nothing at {})",
                                               target, shown_path(m_files, *path)));
        }

        if (size > max_embedded) {
            return too_large(target, "a file");
        }

        return read_embedded(*path);
    }

    /// The files under the folder that EmbedFolder names with `target` in the file `from`, by
    /// their paths in it, in order. Hidden ones, whose names start with '.', and those in hidden
    /// folders, are left out.
    std::expected<std::vector<embedded_file_t>, std::string>
    embed_folder(const span_t at, const std::string_view target) {
        const std::expected<std::string, std::string> path = embedded_path(at.file, target, true);
        if (!path) {
            return std::unexpected(path.error());
        }

        warn_respelled_embed(at, target);

        if (!is_folder(*path)) {
            std::uint64_t size = 0;
            return std::unexpected(
                file_size(*path, size)
                    ? std::format(R"("{}" is a file; Embed puts a file's bytes in the program)",
                                  target)
                    : std::format(R"("{}" isn't a folder: there's nothing at {})", target,
                                  shown_path(m_files, *path)));
        }

        // Data only: the program's own folder would bring its code along, and its executable
        // from the build before, each build's in the next. On disk, it's where the files really
        // are that counts, whatever letter case or link names the folder.
        const std::optional<std::filesystem::path> real_folder =
            m_files.root ? real_path(disk_path(m_files, *path)) : std::nullopt;
        const auto code = std::ranges::find_if(m_loaded, [&](const loaded_t& loaded) {
            if (!m_files.root) {
                return loaded.path.starts_with(within(*path, ""));
            }

            const std::optional<std::filesystem::path> real =
                real_path(disk_path(m_files, loaded.path));
            return real && real_folder && is_under(*real, *real_folder);
        });
        if (code != m_loaded.end()) {
            return std::unexpected(std::format(
                R"("{}" holds the program's own code, {}; EmbedFolder puts a folder of data in the program)",
                target, shown_path(m_files, code->path)));
        }

        std::vector<std::string> inside;
        // On disk, the links and junctions in it, which aren't followed, and the folders of
        // other packages.
        std::vector<std::string> links;
        std::vector<std::string> packages;
        if (!m_files.root) {
            const std::string prefix = within(*path, "");
            for (const library_file_t& file : m_files.files) {
                if (file.path.starts_with(prefix)) {
                    inside.push_back(file.path.substr(prefix.size()));
                }
            }
        } else {
            const std::filesystem::path folder = disk_path(m_files, *path);
            std::error_code error;
            for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
                 !error && entry != std::filesystem::recursive_directory_iterator();
                 entry.increment(error)) {
                std::error_code ignored;
                const std::string relative = to_utf8(entry->path().lexically_relative(folder));
                if (to_utf8(entry->path().filename()).starts_with('.')) {
                    entry.disable_recursion_pending();
                } else if (is_link(entry->path())) {
                    entry.disable_recursion_pending();
                    links.push_back(relative);
                } else if (entry->is_directory(ignored) &&
                           std::filesystem::is_regular_file(entry->path() / "project.toml",
                                                            ignored)) {
                    entry.disable_recursion_pending();
                    packages.push_back(relative);
                } else if (entry->is_regular_file(ignored)) {
                    inside.push_back(relative);
                }
            }

            if (error) {
                return std::unexpected(std::format("can't read the folder {}: {}",
                                                   shown_path(m_files, *path), error.message()));
            }
        }

        // A hidden file's or folder's name starts with '.'.
        std::erase_if(inside, [](const std::string& relative) {
            return std::ranges::any_of(path_parts(relative), [](const std::string_view part) {
                return part.starts_with('.');
            });
        });
        // The order the folder lists them in varies, and the program shouldn't, nor which of
        // its links or packages is reported.
        std::ranges::sort(inside);
        std::ranges::sort(links);
        std::ranges::sort(packages);
        // A link is followed on some systems and not others, and one in a cycle never ends, so
        // the program would differ by where it's built.
        if (!links.empty()) {
            const std::string file = within(*path, links.front());
            if (const std::optional<std::filesystem::path> real = linked_away(file)) {
                return std::unexpected(
                    std::format(R"("{}" in "{}" is a link to '{}', outside {})", links.front(),
                                target, to_utf8(shown_disk_path(*real)), folder_of(file)));
            }

            return std::unexpected(std::format(
                R"("{}" in "{}" is a link, which EmbedFolder doesn't follow, so the program is the same wherever it's built; embed what it leads to by its own path)",
                links.front(), target));
        }

        if (!packages.empty()) {
            return std::unexpected(std::format(
                R"("{}" holds the package in '{}', whose files only its own code embeds)", target,
                to_utf8(shown_disk_path(disk_path(m_files, within(*path, packages.front()))))));
        }

        // Every file is checked before any is read, so a folder that can't be embedded puts no
        // file in the compilation's list.
        std::uint64_t total = 0;
        for (const std::string& relative : inside) {
            const std::string file = within(*path, relative);
            std::uint64_t size = 0;
            if (!file_size(file, size)) {
                return std::unexpected(std::format("can't read {}", shown_path(m_files, file)));
            }

            total += size;
            if (total > max_embedded) {
                return too_large(target, "a folder whose files are");
            }
        }

        std::vector<embedded_file_t> files;
        for (const std::string& relative : inside) {
            std::expected<std::shared_ptr<const std::string>, std::string> bytes =
                read_embedded(within(*path, relative));
            if (!bytes) {
                return std::unexpected(bytes.error());
            }

            files.push_back({ .path = relative, .bytes = std::move(*bytes) });
        }

        return files;
    }

private:
    /// The path in the program of what an embed names with `target` in the file `from`, a folder
    /// when `folder` says: from that file's folder, and in its package's, as a quoted Import's
    /// path is, not through a link or junction that leads out of it, nor into another package
    /// inside it. Empty for the main file's folder without a project.toml.
    [[nodiscard]] std::expected<std::string, std::string>
    embedded_path(const file_id_t from, const std::string_view target, const bool folder) const {
        // Analysis only asks about the program's files, which the loader loaded.
        const import_from_t origin =
            origin_of(std::ranges::find(m_loaded, from, &loaded_t::file)->path);
        const bool absolute = target.starts_with('/') || target.starts_with('\\') ||
                              (target.size() > 1 && target[1] == ':');
        if (path_parts(target).empty() || absolute) {
            return std::unexpected(std::format(
                R"("{}" isn't a path from this file's folder, like "data/logo.png" or "../art/logo.png")",
                target));
        }

        const std::size_t fixed = path_parts(origin.package).size();
        std::vector<std::string_view> parts = path_parts(origin.importer);
        parts.pop_back();
        for (const std::string_view part : path_parts(target)) {
            if (part != "..") {
                parts.push_back(part);
            } else if (parts.size() > fixed) {
                parts.pop_back();
            } else {
                return std::unexpected(
                    std::format(R"("{}" leaves {}, which a program's embedded files are in)",
                                target, origin.folder));
            }
        }

        std::string path;
        for (const std::string_view part : parts) {
            path += (path.empty() ? "" : "/") + std::string(part);
        }

        // Without a project.toml, a program's own paths can't start with std, as the standard
        // library's do.
        if (origin.package.empty() && !parts.empty() && parts.front() == "std") {
            return std::unexpected(std::format(
                R"("{}" is in a folder called std, which is the standard library's name)", target));
        }

        if (const std::optional<std::filesystem::path> real = linked_away(path)) {
            return std::unexpected(std::format(R"("{}" is a link to '{}', outside {})", target,
                                               to_utf8(shown_disk_path(*real)), origin.folder));
        }

        const import_paths_t paths =
            folder ? import_paths_t{ .folder = path } : import_paths_t{ .file = path };
        if (const std::optional<std::filesystem::path> nested = nested_package(paths)) {
            return std::unexpected(std::format(
                R"("{}" is in the package in '{}', whose files only its own code embeds)", target,
                to_utf8(shown_disk_path(*nested))));
        }

        return path;
    }

    /// A path in the program under the folder at `folder`, which is empty for the main file's
    /// folder without a project.toml.
    [[nodiscard]] static std::string within(const std::string& folder,
                                            const std::string_view relative) {
        return folder.empty() ? std::string(relative) : folder + "/" + std::string(relative);
    }

    /// Whether there's a folder at `path` in the program.
    [[nodiscard]] bool is_folder(const std::string& path) const {
        if (const package_t* package = prebuilt_package(m_files, path)) {
            return std::ranges::any_of(package->modules, [&](const library_file_t& module) {
                return module.path.starts_with(within(path, ""));
            });
        }

        if (!m_files.root) {
            return std::ranges::any_of(m_files.files, [&](const library_file_t& file) {
                return file.path.starts_with(within(path, ""));
            });
        }

        std::error_code error;
        return std::filesystem::is_directory(disk_path(m_files, path), error);
    }

    /// The size of the file at `path` in the program, or false when there's none.
    [[nodiscard]] bool file_size(const std::string& path, std::uint64_t& size) const {
        if (const package_t* package = prebuilt_package(m_files, path)) {
            const library_file_t* module = prebuilt_module(*package, path);
            size = module != nullptr ? module->text.size() : 0;
            return module != nullptr;
        }

        if (!m_files.root) {
            const auto found = std::ranges::find(m_files.files, path, &library_file_t::path);
            if (found == m_files.files.end()) {
                return false;
            }

            size = found->text.size();
            return true;
        }

        std::error_code error;
        const std::filesystem::path where = disk_path(m_files, path);
        if (!std::filesystem::is_regular_file(where, error)) {
            return false;
        }

        size = std::filesystem::file_size(where, error);
        return !error;
    }

    /// The bytes of the file at `path` in the program, read once, which every embed of it
    /// shares, and the compilation lists among the files it embeds.
    std::expected<std::shared_ptr<const std::string>, std::string>
    read_embedded(const std::string& path) {
        if (const auto found = m_embedded.find(path); found != m_embedded.end()) {
            return found->second;
        }

        std::optional<std::string> bytes = read_file(m_files, path);
        const std::string shown = shown_path(m_files, path);
        if (!bytes) {
            return std::unexpected(std::format("can't read {}", shown));
        }

        m_compilation.embedded.push_back(shown);
        auto shared = std::make_shared<const std::string>(std::move(*bytes));
        m_embedded.emplace(path, shared);
        return shared;
    }

    /// Why what an embed names, `what`, is too large to put in the program.
    [[nodiscard]] static std::unexpected<std::string> too_large(const std::string_view target,
                                                                const std::string_view what) {
        return std::unexpected(std::format(
            R"("{}" is {} more than {} MiB, the most one embed puts in the program; read one that large when the program runs)",
            target, what, max_embedded / (std::uint64_t{ 1024 } * 1024)));
    }

    compilation_t& m_compilation;
    file_set_t m_files;
    build_mode_t m_mode;
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
    /// The bytes of each file an embed read, by its path in the program.
    std::map<std::string, std::shared_ptr<const std::string>, std::less<>> m_embedded;
    /// The embeds' paths warn_respelled_embed() has looked at, by their file and place.
    std::set<std::pair<std::uint32_t, std::uint32_t>> m_respelled_embeds;
    /// The .proto files read, which stay put while more are read.
    std::vector<std::unique_ptr<proto_entry_t>> m_protos;
    /// The keys of the .proto files being read, each importing the next, for a cycle's error.
    std::vector<std::string> m_reading;

    /// Whether a file of the program is a library's beside its executable.
    [[nodiscard]] bool beside_package(const std::string_view path) const {
        const package_t* package = package_named(m_files, path);
        return package != nullptr && package->beside && !path.starts_with("std/");
    }

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
        if (!m_files.root || path.empty() || path.starts_with("std/") ||
            prebuilt_package(m_files, path) != nullptr) {
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

        if (generated_file(m_files, path) != nullptr) {
            return std::format("generated {}", path);
        }

        // A prebuilt library's modules are in its interface file, by their paths.
        if (prebuilt_package(m_files, path) != nullptr) {
            return std::format("prebuilt {}", path);
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
        if (!m_files.root || prebuilt_package(m_files, path) != nullptr ||
            generated_file(m_files, path) != nullptr) {
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

        const generated_file_t* generated = generated_file(m_files, path);
        const file_id_t file = m_compilation.sources.add(
            shown_path(m_files, path), std::move(*text),
            generated != nullptr ? std::optional(generated->origin) : std::nullopt);
        if (m_files.root && generated == nullptr && prebuilt_package(m_files, path) == nullptr) {
            std::error_code ignored;
            note_disk_path(m_compilation, file,
                           std::filesystem::absolute(disk_path(m_files, path), ignored));
        } else if (const package_t* package =
                       m_files.root ? prebuilt_package(m_files, path) : nullptr) {
            std::error_code ignored;
            const auto index = static_cast<std::size_t>(std::to_underlying(file));
            m_compilation.in_interface.resize(
                std::max(m_compilation.in_interface.size(), index + 1));
            m_compilation.in_interface[index] =
                std::filesystem::absolute(package->interface, ignored);
        }

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
        if (const package_t* package = prebuilt_package(m_files, path)) {
            return prebuilt_module(*package, path) != nullptr;
        }

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

    /// Warns where an embed's path, `target` written at `at`, spells a folder or file in other
    /// letter cases than the disk has it, as warn_respelled does for an Import. Once for each
    /// path, however many times analysis checks it.
    void warn_respelled_embed(const span_t at, const std::string_view target) {
        if (!m_files.root ||
            !m_respelled_embeds.emplace(std::to_underlying(at.file), at.begin).second) {
            return;
        }

        const std::string& from = std::ranges::find(m_loaded, at.file, &loaded_t::file)->path;
        std::filesystem::path real = disk_path(m_files, from).parent_path();
        std::string written = target.starts_with("./") ? "." : "";
        bool differs = false;
        for (const std::string_view part : path_parts(target)) {
            if (part == "..") {
                real = real.parent_path();
                written += written.empty() ? ".." : "/..";
                continue;
            }

            const std::optional<std::string> spelled = on_disk(real, part);
            if (!spelled) {
                return; // nothing there, which the embed reports
            }

            differs = differs || *spelled != part;
            real /= path_from_utf8(*spelled);
            written += std::format("{}{}", written.empty() ? "" : "/", *spelled);
        }

        if (differs) {
            m_compilation.diagnostics.warning(at,
                                              "the disk spells it \"{}\", in other letter cases, "
                                              "which only a disk that ignores them finds",
                                              written);
        }
    }

    /// Lexes and parses a loaded file into the AST, keeping its tokens in `tokens` if given.
    void parse_file(const std::size_t index, std::vector<token_t>* tokens) {
        const source_file_t& file = m_compilation.sources.get(m_loaded[index].file);
        std::vector<span_t> docs;
        std::vector<token_t> lexed = lex(file, m_compilation.diagnostics, m_defined, &docs);
        const std::size_t before = m_compilation.ast.items().size();
        parse_into(file, lexed, m_compilation.diagnostics, m_compilation.ast,
                   prebuilt_package(m_files, m_loaded[index].path) != nullptr, docs);
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

            // In a prebuilt library's interface file, the module of a .proto file is the
            // FutureBlitz fb made from it, which imports the others by their paths in the
            // program, std's well-known types as .proto files.
            if (importer.ends_with(".proto") && prebuilt_package(m_files, importer) != nullptr &&
                node.quoted) {
                const std::optional<std::size_t> loaded =
                    prebuilt_package(m_files, node.target) != nullptr
                        ? load(node.target, node.target_span)
                        : load_proto(node.target, node.target_span);
                if (loaded) {
                    targets.push_back(*loaded);
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

            const std::string& named =
                paths->file.empty() ? paths->folder.value_or("") : paths->file;
            if (m_files.root && prebuilt_package(m_files, named) == nullptr) {
                warn_respelled(node, importer, *paths);
            }

            // An Import stays in its package's files, though another package's folder can be
            // in its own, and a link or junction there can lead anywhere.
            if (const std::optional<std::filesystem::path> nested = nested_package(*paths)) {
                report_nested(node, *paths, *nested);
                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            // A package's tests/ folder is its tests', which use it from outside, so a package
            // builds without it.
            if (in_tests_folder(importer, named)) {
                m_compilation.diagnostics.error(
                    node.target_span,
                    "{} is in the package's tests/ folder, whose tests use the package from "
                    "outside, so the package's own files don't import them",
                    shown_path(m_files, named));
                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            if (report_linked(node, *paths)) {
                m_loaded[index].imports.emplace_back(id, std::move(targets));
                continue;
            }

            // A folder whose files are all links away was reported for them, not as missing.
            bool linked = false;
            if (paths->file.ends_with(".proto") &&
                prebuilt_package(m_files, paths->file) != nullptr) {
                if (const std::optional<std::size_t> file = load(paths->file, node.target_span)) {
                    targets.push_back(*file);
                }
            } else if (paths->file.ends_with(".proto")) {
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
        if (!m_files.root || path.empty() || path.starts_with("std/") ||
            prebuilt_package(m_files, path) != nullptr) {
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
        const std::string& named = paths.file.empty() ? paths.folder.value_or("") : paths.file;
        if (const package_t* package = prebuilt_package(m_files, named)) {
            const std::string interface = to_utf8(shown_disk_path(package->interface));
            if (!node.quoted && paths.file.empty()) {
                m_compilation.diagnostics.error(
                    node.target_span, "the package {} has no modules in its interface file, {}",
                    node.target, interface);
            } else if (!node.quoted) {
                m_compilation.diagnostics.error(
                    node.target_span, "there's no module {}: no {} or folder {}/ in {}",
                    node.target, paths.file, paths.folder.value_or(""), interface);
            } else {
                m_compilation.diagnostics.error(node.target_span,
                                                "there's no module \"{}\": no {} in {}",
                                                node.target, paths.file, interface);
            }

            return;
        }

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
        if (!m_files.root || prebuilt_package(m_files, folder) != nullptr) {
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

    /// Whether `path`, a file or folder an Import in `importer` names, is in the importer's
    /// package's tests/ folder.
    [[nodiscard]] bool in_tests_folder(const std::string& importer,
                                       const std::string_view path) const {
        const std::optional<std::size_t> package = package_of(importer);
        if (m_files.packages.empty() || !package || importer.starts_with("std/")) {
            return false;
        }

        const std::string tests = m_files.packages[*package].prefix + "tests";
        return path == tests || path.starts_with(tests + "/");
    }

    /// Whether a file's items have top-level code, which only a main file's can, besides its
    /// declarations.
    [[nodiscard]] bool has_code(const std::vector<stmt_id_t>& items) const {
        return std::ranges::any_of(items, [this](const stmt_id_t id) {
            const stmt_node_t& node = m_compilation.ast.get(id).node;
            const auto* variable = std::get_if<variable_stmt_t>(&node);
            return (variable != nullptr && variable->kind == variable_kind_t::LOCAL) ||
                   (variable == nullptr && !std::holds_alternative<function_stmt_t>(node) &&
                    !std::holds_alternative<type_stmt_t>(node) &&
                    !std::holds_alternative<enum_stmt_t>(node) &&
                    !std::holds_alternative<alias_stmt_t>(node) &&
                    !std::holds_alternative<extend_stmt_t>(node) &&
                    !std::holds_alternative<import_stmt_t>(node) &&
                    !std::holds_alternative<extern_stmt_t>(node) &&
                    !std::holds_alternative<error_stmt_t>(node));
        });
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

        // What the main file imports, directly or not, which a build has too, where a file with
        // top-level code is an error, not another main file.
        std::vector<bool> reached(m_loaded.size());
        std::vector<std::size_t> work{ 0 };
        while (!work.empty()) {
            const std::size_t at = work.back();
            work.pop_back();
            for (const auto& [statement, targets] : m_loaded[at].imports) {
                for (const std::size_t target : targets) {
                    if (!reached[target]) {
                        reached[target] = true;
                        work.push_back(target);
                    }
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
                // fb test's build of a prebuilt library has a main file of its own, whose code
                // isn't in the library's binary.
                .prebuilt = index != 0 && prebuilt_package(m_files, loaded.path) != nullptr,
                .beside = beside_package(loaded.path),
                .other_main = m_files.testing && index != 0 && !reached[index] &&
                              !m_files.packages.empty() && package_of(loaded.path) == 0 &&
                              has_code(m_items[index]),
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

            // What another check found in the same text.
            if (m_files.cache != nullptr) {
                const std::scoped_lock lock(m_files.cache->mutex);
                const auto known = m_files.cache->files.find(path);
                if (known != m_files.cache->files.end() && known->second.text == *text &&
                    known->second.mode == m_mode) {
                    for (const std_name_t& name : known->second.names) {
                        m_compilation.ast.modules().add_std_name(name);
                    }

                    continue;
                }
            }

            const std::string kept = m_files.cache != nullptr ? *text : std::string();
            std::vector<std_name_t> names;
            source_manager_t sources;
            diagnostic_bag_t ignored;
            const source_file_t& file = sources.get(sources.add(path, std::move(*text)));
            const ast_t ast = fb::parse(file, lex(file, ignored, m_defined), ignored);
            const std::string module = std_import_name(path);
            for (const stmt_id_t id : ast.items()) {
                std::ranges::move(declared_names(ast, id, module), std::back_inserter(names));
            }

            for (const std_name_t& name : names) {
                m_compilation.ast.modules().add_std_name(name);
            }

            if (m_files.cache != nullptr) {
                const std::scoped_lock lock(m_files.cache->mutex);
                std_names_cache_t::file_t found{
                    .text = kept,
                    .mode = m_mode,
                    .names = std::move(names),
                };
                m_files.cache->files.insert_or_assign(path, std::move(found));
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

        if (const auto* alias = std::get_if<alias_stmt_t>(&node)) {
            return named(alias->name.text, alias->modifiers);
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
    loader_t loader(compilation, std::move(files), mode);
    loader.run(path);
    const embed_reader_t embeds{
        .file = [&loader](const span_t at,
                          const std::string_view target) { return loader.embed_file(at, target); },
        .folder =
            [&loader](const span_t at, const std::string_view target) {
                return loader.embed_folder(at, target);
            },
    };
    compilation.analysis =
        analyze(compilation.sources, compilation.ast, compilation.diagnostics, mode, embeds);
    // Each stage adds its own diagnostics, so sorting puts them back in source order.
    compilation.diagnostics.sort();
}

std::vector<std::string> library_files(const package_t& package);

/// Reads the packages a program is made of: the main one's project.toml, then those of the
/// packages it depends on, and theirs, each once. What's wrong goes in the compilation's
/// diagnostics, at the project.toml's lines, except a main project.toml that can't be read.
class package_loader_t {
public:
    /// `program` says the main package is built as a program from a main file it was given,
    /// though it may have none of its own.
    package_loader_t(compilation_t& compilation, const build_mode_t mode, const bool program)
        : m_compilation(compilation), m_mode(mode), m_program(program) {}

    /// The packages, the main one, in `folder`, first. nullopt after an error.
    std::optional<std::vector<package_t>> run(const std::filesystem::path& folder) {
        std::vector<std::size_t> chain;
        // The program's registry packages are where fb.lock beside its project.toml says.
        if (std::ifstream lock(folder / "fb.lock", std::ios::binary); lock) {
            const std::string text{ std::istreambuf_iterator<char>(lock),
                                    std::istreambuf_iterator<char>() };
            m_lockfile = read_lockfile(text);
        }

        if (!load(folder, nullptr, chain, nullptr) || m_failed) {
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
    build_mode_t m_mode;
    bool m_program = false;
    std::vector<package_t> m_packages;
    std::vector<manifest_t> m_manifests;
    /// Each package's folder, as weakly_canonical() has it.
    std::vector<std::filesystem::path> m_folders;
    /// The folders whose project.toml has mistakes, which are reported once.
    std::vector<std::filesystem::path> m_broken;
    std::optional<file_id_t> m_main_file;
    std::optional<std::string> m_unreadable;
    /// The dynamic libraries' names beside the executable so far, with the package and the
    /// [native] name of each.
    std::vector<std::array<std::string, 3>> m_beside;
    /// fb.lock beside the main package's project.toml, or why it can't be read. Nullopt when
    /// there's none.
    std::optional<std::expected<lockfile_t, std::string>> m_lockfile;
    bool m_failed = false;

    template <typename... Args>
    void fail(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        m_compilation.diagnostics.error(span, fmt, std::forward<Args>(args)...);
        m_failed = true;
    }

    /// Where fb.lock has the registry package `dependency` names, by its `range`, unpacked in the
    /// cache, for a
    /// package in `root` whose [registries] are `registries`, and the version it pins. nullopt
    /// after an error saying what's missing, which a build fetches. `cached` says the package
    /// that depends on it is itself from a registry, whose registries its index named already.
    std::optional<std::pair<std::filesystem::path, const locked_package_t*>>
    locked_folder(const dependency_t& dependency, const std::vector<registry_entry_t>& registries,
                  const std::filesystem::path& root, const bool cached,
                  const version_range_t& range) {
        const std::string wanted = std::format("{} = \"{}\"", dependency.name, range.text);
        if (!m_lockfile) {
            fail(dependency.path_span,
                 "{} is from a registry, but there's no fb.lock yet; fb build, fb check, or fb "
                 "update makes it and fetches the package",
                 wanted);
            return std::nullopt;
        }

        if (!*m_lockfile) {
            fail(dependency.path_span, "fb.lock can't be read: {}", m_lockfile->error());
            return std::nullopt;
        }

        const locked_package_t* locked = m_lockfile->value().find(dependency.name);
        const std::optional<std::string> source =
            registry_index(dependency.registry, registries, root);
        const std::expected<version_t, std::string> version =
            locked != nullptr ? parse_version(locked->version) : std::unexpected(std::string());
        if (locked == nullptr || (!cached && source != locked->source)) {
            fail(dependency.path_span,
                 "fb.lock doesn't have {} yet; fb build, fb check, or fb update locks it and "
                 "fetches it",
                 wanted);
            return std::nullopt;
        }

        if (!version || !range.matches(*version)) {
            fail(dependency.path_span,
                 "fb.lock has {} v{}, which isn't in \"{}\"; fb build, fb check, or fb update "
                 "locks another",
                 dependency.name, locked->version, range.text);
            return std::nullopt;
        }

        if (!is_unpacked(*locked, build_target())) {
            fail(dependency.path_span,
                 "{} v{} isn't in the cache yet; fb build or fb check fetches it", dependency.name,
                 locked->version);
            return std::nullopt;
        }

        return std::pair(package_cache_folder(locked->source, locked->name, locked->version),
                         locked);
    }

    /// The package in `folder`, which the dependency `named` names, if any, read with the ones it
    /// depends on. `chain` is the packages on the way to it, for a cycle's message. `locked` is
    /// what fb.lock says of a registry package.
    std::optional<std::size_t> load(const std::filesystem::path& folder, const dependency_t* named,
                                    std::vector<std::size_t>& chain,
                                    const locked_package_t* locked) {
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

        note_disk_path(m_compilation, *file, manifest_path);
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

        if (locked != nullptr && manifest->version != locked->version) {
            fail(named->path_span,
                 "the cache's {} in '{}' is v{}, but fb.lock says v{}; deleting that folder "
                 "lets fb build fetch it again",
                 manifest->name, to_utf8(root), manifest->version, locked->version);
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

        // The main file project.toml names has to be there. Without 'main' or 'type', a package
        // without main.fb is a library, and in one whose type says so, main.fb is a module.
        const std::filesystem::path main = root / path_from_utf8(manifest->main);
        const bool library = manifest->type == package_type_t::LIBRARY;
        if (!library && has_main(root, *manifest)) {
            package.main = std::filesystem::weakly_canonical(main, ignored);
        } else if (!library && manifest->main_span) {
            fail(*manifest->main_span, "there's no {} in the package's folder, which 'main' names",
                 manifest->main);
        } else if (manifest->type == package_type_t::EXECUTABLE && named == nullptr) {
            fail(manifest->type_span.value_or(manifest->name_span),
                 "an executable needs its main file, but there's no {} in the package's folder",
                 manifest->main);
        }

        read_prebuilt(*manifest, package);

        // Only a program's own package builds an executable; the ones it uses are libraries.
        if (named != nullptr &&
            (!package.main.empty() || manifest->type == package_type_t::EXECUTABLE)) {
            fail(named->name_span,
                 "{} is an executable, so it can't be a dependency; a package that programs use is "
                 "a library, type = \"library\" in its project.toml{}",
                 named->name, manifest->main_span ? ", without 'main'" : "");
        }

        for (const native_t& native : manifest->natives) {
            package.natives.push_back(native.name);
            // A package ships its natives per target, which {target} names.
            const std::string path = with_target(native.path);
            if (native.kind == native_kind_t::SYSTEM) {
                package.linked.system.push_back(native.path);
                package.system_spans.push_back(native.path_span);
            } else if (path.ends_with('/') || path.ends_with('\\')) {
                // A folder's path, whose files would have names like "lib.a".
                fail(native.path_span, "\"{}\" is a folder, not a {} library", native.path,
                     native.kind == native_kind_t::DYNAMIC ? "dynamic" : "static");
            } else if (native.kind == native_kind_t::DYNAMIC) {
                read_dynamic(root, *manifest, native, path, package);
            } else if (is_dynamic_file(path_from_utf8(path))) {
                fail(native.path_span,
                     "\"{}\" is a dynamic library, which [native] takes as {} = {{ dynamic = "
                     "\"{}\" }}",
                     native.path, native.name, as_dynamic_path(native.path));
            } else if (const std::optional<std::filesystem::path> archive =
                           find_archive(root, path)) {
                if (is_archive(*archive)) {
                    package.linked.archives.push_back(*archive);
                } else {
                    fail(native.path_span,
                         "\"{}\" names {}, which isn't a static library: it doesn't start as an "
                         "archive does",
                         native.path, shown(path, *archive));
                }
            } else {
                report_no_archive(root, *manifest, native, path);
            }
        }

        const std::vector<dependency_t> dependencies = manifest->dependencies;
        const std::vector<registry_entry_t> registries = manifest->registries;
        m_packages.push_back(std::move(package));
        m_manifests.push_back(std::move(*manifest));
        m_folders.push_back(root);
        chain.push_back(index);
        // Only the program's own project.toml says which libraries go beside it.
        const bool program = index == 0 && (m_program || !m_packages.front().main.empty());
        for (const dependency_t& dependency : dependencies) {
            std::optional<std::pair<std::filesystem::path, const locked_package_t*>> found;
            if (dependency.range) {
                found = locked_folder(dependency, registries, root, locked != nullptr,
                                      *dependency.range);
            } else if (locked != nullptr) {
                // A registry's package is whole without anything else on its publisher's disk.
                fail(dependency.path_span,
                     "{} v{} is from a registry, so its dependencies are too, but {} is a path",
                     m_packages[index].name, m_packages[index].version, dependency.name);
            } else {
                found = std::pair(root / path_from_utf8(dependency.path), nullptr);
            }

            if (!found) {
                continue;
            }

            const std::optional<std::size_t> loaded =
                load(found->first, &dependency, chain, found->second);
            if (dependency.link_span && !program) {
                m_compilation.diagnostics.warning(
                    *dependency.link_span,
                    "only a program's own project.toml says how a library links, so this 'link' "
                    "is ignored");
            } else if (loaded && dependency.beside) {
                m_packages[*loaded].beside = true;
            }
        }

        chain.pop_back();
        return index;
    }

    /// A prebuilt library's interface file, lib/<target>/<mode>/<name>.fbi, and its modules,
    /// when its folder has one for this build: its .fb source is what isn't there.
    void read_prebuilt(const manifest_t& manifest, package_t& package) {
        const std::filesystem::path folder = std::filesystem::path("lib") /
                                             path_from_utf8(build_target()) /
                                             path_from_utf8(mode_folder(m_mode));
        const std::filesystem::path interface =
            package.root / folder / path_from_utf8(manifest.name + ".fbi");
        // Named as project.toml's paths are, with '/'s.
        const std::string shown =
            std::format("lib/{}/{}/{}.fbi", build_target(), mode_folder(m_mode), manifest.name);
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(interface, ignored)) {
            // A prebuilt library without this build has no source either.
            if (library_files(package).empty() &&
                has_interface(package.root / "lib", manifest.name)) {
                fail(manifest.name_span, "{} is prebuilt, but not for this build: there's no {}",
                     manifest.name, shown);
            }

            return;
        }

        if (!library_files(package).empty()) {
            fail(manifest.name_span,
                 "{} has both .fb source and a prebuilt build, {}; a package is one or the other",
                 manifest.name, shown);
            return;
        }

        std::ifstream file(interface, std::ios::binary);
        const std::string text{ std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>() };
        const std::optional<interface_header_t> header = read_interface_header(text);
        std::optional<std::vector<library_file_t>> modules = interface_modules(text);
        if (!header || !modules) {
            fail(manifest.name_span, "{} isn't an interface file that fb writes", shown);
            return;
        }

        if (header->name != manifest.name || header->version != manifest.version) {
            fail(manifest.name_span, "{} is the interface of {} v{}, but project.toml says {} v{}",
                 shown, header->name, header->version, manifest.name, manifest.version);
            return;
        }

        // Its modules are the package's, each under its name, and its .proto files'.
        for (const library_file_t& module : *modules) {
            if (!module.path.starts_with(package.prefix) ||
                (!module.path.ends_with(".fb") && !module.path.ends_with(".proto"))) {
                fail(manifest.name_span, "{} isn't an interface file that fb writes", shown);
                return;
            }
        }

        package.interface = std::filesystem::weakly_canonical(interface, ignored);
        package.modules = std::move(*modules);
    }

    /// Whether a package has its main file: the one 'main' names, or else main.fb, by that
    /// exact name, so a package is the same kind on a disk that ignores letter case as on one
    /// that doesn't.
    static bool has_main(const std::filesystem::path& root, const manifest_t& manifest) {
        std::error_code error;
        if (manifest.main_span) {
            return std::filesystem::is_regular_file(root / path_from_utf8(manifest.main), error);
        }

        for (auto entry = std::filesystem::directory_iterator(root, error);
             !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
            std::error_code ignored;
            if (to_utf8(entry->path().filename()) == manifest.main &&
                entry->is_regular_file(ignored)) {
                return true;
            }
        }

        return false;
    }

    /// Whether `folder` has an interface file of the library `name` for some target and mode,
    /// lib/<target>/<mode>/<name>.fbi.
    static bool has_interface(const std::filesystem::path& folder, const std::string& name) {
        std::error_code error;
        for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
             !error && entry != std::filesystem::recursive_directory_iterator();
             entry.increment(error)) {
            std::error_code ignored;
            if (entry.depth() == 2 && entry->is_regular_file(ignored) &&
                entry->path().filename() == path_from_utf8(name + ".fbi")) {
                return true;
            }
        }

        return false;
    }

    /// A [native] library's path with each "{target}" the build's target.
    static std::string with_target(std::string path) {
        constexpr std::string_view target = "{target}";
        for (std::size_t at = path.find(target); at != std::string::npos;
             at = path.find(target, at)) {
            path.replace(at, target.size(), build_target());
        }

        return path;
    }

    /// The name a dynamic library's file has on this target: x.dll, libx.so, or libx.dylib.
    static std::string dynamic_file_name(const std::string& name) {
        const std::string_view target = build_target();
        if (target.starts_with("windows")) {
            return name + ".dll";
        }

        return "lib" + name + (target.starts_with("macos") ? ".dylib" : ".so");
    }

    /// Whether a file's name is a dynamic library's, x.dll, libx.so, or libx.dylib, in any
    /// letter case.
    static bool is_dynamic_file(const std::filesystem::path& path) {
        return extension_is(path, ".dll") || extension_is(path, ".so") ||
               extension_is(path, ".dylib");
    }

    /// The [native] path of a dynamic library whose file, or import library, `written` names,
    /// in its folder as written: without its extension, and without the "lib" before its name
    /// unless it's a DLL's or a .lib's, so "libs/libx.so" is "libs/x".
    static std::string as_dynamic_path(const std::string& written) {
        std::string name = to_utf8(path_from_utf8(written).filename());
        const auto ends_with = [&](const std::string_view suffix) {
            return name.size() > suffix.size() &&
                   std::ranges::equal(std::string_view(name).substr(name.size() - suffix.size()),
                                      suffix, [](const char a, const char b) {
                                          return std::tolower(static_cast<unsigned char>(a)) == b;
                                      });
        };
        bool prefixed = true;
        for (const std::string_view extension :
             { ".dll.a", ".dll", ".so", ".dylib", ".lib", ".a" }) {
            if (ends_with(extension)) {
                prefixed = extension != ".dll" && extension != ".lib";
                name.resize(name.size() - extension.size());
                break;
            }
        }

        if (prefixed && name.starts_with("lib") && name.size() > 3) {
            name.erase(0, 3);
        }

        const std::size_t slash = written.find_last_of("/\\");
        return slash == std::string::npos ? name : written.substr(0, slash + 1) + name;
    }

    /// What this target's dynamic libraries are called, as in "a DLL".
    static std::string_view dynamic_kind() {
        const std::string_view target = build_target();
        if (target.starts_with("windows")) {
            return "a DLL";
        }

        return target.starts_with("macos") ? "a dylib" : "a shared object";
    }

    /// A [native] dynamic library's files, which `path`, with the build's target, names without
    /// the "lib" and extension it has: on Windows x.dll and its import library, x.lib,
    /// libx.dll.a, or libx.a, on Linux libx.so, and on macOS libx.dylib, whose install name the
    /// executable finds beside it, @rpath/libx.dylib or in @loader_path.
    void read_dynamic(const std::filesystem::path& root, const manifest_t& manifest,
                      const native_t& native, const std::string& path, package_t& package) {
        const std::filesystem::path written = path_from_utf8(path);
        if (is_dynamic_file(written) || extension_is(written, ".lib") ||
            extension_is(written, ".a")) {
            fail(native.path_span,
                 "a dynamic library's path leaves out its file's \"lib\" and extension, so it's "
                 "the same on every OS, like \"{}\"",
                 as_dynamic_path(native.path));
            return;
        }

        std::error_code ignored;
        if (std::filesystem::is_directory(root / written, ignored)) {
            fail(native.path_span, "\"{}\" is a folder, not a dynamic library", native.path);
            return;
        }

        const std::string_view target = build_target();
        const std::string name = to_utf8(written.filename());
        const std::string file_name = dynamic_file_name(name);
        const std::filesystem::path folder = root / written.parent_path();
        const std::filesystem::path file = folder / path_from_utf8(file_name);
        if (!std::filesystem::is_regular_file(file, ignored)) {
            fail(native.path_span,
                 "the {} package has no dynamic library \"{}\" for {}: there's no {}{}",
                 manifest.name, native.name, target, shown(path, file),
                 written.is_absolute() ? "" : ", from its folder");
            return;
        }

        std::ifstream in(file, std::ios::binary);
        const std::string bytes{ std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>() };
        const std::expected<std::string, library_problem_t> recorded =
            dynamic_library_name(bytes, target);
        const bool windows = target.starts_with("windows");
        const bool apple = target.starts_with("macos");
        if (!recorded) {
            const std::string_view kind = dynamic_kind();
            if (recorded.error() == library_problem_t::NOT_A_LIBRARY) {
                fail(native.path_span,
                     "\"{}\" names {}, which isn't a dynamic library: it doesn't start as {} "
                     "does",
                     native.path, shown(path, file), kind);
            } else if (recorded.error() == library_problem_t::EXECUTABLE) {
                fail(native.path_span,
                     "\"{}\" names {}, which is an executable, not a dynamic library", native.path,
                     shown(path, file));
            } else {
                fail(native.path_span, "\"{}\" names {}, which is {} for another CPU than {}'s",
                     native.path, shown(path, file), kind, target);
            }

            return;
        }

        dynamic_native_t library{
            .linked = std::filesystem::weakly_canonical(file, ignored),
            .file = std::filesystem::weakly_canonical(file, ignored),
            .beside = file_name,
        };
        if (windows) {
            // The executable links the DLL's functions through its import library.
            const std::array<std::string, 3> imports{
                name + ".lib",
                "lib" + name + ".dll.a",
                "lib" + name + ".a",
            };
            const auto found = std::ranges::find_if(imports, [&](const std::string& import) {
                return std::filesystem::is_regular_file(folder / path_from_utf8(import), ignored);
            });
            if (found == imports.end()) {
                fail(native.path_span,
                     "the {} package's {} has no import library beside it: there's no {}, {}, or "
                     "{}",
                     manifest.name, shown(path, file), imports[0], imports[1], imports[2]);
                return;
            }

            const std::filesystem::path import = folder / path_from_utf8(*found);
            if (!is_archive(import)) {
                fail(native.path_span,
                     "\"{}\" names {}, whose import library {} isn't one: it doesn't start as "
                     "an archive does",
                     native.path, shown(path, file), shown(path, import));
                return;
            }

            library.linked = std::filesystem::weakly_canonical(import, ignored);
        } else if (apple) {
            // The executable records the install name, and looks for an @rpath one in
            // @loader_path, beside it.
            const std::string rpath = "@rpath/" + file_name;
            constexpr std::string_view loader = "@loader_path/";
            const std::string_view rest =
                std::string_view(*recorded).substr(std::min(recorded->size(), loader.size()));
            if (recorded->starts_with(loader) && is_file_name(rest)) {
                library.beside = std::string(rest);
            } else if (*recorded != rpath) {
                fail(native.path_span,
                     "the {} package's {} has the install name {}, but the program finds it "
                     "beside the executable, as {} or a name in @loader_path; link it with "
                     "-install_name {}",
                     manifest.name, shown(path, file), *recorded, rpath, rpath);
                return;
            }
        } else if (!recorded->empty()) {
            // The executable records the SONAME, and finds the library by it.
            if (!is_file_name(*recorded)) {
                fail(native.path_span,
                     "the {} package's {} has the SONAME {}, which isn't a file's name, but the "
                     "program finds it beside the executable by one",
                     manifest.name, shown(path, file), *recorded);
                return;
            }

            library.beside = *recorded;
        }

        // Two files of one name can't both go beside it, in any letter case.
        for (const auto& [beside, owner, other] : m_beside) {
            if (folded_name(beside) == folded_name(library.beside)) {
                fail(native.path_span,
                     "the {} package's \"{}\" goes beside the executable as {}, as the {} "
                     "package's \"{}\" does",
                     manifest.name, native.name, library.beside, owner, other);
                return;
            }
        }

        m_beside.push_back({ library.beside, manifest.name, native.name });
        package.linked.dynamic.push_back(std::move(library));
    }

    /// Whether a name is a file's in a folder: not empty, ".", or "..", and without a '/'.
    static bool is_file_name(const std::string_view name) {
        return !name.empty() && name != "." && name != ".." && !name.contains('/');
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
    /// libs/x.lib, from the package's folder", by the paths as written, or with {target} the
    /// build's target, which makes it one the package doesn't have for the target, or that it's
    /// a folder.
    void report_no_archive(const std::filesystem::path& root, const manifest_t& manifest,
                           const native_t& native, const std::string& path) {
        const std::filesystem::path written = path_from_utf8(path);
        std::error_code ignored;
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

        if (path != native.path) {
            fail(native.path_span,
                 "the {} package has no static library \"{}\" for {}: there's no {}{}",
                 manifest.name, native.name, build_target(), listed,
                 written.is_absolute() ? "" : ", from its folder");
            return;
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

/// The build a compilation has of each of `packages`: a prebuilt one's, from its interface
/// file's first line, or for one compiled from source, its files'.
void record_builds(compilation_t& compilation, const std::vector<package_t>& packages) {
    for (const package_t& package : packages) {
        std::string build;
        if (!package.interface.empty()) {
            std::ifstream in(package.interface, std::ios::binary);
            std::string line;
            std::getline(in, line);
            build = read_interface_header(line).value_or(interface_header_t{}).hash;
        } else {
            std::vector<library_file_t> files;
            for (const module_t& module : compilation.ast.modules().modules()) {
                if (module.path.starts_with(package.prefix)) {
                    files.push_back({
                        .path = module.path,
                        .text = std::string(compilation.sources.get(module.file).text()),
                    });
                }
            }

            build = source_build(std::move(files));
        }

        compilation.builds.push_back({
            .name = package.name,
            .version = package.version,
            .build = std::move(build),
        });
    }
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
        // A beside library's static libraries are in the executable too, as its dependencies
        // are, and its dynamic ones go beside it.
        const native_libraries_t& linked = packages[index].linked;
        std::ranges::copy(linked.archives, std::back_inserter(natives.archives));
        std::ranges::copy(linked.dynamic, std::back_inserter(natives.dynamic));
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
/// build/ folder, its tests/ folder, whose tests use it from outside, hidden folders, links, or
/// the folders of other packages in it, which have their own project.toml.
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
            if (name.starts_with('.') ||
                (entry.depth() == 0 && (name == "build" || name == "tests")) ||
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

/// For fb test, the package of a package's tests/ folder, if it has one: "<name>.tests", whose
/// files use the package from outside, as one that depends on it does, with what it depends on.
/// A tests/ folder with a project.toml of its own is a package of its own, which fb test leaves
/// alone.
std::optional<package_t> tests_package(const package_t& own) {
    std::error_code error;
    const std::filesystem::path root = own.root / "tests";
    if (!std::filesystem::is_directory(root, error) || is_link(root) ||
        std::filesystem::exists(root / "project.toml", error)) {
        return std::nullopt;
    }

    package_t tests{
        .name = own.name + ".tests",
        .version = own.version,
        .root = root,
        .prefix = own.name + ".tests/",
        .dependencies = { own.name },
    };
    std::ranges::copy(own.dependencies, std::back_inserter(tests.dependencies));
    return tests;
}

/// Adds a program's main file, at `path` on disk, to the compilation, with `read`'s text in place
/// of the disk's when it has one. Fails when it can't be read.
std::expected<file_id_t, std::string> load_main(compilation_t& compilation,
                                                const std::filesystem::path& path,
                                                const source_reader_t& read) {
    std::optional<std::string> text = read ? read(path) : std::nullopt;
    const std::expected<file_id_t, std::string> file =
        text ? compilation.sources.add(to_utf8(shown_disk_path(path)), std::move(*text))
             : compilation.sources.load(shown_disk_path(path));
    if (file) {
        std::error_code error;
        note_disk_path(compilation, *file, std::filesystem::absolute(path, error));
    }

    return file;
}

/// What a package is compiled for.
enum class purpose_t : std::uint8_t {
    /// fb check, build, and run.
    BUILD,
    /// fb test: every file of it, its tests/ folder's, and its examples.
    TEST,
    /// fb doc: every file of it.
    DOC,
    /// check_files(): the files its request names, each a module of its own, as fb test's are.
    CHECK,
};

/// How an example Imports the module it's an example of.
enum class examples_of_t : std::uint8_t {
    /// A package's, by its dotted name.
    PACKAGE,
    /// std's, by its folder's name, as "std.collections".
    STANDARD,
    /// A program's main file without a project.toml, by its path, as "./game": the examples go
    /// beside it.
    PROGRAM,
};

/// Whether a module's name is one an Import takes: names between its dots.
bool importable_name(const std::string_view name) {
    for (const auto part : std::views::split(name, '.')) {
        const std::string_view each(part.begin(), part.end());
        const auto letter = [](const char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        if (each.empty() || !letter(each.front()) || keyword_kind(each) ||
            !std::ranges::all_of(
                each, [&](const char c) { return letter(c) || (c >= '0' && c <= '9'); })) {
            return false;
        }
    }

    return true;
}

/// For fb test: the examples of the doc comments in `paths`, files of `files`, each a module of
/// its own at a path that starts with `prefix`, which the front end loads from memory. Its lines
/// show where they are in their file, a copy of which goes in the compilation's sources for
/// messages to show.
void add_examples(compilation_t& compilation, file_set_t& files,
                  const std::vector<std::string>& paths, const std::string& prefix,
                  const build_mode_t mode, const examples_of_t of) {
    const std::vector<std::string_view> defined = defined_symbols(mode);
    for (const std::string& path : paths) {
        std::optional<std::string> text = read_file(files, path);
        if (!text) {
            continue;
        }

        const source_file_t file(file_id_t{}, path, *text);
        diagnostic_bag_t ignored;
        std::vector<span_t> docs;
        const std::vector<token_t> tokens = lex(file, ignored, defined, &docs);
        if (docs.empty()) {
            continue;
        }

        const ast_t ast = parse(file, tokens, ignored, docs);
        const std::string module = module_name(path);
        std::string imported = module;
        if (of == examples_of_t::STANDARD) {
            imported = std_import_name(path);
        } else if (of == examples_of_t::PROGRAM) {
            imported = std::format("\"./{}\"", path.substr(0, path.size() - 3));
        }

        std::vector<example_module_t> examples = example_modules(ast, file, module, imported);
        if (examples.empty()) {
            continue;
        }

        const std::string shown = shown_path(files, path);
        const file_id_t origin = compilation.sources.add(shown, std::move(*text));
        // The LSP shows an example's places in its doc comment, in the file on disk.
        if (files.root && prebuilt_package(files, path) == nullptr) {
            std::error_code error;
            note_disk_path(compilation, origin,
                           std::filesystem::absolute(disk_path(files, path), error));
        }

        // An example Imports its module by its name, which has to be one an Import takes.
        const bool importable = of == examples_of_t::PROGRAM || importable_name(module);
        for (example_module_t& example : examples) {
            if (!importable) {
                compilation.diagnostics.error(
                    { .file = origin, .begin = example.fence.begin, .end = example.fence.end },
                    "fb test can't run this example: it imports its file's module, {}, which "
                    "isn't a name an Import takes, as a file's name with a '-' in it isn't",
                    module);
                continue;
            }

            const std::string generated =
                std::format("{}{}.fb", prefix, compilation.doc_tests.size() + 1);
            compilation.doc_tests.push_back({ .path = generated, .name = std::move(example.name) });
            files.extra.push_back(generated);
            files.generated.push_back({
                .path = generated,
                .text = std::move(example.text),
                .shown = shown,
                .origin = {
                    .file = origin,
                    .lines = std::move(example.lines),
                    .made = std::move(example.added),
                },
            });
        }
    }
}

/// A package's program: its main file, given or its project.toml's, or for a library, its
/// files, each a module of its own, so it's checked. For fb test and fb doc, each of its files
/// is a module, the main file too, and for fb test, so is each file of its tests/ folder, in a
/// package of its own, and each of its examples, in another. For check_files(), `request`
/// names the files that are modules.
std::expected<compilation_t, std::string>
compile_package(const std::filesystem::path& folder,
                const std::optional<std::filesystem::path>& main, const build_mode_t mode,
                const purpose_t purpose = purpose_t::BUILD,
                const check_request_t* request = nullptr) {
    const bool testing = purpose != purpose_t::BUILD;
    compilation_t compilation;
    package_loader_t reader(compilation, mode, main.has_value());
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

    // A copy, since fb test's build adds a package.
    const package_t own = packages->front();
    const manifest_t& manifest = reader.main();
    compilation.natives = natives_of(*packages, compilation.system_spans);
    const std::vector<package_t> others(std::next(packages->begin()), packages->end());
    for (const package_t& package : *packages) {
        if (!package.beside && !package.interface.empty()) {
            compilation.interfaces.push_back(package.interface);
        }
    }

    // The libraries beside the executable, each after those it depends on, which load first.
    // Each links the program's system and dynamic libraries, which its code, or a dependency's it
    // has, may call: the executable doesn't pass on what it imports itself.
    std::vector<bool> seen(packages->size());
    std::vector<std::size_t> finished;
    finish(*packages, 0, seen, finished);
    for (const std::size_t index : finished) {
        if (const package_t& package = packages->at(index); package.beside) {
            compilation.beside.push_back({
                .name = package.name,
                .root = package.root,
                .interface = package.interface,
                .natives = {
                    .system = compilation.natives.system,
                    .dynamic = compilation.natives.dynamic,
                },
            });
        }
    }

    compilation.package = package_info_t{
        .name = own.name,
        .version = own.version,
        .root = own.root,
        .program = own.name,
        .interface = own.interface,
    };
    std::error_code error;
    const std::filesystem::path written =
        std::filesystem::weakly_canonical(own.root / path_from_utf8(manifest.main), error);
    std::filesystem::path main_path = written;
    if (main) {
        main_path = std::filesystem::weakly_canonical(*main, error);
        // A library's main.fb is a module like the others, which names its program too.
        if (manifest.type == package_type_t::LIBRARY ||
            !std::filesystem::equivalent(main_path, written, error)) {
            compilation.package->program = to_utf8(main_path.stem());
        }
    }

    // fb test's build has each of the package's files, and its tests/ folder's, in a package of
    // its own, and its examples, in another, as fb doc has the package's files.
    std::vector<std::string> extra;
    std::vector<generated_file_t> generated;
    if (purpose == purpose_t::TEST || purpose == purpose_t::DOC) {
        extra = library_files(own);
    }

    // The examples of the doc comments in `documented`, files of the package, each a module of
    // the package of its examples.
    const auto add_doc_tests = [&](const std::vector<std::string>& documented) {
        file_set_t files{
            .root = own.root,
            .packages = *packages,
            .read = request != nullptr ? request->read : source_reader_t{},
        };
        add_examples(compilation, files, documented, own.name + ".doctests/", mode,
                     examples_of_t::PACKAGE);
        std::ranges::copy(files.extra, std::back_inserter(extra));
        generated = std::move(files.generated);
        package_t examples{
            .name = own.name + ".doctests",
            .version = own.version,
            .root = own.root,
            .prefix = own.name + ".doctests/",
            .dependencies = { own.name },
        };
        std::ranges::copy(own.dependencies, std::back_inserter(examples.dependencies));
        packages->push_back(std::move(examples));
    };

    // The package's own folders: its own, and its tests/ folder's, by their places in `packages`.
    std::vector<std::size_t> folders{ 0 };
    if (purpose == purpose_t::TEST || purpose == purpose_t::CHECK) {
        // The package's own files' examples, the main file's among them, not its tests', or a
        // prebuilt library's, in its interface file, which run against its binary.
        std::vector<std::string> documented = extra;
        for (const library_file_t& module : own.modules) {
            documented.push_back(module.path);
        }

        if (std::optional<package_t> tests = tests_package(own)) {
            if (request == nullptr) {
                std::ranges::copy(library_files(*tests), std::back_inserter(extra));
            }

            packages->push_back(std::move(*tests));
            folders.push_back(packages->size() - 1);
        }

        if (purpose == purpose_t::TEST) {
            add_doc_tests(documented);
        }
    }

    // check_files()'s, by their paths in the program, the tests/ folder's first, as it's inside
    // the package's.
    if (request != nullptr) {
        // Both as real_path() gives them, in the disk's letter case, as is_under() compares, though
        // an editor may name a file in other cases.
        for (const std::filesystem::path& file : request->files) {
            const std::optional<std::filesystem::path> real = real_path(file);
            for (const std::size_t index : folders | std::views::reverse) {
                const package_t& package = packages->at(index);
                const std::optional<std::filesystem::path> root = real_path(package.root);
                if (real && root && is_under(*real, *root)) {
                    extra.push_back(package.prefix + program_path(*root, *real));
                    break;
                }
            }
        }

        // A check for references and renames has the package's own files' examples too, whose
        // code uses its names as fb test compiles it.
        if (request->examples) {
            std::vector<std::string> documented;
            std::ranges::copy_if(
                extra, std::back_inserter(documented),
                [&](const std::string& path) { return path.starts_with(own.prefix); });
            add_doc_tests(documented);
        }
    }

    const source_reader_t read = request != nullptr ? request->read : source_reader_t{};
    if (!main &&
        (request != nullptr || manifest.type == package_type_t::LIBRARY || own.main.empty())) {
        // Without a main file, it's a library, whose files are each checked. The main file is
        // empty, and named unlike any of them.
        compilation.package->library = true;
        // A prebuilt library's modules are in its interface file.
        if (!testing) {
            extra = library_files(own);
        }

        if (request == nullptr) {
            for (const library_file_t& module : own.modules) {
                extra.push_back(module.path);
            }
        }

        std::string name = "(library).fb";
        for (int i = 2; std::ranges::contains(extra, own.prefix + name); ++i) {
            name = std::format("(library {}).fb", i);
        }

        compilation.file =
            compilation.sources.add(to_utf8(shown_disk_path(own.root / name)), std::string());
        const std::filesystem::path root = own.root;
        const std::string path = own.prefix + name;
        run_front_end(compilation,
                      {
                          .root = root,
                          .packages = std::move(*packages),
                          .extra = std::move(extra),
                          .testing = testing,
                          .read = read,
                          .cache = request != nullptr ? request->cache : nullptr,
                          .generated = std::move(generated),
                      },
                      path, mode);
        record_builds(compilation, others);
        return compilation;
    }

    const std::expected<file_id_t, std::string> file = load_main(compilation, main_path, read);
    if (!file) {
        return std::unexpected(file.error());
    }

    compilation.file = *file;
    const std::filesystem::path root = own.root;
    // In the disk's letter case, which the main file may be given in another of.
    const std::string path = own.prefix + program_path(real_path(own.root).value_or(own.root),
                                                       real_path(main_path).value_or(main_path));
    std::erase(extra, path);
    run_front_end(compilation,
                  {
                      .root = root,
                      .packages = std::move(*packages),
                      .extra = std::move(extra),
                      .testing = testing,
                      .read = read,
                      .cache = request != nullptr ? request->cache : nullptr,
                      .generated = std::move(generated),
                  },
                  path, mode);
    record_builds(compilation, others);
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

namespace {

/// Why the standard library can't be read, which every program loads std/core/ and
/// std.collections.map of, or nullopt when it can.
std::optional<std::string> unreadable_std() {
    std::error_code error;
    const std::filesystem::path& std_folder = toolchain().std;
    if (!std::filesystem::is_regular_file(std_folder / "core" / "error.fb", error) ||
        !std::filesystem::is_regular_file(std_folder / "collections" / "map.fb", error)) {
        return std::format("cannot read the standard library in '{}'", to_utf8(std_folder));
    }

    return std::nullopt;
}

/// A .fb file outside a package, with the files it imports from its folder, and `read`'s texts
/// in place of theirs on disk, with the std names `cache` has.
std::expected<compilation_t, std::string> compile_loose(const std::filesystem::path& path,
                                                        const build_mode_t mode,
                                                        const source_reader_t& read,
                                                        std_names_cache_t* cache = nullptr) {
    compilation_t compilation;
    const std::expected<file_id_t, std::string> file = load_main(compilation, path, read);
    if (!file) {
        return std::unexpected(file.error());
    }

    compilation.file = *file;
    const std::filesystem::path folder = path.has_parent_path() ? path.parent_path() : ".";
    run_front_end(compilation, { .root = folder, .read = read, .cache = cache },
                  to_utf8(path.filename()), mode);
    return compilation;
}

} // namespace

std::filesystem::path compilation_t::disk_path(const file_id_t id) const {
    const auto index = static_cast<std::size_t>(std::to_underlying(id));
    return index < on_disk.size() ? on_disk[index] : std::filesystem::path();
}

std::filesystem::path compilation_t::interface_path(const file_id_t id) const {
    const auto index = static_cast<std::size_t>(std::to_underlying(id));
    return index < in_interface.size() ? in_interface[index] : std::filesystem::path();
}

std::expected<compilation_t, std::string> check_files(const check_request_t& request) {
    if (const std::optional<std::string> unreadable = unreadable_std()) {
        return std::unexpected(*unreadable);
    }

    if (request.files.empty()) {
        return std::unexpected(std::string("there are no files to check"));
    }

    std::error_code error;
    const std::filesystem::path first =
        std::filesystem::weakly_canonical(request.files.front(), error);
    const std::optional<std::filesystem::path> standard = real_path(toolchain().std);
    const std::optional<std::filesystem::path> real = real_path(first);
    if (standard && real && is_under(*real, *standard)) {
        // The standard library's files, with an empty main file beside them, which no file of
        // std is named like.
        compilation_t compilation;
        compilation.file = compilation.sources.add("(std).fb", std::string());
        std::vector<std::string> extra;
        for (const std::filesystem::path& file : request.files) {
            if (const std::optional<std::filesystem::path> at = real_path(file);
                at && is_under(*at, *standard)) {
                extra.push_back("std/" + program_path(*standard, *at));
            }
        }

        run_front_end(compilation,
                      {
                          .root = *standard,
                          .extra = std::move(extra),
                          .read = request.read,
                          .cache = request.cache,
                      },
                      "(std).fb", request.mode);
        return compilation;
    }

    if (const std::optional<std::filesystem::path> root = find_package(first.parent_path())) {
        return compile_package(*root, request.main, request.mode, purpose_t::CHECK, &request);
    }

    return compile_loose(first, request.mode, request.read, request.cache);
}

std::expected<compilation_t, std::string> compile_file(const std::filesystem::path& path,
                                                       const build_mode_t mode) {
    std::error_code error;
    if (const std::optional<std::string> unreadable = unreadable_std()) {
        return std::unexpected(*unreadable);
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

    return compile_loose(path, mode, {});
}

std::expected<compilation_t, std::string> compile_tests(const std::filesystem::path& path,
                                                        const build_mode_t mode) {
    if (const std::optional<std::string> unreadable = unreadable_std()) {
        return std::unexpected(*unreadable);
    }

    std::error_code error;
    std::optional<std::filesystem::path> root;
    if (std::filesystem::is_directory(path, error) &&
        std::filesystem::is_regular_file(path / "project.toml", error)) {
        root = path;
    } else if (std::filesystem::is_regular_file(path, error) &&
               folded(to_utf8(path.extension())) == ".fb") {
        root = find_package(path.has_parent_path() ? path.parent_path()
                                                   : std::filesystem::current_path(error));
    }

    if (root) {
        return compile_package(*root, std::nullopt, mode, purpose_t::TEST);
    }

    if (!std::filesystem::is_regular_file(path, error) ||
        folded(to_utf8(path.extension())) != ".fb") {
        return compile_file(path, mode);
    }

    // A file outside a package is tested with the files it imports, as it's built, and its
    // examples, each a module beside it, which imports it by its path.
    compilation_t compilation;
    const std::expected<file_id_t, std::string> file = compilation.sources.load(path);
    if (!file) {
        return std::unexpected(file.error());
    }

    compilation.file = *file;
    const std::filesystem::path folder = path.has_parent_path() ? path.parent_path() : ".";
    const std::string name = to_utf8(path.filename());
    file_set_t files{ .root = folder, .testing = true };
    add_examples(compilation, files, { name }, "(doc tests) ", mode, examples_of_t::PROGRAM);
    run_front_end(compilation, std::move(files), name, mode);
    return compilation;
}

namespace {

/// The standard library's program: an empty main file, named unlike any of std's, and `files`'
/// extras, std's files or the modules of their examples.
compilation_t compile_standard(compilation_t compilation, file_set_t files,
                               const build_mode_t mode) {
    const std::string path = "(std).fb";
    compilation.file = compilation.sources.add(path, std::string());
    files.testing = true;
    run_front_end(compilation, std::move(files), path, mode);
    return compilation;
}

} // namespace

std::expected<compilation_t, std::string> compile_standard_tests(const build_mode_t mode) {
    if (const std::optional<std::string> unreadable = unreadable_std()) {
        return std::unexpected(*unreadable);
    }

    // A program of std's examples, which load what they import.
    compilation_t compilation;
    std::error_code error;
    file_set_t files{ .root = std::filesystem::current_path(error) };
    add_examples(compilation, files, std_files(files), "(doc tests)/", mode,
                 examples_of_t::STANDARD);
    return compile_standard(std::move(compilation), std::move(files), mode);
}

std::expected<compilation_t, std::string> compile_docs(const std::filesystem::path& path,
                                                       const build_mode_t mode) {
    if (const std::optional<std::string> unreadable = unreadable_std()) {
        return std::unexpected(*unreadable);
    }

    std::error_code error;
    // What isn't there can't be read, as fb build says.
    if (!std::filesystem::exists(path, error)) {
        std::error_code missing;
        static_cast<void>(std::filesystem::file_size(path, missing));
        return std::unexpected(
            std::format("cannot read '{}': {}", to_utf8(path), missing.message()));
    }

    std::optional<std::filesystem::path> root;
    if (std::filesystem::is_directory(path, error)) {
        if (!std::filesystem::is_regular_file(path / "project.toml", error)) {
            return std::unexpected(std::format(
                "there's no project.toml in '{}'; fb doc documents a package, from its folder or "
                "one of its .fb files, or std",
                to_utf8(path)));
        }

        root = path;
    } else if (std::filesystem::is_regular_file(path, error) &&
               folded(to_utf8(path.extension())) == ".fb") {
        root = find_package(path.has_parent_path() ? path.parent_path()
                                                   : std::filesystem::current_path(error));
    } else if (std::filesystem::is_regular_file(path, error)) {
        return std::unexpected(std::format(
            "'{}' isn't a .fb file; give a package's folder, one of its .fb files, or std",
            to_utf8(path)));
    }

    if (!root) {
        return std::unexpected(std::format(
            "'{}' isn't in a package, which a project.toml makes; fb doc documents a package, or "
            "std",
            to_utf8(path)));
    }

    return compile_package(*root, std::nullopt, mode, purpose_t::DOC);
}

std::expected<compilation_t, std::string> compile_standard_docs(const build_mode_t mode) {
    if (const std::optional<std::string> unreadable = unreadable_std()) {
        return std::unexpected(*unreadable);
    }

    std::error_code error;
    file_set_t files{ .root = std::filesystem::current_path(error) };
    files.extra = std_files(files);
    return compile_standard(compilation_t(), std::move(files), mode);
}

std::vector<test_case_t> package_tests(const compilation_t& compilation) {
    const module_table_t& modules = compilation.ast.modules();
    const std::optional<module_id_t> main = modules.main();
    if (!main) {
        return {};
    }

    const package_id_t own = modules.get(*main).package;
    const std::string outside = modules.package_name(own) + ".tests";
    std::vector<test_case_t> tests;
    for (const test_function_t& test : compilation.analysis.tests) {
        const symbol_t& function = compilation.analysis.symbol(test.function);
        const std::optional<module_id_t> module = modules.of(function.span.file);
        if (!module) {
            continue;
        }

        const module_t& declared = modules.get(*module);
        // An example's test is named after what it's an example of.
        const auto example =
            std::ranges::find(compilation.doc_tests, declared.path, &doc_test_t::path);
        if (example != compilation.doc_tests.end()) {
            tests.push_back({
                .function = test.function,
                .name = example->name,
                .main_thread = test.main_thread,
                .example = true,
            });
        } else if (declared.package == own || (modules.has_manifest(own) &&
                                               modules.package_name(declared.package) == outside)) {
            tests.push_back({
                .function = test.function,
                .name = std::format("{}.{}", declared.name, function.name),
                .main_thread = test.main_thread,
            });
        }
    }

    return tests;
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
    const std::filesystem::path& folder = toolchain().std;
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

std::vector<const build_config::bundled_library_t*>
bundled_libraries(const mir_module_t& module, const std::span<const library_binary_t> binaries) {
    // Each the program calls, and the prebuilt libraries' code, then those they're built on, in
    // the table's order.
    std::vector<std::string_view> wanted;
    for (const library_binary_t& binary : binaries) {
        for (const std::string& name : binary.bundled) {
            if (!std::ranges::contains(wanted, name) &&
                std::ranges::contains(build_config::bundled_libraries, std::string_view(name),
                                      &build_config::bundled_library_t::name)) {
                wanted.emplace_back(name);
            }
        }
    }

    for (const build_config::bundled_library_t& library : build_config::bundled_libraries) {
        const auto has_prefix = [&](const std::string_view symbol) {
            return std::ranges::any_of(
                std::views::split(library.prefixes, ' '), [&](const auto prefix) {
                    return symbol.starts_with(std::string_view(prefix.begin(), prefix.end()));
                });
        };
        const bool calls =
            !library.prefixes.empty() &&
            std::ranges::any_of(module.functions, [&](const mir_function_t& function) {
                return function.external && !function.external->c_abi &&
                       has_prefix(function.external->symbol);
            });
        if (calls && !std::ranges::contains(wanted, library.name)) {
            wanted.push_back(library.name);
        }
    }

    std::vector<std::string_view> unseen = wanted;
    while (!unseen.empty()) {
        const auto library = std::ranges::find(build_config::bundled_libraries, unseen.back(),
                                               &build_config::bundled_library_t::name);
        unseen.pop_back();
        for (const auto part : std::views::split(library->needs, ';')) {
            const std::string_view name(part.begin(), part.end());
            if (!name.empty() && !std::ranges::contains(wanted, name)) {
                wanted.push_back(name);
                unseen.push_back(name);
            }
        }
    }

    std::vector<const build_config::bundled_library_t*> used;
    for (const build_config::bundled_library_t& library : build_config::bundled_libraries) {
        if (std::ranges::contains(wanted, library.name)) {
            used.push_back(&library);
        }
    }

    return used;
}

namespace {

/// Copies each dynamic library beside the executable at `output`, under the name it records.
std::expected<void, std::string> copy_beside(const std::vector<dynamic_native_t>& libraries,
                                             const std::filesystem::path& output) {
    for (const dynamic_native_t& library : libraries) {
        const std::filesystem::path copy = output.parent_path() / path_from_utf8(library.beside);
        std::error_code error;
        if (std::filesystem::equivalent(library.file, copy, error)) {
            continue;
        }

        std::filesystem::copy_file(library.file, copy,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return std::unexpected(std::format("cannot copy {} beside the executable, as {}: {}",
                                               to_utf8(library.file), to_utf8(copy),
                                               error.message()));
        }
    }

    return {};
}

} // namespace

namespace {

/// A prebuilt library's binary, the .fblib beside its interface file, which has to be the same
/// build, this fb's for this target and the mode of its folder, built with `builds`, the
/// program's builds of the packages it was built with.
std::expected<library_binary_t, std::string>
read_prebuilt_binary(const std::filesystem::path& interface,
                     const std::vector<package_build_t>& builds) {
    std::filesystem::path path = interface;
    path.replace_extension(".fblib");
    const auto read = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        return in ? std::optional(std::string{ std::istreambuf_iterator<char>(in),
                                               std::istreambuf_iterator<char>() })
                  : std::nullopt;
    };
    const std::optional<std::string> text = read(interface);
    const std::optional<std::string> bytes = read(path);
    const std::string shown = to_utf8(shown_disk_path(path));
    if (!bytes) {
        return std::unexpected(std::format("there's no {}, the prebuilt library's binary beside "
                                           "its interface file",
                                           shown));
    }

    std::optional<library_binary_t> binary = read_binary(*bytes);
    const std::optional<interface_header_t> header =
        text ? read_interface_header(*text) : std::nullopt;
    if (!binary || !header) {
        return std::unexpected(std::format("{} isn't a library's binary that fb writes", shown));
    }

    // A program is built against one exact build of each library.
    const interface_header_t& built = binary->header;
    if (built.name != header->name || built.version != header->version ||
        built.hash != header->hash) {
        return std::unexpected(std::format(
            "{} is the build {} v{} ({}), but its interface file, {}, is {} v{} ({}); a program is "
            "built against one build of a library",
            shown, built.name, built.version, built.hash, to_utf8(shown_disk_path(interface)),
            header->name, header->version, header->hash));
    }

    // Its hash is of its build's mode, target, and fb, its interface file, and its code.
    const std::string shown_interface = to_utf8(shown_disk_path(interface));
    const build_mode_t mode = interface.parent_path().filename() == "release"
                                  ? build_mode_t::RELEASE
                                  : build_mode_t::DEBUG;
    std::string body(text->substr(std::min(text->find('\n'), text->size() - 1) + 1));
    std::erase(body, '\r');
    if (abi_hash(body, binary->code, mode) != header->hash) {
        return std::unexpected(std::format(
            "{} isn't the build its first line names, {} v{} ({}), of this fb, {}, for {} in {}: "
            "it's another fb's, target's, or mode's build, or its files were changed; build {} "
            "again",
            shown_interface, header->name, header->version, header->hash, build_config::version,
            build_target(), mode_folder(mode), header->name));
    }

    // And it's built with the builds of its packages that the program has.
    const std::vector<package_build_t> uses =
        interface_uses(*text).value_or(std::vector<package_build_t>{});
    for (const package_build_t& used : uses) {
        const auto found = std::ranges::find(builds, used.name, &package_build_t::name);
        if (found != builds.end() &&
            (found->version != used.version || found->build != used.build)) {
            const auto named = [](const package_build_t& build) {
                return std::format("{} v{} ({})", build.name, build.version, build.build);
            };
            return std::unexpected(std::format(
                "{} v{} was built with {}, but the program has {}; build {} again with it",
                header->name, header->version, named(used), named(*found), header->name));
        }
    }

    return std::move(*binary);
}

/// The file a library beside the executable is, by the OS's own kind of dynamic library's name.
std::string beside_file(const std::string_view name) {
#ifdef _WIN32
    return std::format("{}.dll", name);
#elifdef __APPLE__
    return std::format("lib{}.dylib", name);
#else
    return std::format("lib{}.so", name);
#endif
}

/// The library of the OS's that a file beside the executable would take the name of, which the
/// OS would find in its place: one of Windows' known DLLs, the C and C++ libraries, or one of the
/// program's system libraries. Empty for none.
std::string system_library_name(const std::string_view file,
                                const std::vector<std::string>& system) {
    constexpr auto known = std::to_array<std::string_view>({
#ifdef _WIN32
        "advapi32", "bcrypt",   "clbcatq",    "combase", "comctl32", "comdlg32",
        "crypt32",  "dbghelp",  "gdi32",      "gdiplus", "imagehlp", "imm32",
        "iphlpapi", "kernel32", "kernelbase", "msvcrt",  "normaliz", "nsi",
        "ntdll",    "ole32",    "oleaut32",   "psapi",   "rpcrt4",   "sechost",
        "secur32",  "setupapi", "shcore",     "shell32", "shlwapi",  "ucrtbase",
        "user32",   "userenv",  "version",    "winmm",   "wldap32",  "ws2_32",
#elifdef __APPLE__
        "System",
        "c++",
        "objc",
#else
        "c",
        "m",
        "dl",
        "pthread",
        "rt",
        "stdc++",
        "gcc_s",
#endif
    });
    for (const std::string_view name : known) {
        if (folded_name(beside_file(name)) == folded_name(std::string(file))) {
            return std::string(name);
        }
    }

    for (const std::string& name : system) {
        if (folded_name(beside_file(name)) == folded_name(std::string(file))) {
            return name;
        }
    }

    return {};
}

} // namespace

std::expected<library_build_t, std::string> build_library(const compilation_t& compilation,
                                                          const build_mode_t mode,
                                                          diagnostic_bag_t& diagnostics,
                                                          const std::string& folder) {
    if (!compilation.package) {
        return std::unexpected("only a package is a library");
    }

    const mir_module_t module =
        lower(compilation.ast, compilation.analysis, compilation.package->name);
    codegen_options_t options{ .mode = mode, .folder = folder };
    for (const module_t& each : compilation.ast.modules().modules()) {
        options.paths.emplace(each.file, each.path);
    }

    options.paths[compilation.file] = compilation.package->name;
    const std::expected<std::string, std::string> code =
        emit_bitcode(module, compilation.sources, compilation.file, options);
    if (!code) {
        return std::unexpected(code.error());
    }

    const std::optional<std::string> interface =
        write_interface(compilation, mode, diagnostics, *code);
    const std::optional<interface_header_t> header =
        interface ? read_interface_header(*interface) : std::nullopt;
    if (!header) {
        return std::unexpected("");
    }

    library_binary_t binary{ .header = *header, .code = *code };
    for (const build_config::bundled_library_t* library : bundled_libraries(module)) {
        binary.bundled.emplace_back(library->name);
    }

    return library_build_t{ .interface = *interface, .binary = write_binary(binary) };
}

std::expected<std::vector<library_binary_t>, std::string>
prebuilt_binaries(const compilation_t& compilation) {
    std::vector<library_binary_t> binaries;
    for (const std::filesystem::path& interface : compilation.interfaces) {
        std::expected<library_binary_t, std::string> binary =
            read_prebuilt_binary(interface, compilation.builds);
        if (!binary) {
            return std::unexpected(binary.error());
        }

        binaries.push_back(std::move(*binary));
    }

    return binaries;
}

namespace {

/// The binary of a library beside the executable: its prebuilt one, built with `builds`, the
/// program's, or a build of its source.
std::expected<library_binary_t, std::string>
beside_binary(const beside_library_t& library, const build_mode_t mode,
              const std::vector<package_build_t>& builds) {
    if (!library.interface.empty()) {
        return read_prebuilt_binary(library.interface, builds);
    }

    const std::expected<compilation_t, std::string> compiled =
        compile_package(library.root, std::nullopt, mode);
    if (!compiled) {
        return std::unexpected(compiled.error());
    }

    if (const std::size_t errors = compiled->diagnostics.error_count(); errors > 0) {
        return std::unexpected(std::format(
            "{}, the library beside the program, has {} error{}; `fb check {}` shows them",
            library.name, errors, errors == 1 ? "" : "s", to_utf8(shown_disk_path(library.root))));
    }

    // Its build's error is at its line. Built for this build alone, its debug info has the
    // folders its files are in here, which a debugger finds them by.
    diagnostic_bag_t diagnostics;
    std::error_code no_folder;
    const std::expected<library_build_t, std::string> built = build_library(
        *compiled, mode, diagnostics, to_utf8(std::filesystem::current_path(no_folder)));
    if (!built && (!built.error().empty() || diagnostics.diagnostics().empty())) {
        return std::unexpected(built.error());
    }

    if (!built) {
        // "path:line:col: error: message", without its "error", and how many more there are.
        std::string shown = render_short(diagnostics.diagnostics().front(), compiled->sources);
        while (shown.ends_with('\n')) {
            shown.pop_back();
        }

        if (const std::size_t at = shown.find(": error"); at != std::string::npos) {
            if (const std::size_t end = shown.find(": ", at + 2); end != std::string::npos) {
                shown.erase(at, end - at);
            }
        }

        const std::size_t errors = diagnostics.error_count();
        return std::unexpected(std::format(
            "{}, the library beside the program, can't be built: {}{}", library.name, shown,
            errors > 1 ? std::format("; it has {} more, which `fb check {}` shows", errors - 1,
                                     to_utf8(shown_disk_path(library.root)))
                       : std::string()));
    }

    std::optional<library_binary_t> binary = read_binary(built->binary);
    if (!binary) {
        return std::unexpected(std::format("{}'s build isn't one fb reads", library.name));
    }

    return std::move(*binary);
}

/// On macOS, an executable and the libraries beside it each get a .dSYM beside them with their
/// debug info, a release build's line tables too, which a debugger finds them by, as dsymutil makes
/// it from the object files, which the build removes. Elsewhere the debug info is in the binaries.
std::expected<void, std::string>
write_debug_symbols(std::span<const std::filesystem::path> binaries) {
    if (!build_config::target_triple.contains("apple")) {
        return {};
    }

    for (const std::filesystem::path& binary : binaries) {
        const std::array arguments{ std::string("dsymutil"), to_utf8(binary) };
        const std::expected<process_result_t, std::string> made =
            run_process("/usr/bin/xcrun", arguments);
        if (!made || made->exit_code != 0) {
            return std::unexpected(std::format("couldn't write {}'s debug info with dsymutil: {}",
                                               to_utf8(binary.filename()),
                                               made ? made->output : made.error()));
        }
    }

    return {};
}

} // namespace

std::expected<void, std::string> build_program(const mir_module_t& module,
                                               const compilation_t& compilation,
                                               const std::filesystem::path& output,
                                               const build_mode_t mode, program_build_t* built) {
    // A prebuilt library's code is its binary's, which links into the program's.
    std::expected<std::vector<library_binary_t>, std::string> binaries =
        prebuilt_binaries(compilation);
    if (!binaries) {
        return std::unexpected(binaries.error());
    }

    // And a library beside it is a file of its own.
    std::vector<library_binary_t> beside;
    for (const beside_library_t& library : compilation.beside) {
        std::expected<library_binary_t, std::string> binary =
            beside_binary(library, mode, compilation.builds);
        if (!binary) {
            return std::unexpected(binary.error());
        }

        beside.push_back(std::move(*binary));
    }

    // A dynamic library beside the executable would go over it.
    std::filesystem::path executable = output.filename();
#ifdef _WIN32
    if (executable.extension().empty()) {
        executable += ".exe";
    }
#endif
    for (const dynamic_native_t& library : compilation.natives.dynamic) {
        if (folded_name(library.beside) == folded_name(to_utf8(executable))) {
            return std::unexpected(std::format(
                "the executable {} would have the name of {}, which goes beside it as {}; give "
                "it another name",
                to_utf8(output), to_utf8(library.file), library.beside));
        }
    }

    // A library beside it can't have another file's name there, nor a library's of the OS's.
    for (std::size_t i = 0; i < compilation.beside.size(); ++i) {
        const beside_library_t& library = compilation.beside[i];
        const std::string file = beside_file(library.name);
        for (std::size_t j = 0; j < i; ++j) {
            if (folded_name(beside_file(compilation.beside[j].name)) == folded_name(file)) {
                return std::unexpected(std::format(
                    "the libraries {} and {} would both go beside the executable as {}; rename "
                    "one",
                    compilation.beside[j].name, library.name, file));
            }
        }

        if (folded_name(file) == folded_name(to_utf8(executable))) {
            return std::unexpected(std::format(
                "the library {} goes beside the executable as {}, which is the executable's own "
                "name; give it another name",
                library.name, file));
        }

        for (const dynamic_native_t& native : compilation.natives.dynamic) {
            if (folded_name(native.beside) == folded_name(file)) {
                return std::unexpected(std::format(
                    "the library {} goes beside the executable as {}, which {} goes beside it "
                    "as too; rename one",
                    library.name, file, to_utf8(native.file)));
            }
        }

        if (const std::string taken = system_library_name(file, compilation.natives.system);
            !taken.empty()) {
            return std::unexpected(std::format(
                "the library {} would go beside the executable as {}, which is the name of {}, a "
                "library of the OS's; rename the package",
                library.name, file, taken));
        }
    }

    // The output's folder may not exist yet, as with -o or fb run. If it can't be made, linking
    // reports that.
    std::error_code ignored;
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path(), ignored);
    }

    // A bundled library's system libraries come with it, after the packages' own.
    native_libraries_t natives = compilation.natives;
    std::vector<library_binary_t> all = *binaries;
    std::ranges::copy(beside, std::back_inserter(all));
    const std::vector<const build_config::bundled_library_t*> bundled =
        bundled_libraries(module, all);
    for (const build_config::bundled_library_t* library : bundled) {
        if (!library->runtime.empty()) {
            natives.bundled.push_back(toolchain().libraries / path_from_utf8(library->runtime));
        }

        for (const auto part : std::views::split(library->archives, ';')) {
            natives.bundled.push_back(toolchain().libraries /
                                      path_from_utf8(std::string_view(part.begin(), part.end())));
        }

        for (const auto part : std::views::split(library->system, ' ')) {
            const std::string name(part.begin(), part.end());
            if (!name.empty() && !std::ranges::contains(natives.system, name)) {
                natives.system.push_back(name);
            }
        }
    }

    // The object files are only needed for linking, and are removed even when linking fails.
    const std::filesystem::path folder = temporary_path(output.stem(), "");
    std::filesystem::create_directories(folder, ignored);
    const std::filesystem::path object = folder / "program.o";
    std::error_code no_folder;
    codegen_options_t options{
        .mode = mode,
        .folder = to_utf8(std::filesystem::current_path(no_folder)),
        .executable = to_utf8(executable),
    };
    for (std::size_t i = 0; i < binaries->size(); ++i) {
        std::filesystem::path path = compilation.interfaces[i];
        path.replace_extension(".fblib");
        options.linked.emplace_back(to_utf8(shown_disk_path(path)), binaries->at(i).code);
    }

    // Each library beside it takes what it shares from the executable, which keeps and exports
    // it, the runtime's functions among it.
    exports_t exports{ .import_library = folder / "program.lib" };
    for (std::size_t i = 0; i < beside.size(); ++i) {
        const interface_header_t& header = beside[i].header;
        options.beside.push_back({
            .package = compilation.beside[i].name,
            .file = beside_file(compilation.beside[i].name),
            .build = std::format("{} {} {}", header.name, header.version, header.hash),
            .code = beside[i].code,
            .source =
                compilation.beside[i].interface.empty()
                    ? std::format("the library {}", compilation.beside[i].name)
                    : to_utf8(shown_disk_path(std::filesystem::path(compilation.beside[i].interface)
                                                  .replace_extension(".fblib"))),
        });
        if (std::expected<void, std::string> read = read_beside_names(options.beside.back());
            !read) {
            remove_temporary(folder);
            return std::unexpected(read.error());
        }
    }

    std::vector<std::string> own;
    for (const beside_binary_t& library : options.beside) {
        std::ranges::copy(library.functions, std::back_inserter(own));
        for (const std::string& global : library.globals) {
            own.push_back(std::format("fb.(address).{}", global));
        }
    }

    for (std::size_t i = 0; i < beside.size(); ++i) {
        const std::expected<std::vector<std::string>, std::string> imports =
            emit_beside_object(options.beside[i], options, folder / std::format("beside{}.o", i));
        if (!imports) {
            remove_temporary(folder);
            return std::unexpected(imports.error());
        }

        // What another library beside it has, it takes from that one's file.
        for (const std::string& name : *imports) {
            if (std::ranges::contains(own, name)) {
                continue;
            }

            options.exports.push_back(name);
            if (!name.starts_with("fb.")) {
                exports.symbols.push_back(name);
            }
        }
    }

    // Each library beside it, after those it depends on, links against their import libraries
    // on Windows, and the executable's. An executable whose libraries can't all be made isn't
    // left, nor are those that were.
    std::vector<std::filesystem::path> import_libraries{ exports.import_library };
    std::expected<void, std::string> linked =
        emit_object(module, compilation.sources, compilation.file, options, object)
            .and_then([&] {
                return link_program(std::array{ object }, natives, output,
                                    beside.empty() ? nullptr : &exports,
                                    mode == build_mode_t::DEBUG);
            })
            .and_then([&] -> std::expected<void, std::string> {
                for (std::size_t i = 0; i < beside.size(); ++i) {
                    const std::filesystem::path linked = folder / std::format("beside{}.o", i);
                    const std::filesystem::path library = folder / std::format("beside{}.lib", i);
                    if (std::expected<void, std::string> done = link_beside(
                            std::array{ linked }, compilation.beside[i].natives, import_libraries,
                            output.parent_path() / path_from_utf8(options.beside[i].file), library,
                            mode == build_mode_t::DEBUG);
                        !done) {
                        std::filesystem::remove(output.parent_path() / executable, ignored);
                        for (std::size_t j = 0; j < i; ++j) {
                            std::filesystem::remove(output.parent_path() /
                                                        path_from_utf8(options.beside[j].file),
                                                    ignored);
                        }

                        return done;
                    }

                    import_libraries.insert(import_libraries.begin(), library);
                }

                return {};
            })
            .and_then([&] {
                std::vector<std::filesystem::path> binaries{ output };
                for (const beside_binary_t& library : options.beside) {
                    binaries.push_back(output.parent_path() / path_from_utf8(library.file));
                }

                return write_debug_symbols(binaries);
            })
            .and_then([&] { return copy_beside(natives.dynamic, output); });
    remove_temporary(folder);
    if (linked && built != nullptr) {
        built->bundled = bundled;
        for (const beside_binary_t& library : options.beside) {
            built->beside.push_back(library.file);
        }
    }

    return linked;
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

bool remove_temporary(const std::filesystem::path& path) {
    constexpr int tries = 40;
    constexpr auto pause = std::chrono::milliseconds(50);
    for (int i = 0; i < tries; ++i) {
        std::error_code removing;
        std::filesystem::remove_all(path, removing);
        std::error_code looking;
        if (!std::filesystem::exists(path, looking) && !looking) {
            return true;
        }

        std::this_thread::sleep_for(pause);
    }

    return false;
}

} // namespace fb
