#pragma once

#include "fb/driver.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// The build an interface file's first line names, "// fbi physics 0.2.0 <hash>": the library's
/// name and version, and its ABI hash.
struct interface_header_t {
    std::string name;
    std::string version;
    /// 32 hex digits.
    std::string hash;
};

/// The target a build is for, as `{target}` and lib/<target>/ name it: "windows-x64",
/// "linux-x64", "linux-arm64", "macos-arm64", or "macos-x64".
[[nodiscard]] std::string_view build_target();

/// "debug" or "release", as lib/<target>/<mode>/ names a mode.
[[nodiscard]] std::string_view mode_folder(build_mode_t mode);

/// Writes the interface file of the library that `compilation` checked without errors (spec 11):
/// FutureBlitz source for `mode` and this target, with every #If resolved, holding every Public
/// declaration, with the bodies of generic, @Inline, and Const code, the Private and Package
/// declarations that code reaches, and every Field of the types it shows. A declaration whose
/// code stays in the library's binary is its first line and "=> ...", or for a Global or Static
/// Field, "= ...". nullopt after an error in `diagnostics`, at what can't go in one, like an
/// Embed in code it keeps.
[[nodiscard]] std::optional<std::string>
write_interface(const compilation_t& compilation, build_mode_t mode, diagnostic_bag_t& diagnostics);

/// An interface file's first line, read. nullopt when it isn't one.
[[nodiscard]] std::optional<interface_header_t> read_interface_header(std::string_view text);

/// An interface file's modules, by their paths in the program, each its part of the file after
/// empty lines in place of those before it, so its lines are the file's. nullopt when its lines
/// aren't an interface file's.
[[nodiscard]] std::optional<std::vector<library_file_t>> interface_modules(std::string_view text);

} // namespace fb
