#include "fb/driver.hpp"

#include "fb/lexer.hpp"
#include "fb/link.hpp"
#include "fb/parser.hpp"

#include <array>
#include <format>
#include <random>
#include <system_error>
#include <utility>

namespace fb {
namespace {

void run_front_end(compilation_t& compilation) {
    const source_file_t& file = compilation.source();
    compilation.tokens = lex(file, compilation.diagnostics);
    compilation.ast = parse(file, compilation.tokens, compilation.diagnostics);
    compilation.analysis = analyze(file, compilation.ast, compilation.diagnostics);
    // Each stage adds its own diagnostics, so sorting puts them back in source order.
    compilation.diagnostics.sort();
}

} // namespace

compilation_t compile(std::string path, std::string text) {
    compilation_t compilation;
    compilation.file = compilation.sources.add(std::move(path), std::move(text));
    run_front_end(compilation);
    return compilation;
}

std::expected<compilation_t, std::string> compile_file(const std::filesystem::path& path) {
    compilation_t compilation;
    const std::expected<file_id_t, std::string> file = compilation.sources.load(path);
    if (!file) {
        return std::unexpected(file.error());
    }

    compilation.file = *file;
    run_front_end(compilation);
    return compilation;
}

std::expected<void, std::string> build_program(const mir_module_t& module,
                                               const source_file_t& file,
                                               const std::filesystem::path& output,
                                               const build_mode_t mode) {
    // The output's folder may not exist yet, as with -o or fb run. If it can't be made, linking
    // reports that.
    std::error_code ignored;
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path(), ignored);
    }

    // The object file is only needed for linking, and is removed even when linking fails.
    const std::filesystem::path object = temporary_path(output.stem(), ".o");
    std::expected<void, std::string> built =
        emit_object(module, file, { .mode = mode }, object).and_then([&] {
            return link_program(std::array{ object }, output);
        });
    std::filesystem::remove(object, ignored);
    return built;
}

std::filesystem::path temporary_path(const std::filesystem::path& stem,
                                     const std::string_view extension) {
    std::error_code ignored;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path(ignored) / "futureblitz";
    std::filesystem::create_directories(folder, ignored);

    // A random suffix keeps compilations that run at the same time, like parallel tests, apart.
    std::random_device random;
    std::filesystem::path name = stem;
    name += std::format("-{:08x}{}", random(), extension);
    return folder / name;
}

} // namespace fb
