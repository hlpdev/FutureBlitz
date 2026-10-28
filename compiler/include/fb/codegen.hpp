#pragma once

#include "fb/mir.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fb {

/// Division by zero, and a float cast to an integer it doesn't fit, panic in both modes.
enum class build_mode_t : std::uint8_t {
    /// Unoptimized. Integer overflow and out-of-range shift amounts panic.
    DEBUG,
    /// Optimized. Integer overflow wraps around, and shift amounts are masked.
    RELEASE,
};

/// A library beside the executable, which main loads as it starts: its package's name, as MIR's
/// `beside` has it, its file's name beside the executable, its build, "<name> <version> <hash>",
/// which the executable checks the file is, and its code, LLVM bitcode as emit_bitcode() gives
/// it, whose shared code the executable has for it, and whose own its file has.
struct beside_binary_t {
    std::string package;
    std::string file;
    std::string build;
    std::string code;
    /// Its own functions and Globals, which its file has and exports, as read_beside_names()
    /// reads them.
    std::vector<std::string> functions;
    std::vector<std::string> globals;
};

struct codegen_options_t {
    build_mode_t mode = build_mode_t::DEBUG;
    /// An LLVM target triple, like "x86_64-pc-windows-msvc". Empty means the target the
    /// runtime was built for.
    std::string target_triple;
    /// The binaries of the prebuilt libraries the program uses, LLVM bitcode as
    /// emit_bitcode() gives it, which are linked into its module before it's optimized, each
    /// with the path of its .fblib, which an error names.
    std::vector<std::pair<std::string, std::string>> linked;
    /// The paths panics name files by, in place of their sources' own: a library's build names
    /// its modules by their paths in the program, so its binary is the same wherever it's built.
    std::map<file_id_t, std::string> paths;
    /// The libraries beside the executable, and the executable's file's name, which on Windows
    /// they take what they use from it by.
    std::vector<beside_binary_t> beside;
    std::string executable;
    /// What the libraries beside the executable take from it, by name, which it keeps and
    /// exports, as emit_beside_object() gives them.
    std::vector<std::string> exports;
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

/// Reads the names of `binary`'s own functions and Globals from its code, which every binary of
/// the program takes from its file: another library beside the executable's, and the
/// executable's too, through the executable. Fails when its code isn't bitcode that fb reads.
[[nodiscard]] std::expected<void, std::string> read_beside_names(beside_binary_t& binary);

/// A library beside the executable's file, as an object file at `path`: its own code, from
/// `binary`, which takes the code it shares, the runtime's, std's, and its records, from the
/// executable, the build it is, which the executable checks, and for each of its Globals the
/// function that gives its address, which is how the executable reaches it. Gives the names it
/// takes from the executable. The other libraries beside it in `options` it reaches through the
/// executable.
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
emit_beside_object(const beside_binary_t& binary, const codegen_options_t& options,
                   const std::filesystem::path& path);

/// Like emit_ir(), but writes a native object file to `path`.
[[nodiscard]] std::expected<void, std::string>
emit_object(const mir_module_t& module, const source_manager_t& sources, file_id_t main,
            const codegen_options_t& options, const std::filesystem::path& path);

} // namespace fb
