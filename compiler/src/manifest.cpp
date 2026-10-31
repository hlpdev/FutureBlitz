#include "fb/manifest.hpp"

#include "fb/token.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The line widths [format] takes.
constexpr std::int64_t min_format_width = 40;
constexpr std::int64_t max_format_width = 1000;

/// Whether text could be an identifier: ASCII letters, digits, and '_', not starting with a
/// digit, and not a keyword.
bool is_identifier(const std::string_view text) {
    const auto letter = [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto letter_or_digit = [&](const char c) { return letter(c) || (c >= '0' && c <= '9'); };
    return !text.empty() && letter(text.front()) && std::ranges::all_of(text, letter_or_digit) &&
           !keyword_kind(text);
}

/// The most keywords a package can have, and the longest one.
constexpr std::size_t most_keywords = 5;
constexpr std::size_t longest_keyword = 32;

/// Whether a keyword is lowercase ASCII letters, digits, and '-', not starting with '-'.
bool is_keyword(const std::string_view text) {
    return !text.empty() && text.size() <= longest_keyword && text.front() != '-' &&
           std::ranges::all_of(text, [](const char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}

/// A table's keys and values in the order the file has them, which toml++ doesn't keep.
std::vector<std::pair<const toml::key*, const toml::node*>> in_order(const toml::table& table) {
    std::vector<std::pair<const toml::key*, const toml::node*>> entries;
    for (const auto& [key, node] : table) {
        entries.emplace_back(&key, &node);
    }

    std::ranges::sort(entries, {}, [](const auto& entry) {
        const toml::source_position at = entry.first->source().begin;
        return std::pair(at.line, at.column);
    });
    return entries;
}

/// Reads one project.toml into a manifest, reporting at the file's lines.
class reader_t {
public:
    reader_t(const source_file_t& file, diagnostic_bag_t& diagnostics)
        : m_file(file), m_diagnostics(diagnostics) {}

    std::optional<manifest_t> read() {
        toml::parse_result parsed = toml::parse(m_file.text(), m_file.path());
        if (!parsed) {
            const toml::parse_error& error = parsed.error();
            m_diagnostics.error(error_span(error), "project.toml isn't valid TOML: {}",
                                error.description());
            return std::nullopt;
        }

        const toml::table& root = parsed.table();
        manifest_t manifest;
        bool project = false;
        for (const auto& [key, node] : in_order(root)) {
            const std::string_view name = key->str();
            if (name == "project") {
                project = true;
                read_project(*key, *node, manifest);
            } else if (name == "dependencies") {
                read_dependencies(*key, *node, manifest);
            } else if (name == "native") {
                read_natives(*key, *node, manifest);
            } else if (name == "format") {
                read_format(*key, *node, manifest);
            } else if (name == "build") {
                read_build(*key, *node, manifest);
            } else if (name == "registries") {
                read_registries(*key, *node, manifest);
            } else {
                m_diagnostics.warning(span_of(key->source()), "project.toml doesn't use '{}'",
                                      name);
            }
        }

        if (!project) {
            fail({ .file = m_file.id() },
                 "project.toml needs a [project] table, with the package's name and version");
        }

        // [dependencies] can come after the [build] that names one of them.
        const build_step_t step = manifest.build.value_or(build_step_t{});
        if (step.package && step.package_span &&
            std::ranges::find(manifest.dependencies, *step.package, &dependency_t::name) ==
                manifest.dependencies.end()) {
            fail(*step.package_span,
                 "[build]'s package is one of [dependencies], whose program the build step is, "
                 "and there's no {} there",
                 *step.package);
        }

        // [registries] can come after the dependencies that name its registries.
        for (const dependency_t& dependency : manifest.dependencies) {
            if (dependency.registry && *dependency.registry != default_registry &&
                std::ranges::find(manifest.registries, *dependency.registry,
                                  &registry_entry_t::name) == manifest.registries.end()) {
                fail(dependency.registry_span.value_or(dependency.name_span),
                     "there's no registry '{}' in [registries], which names each by its index, "
                     "like {} = {{ index = \"https://packages.example.com/index/\" }}",
                     *dependency.registry, *dependency.registry);
            }
        }

        if (m_failed) {
            return std::nullopt;
        }

        return manifest;
    }

private:
    const source_file_t& m_file;
    diagnostic_bag_t& m_diagnostics;
    bool m_failed = false;

    template <typename... Args>
    void fail(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        m_diagnostics.error(span, fmt, std::forward<Args>(args)...);
        m_failed = true;
    }

    /// Where toml++ found the file wrong, but for text that isn't UTF-8, which it places at the
    /// character before, the first byte that isn't.
    [[nodiscard]] span_t error_span(const toml::parse_error& error) const {
        const std::string_view text = m_file.text();
        if (!error.description().contains("utf-8")) {
            return span_of(error.source());
        }

        for (std::size_t at = 0; at < text.size();) {
            const decoded_t decoded = decode_utf8(text, at);
            if (!decoded.valid()) {
                const auto begin = static_cast<std::uint32_t>(at);
                return { .file = m_file.id(), .begin = begin, .end = begin + 1 };
            }

            at += decoded.length;
        }

        return span_of(error.source());
    }

    /// Where a part of the file is. toml++ counts lines and columns from 1, in code points.
    [[nodiscard]] span_t span_of(const toml::source_region& region) const {
        const auto offset = [&](const toml::source_position position) {
            const location_t location{
                .line = position.line > 0 ? position.line - 1 : 0,
                .column = position.column > 0 ? position.column - 1 : 0,
            };
            return m_file.offset_of(location, column_unit_t::CODEPOINT);
        };
        const std::uint32_t begin = offset(region.begin);
        return { .file = m_file.id(), .begin = begin, .end = std::max(begin, offset(region.end)) };
    }

    /// Where the key at `begin` starts with the dotted keys before it, as `engine.` in
    /// `engine.path`, quoted or not, with spaces around the dots, which toml++ leaves out of the
    /// last key's place.
    [[nodiscard]] std::uint32_t dotted_start(const std::uint32_t begin) const {
        const std::string_view text = m_file.text();
        const auto blank = [&](const std::size_t at) {
            return text[at] == ' ' || text[at] == '\t';
        };
        std::size_t start = begin;
        while (true) {
            std::size_t at = start;
            while (at > 0 && blank(at - 1)) {
                --at;
            }

            if (at == 0 || text[at - 1] != '.') {
                return static_cast<std::uint32_t>(start);
            }

            --at;
            while (at > 0 && blank(at - 1)) {
                --at;
            }

            // The key before the dot: quoted, as "engine" or 'engine', or bare.
            if (at > 0 && (text[at - 1] == '"' || text[at - 1] == '\'')) {
                const std::size_t open = text.rfind(text[at - 1], at - 2);
                if (at < 2 || open == std::string_view::npos) {
                    return static_cast<std::uint32_t>(start);
                }

                at = open;
            } else {
                const std::size_t end = at;
                while (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0 ||
                                  text[at - 1] == '_' || text[at - 1] == '-')) {
                    --at;
                }

                if (at == end) {
                    return static_cast<std::uint32_t>(start);
                }
            }

            start = at;
        }
    }

    /// A key's string value, or nullopt after reporting that it isn't one.
    std::optional<std::string> string_of(const toml::key& key, const toml::node& node,
                                         const std::string_view example) {
        if (const std::optional<std::string> text = node.value<std::string>()) {
            return text;
        }

        fail(span_of(node.source()), "'{}' is text, like {}", key.str(), example);
        return std::nullopt;
    }

    void read_project(const toml::key& key, const toml::node& node, manifest_t& manifest) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            fail(span_of(key.source()), "'project' is a table, [project], with the package's name "
                                        "and version");
            return;
        }

        bool named = false;
        bool versioned = false;
        for (const auto& [key_of_field, node_of_field] : in_order(*table)) {
            const toml::key& field = *key_of_field;
            const toml::node& value = *node_of_field;
            const std::string_view name = field.str();
            if (name == "name") {
                named = true;
                const std::optional<std::string> text = string_of(field, value, "\"game\"");
                manifest.name_span = span_of(value.source());
                const std::optional<std::string> wrong =
                    text ? package_name_error(*text) : std::nullopt;
                if (wrong) {
                    fail(manifest.name_span, "{}", *wrong);
                } else if (text) {
                    manifest.name = *text;
                }
            } else if (name == "version") {
                versioned = true;
                const std::optional<std::string> text = string_of(field, value, "\"0.1.0\"");
                if (text && !parse_version(*text)) {
                    fail(span_of(value.source()),
                         R"(a package's version is three numbers, like "0.1.0", and maybe a )"
                         R"(pre-release, like "1.0.0-beta.1", not "{}")",
                         *text);
                } else if (text) {
                    manifest.version = *text;
                }
            } else if (name == "description") {
                manifest.description = string_of(field, value, "\"A physics engine\"");
            } else if (name == "license") {
                manifest.license = string_of(field, value, "\"MIT OR Apache-2.0\"");
                const std::optional<std::string> wrong =
                    manifest.license ? license_error(*manifest.license) : std::nullopt;
                if (wrong) {
                    fail(span_of(value.source()), "{}", *wrong);
                }
            } else if (name == "readme" || name == "license_file") {
                std::optional<std::string> file =
                    string_of(field, value, name == "readme" ? "\"README.md\"" : "\"LICENSE-MIT\"");
                if (file && !is_inside(*file)) {
                    fail(span_of(value.source()),
                         R"('{}' is a file in the package's folder, like "{}", not "{}")", name,
                         name == "readme" ? "README.md" : "LICENSE-MIT", *file);
                    file.reset();
                } else if (file) {
                    file = with_slashes(std::move(*file));
                }

                (name == "readme" ? manifest.readme : manifest.license_file) = std::move(file);
            } else if (name == "repository" || name == "homepage" || name == "documentation") {
                std::optional<std::string> url =
                    string_of(field, value, "\"https://github.com/me/physics\"");
                if (url && !url->starts_with("https://")) {
                    fail(span_of(value.source()), "'{}' is an https:// URL, not \"{}\"", name,
                         *url);
                }

                if (name == "repository") {
                    manifest.repository = std::move(url);
                } else if (name == "homepage") {
                    manifest.homepage = std::move(url);
                } else {
                    manifest.documentation = std::move(url);
                }
            } else if (name == "keywords") {
                read_keywords(value, manifest);
            } else if (name == "fb") {
                const std::optional<std::string> text = string_of(field, value, "\">=0.2\"");
                const std::expected<version_range_t, std::string> range =
                    text ? parse_range(*text) : std::unexpected(std::string());
                if (text && !range) {
                    fail(span_of(value.source()), "{}", range.error());
                } else if (range) {
                    manifest.fb = *range;
                }
            } else if (name == "type") {
                const std::optional<std::string> text = string_of(field, value, "\"library\"");
                manifest.type_span = span_of(value.source());
                if (text == "executable") {
                    manifest.type = package_type_t::EXECUTABLE;
                } else if (text == "library") {
                    manifest.type = package_type_t::LIBRARY;
                } else if (text) {
                    fail(*manifest.type_span,
                         R"(a package's type is "executable" or "library", not "{}")", *text);
                }
            } else if (name == "main") {
                const std::optional<std::string> text = string_of(field, value, "\"main.fb\"");
                manifest.main_span = span_of(value.source());
                if (text && !is_package_file(*text)) {
                    fail(*manifest.main_span,
                         "'main' is a .fb file in the package's folder, like \"main.fb\" or "
                         "\"src/game.fb\", not \"{}\"",
                         *text);
                } else if (text) {
                    manifest.main = with_slashes(*text);
                }
            } else if (name == "build") {
                fail(span_of(field.source()),
                     "a build step is a table of its own, [build], with the program it runs, "
                     "like program = \"tools/generate.fb\"");
            } else {
                m_diagnostics.warning(span_of(field.source()), "[project] doesn't use '{}'", name);
            }
        }

        if (!named) {
            fail(span_of(key.source()), "[project] needs the package's name, like name = \"game\"");
        }

        if (!versioned) {
            fail(span_of(key.source()), "[project] needs a version, like version = \"0.1.0\"");
        }

        if (manifest.type == package_type_t::LIBRARY && manifest.main_span) {
            fail(*manifest.main_span, "a library has no main file; its files are its modules");
        }
    }

    /// [format]: how `fb fmt` formats the package's code, which is only its line width.
    void read_format(const toml::key& key, const toml::node& node, manifest_t& manifest) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            fail(span_of(key.source()), "'format' is a table, [format], with the line width");
            return;
        }

        for (const auto& [key_of_field, node_of_field] : in_order(*table)) {
            const toml::key& field = *key_of_field;
            const toml::node& value = *node_of_field;
            if (field.str() != "width") {
                m_diagnostics.warning(span_of(field.source()), "[format] doesn't use '{}'",
                                      field.str());
                continue;
            }

            const std::optional<std::int64_t> width = value.value_exact<std::int64_t>();
            if (!width || *width < min_format_width || *width > max_format_width) {
                fail(span_of(value.source()),
                     "[format]'s width is a whole number of columns from {} to {}, like 100",
                     min_format_width, max_format_width);
                continue;
            }

            manifest.format_width = static_cast<std::uint32_t>(*width);
        }
    }

    /// [build]: the program a build step runs, the package's own or a dependency's.
    void read_build(const toml::key& key, const toml::node& node, manifest_t& manifest) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            fail(span_of(key.source()),
                 "'build' is a table, [build], with the program its build step runs");
            return;
        }

        build_step_t step;
        bool named = false;
        bool failed = false;
        for (const auto& [key_of_field, node_of_field] : in_order(*table)) {
            const toml::key& field = *key_of_field;
            const toml::node& value = *node_of_field;
            if (field.str() == "program") {
                named = true;
                const std::optional<std::string> text =
                    string_of(field, value, "\"tools/generate.fb\"");
                step.program_span = span_of(value.source());
                if (text && !is_package_file(*text)) {
                    fail(step.program_span,
                         "[build]'s program is a .fb file in its package's folder, like "
                         "\"tools/generate.fb\", not \"{}\"",
                         *text);
                }

                failed = failed || !text || !is_package_file(*text);
                step.program = with_slashes(text.value_or(""));
            } else if (field.str() == "package") {
                const std::optional<std::string> text = string_of(field, value, "\"tools\"");
                step.package_span = span_of(value.source());
                failed = failed || !text;
                step.package = text;
            } else {
                m_diagnostics.warning(span_of(field.source()), "[build] doesn't use '{}'",
                                      field.str());
            }
        }

        if (!named) {
            fail(span_of(key.source()),
                 "[build] needs the program its build step runs, like program = "
                 "\"tools/generate.fb\"");
            return;
        }

        if (!failed) {
            manifest.build = std::move(step);
        }
    }

    /// A path as project.toml writes it, with each '\\' as '/', which an Import's path takes as
    /// one too, so a project.toml written on Windows means the same everywhere.
    static std::string with_slashes(std::string path) {
        std::ranges::replace(path, '\\', '/');
        return path;
    }

    /// Whether a path is a .fb file in the package's folder: relative, without "..".
    static bool is_package_file(const std::string_view path) {
        return path.ends_with(".fb") && is_inside(path);
    }

    /// [project] keywords: at most five, each lowercase letters, digits, and '-'.
    void read_keywords(const toml::node& value, manifest_t& manifest) {
        const toml::array* keywords = value.as_array();
        const std::string_view like = R"(like keywords = ["physics", "2d"])";
        if (keywords == nullptr) {
            fail(span_of(value.source()), "'keywords' is a list of words, {}", like);
            return;
        }

        if (keywords->size() > most_keywords) {
            fail(span_of(value.source()), "a package has at most {} keywords", most_keywords);
            return;
        }

        for (const toml::node& keyword : *keywords) {
            const std::optional<std::string> text = keyword.value<std::string>();
            if (!text || !is_keyword(*text)) {
                fail(span_of(keyword.source()),
                     "a keyword is lowercase letters, digits, and '-', not starting with '-', at "
                     "most {} long, {}",
                     longest_keyword, like);
                continue;
            }

            manifest.keywords.push_back(*text);
        }
    }

    /// Whether a path is in the package's folder: relative, without "..".
    static bool is_inside(const std::string_view path) {
        if (path.empty() || path.starts_with('/') || path.starts_with('\\') ||
            (path.size() > 1 && path[1] == ':')) {
            return false;
        }

        std::size_t begin = 0;
        while (begin <= path.size()) {
            const std::size_t end = std::min(path.find_first_of("/\\", begin), path.size());
            if (path.substr(begin, end - begin) == "..") {
                return false;
            }

            begin = end + 1;
        }

        return true;
    }

    void read_dependencies(const toml::key& key, const toml::node& node, manifest_t& manifest) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            fail(span_of(key.source()),
                 "'dependencies' is a table, [dependencies], of packages by name");
            return;
        }

        for (const auto& [key_of_field, node_of_field] : in_order(*table)) {
            const toml::key& field = *key_of_field;
            const toml::node& value = *node_of_field;
            const std::string_view name = field.str();
            const span_t name_span = span_of(field.source());
            if (!is_identifier(name)) {
                fail(name_span, "a dependency's name is an identifier, like physics, not \"{}\"",
                     name);
                continue;
            }

            if (name == "std") {
                fail(name_span, "std is the standard library's name");
                continue;
            }

            const std::string like = std::format(
                R"(like {} = "1.2", or a local package's folder, like {} = {{ path = "../{}" }})",
                name, name, name);
            dependency_t dependency{ .name = std::string(name), .name_span = name_span };
            if (const std::optional<std::string> text = value.value<std::string>()) {
                dependency.range_span = span_of(value.source());
                if (read_range(*text, dependency)) {
                    manifest.dependencies.push_back(std::move(dependency));
                }

                continue;
            }

            const toml::table* details = value.as_table();
            if (details == nullptr) {
                fail(span_of(value.source()), "a dependency is a range of a package's versions, {}",
                     like);
                continue;
            }

            std::optional<std::string> path;
            std::optional<std::string> version;
            bool written = false;
            bool failed = false;
            bool beside = false;
            std::optional<span_t> link_span;
            for (const auto& [detail, setting] : in_order(*details)) {
                if (detail->str() == "path") {
                    written = true;
                    path = string_of(*detail, *setting, "\"../physics\"");
                    dependency.path_span = span_of(setting->source());
                    dependency.path_entry_span = {
                        .file = m_file.id(),
                        .begin = dotted_start(span_of(detail->source()).begin),
                        .end = dependency.path_span.end,
                    };
                    failed = failed || !path;
                } else if (detail->str() == "version") {
                    written = true;
                    version = string_of(*detail, *setting, "\"1.2\"");
                    dependency.range_span = span_of(setting->source());
                    failed = failed || !version;
                } else if (detail->str() == "registry") {
                    dependency.registry = string_of(*detail, *setting, "\"company\"");
                    dependency.registry_span = span_of(setting->source());
                    failed = failed || !dependency.registry;
                } else if (detail->str() == "link") {
                    const std::optional<std::string> text =
                        string_of(*detail, *setting, "\"beside\"");
                    link_span = span_of(setting->source());
                    beside = text == "beside";
                    if (text && *text != "embed" && *text != "beside") {
                        fail(*link_span,
                             R"(a dependency's link is "embed", into the executable, or "beside", )"
                             R"(a file of its own beside it, not "{}")",
                             *text);
                    }
                } else {
                    m_diagnostics.warning(span_of(detail->source()),
                                          "a dependency doesn't use '{}'", detail->str());
                }
            }

            // A path or version that isn't text is reported already.
            if (!written) {
                fail(span_of(value.source()), "a dependency needs a range of versions, {}", like);
                continue;
            }

            if (failed) {
                continue;
            }

            dependency.beside = beside;
            dependency.link_span = link_span;
            // With a version too, a local package is published, to the registry it names.
            if (path && !version && dependency.registry) {
                fail(*dependency.registry_span,
                     "a local package, by its path, isn't from a registry, so it takes no "
                     "'registry' without a 'version' too, which a published package depends on");
                continue;
            }

            if (path) {
                dependency.path = with_slashes(std::move(*path));
            }

            if (!version || read_range(*version, dependency)) {
                manifest.dependencies.push_back(std::move(dependency));
            }
        }
    }

    /// A dependency's range, into `dependency`, whose range_span is where it's written. False
    /// after an error.
    bool read_range(const std::string_view text, dependency_t& dependency) {
        std::expected<version_range_t, std::string> range = parse_range(text);
        if (!range) {
            fail(dependency.range_span, "{}", range.error());
            return false;
        }

        dependency.range = std::move(*range);
        return true;
    }

    /// [registries]: each by its name, as `company = { index = "https://..." }`.
    void read_registries(const toml::key& key, const toml::node& node, manifest_t& manifest) {
        const toml::table* table = node.as_table();
        const std::string_view like =
            R"(like company = { index = "https://packages.example.com/index/" })";
        if (table == nullptr) {
            fail(span_of(key.source()), "'registries' is a table, [registries], of registries "
                                        "by name");
            return;
        }

        for (const auto& [key_of_field, node_of_field] : in_order(*table)) {
            const toml::key& field = *key_of_field;
            const toml::node& value = *node_of_field;
            const std::string_view name = field.str();
            if (!is_identifier(name)) {
                fail(span_of(field.source()),
                     "a registry's name is an identifier, like company, not \"{}\"", name);
                continue;
            }

            const toml::table* details = value.as_table();
            if (details == nullptr) {
                fail(span_of(value.source()), "a registry is its index, {}", like);
                continue;
            }

            std::optional<std::string> index;
            span_t index_span = span_of(value.source());
            for (const auto& [detail, setting] : in_order(*details)) {
                if (detail->str() != "index") {
                    m_diagnostics.warning(span_of(detail->source()), "a registry doesn't use '{}'",
                                          detail->str());
                    continue;
                }

                index = string_of(*detail, *setting, "\"https://packages.example.com/index/\"");
                index_span = span_of(setting->source());
                if (!index) {
                    break;
                }
            }

            if (!index) {
                if (!details->contains("index")) {
                    fail(span_of(value.source()), "a registry needs its index, {}", like);
                }

                continue;
            }

            if (index->empty()) {
                fail(index_span, "a registry's index can't be empty");
                continue;
            }

            std::expected<std::string, std::string> schemed = with_index_scheme(*index);
            if (!schemed) {
                fail(index_span, "{}", schemed.error());
                continue;
            }

            index = std::move(*schemed);
            if (index->starts_with("http://") && !is_loopback_url(*index)) {
                fail(index_span,
                     "a registry's index is https://, or http:// only on this machine "
                     "(localhost, 127.0.0.1, or [::1]), so what's downloaded can't be changed on "
                     "the way");
                continue;
            }

            const bool local = is_local_index(*index);
            manifest.registries.push_back({
                .name = std::string(name),
                .index = local && !index->starts_with("file:") ? with_slashes(std::move(*index))
                                                               : std::move(*index),
                .index_span = index_span,
            });
        }
    }

    /// [native]: each library by its name, a static archive, as `x = "libs/x"` or
    /// `x = { static = "libs/x" }`, a system library, as `x = { system = "x" }`, or a dynamic
    /// one, as `x = { dynamic = "native/{target}/x" }`.
    void read_natives(const toml::key& key, const toml::node& node, manifest_t& manifest) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            fail(span_of(key.source()), "'native' is a table, [native], of C libraries by name");
            return;
        }

        for (const auto& [key_of_field, node_of_field] : in_order(*table)) {
            const toml::key& field = *key_of_field;
            const toml::node& value = *node_of_field;
            const std::string_view name = field.str();
            const span_t name_span = span_of(field.source());
            if (!is_library_name(name)) {
                fail(name_span,
                     "a native library's name is letters, digits, and '_', '-', '.', or '+', not "
                     "starting with '-', like joltc, not \"{}\"",
                     name);
                continue;
            }

            native_t native{ .name = std::string(name), .name_span = name_span };
            if (const std::optional<std::string> path = value.value<std::string>()) {
                native.path = *path;
                native.path_span = span_of(value.source());
            } else if (!read_native(name, value, native)) {
                continue;
            }

            if (native.path.empty()) {
                fail(native.path_span, "a native library's path or name can't be empty");
                continue;
            }

            if (native.kind == native_kind_t::SYSTEM && !is_library_name(native.path)) {
                fail(native.path_span,
                     "a system library is the name the linker takes, like system = \"m\" for "
                     "-lm, not \"{}\"",
                     native.path);
                continue;
            }

            if (native.kind != native_kind_t::SYSTEM) {
                native.path = with_slashes(std::move(native.path));
                if (!has_only_target(native.path)) {
                    fail(native.path_span,
                         "a native library's path takes {{target}} for the build's target, as in "
                         "\"native/{{target}}/{}\", and no other {{...}}",
                         name);
                    continue;
                }
            }

            manifest.natives.push_back(std::move(native));
        }
    }

    /// Whether a native library's path has no '{' or '}' but those of "{target}"s.
    static bool has_only_target(const std::string_view path) {
        std::string rest(path);
        for (std::size_t at = rest.find("{target}"); at != std::string::npos;
             at = rest.find("{target}", at)) {
            rest.erase(at, std::string_view("{target}").size());
        }

        return !rest.contains('{') && !rest.contains('}');
    }

    /// A native library's table, `{ static = "libs/x" }`, `{ system = "x" }`, or
    /// `{ dynamic = "native/{target}/x" }`. False after an error.
    bool read_native(const std::string_view name, const toml::node& value, native_t& native) {
        const std::string example = std::format(
            R"({} = "libs/{}" for a static archive, {} = {{ system = "{}" }}, or {} = {{ dynamic )"
            R"(= "native/{{target}}/{}" }})",
            name, name, name, name, name, name);
        const toml::table* details = value.as_table();
        if (details == nullptr) {
            fail(span_of(value.source()), "a native library is like {}", example);
            return false;
        }

        std::optional<std::string_view> kind;
        bool failed = false;
        for (const auto& [detail, setting] : in_order(*details)) {
            const std::string_view written = detail->str();
            if (written != "static" && written != "system" && written != "dynamic") {
                m_diagnostics.warning(span_of(detail->source()),
                                      "a native library doesn't use '{}'", written);
                continue;
            }

            if (kind) {
                fail(span_of(detail->source()), "a native library is {} or {}, not both", *kind,
                     written);
                failed = true;
                continue;
            }

            kind = written;
            native.kind = native_kind_t::SYSTEM;
            std::string_view like = "\"m\"";
            if (written == "static") {
                native.kind = native_kind_t::STATIC;
                like = "\"libs/x\"";
            } else if (written == "dynamic") {
                native.kind = native_kind_t::DYNAMIC;
                like = "\"native/{target}/x\"";
            }

            const std::optional<std::string> text = string_of(*detail, *setting, like);
            if (!text) {
                failed = true;
                continue;
            }

            native.path = *text;
            native.path_span = span_of(setting->source());
        }

        if (!kind && !failed) {
            fail(span_of(value.source()), "a native library is like {}", example);
            return false;
        }

        return !failed;
    }
};

} // namespace

std::optional<manifest_t> read_manifest(const source_file_t& file, diagnostic_bag_t& diagnostics) {
    return reader_t(file, diagnostics).read();
}

std::optional<std::string> published_manifest(const source_file_t& file,
                                              const manifest_t& manifest) {
    std::vector<span_t> paths;
    for (const dependency_t& dependency : manifest.dependencies) {
        if (!dependency.path.empty() && dependency.range) {
            paths.push_back(dependency.path_entry_span);
        }
    }

    // From the last, so the spans before stay where they are.
    std::ranges::sort(paths, std::ranges::greater{}, &span_t::begin);
    std::string text(file.text());
    const auto blank = [](const char c) { return c == ' ' || c == '\t' || c == '\r'; };
    for (const span_t& path : paths) {
        const std::size_t line =
            path.begin == 0 ? std::string::npos : text.rfind('\n', path.begin - 1);
        const std::size_t line_begin = line == std::string::npos ? 0 : line + 1;
        const std::size_t line_end = std::min(text.find('\n', path.end), text.size());
        const std::string_view before =
            std::string_view(text).substr(line_begin, path.begin - line_begin);
        std::size_t after = path.end;
        while (after < line_end && blank(text[after])) {
            ++after;
        }

        // A line of its own, in a dependency's table, or a dotted key's, like `physics.path`.
        if ((after == line_end || text[after] == '#') &&
            std::ranges::all_of(before, [&](const char c) {
                return blank(c) || c == '.' || c == '_' || c == '-' || c == '"' || c == '\'' ||
                       (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            })) {
            text.erase(line_begin, std::min(line_end + 1, text.size()) - line_begin);
            continue;
        }

        // In an inline table, with the comma after it, or the one before it when it's last.
        if (after < line_end && text[after] == ',') {
            ++after;
            while (after < line_end && blank(text[after])) {
                ++after;
            }

            text.erase(path.begin, after - path.begin);
            continue;
        }

        std::size_t begin = path.begin;
        while (begin > line_begin && blank(text[begin - 1])) {
            --begin;
        }

        if (begin == line_begin || text[begin - 1] != ',') {
            return std::nullopt;
        }

        --begin;
        while (begin > line_begin && blank(text[begin - 1])) {
            --begin;
        }

        text.erase(begin, path.end - begin);
    }

    // It has to read as the same manifest, without those paths.
    const source_file_t published(file.id(), file.path(), text);
    diagnostic_bag_t diagnostics;
    const std::optional<manifest_t> read = read_manifest(published, diagnostics);
    if (!read || read->dependencies.size() != manifest.dependencies.size()) {
        return std::nullopt;
    }

    const auto range_of = [](const dependency_t& dependency) {
        return dependency.range ? std::optional(dependency.range->text) : std::nullopt;
    };
    for (std::size_t i = 0; i < manifest.dependencies.size(); ++i) {
        const dependency_t& was = manifest.dependencies[i];
        const dependency_t& now = read->dependencies[i];
        if (now.name != was.name || now.path != (was.range ? std::string() : was.path) ||
            range_of(now) != range_of(was) || now.registry != was.registry ||
            now.beside != was.beside) {
            return std::nullopt;
        }
    }

    return text;
}

std::optional<std::string> package_name_error(const std::string_view text) {
    if (keyword_kind(text)) {
        return std::format("{} is a keyword, which a package's name can't be", text);
    }

    if (!is_identifier(text)) {
        return std::format("a package's name is an identifier, like game or my_physics, not \"{}\"",
                           text);
    }

    if (text == "std") {
        return "std is the standard library's name";
    }

    return std::nullopt;
}

bool is_loopback_url(const std::string_view url) {
    if (!url.starts_with("http://")) {
        return false;
    }

    std::string_view rest = url.substr(std::string_view("http://").size());
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos) {
        rest = rest.substr(at + 1);
    }

    std::string_view host = rest;
    if (host.starts_with('[')) {
        host = host.substr(0, host.find(']') + 1);
    } else {
        host = host.substr(0, host.find(':'));
    }

    return host == "localhost" || host.ends_with(".localhost") || host == "127.0.0.1" ||
           host == "[::1]";
}

std::expected<std::string, std::string> with_index_scheme(const std::string_view index) {
    const std::size_t colon = index.find("://");
    const std::string_view scheme = index.substr(0, colon);
    const bool written = colon != std::string_view::npos && !scheme.empty() &&
                         std::ranges::all_of(scheme, [](const char c) {
                             return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                    (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
                         });
    if (!written) {
        return std::string(index);
    }

    std::string lower(scheme);
    std::ranges::transform(lower, lower.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (lower != "http" && lower != "https" && lower != "file") {
        return std::unexpected(std::format(
            "a registry's index is an https:// URL, an http:// one on this machine, or a folder, "
            "not a {}:// URL",
            scheme));
    }

    return lower + std::string(index.substr(colon));
}

bool is_local_index(const std::string_view index) {
    return !index.starts_with("https://") && !index.starts_with("http://");
}

std::optional<std::string> license_error(const std::string_view text) {
    // Its words: license names, AND, OR, WITH, and parentheses.
    std::vector<std::string_view> words;
    for (std::size_t at = 0; at < text.size();) {
        if (text[at] == ' ') {
            ++at;
        } else if (text[at] == '(' || text[at] == ')') {
            words.push_back(text.substr(at, 1));
            ++at;
        } else {
            const std::size_t end = std::min(text.find_first_of(" ()", at), text.size());
            words.push_back(text.substr(at, end - at));
            at = end;
        }
    }

    std::string wrong = std::format(
        R"(a license is an SPDX expression, like "MIT" or "MIT OR Apache-2.0", not "{}")", text);
    // A license's name: letters, digits, '.', and '-', and maybe a '+' after, or a
    // DocumentRef-x:LicenseRef-y.
    const auto is_name = [](std::string_view word) {
        if (word.ends_with('+')) {
            word.remove_suffix(1);
        }

        if (word.starts_with("DocumentRef-") && word.contains(':')) {
            word = word.substr(word.find(':') + 1);
        }

        return !word.empty() && word != "AND" && word != "OR" && word != "WITH" &&
               std::ranges::all_of(word, [](const char c) {
                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-';
               });
    };

    // expression = term ("OR" term)*, term = atom ("AND" atom)*, and atom = name ["WITH" name]
    // or "(" expression ")".
    std::size_t at = 0;
    const auto expression = [&](const auto& self) -> bool {
        const auto atom = [&] {
            if (at < words.size() && words[at] == "(") {
                ++at;
                if (!self(self) || at >= words.size() || words[at] != ")") {
                    return false;
                }

                ++at;
                return true;
            }

            if (at >= words.size() || !is_name(words[at])) {
                return false;
            }

            ++at;
            if (at < words.size() && words[at] == "WITH") {
                ++at;
                if (at >= words.size() || !is_name(words[at])) {
                    return false;
                }

                ++at;
            }

            return true;
        };
        const auto term = [&] {
            if (!atom()) {
                return false;
            }

            while (at < words.size() && words[at] == "AND") {
                ++at;
                if (!atom()) {
                    return false;
                }
            }

            return true;
        };
        if (!term()) {
            return false;
        }

        while (at < words.size() && words[at] == "OR") {
            ++at;
            if (!term()) {
                return false;
            }
        }

        return true;
    };

    if (!expression(expression) || at != words.size()) {
        return wrong;
    }

    return std::nullopt;
}

bool is_library_name(const std::string_view text) {
    return !text.empty() && !text.starts_with('-') && std::ranges::all_of(text, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '+';
    });
}

} // namespace fb
