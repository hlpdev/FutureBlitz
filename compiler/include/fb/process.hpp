#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fb {

/// What a finished program returned and printed.
struct process_result_t {
    /// On POSIX, a program killed by a signal gives 128 plus the signal number.
    int exit_code = 0;
    /// What it wrote to stdout and stderr, together.
    std::string output;
};

/// Runs a program and collects what it prints to stdout and stderr. `arguments` come after the
/// program's own name. With `input`, the program's stdin is that text, of any length, which ends
/// there; without, it's this process's. Fails only when the program can't be started.
[[nodiscard]] std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            std::optional<std::string_view> input = std::nullopt);

/// The program that's running, like fb, with its whole path. Empty when the system won't say.
[[nodiscard]] std::filesystem::path current_executable();

/// Where a file or folder really is, as the system has it: with the links and junctions on the
/// way followed, and each name in the disk's own letter case. nullopt when it isn't there.
[[nodiscard]] std::optional<std::filesystem::path> real_path(const std::filesystem::path& path);

/// Runs a program in this terminal, with this process's stdin, stdout, and stderr, and returns
/// its exit code.
[[nodiscard]] std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                                           std::span<const std::string> arguments);

} // namespace fb
