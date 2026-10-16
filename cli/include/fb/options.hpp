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
    /// The .fb file. Empty only for HELP and VERSION.
    std::filesystem::path input;
    /// From `-o`, which only `fb build` takes. Nullopt puts the executable next to the source.
    std::optional<std::filesystem::path> output;
    /// RELEASE with `--release`.
    build_mode_t mode = build_mode_t::DEBUG;
    color_t color = color_t::AUTO;
    std::vector<stage_t> show; // sorted, without repeats
};

/// The command's name, like "build".
[[nodiscard]] std::string_view command_name(command_t command);
/// The name `--show` takes, like "mir".
[[nodiscard]] std::string_view stage_name(stage_t stage);

/// The arguments after the program's name, as UTF-8. No arguments mean HELP. Errors are messages
/// for people.
[[nodiscard]] std::expected<options_t, std::string>
parse_options(std::span<const std::string_view> arguments);

} // namespace fb
