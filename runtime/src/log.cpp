// std.log's part of the runtime: what the default logger reads of the environment.

#include "fb/log.hpp"

#include "fb/runtime.hpp"
#include "internal.hpp"

#include <cstdint>
#include <cstdlib>

extern "C" {

fb_string_t* fb_log_setting() {
    const char* setting = std::getenv("FB_LOG");
    return setting == nullptr ? nullptr : fb_string_from_cstring(setting);
}

std::int32_t fb_log_colors() {
    return fb::runtime::stderr_colors() ? 1 : 0;
}
}
