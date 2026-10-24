// Which rules std.io's Path reads paths by: the target's, which only the runtime is built for.

#include "fb/files.hpp"

#include <cstdint>

extern "C" {

std::int32_t fb_path_windows() {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}
}
