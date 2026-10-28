#pragma once

#include "fb/source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {

/// A statement's index in its ast_t, which ast.hpp defines.
enum class stmt_id_t : std::uint32_t;

/// A module's index in its module_table_t, in the order the driver added them.
enum class module_id_t : std::uint32_t {};

/// A package's index in its module_table_t.
enum class package_id_t : std::uint32_t {};

/// One Import or From line of a module, as the driver resolved it.
struct module_import_t {
    /// The import_stmt_t.
    stmt_id_t statement{};
    /// The modules it names: one file's, or each of a folder's files'. Empty when it names
    /// nothing, which the driver reported.
    std::vector<module_id_t> modules;
};

/// One file of a program, which has its own top-level names.
struct module_t {
    /// Its path from its root, without ".fb" and with dots for folders: "std.core.error" for
    /// std/core/error.fb. Without a project.toml, the main module is named by its file's stem,
    /// and in a package by its path, like "game.main".
    std::string name;
    /// Its path in the program: "std/core/error.fb", one from the main file's folder, like
    /// "ui/hud.fb", or a package's, like "game/ui/hud.fb". Without a project.toml, the main
    /// file's is its name, and in a package its path, like "game/main.fb".
    std::string path;
    file_id_t file{};
    /// The program's main module, whose top-level code runs.
    bool is_main = false;
    /// What it imports, in the order its lines say. std's names need an Import too, std.core's
    /// as well.
    std::vector<module_import_t> imports;
    /// The package it's part of, whose modules see its Package declarations.
    package_id_t package{};
    /// From a prebuilt library's interface file, whose code is in the library's binary.
    bool prebuilt = false;
    /// A library's beside the executable, whose code is in its file of its own.
    bool beside = false;
};

/// A name that a standard library module the program doesn't load declares, for an error to
/// say which module to import: a top-level name, or a Method an Extend of T[] adds.
struct std_name_t {
    std::string name;
    /// What an Import names to get it, like "std.collections".
    std::string module;
    bool array_method = false;
    /// The built-in type an Extend adds it to, like "CString". Empty for a top-level name or a
    /// Method of T[].
    std::string extended;
};

/// A module's name from its path: "std/core/error.fb" is "std.core.error".
[[nodiscard]] std::string module_name(std::string_view path);

/// A program's modules, one per file, in the order their Globals initialize: std/core/'s
/// first, then each module after the ones it imports, the main module last.
class module_table_t {
public:
    module_id_t add(module_t module);

    /// `id` must come from this table.
    [[nodiscard]] const module_t& get(module_id_t id) const;

    /// The module a file is. nullopt for a file the table doesn't have, as in an AST that the
    /// parser made by itself.
    [[nodiscard]] std::optional<module_id_t> of(file_id_t file) const;

    /// Whether a file is the main module. A file the table doesn't have counts as the main
    /// module.
    [[nodiscard]] bool is_main(file_id_t file) const;

    /// The main module, if the table has one.
    [[nodiscard]] std::optional<module_id_t> main() const;

    [[nodiscard]] std::size_t size() const {
        return m_modules.size();
    }

    [[nodiscard]] const std::vector<module_t>& modules() const {
        return m_modules;
    }

    void add_std_name(std_name_t name) {
        m_std_names.push_back(std::move(name));
    }

    /// A package, by the name messages show: the standard library's is "std", a project.toml's
    /// is the name it gives, and without one, a program's own files are its main module's. Only
    /// the standard library's is `standard`, which a program's name can't make it. `natives` are
    /// its project.toml's [native] libraries' names, which `Lib` can give: nullopt without a
    /// project.toml, and none for the standard library.
    package_id_t add_package(std::string name, bool standard = false,
                             std::optional<std::vector<std::string>> natives = std::nullopt);

    /// `id` must come from this table.
    [[nodiscard]] const std::string& package_name(package_id_t id) const;

    /// Whether a package is the standard library. `id` must come from this table.
    [[nodiscard]] bool is_standard(package_id_t id) const;

    /// A package's [native] libraries' names, as add_package() was given them. `id` must come
    /// from this table.
    [[nodiscard]] const std::optional<std::vector<std::string>>& natives(package_id_t id) const;

    /// Whether a package has a project.toml, whose modules Imports name with dots, like
    /// "physics.shapes", rather than by their paths. `id` must come from this table.
    [[nodiscard]] bool has_manifest(package_id_t id) const {
        return natives(id).has_value();
    }

    /// Records that `package` depends on `on`, as its project.toml says.
    void add_dependency(package_id_t package, package_id_t on);

    /// Whether `package`'s project.toml names `on` as a dependency.
    [[nodiscard]] bool depends_on(package_id_t package, package_id_t on) const;

    /// The module that declares the member `name` an Extend adds to the built-in type named
    /// `type`, among the standard library's that the program doesn't load. nullopt when none
    /// does.
    [[nodiscard]] std::optional<std::string_view> std_member_home(std::string_view type,
                                                                  std::string_view name) const;

    /// The module that declares `name` among the standard library's that the program doesn't
    /// load, or among their Methods of T[] for `array_method`. nullopt when none does.
    [[nodiscard]] std::optional<std::string_view> std_home(std::string_view name,
                                                           bool array_method) const;

private:
    std::vector<module_t> m_modules;
    /// Each package's name, by package_id_t.
    std::vector<std::string> m_packages;
    /// Each package's [native] libraries' names, by package_id_t.
    std::vector<std::optional<std::vector<std::string>>> m_natives;
    /// Each package's dependencies, by package_id_t.
    std::vector<std::vector<package_id_t>> m_dependencies;
    /// The standard library's package, once it's added.
    std::optional<package_id_t> m_standard;
    /// Each file's module, by file_id_t.
    std::vector<std::optional<module_id_t>> m_by_file;
    std::vector<std_name_t> m_std_names;
};

} // namespace fb
