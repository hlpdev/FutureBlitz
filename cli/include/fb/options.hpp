#pragma once

#include "fb/codegen.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// What fb was asked to do. HELP and VERSION don't take a file.
enum class command_t : std::uint8_t {
    HELP,
    VERSION,
    /// Makes a package in a new folder.
    NEW,
    CHECK,
    BUILD,
    RUN,
    /// Builds a package's tests and runs them.
    TEST,
    /// Runs fb-bindgen, which sits next to fb.
    BINDGEN,
    /// Formats .fb files in place.
    FMT,
    /// Writes a package's public API as JSON, for its reference pages.
    DOC,
    /// Adds dependencies from a registry to project.toml, and locks and fetches them.
    ADD,
    /// Resolves the package's registry dependencies anew, from their newest versions.
    UPDATE,
    /// Searches a registry's packages.
    SEARCH,
    /// Publishes a package to a registry.
    PUBLISH,
    /// Yanks a published version, or takes its yank back.
    YANK,
    /// Lists, adds, or removes a package's owners.
    OWNER,
    /// Signs in to a registry, and keeps its token.
    LOGIN,
    /// Forgets a registry's token.
    LOGOUT,
};

/// The `--color` choice. See use_color for what AUTO means.
enum class color_t : std::uint8_t {
    AUTO,
    ALWAYS,
    NEVER,
};

/// The stages `--show` can print, in pipeline order.
enum class stage_t : std::uint8_t {
    TOKENS,
    AST,
    TYPES,
    MIR,
    LLVM,
};

/// What the command line asked for.
struct options_t {
    command_t command = command_t::HELP;
    std::optional<command_t> help_for; // "fb help build" or "fb build --help"
    /// The .fb file, or a package's folder, which for NEW is the one it makes, or for `fb test`
    /// and `fb doc`, "std", the standard library. Empty for the current folder, and for HELP and
    /// VERSION.
    std::filesystem::path input;
    /// From `-o`, which only `fb build` and `fb doc` take. Nullopt puts the executable next to the
    /// source, or in a package's build/ folder, and fb doc's JSON in that folder too.
    std::optional<std::filesystem::path> output;
    /// RELEASE with `--release`.
    build_mode_t mode = build_mode_t::DEBUG;
    color_t color = color_t::AUTO;
    std::vector<stage_t> show; // sorted, without repeats
    /// What `fb bindgen` passes on to fb-bindgen, as it was written, but for fb's --color.
    std::vector<std::string> passed;
    /// What `fb run` passes to the program: what follows "--".
    std::vector<std::string> program_arguments;
    /// From `--filter`, which only `fb test` takes: what the names of the tests it runs have in
    /// them. Nullopt runs them all.
    std::optional<std::string> filter;
    /// What `fb fmt` formats: .fb files and folders. Empty for the current package.
    std::vector<std::filesystem::path> paths;
    /// `fb fmt --check`: lists the files it would change, without changing them. `fb doc
    /// --check`: checks that every module and every declaration its users see has a doc comment
    /// of its own, and that every link names something, without writing the JSON.
    bool check = false;
    /// `--offline`: registry packages come only from the cache.
    bool offline = false;
    /// `--registry`: a registry's name in [registries], or its index's URL or folder. Nullopt for
    /// futureblitz.dev's.
    std::optional<std::string> registry;
    /// What the package commands take: `fb add`'s packages, `fb update`'s, `fb search`'s words,
    /// `fb yank`'s `name@version`, and `fb owner`'s package.
    std::vector<std::string> names;
    /// `fb publish --dry-run`: builds and checks the archives, without publishing them.
    bool dry_run = false;
    /// `fb login --token`: reads the token from stdin.
    bool token = false;
    /// `fb yank --undo`.
    bool undo = false;
    /// `fb owner --add <who>` or `--remove <who>`, and which.
    std::optional<std::string> owner;
    bool remove_owner = false;
    /// `fb add --beside`: the dependency goes beside the executable.
    bool beside = false;
    /// `fb search --limit <n>`: how many it shows.
    std::size_t limit = 10;
};

/// The command's name, like "build".
[[nodiscard]] std::string_view command_name(command_t command);
/// Whether a command is the package manager's, from ADD to LOGOUT.
[[nodiscard]] bool is_package_command(command_t command);
/// The name `--show` takes, like "mir".
[[nodiscard]] std::string_view stage_name(stage_t stage);

/// Bad usage, as a message for people, and the --color given before the mistake, which the
/// message follows.
struct usage_error_t {
    std::string message;
    color_t color = color_t::AUTO;
};

/// The arguments after the program's name, `all`, as UTF-8. No arguments mean HELP.
[[nodiscard]] std::expected<options_t, usage_error_t>
parse_options(std::span<const std::string_view> all);

} // namespace fb
