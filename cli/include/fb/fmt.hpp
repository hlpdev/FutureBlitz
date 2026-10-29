#pragma once

#include "fb/options.hpp"

namespace fb {

/// `fb fmt`: formats the .fb files of the paths given, or of the current package, in place, or
/// with `--check` lists the ones it would change. Returns 1 when a file isn't formatted with
/// `--check`, or can't be formatted, and 2 for a path that isn't there.
[[nodiscard]] int format_command(const options_t& options);

} // namespace fb
