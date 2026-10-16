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

/// Everything the front end makes from one file. The tokens, AST, and analysis refer to the
/// file in `sources`, which stays put when the compilation moves.
struct compilation_t {
    source_manager_t sources;
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

/// One file through the front end: every stage runs, even after errors.
[[nodiscard]] compilation_t compile(std::string path, std::string text);

/// Fails when the file can't be read.
[[nodiscard]] std::expected<compilation_t, std::string>
compile_file(const std::filesystem::path& path);

/// Writes an executable for a module from a compilation without errors, creating the output's
/// folder if needed. The object file in between is temporary.
[[nodiscard]] std::expected<void, std::string> build_program(const mir_module_t& module,
                                                             const source_file_t& file,
                                                             const std::filesystem::path& output,
                                                             build_mode_t mode);

/// A new path in the system's temporary folder, like "futureblitz/hello-1a2b3c4d.exe".
[[nodiscard]] std::filesystem::path temporary_path(const std::filesystem::path& stem,
                                                   std::string_view extension);

} // namespace fb
