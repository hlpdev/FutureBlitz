#include "fb/manifest.hpp"

#include "fb/token.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace fb {
namespace {

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

/// Whether text is three numbers with dots between, like "0.1.0".
bool is_version(const std::string_view text) {
    std::size_t numbers = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = std::min(text.find('.', begin), text.size());
        const std::string_view number = text.substr(begin, end - begin);
        if (number.empty() ||
            !std::ranges::all_of(number, [](const char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }

        ++numbers;
        begin = end + 1;
    }

    return numbers == 3;
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
            } else {
                m_diagnostics.warning(span_of(key->source()), "project.toml doesn't use '{}'",
                                      name);
            }
        }

        if (!project) {
            fail({ .file = m_file.id() },
                 "project.toml needs a [project] table, with the package's name and version");
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
                if (text && !is_identifier(*text)) {
                    fail(manifest.name_span,
                         "a package's name is an identifier, like game or my_physics, not \"{}\"",
                         *text);
                } else if (text && *text == "std") {
                    fail(manifest.name_span, "std is the standard library's name");
                } else if (text) {
                    manifest.name = *text;
                }
            } else if (name == "version") {
                versioned = true;
                const std::optional<std::string> text = string_of(field, value, "\"0.1.0\"");
                if (text && !is_version(*text)) {
                    fail(span_of(value.source()),
                         R"(a package's version is three numbers, like "0.1.0", not "{}")", *text);
                } else if (text) {
                    manifest.version = *text;
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

    /// A path as project.toml writes it, with each '\\' as '/', which an Import's path takes as
    /// one too, so a project.toml written on Windows means the same everywhere.
    static std::string with_slashes(std::string path) {
        std::ranges::replace(path, '\\', '/');
        return path;
    }

    /// Whether a path is a .fb file in the package's folder: relative, without "..".
    static bool is_package_file(const std::string_view path) {
        if (!path.ends_with(".fb") || path.starts_with('/') || path.starts_with('\\') ||
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

            if (value.is_string()) {
                fail(span_of(value.source()),
                     "versioned dependencies come with the package manager in M8; give the "
                     "package's folder, like {} = {{ path = \"../{}\" }}",
                     name, name);
                continue;
            }

            const toml::table* details = value.as_table();
            if (details == nullptr) {
                fail(span_of(value.source()),
                     "a dependency is its package's folder, like {} = {{ path = \"../{}\" }}", name,
                     name);
                continue;
            }

            std::optional<std::string> path;
            span_t path_span = name_span;
            bool written = false;
            for (const auto& [detail, setting] : in_order(*details)) {
                if (detail->str() == "path") {
                    written = true;
                    path = string_of(*detail, *setting, "\"../physics\"");
                    path_span = span_of(setting->source());
                } else {
                    m_diagnostics.warning(span_of(detail->source()),
                                          "a dependency doesn't use '{}'", detail->str());
                }
            }

            // A path that isn't text is reported already.
            if (!path && !written) {
                fail(span_of(value.source()),
                     "a dependency needs its package's folder, like {} = {{ path = \"../{}\" }}",
                     name, name);
                continue;
            }

            if (!path) {
                continue;
            }

            manifest.dependencies.push_back({
                .name = std::string(name),
                .name_span = name_span,
                .path = with_slashes(std::move(*path)),
                .path_span = path_span,
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

bool is_library_name(const std::string_view text) {
    return !text.empty() && !text.starts_with('-') && std::ranges::all_of(text, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '+';
    });
}

} // namespace fb
