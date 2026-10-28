#pragma once

#include "fb/mir.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fb {

/// Division by zero, and a float cast to an integer it doesn't fit, panic in both modes.
enum class build_mode_t : std::uint8_t {
    /// Unoptimized. Integer overflow and out-of-range shift amounts panic.
    DEBUG,
    /// Optimized. Integer overflow wraps around, and shift amounts are masked.
    RELEASE,
};

struct codegen_options_t {
    build_mode_t mode = build_mode_t::DEBUG;
    /// An LLVM target triple, like "x86_64-pc-windows-msvc". Empty means the target the
    /// runtime was built for.
    std::string target_triple;
    /// The binaries of the prebuilt libraries the program uses, LLVM bitcode as
    /// emit_bitcode() gives it, which are linked into its module before it's optimized.
    std::vector<std::string> linked;
};

/// The module as LLVM IR text, optimized in release builds. `sources` gives panics their path,
/// line, and column, and the module is named after the `main` file. Fails when the target isn't
/// available or the IR is invalid.
[[nodiscard]] std::expected<std::string, std::string> emit_ir(const mir_module_t& module,
                                                              const source_manager_t& sources,
                                                              file_id_t main,
                                                              const codegen_options_t& options);

/// A library's build, `module` as lower() gives it for the library, as LLVM bitcode: its own code,
/// and the code it uses that programs share, which a program's build links and optimizes with its
/// own.
[[nodiscard]] std::expected<std::string, std::string>
emit_bitcode(const mir_module_t& module, const source_manager_t& sources, file_id_t main,
             const codegen_options_t& options);

/// Like emit_ir(), but writes a native object file to `path`.
[[nodiscard]] std::expected<void, std::string>
emit_object(const mir_module_t& module, const source_manager_t& sources, file_id_t main,
            const codegen_options_t& options, const std::filesystem::path& path);

} // namespace fb
