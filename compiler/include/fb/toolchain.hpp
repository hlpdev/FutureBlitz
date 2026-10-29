#pragma once

#include <filesystem>

namespace fb {

/// Where fb finds what it builds programs with. A release keeps it beside fb: lld in fb's bin/
/// folder, and lib/<target>/, notices/, and std/ in the folder above it. A build folder keeps
/// it where CMake put it.
struct toolchain_t {
    /// lld, which links for every target, as its -flavor says.
    std::filesystem::path linker;
    /// lib/<target>/: the runtime's archives, std's bundled libraries, and the C runtime's
    /// startup files and libraries, which a program links.
    std::filesystem::path libraries;
    /// The bundled libraries' notices.
    std::filesystem::path notices;
    /// The standard library's source files.
    std::filesystem::path std;
};

/// The toolchain of the fb at `executable`: a release's, when its folder has lld and the folder
/// above it the target's libraries and std, or else this build folder's.
[[nodiscard]] toolchain_t find_toolchain(const std::filesystem::path& executable);

/// This fb's toolchain, found the first time it's asked for.
[[nodiscard]] const toolchain_t& toolchain();

} // namespace fb
