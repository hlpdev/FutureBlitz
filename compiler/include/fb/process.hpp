#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace fb {

/// What a finished program returned and printed.
struct process_result_t {
    /// On POSIX, a program killed by a signal gives 128 plus the signal number.
    int exit_code = 0;
    /// What it wrote to stdout and stderr, together.
    std::string output;
};

/// Runs a program and collects what it prints to stdout and stderr. `arguments` come after the
/// program's own name. Fails only when the program can't be started.
[[nodiscard]] std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments);

/// Runs a program in this terminal, with this process's stdin, stdout, and stderr, and returns
/// its exit code.
[[nodiscard]] std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                                           std::span<const std::string> arguments);

} // namespace fb
