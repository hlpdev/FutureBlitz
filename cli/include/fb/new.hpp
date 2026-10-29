#pragma once

#include "fb/options.hpp"

namespace fb {

/// `fb new <folder>`: makes a package in the folder, which mustn't be there or must be empty,
/// named by the folder's name: its project.toml, a main.fb that prints a greeting, and a
/// .gitignore of its build/ folder. Returns 2 for a name a package can't have, and 1 for a folder
/// that isn't empty or a file that can't be written.
[[nodiscard]] int new_command(const options_t& options);

} // namespace fb
