#pragma once

#include "fb/options.hpp"

namespace fb {

/// `fb mcp [folder]`: serves MCP over stdio for the package the folder is in, or the current
/// folder's, a message a line each way, until stdin ends, when it ends the process with 0 at once.
/// Returns 2 for a path that isn't a folder.
[[nodiscard]] int mcp_command(const options_t& options);

} // namespace fb
