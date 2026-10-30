#pragma once

#include "fb/console.hpp"
#include "fb/options.hpp"

#include <filesystem>

namespace fb {

/// The package manager's commands: `fb add`, `update`, `search`, `publish`, `yank`, `owner`,
/// `login`, and `logout`. Returns fb's exit code.
[[nodiscard]] int package_command(const options_t& options);

/// Before `fb check`, `build`, `run`, `test`, and `doc` compile what's at `input`: locks the
/// registry dependencies of the package it's in, and fetches what fb.lock needs into the cache,
/// saying what it adds, changes, and downloads. False after saying why it couldn't; true too for
/// a .fb file outside a package, and for a project.toml with mistakes, which the compile reports.
[[nodiscard]] bool prepare_packages(const console_t& console, const options_t& options,
                                    const std::filesystem::path& input);

} // namespace fb
