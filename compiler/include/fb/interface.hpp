#pragma once

#include "fb/driver.hpp"
#include "fb/library.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// The target a build is for, as `{target}` and lib/<target>/ name it: "windows-x64",
/// "linux-x64", "linux-arm64", "macos-arm64", or "macos-x64".
[[nodiscard]] std::string_view build_target();

/// "debug" or "release", as lib/<target>/<mode>/ names a mode.
[[nodiscard]] std::string_view mode_folder(build_mode_t mode);

/// Writes the interface file of the library that `compilation` checked without errors (spec 11):
/// FutureBlitz source for `mode` and this target, with every #If resolved, holding every Public
/// declaration, with the bodies of generic, @Inline, and Const code, the Private and Package
/// declarations that code reaches, every member of the types it shows, Private ones as their
/// first lines, and every Extend, after a line for each package it's built with. A declaration
/// whose code stays in the library's binary is its first line and "=> ...", or for a Global or
/// Static Field, "= ...". nullopt after an error in `diagnostics`, at what can't go in one, like
/// an Embed in code it keeps. Its ABI hash covers `code`, the library's binary's.
[[nodiscard]] std::optional<std::string> write_interface(const compilation_t& compilation,
                                                         build_mode_t mode,
                                                         diagnostic_bag_t& diagnostics,
                                                         std::string_view code = {});

/// The ABI hash of a library's build for `mode` and this target and fb: of `body`, its interface
/// file's text after its first line, and `code`, its binary's.
[[nodiscard]] std::string abi_hash(std::string_view body, std::string_view code, build_mode_t mode);

/// The build of a package compiled from source: "source:" and a hash of its files' paths and
/// texts.
[[nodiscard]] std::string source_build(std::vector<library_file_t> files);

/// The builds an interface file says its library was built with, from its "// uses <name>
/// <version> <build>" lines. nullopt when it isn't an interface file.
[[nodiscard]] std::optional<std::vector<package_build_t>> interface_uses(std::string_view text);

/// A .fblib's bytes: "// fblib <name> <version> <hash>", "// bundled <names>", and the code.
[[nodiscard]] std::string write_binary(const library_binary_t& binary);

/// A .fblib, read. nullopt when it isn't one.
[[nodiscard]] std::optional<library_binary_t> read_binary(std::string_view bytes);

/// An interface file's first line, read. nullopt when it isn't one.
[[nodiscard]] std::optional<interface_header_t> read_interface_header(std::string_view text);

/// An interface file's modules, by their paths in the program, each its part of the file after
/// empty lines in place of those before it, so its lines are the file's. nullopt when its lines
/// aren't an interface file's.
[[nodiscard]] std::optional<std::vector<library_file_t>> interface_modules(std::string_view text);

} // namespace fb
