// std.log's part of the runtime: what the default logger reads of the environment.

#include "fb/log.hpp"

#include "fb/runtime.hpp"
#include "internal.hpp"

#include <cstdint>
#include <optional>
#include <string>

extern "C" {

fb_string_t* fb_log_setting() {
    const std::optional<std::string> setting = fb::runtime::environment("FB_LOG");
    return setting ? fb_string_from_cstring(setting->c_str()) : nullptr;
}

std::int32_t fb_log_colors() {
    return fb::runtime::stderr_colors() ? 1 : 0;
}
}
