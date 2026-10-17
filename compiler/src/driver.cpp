#include "fb/driver.hpp"

#include "fb/build_config.hpp"
#include "fb/lexer.hpp"
#include "fb/link.hpp"
#include "fb/parser.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <utility>

namespace fb {
namespace {

/// The main file is already in `compilation`, and the library files are added after it.
void run_front_end(compilation_t& compilation, std::vector<library_file_t> library) {
    // The library files come first in the AST, so a main file that declares one of their names
    // again is the one with the error.
    for (library_file_t& added : library) {
        const source_file_t& file = compilation.sources.get(
            compilation.sources.add(std::move(added.path), std::move(added.text)));
        const std::vector<token_t> tokens = lex(file, compilation.diagnostics);
        parse_into(file, tokens, compilation.diagnostics, compilation.ast);
    }

    const source_file_t& file = compilation.source();
    compilation.tokens = lex(file, compilation.diagnostics);
    parse_into(file, compilation.tokens, compilation.diagnostics, compilation.ast);
    compilation.ast.set_main_file(compilation.file);
    compilation.analysis = analyze(compilation.sources, compilation.ast, compilation.diagnostics);
    // Each stage adds its own diagnostics, so sorting puts them back in source order.
    compilation.diagnostics.sort();
}

} // namespace

compilation_t compile(std::string path, std::string text) {
    std::expected<std::vector<library_file_t>, std::string> library = standard_library();
    compilation_t compilation =
        compile(std::move(path), std::move(text), library.value_or(std::vector<library_file_t>{}));
    if (!library) {
        compilation.diagnostics.error({ .file = compilation.file }, "{}", library.error());
    }

    return compilation;
}

compilation_t compile(std::string path, std::string text, std::vector<library_file_t> library) {
    compilation_t compilation;
    compilation.file = compilation.sources.add(std::move(path), std::move(text));
    run_front_end(compilation, std::move(library));
    return compilation;
}

std::expected<compilation_t, std::string> compile_file(const std::filesystem::path& path) {
    compilation_t compilation;
    const std::expected<file_id_t, std::string> file = compilation.sources.load(path);
    if (!file) {
        return std::unexpected(file.error());
    }

    std::expected<std::vector<library_file_t>, std::string> library = standard_library();
    if (!library) {
        return std::unexpected(library.error());
    }

    compilation.file = *file;
    run_front_end(compilation, std::move(*library));
    return compilation;
}

std::expected<std::vector<library_file_t>, std::string> standard_library() {
    const std::filesystem::path folder(build_config::std_directory);
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
         !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        if (entry->is_regular_file() && entry->path().extension() == ".fb") {
            paths.push_back(entry->path());
        }
    }

    if (error) {
        return std::unexpected(std::format("cannot read the standard library in '{}': {}",
                                           to_utf8(folder), error.message()));
    }

    // The order the folder lists them in varies, and the program shouldn't.
    std::ranges::sort(paths);
    std::vector<library_file_t> library;
    library.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        std::string name = "std/" + to_utf8(path.lexically_relative(folder));
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected(std::format("cannot open '{}'", name));
        }

        library.push_back({
            .path = std::move(name),
            .text = { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() },
        });
    }

    return library;
}

std::expected<void, std::string> build_program(const mir_module_t& module,
                                               const compilation_t& compilation,
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
        emit_object(module, compilation.sources, compilation.file, { .mode = mode }, object)
            .and_then([&] { return link_program(std::array{ object }, output); });
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
