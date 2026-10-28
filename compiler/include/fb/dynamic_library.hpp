#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace fb {

/// Why a file isn't a dynamic library a program for a target can load.
enum class library_problem_t : std::uint8_t {
    /// It isn't the target's kind of dynamic library: a DLL on Windows, a shared object on Linux,
    /// or a dylib on macOS.
    NOT_A_LIBRARY,
    /// It is one, but for another CPU, or of another width or byte order.
    OTHER_TARGET,
    /// It's an executable of the target's kind, not a library.
    EXECUTABLE,
};

/// The name a dynamic library's file, `bytes`, gives itself, which an executable linked to it
/// records: on Linux its SONAME, or "" for one without, on macOS its install name, and on
/// Windows "", where the executable records the file's own name. `target` is one of
/// build_target()'s, like "linux-x64".
[[nodiscard]] std::expected<std::string, library_problem_t>
dynamic_library_name(std::string_view bytes, std::string_view target);

} // namespace fb
