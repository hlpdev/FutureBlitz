#include "fb/fmt.hpp"

#include "fb/commands.hpp"
#include "fb/diagnostic.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/driver.hpp"
#include "fb/format.hpp"
#include "fb/manifest.hpp"
#include "fb/source.hpp"
#include "fb/style.hpp"
#include "fb/terminal.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fb {
namespace {

/// Writes to stdout or stderr, in the same order when both go to the same place.
void write(std::FILE* stream, const std::string_view text) {
    std::fflush(stdout);
    std::fwrite(text.data(), 1, text.size(), stream);
}

/// "1 file" or "3 files".
std::string files(const std::size_t number) {
    return std::format("{} file{}", number, number == 1 ? "" : "s");
}

/// A file's bytes, or nullopt when it can't be read.
std::optional<std::string> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    if (file.bad()) {
        return std::nullopt;
    }

    return text;
}

/// Formats the files `fb fmt` finds, each to its package's width.
class formatting_t {
public:
    explicit formatting_t(const options_t& options)
        : m_options(options), m_err{ .color = use_color(options.color, stream_t::STANDARD_ERROR) } {
    }

    int run() {
        std::vector<std::filesystem::path> paths = m_options.paths;
        if (paths.empty()) {
            std::error_code error;
            const std::optional<std::filesystem::path> package =
                find_package(std::filesystem::current_path(error));
            if (!package) {
                this->error("there's no project.toml in this folder or above it; give .fb files or "
                            "folders to format");
                return usage_exit_code;
            }

            paths.push_back(*package);
        }

        std::vector<std::filesystem::path> found;
        for (const std::filesystem::path& path : paths) {
            if (!collect(path, found)) {
                return usage_exit_code;
            }
        }

        // Each file once, however it was reached, as "src/main.fb" and "./src/main.fb" are, by
        // the spelling it was first found by.
        std::ranges::sort(found);
        std::vector<std::filesystem::path> seen;
        for (const std::filesystem::path& path : found) {
            std::error_code error;
            std::filesystem::path key = std::filesystem::weakly_canonical(path, error);
            if (error) {
                key = std::filesystem::absolute(path, error).lexically_normal();
            }

            if (!std::ranges::contains(seen, key)) {
                seen.push_back(std::move(key));
                format_file(path);
            }
        }

        if (m_failed > 0) {
            error(std::format("couldn't format {}", files(m_failed)));
        }

        if (m_options.check && m_changed > 0) {
            error(std::format("{} {} formatted; fb fmt formats them", files(m_changed),
                              m_changed == 1 ? "isn't" : "aren't"));
        }

        return m_failed > 0 || (m_options.check && m_changed > 0) ? compile_errors_exit_code : 0;
    }

private:
    const options_t& m_options;
    style_t m_err;
    /// Each package's width, by its folder.
    std::map<std::filesystem::path, std::optional<std::uint32_t>> m_widths;
    std::size_t m_changed = 0;
    std::size_t m_failed = 0;

    void error(const std::string_view message) const {
        write(stderr, std::format("{}: {}\n", m_err.paint("error", tone_t::ERROR),
                                  m_err.paint(message, tone_t::BOLD)));
    }

    void status(const std::string_view verb, const std::string_view message) const {
        write(stderr,
              std::format("{} {}\n", m_err.paint(std::format("{:>12}", verb), tone_t::SUCCESS),
                          message));
    }

    /// The .fb files at `path`: itself, or those in the folder and the folders in it, but for
    /// hidden ones and a package's build/. False after reporting a path that isn't there.
    bool collect(const std::filesystem::path& path, std::vector<std::filesystem::path>& found) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            // Its extension in any letter case, as the other commands take it.
            std::string extension = to_utf8(path.extension());
            std::ranges::transform(extension, extension.begin(), [](const char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
            if (extension != ".fb") {
                this->error(std::format("{} isn't a .fb file", to_utf8(path)));
                return false;
            }

            found.push_back(path);
            return true;
        }

        if (!std::filesystem::is_directory(path, error)) {
            this->error(std::format("there's no file or folder {}", to_utf8(path)));
            return false;
        }

        auto entry = std::filesystem::recursive_directory_iterator(path, error);
        for (; !error && entry != std::filesystem::recursive_directory_iterator();
             entry.increment(error)) {
            const std::filesystem::path& at = entry->path();
            const std::string name = to_utf8(at.filename());
            if (entry->is_directory(error)) {
                const bool build = name == "build" && std::filesystem::is_regular_file(
                                                          at.parent_path() / "project.toml", error);
                if (name.starts_with('.') || build) {
                    entry.disable_recursion_pending();
                }

                continue;
            }

            if (at.extension() == ".fb" && entry->is_regular_file(error)) {
                found.push_back(at);
            }
        }

        if (error) {
            this->error(
                std::format("can't read the folder {}: {}", to_utf8(path), error.message()));
            return false;
        }

        return true;
    }

    /// The width of the package `file` is in, from its project.toml, or nullopt after its
    /// errors, which are reported.
    std::optional<std::uint32_t> width_for(const std::filesystem::path& file) {
        std::error_code error;
        const std::optional<std::filesystem::path> package =
            find_package(std::filesystem::absolute(file, error).parent_path());
        if (!package) {
            return default_format_width;
        }

        if (const auto known = m_widths.find(*package); known != m_widths.end()) {
            return known->second;
        }

        source_manager_t sources;
        const std::expected<file_id_t, std::string> loaded =
            sources.load(*package / "project.toml");
        std::optional<std::uint32_t> width;
        if (!loaded) {
            this->error(loaded.error());
        } else {
            diagnostic_bag_t diagnostics;
            const std::optional<manifest_t> manifest =
                read_manifest(sources.get(*loaded), diagnostics);
            // Its warnings too, once for the package, as a key it doesn't use may be a misspelled
            // width.
            report(diagnostics.diagnostics(), sources);
            if (manifest) {
                width = manifest->format_width.value_or(default_format_width);
            }
        }

        m_widths.emplace(*package, width);
        return width;
    }

    void report(const std::vector<diagnostic_t>& diagnostics,
                const source_manager_t& sources) const {
        for (const diagnostic_t& diagnostic : diagnostics) {
            write(stderr, render_pretty(diagnostic, sources, { .color = m_err.color }));
        }
    }

    void format_file(const std::filesystem::path& path) {
        const std::string shown = to_utf8(path);
        const std::optional<std::uint32_t> width = width_for(path);
        const std::optional<std::string> bytes = read_bytes(path);
        if (!width || !bytes) {
            if (!bytes) {
                error(std::format("can't read {}", shown));
            }

            ++m_failed;
            return;
        }

        source_manager_t sources;
        const source_file_t& file = sources.get(sources.add(shown, *bytes));
        const std::expected<std::string, std::vector<diagnostic_t>> formatted =
            format_source(file, *width);
        if (!formatted) {
            report(formatted.error(), sources);
            ++m_failed;
            return;
        }

        if (*formatted == *bytes) {
            return;
        }

        ++m_changed;
        if (m_options.check) {
            write(stdout, shown + "\n");
            return;
        }

        // Opening for writing truncates the file.
        std::ofstream out(path, std::ios::binary);
        out << *formatted;
        out.close();
        if (!out) {
            error(std::format("can't write {}", shown));
            ++m_failed;
            return;
        }

        status("Formatted", shown);
    }
};

} // namespace

int format_command(const options_t& options) {
    return formatting_t(options).run();
}

} // namespace fb
