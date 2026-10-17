#pragma once

#include "fb/ast.hpp"
#include "fb/codegen.hpp"
#include "fb/diagnostic.hpp"
#include "fb/mir.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

#ifdef _WIN32
inline constexpr std::string_view executable_extension = ".exe";
#else
inline constexpr std::string_view executable_extension = "";
#endif

/// Everything the front end makes from a program: its main file, and the library files that
/// come after it in `sources`. The tokens are the main file's, and the AST and analysis cover
/// every file. They refer to the files in `sources`, which stay put when the compilation moves.
struct compilation_t {
    source_manager_t sources;
    /// The main file.
    file_id_t file{};
    /// Sorted by position.
    diagnostic_bag_t diagnostics;
    std::vector<token_t> tokens;
    ast_t ast;
    analysis_t analysis;

    [[nodiscard]] const source_file_t& source() const {
        return sources.get(file);
    }
};

/// A file of declarations that a program includes, like the standard library's.
struct library_file_t {
    std::string path;
    std::string text;
};

/// A main file through the front end, with the standard library: every stage runs, even after
/// errors.
[[nodiscard]] compilation_t compile(std::string path, std::string text);

/// Like compile(), but with `library` in place of the standard library.
[[nodiscard]] compilation_t compile(std::string path, std::string text,
                                    std::vector<library_file_t> library);

/// Fails when the file, or one of the standard library's, can't be read.
[[nodiscard]] std::expected<compilation_t, std::string>
compile_file(const std::filesystem::path& path);

/// The standard library's files, from std/ in the source tree, sorted by path. Each is named
/// like "std/core/error.fb". Fails when one can't be read.
[[nodiscard]] std::expected<std::vector<library_file_t>, std::string> standard_library();

/// Writes an executable for a module from a compilation without errors, creating the output's
/// folder if needed. The object file in between is temporary.
[[nodiscard]] std::expected<void, std::string> build_program(const mir_module_t& module,
                                                             const compilation_t& compilation,
                                                             const std::filesystem::path& output,
                                                             build_mode_t mode);

/// A new path in the system's temporary folder, like "futureblitz/hello-1a2b3c4d.exe".
[[nodiscard]] std::filesystem::path temporary_path(const std::filesystem::path& stem,
                                                   std::string_view extension);

} // namespace fb
