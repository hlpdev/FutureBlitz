#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.log's functions in the runtime: the FB_LOG variable, which sets the default logger's levels,
// and whether its lines on stderr are colored. A String they give is the caller's.

extern "C" {

/// FB_LOG's value, or null when it isn't set.
fb_string_t* fb_log_setting();
/// 1 when stderr is a terminal that shows colors and NO_COLOR isn't set, as for panics, and 0
/// when it isn't.
std::int32_t fb_log_colors();
}
