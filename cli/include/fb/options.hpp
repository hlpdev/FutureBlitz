#pragma once

#include "fb/codegen.hpp"

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
    CHECK,
    BUILD,
    RUN,
    /// Builds a package's tests and runs them.
    TEST,
    /// Runs fb-bindgen, which sits next to fb.
    BINDGEN,
    /// Formats .fb files in place.
    FMT,
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
    /// The .fb file, or a package's folder. Empty for the current folder, and for HELP and
    /// VERSION.
    std::filesystem::path input;
    /// From `-o`, which only `fb build` takes. Nullopt puts the executable next to the source, or
    /// in a package's build/ folder.
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
    /// `fb fmt --check`: lists the files it would change, without changing them.
    bool check = false;
};

/// The command's name, like "build".
[[nodiscard]] std::string_view command_name(command_t command);
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
