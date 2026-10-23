#pragma once

#include "fb/options.hpp"

namespace fb {

/// fb's exit code for compile errors, a failed build, or an unexpected failure in fb itself.
inline constexpr int compile_errors_exit_code = 1;
/// fb's exit code for bad arguments or a source file that can't be read.
inline constexpr int usage_exit_code = 2;

/// Runs a command and returns fb's exit code. `fb run` returns the program's own exit code once
/// the program builds. Stage views go to stdout, and everything else to stderr.
[[nodiscard]] int run_command(const options_t& options);

/// Prints "error: ..." for bad arguments to stderr, then a pointer to the help.
void report_usage_error(const usage_error_t& error);

} // namespace fb
