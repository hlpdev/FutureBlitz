#pragma once

#include "fb/diagnostic.hpp"
#include "fb/source.hpp"
#include "fb/style.hpp"

#include <string>

namespace fb {

struct render_options_t {
    bool color = false;
};

/// One line, "path:line:col: error[code]: message", without color. Lines and columns start at
/// 1, and columns count code points.
[[nodiscard]] std::string render_short(const diagnostic_t& diagnostic,
                                       const source_manager_t& sources);

/// render_short()'s line, then the source line with the span underlined by '^'. Each label
/// follows as a note underlined by '-', and then come the notes.
[[nodiscard]] std::string render_pretty(const diagnostic_t& diagnostic,
                                        const source_manager_t& sources,
                                        render_options_t options = {});

} // namespace fb
