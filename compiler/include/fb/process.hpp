#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fb {

/// What a finished program returned and printed.
struct process_result_t {
    /// On POSIX, a program killed by a signal gives 128 plus the signal number.
    int exit_code = 0;
    /// What it wrote to stdout and stderr, together.
    std::string output;
};

/// Variables a program's environment has besides this process's, each by its name and value, in
/// place of one of this process's of that name.
using environment_t = std::span<const std::pair<std::string, std::string>>;

/// Runs a program and collects what it prints to stdout and stderr. `arguments` come after the
/// program's own name. With `input`, the program's stdin is that text, of any length, which ends
/// there; without, it's this process's. Its environment is this process's, with `environment`'s
/// variables. Fails only when the program can't be started.
[[nodiscard]] std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            std::optional<std::string_view> input = std::nullopt, environment_t environment = {});

/// The program that's running, like fb, with its whole path. Empty when the system won't say.
[[nodiscard]] std::filesystem::path current_executable();

/// Where a file or folder really is, as the system has it: with the links and junctions on the
/// way followed, and each name in the disk's own letter case. nullopt when it isn't there.
[[nodiscard]] std::optional<std::filesystem::path> real_path(const std::filesystem::path& path);

/// Runs a program in this terminal, with this process's stdin, stdout, and stderr, and returns
/// its exit code.
[[nodiscard]] std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                                           std::span<const std::string> arguments);

/// Starts a program and doesn't wait for it, with none of this process's stdin, stdout, or
/// stderr, as a browser is opened. Fails only when it can't be started.
[[nodiscard]] std::expected<void, std::string>
start_detached(const std::filesystem::path& program, std::span<const std::string> arguments);

} // namespace fb
