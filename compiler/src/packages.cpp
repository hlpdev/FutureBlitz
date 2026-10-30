#include "fb/packages.hpp"

#include "fb/build_config.hpp"
#include "fb/diagnostic.hpp"
#include "fb/interface.hpp"
#include "fb/package_archive.hpp"
#include "fb/resolver.hpp"
#include "fb/source.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

std::optional<std::string> read_text(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::ostringstream bytes;
    bytes << in.rdbuf();
    return std::move(bytes).str();
}

/// The registry dependencies of a package and the local packages it depends on, and theirs.
struct wanted_t {
    std::vector<requirement_t> requirements;
    /// A project.toml among them has mistakes.
    bool broken = false;
};

/// Adds what the package in `folder` and its path dependencies ask for to `wanted`, reading each
/// folder once.
void collect(const std::filesystem::path& folder, std::set<std::filesystem::path>& seen,
             wanted_t& wanted) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(folder, error);
    if (!seen.insert(root).second) {
        return;
    }

    const std::optional<std::string> text = read_text(root / "project.toml");
    if (!text) {
        wanted.broken = true;
        return;
    }

    const source_file_t file(file_id_t{}, to_utf8(root / "project.toml"), *text);
    diagnostic_bag_t diagnostics;
    const std::optional<manifest_t> manifest = read_manifest(file, diagnostics);
    if (!manifest) {
        wanted.broken = true;
        return;
    }

    for (const dependency_t& dependency : manifest->dependencies) {
        if (!dependency.range) {
            collect(root / path_from_utf8(dependency.path), seen, wanted);
            continue;
        }

        const std::optional<std::string> source =
            registry_index(dependency.registry, manifest->registries, root);
        if (!source) {
            wanted.broken = true;
            continue;
        }

        wanted.requirements.push_back({
            .name = dependency.name,
            .range = *dependency.range,
            .source = *source,
            .who = manifest->name,
        });
    }
}

/// The locked packages `requirements` reach, by their names, or nullopt when fb.lock doesn't
/// have what they ask for: a package, from its registry, of a version in its range, and each
/// locked package's own dependencies.
std::optional<lockfile_t> locked_for(const std::vector<requirement_t>& requirements,
                                     const lockfile_t& lockfile) {
    std::set<std::string, std::less<>> reached;
    std::vector<std::string> pending;
    for (const requirement_t& requirement : requirements) {
        const locked_package_t* locked = lockfile.find(requirement.name);
        if (locked == nullptr || locked->source != requirement.source) {
            return std::nullopt;
        }

        const std::expected<version_t, std::string> version = parse_version(locked->version);
        if (!version || !requirement.range.matches(*version)) {
            return std::nullopt;
        }

        pending.push_back(requirement.name);
    }

    while (!pending.empty()) {
        const std::string name = std::move(pending.back());
        pending.pop_back();
        if (!reached.insert(name).second) {
            continue;
        }

        const locked_package_t* locked = lockfile.find(name);
        if (locked == nullptr) {
            return std::nullopt;
        }

        for (const std::string& dependency : locked->dependencies) {
            pending.push_back(dependency);
        }
    }

    lockfile_t out;
    for (const locked_package_t& package : lockfile.packages) {
        if (reached.contains(package.name)) {
            out.packages.push_back(package);
        }
    }

    return out;
}

/// fb.lock's packages from what the resolver picked.
lockfile_t lockfile_of(const std::vector<resolved_package_t>& resolved) {
    lockfile_t out;
    for (const resolved_package_t& package : resolved) {
        locked_package_t locked{
            .name = package.entry.name,
            .version = package.entry.version,
            .source = package.source,
            .checksum = package.entry.source.sha256,
        };
        for (const auto& [target, archive] : package.entry.targets) {
            locked.targets.emplace(target, archive.sha256);
        }

        for (const index_dependency_t& dependency : package.entry.dependencies) {
            locked.dependencies.push_back(dependency.name);
        }

        out.packages.push_back(std::move(locked));
    }

    std::ranges::sort(out.packages, {}, &locked_package_t::name);
    return out;
}

/// Says what changed between two fb.locks: what's added, updated, and removed.
void report_changes(const lockfile_t& before, const lockfile_t& after, const status_t& status) {
    if (!status) {
        return;
    }

    for (const locked_package_t& package : after.packages) {
        const locked_package_t* old = before.find(package.name);
        if (old == nullptr) {
            status("Adding", std::format("{} v{}", package.name, package.version));
        } else if (old->version != package.version || old->source != package.source) {
            status("Updating",
                   std::format("{} v{} -> v{}", package.name, old->version, package.version));
        }
    }

    for (const locked_package_t& package : before.packages) {
        if (after.find(package.name) == nullptr) {
            status("Removing", std::format("{} v{}", package.name, package.version));
        }
    }
}

/// The marker a whole unpacked archive leaves in its folder: "source" for a package's own, or
/// its target's name.
std::filesystem::path marker_of(const std::filesystem::path& folder, const std::string_view what) {
    return folder / path_from_utf8(std::format(".fbpkg-{}", what));
}

bool has_marker(const std::filesystem::path& folder, const std::string_view what,
                const std::string_view checksum) {
    return read_text(marker_of(folder, what)) == checksum;
}

/// Makes sure `locked` is unpacked in the cache, with its archive for `target`.
std::expected<void, std::string> fetch(const locked_package_t& locked,
                                       const std::string_view target, registry_t& registry) {
    const std::filesystem::path folder =
        package_cache_folder(locked.source, locked.name, locked.version);
    std::error_code error;
    if (!has_marker(folder, "source", locked.checksum)) {
        // A folder without its marker was left by something else, so it's made again.
        if (std::filesystem::exists(folder, error)) {
            std::filesystem::remove_all(folder, error);
        }

        const std::expected<std::string, std::string> bytes =
            registry.archive(locked.name, locked.version, archive_name(locked.name, locked.version),
                             locked.checksum);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }

        if (const std::expected<void, std::string> unpacked =
                unpack_archive(*bytes, folder, "source", locked.checksum, most_source_unpacked);
            !unpacked) {
            return std::unexpected(std::format("{} v{} couldn't be unpacked: {}", locked.name,
                                               locked.version, unpacked.error()));
        }
    }

    const auto own = locked.targets.find(target);
    if (own == locked.targets.end() || has_marker(folder, target, own->second)) {
        return {};
    }

    const std::expected<std::string, std::string> bytes =
        registry.archive(locked.name, locked.version,
                         archive_name(locked.name, locked.version, target), own->second);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }

    if (const std::expected<void, std::string> unpacked =
            unpack_archive(*bytes, folder, target, own->second, most_target_unpacked);
        !unpacked) {
        return std::unexpected(std::format("{} v{}'s {} archive couldn't be unpacked: {}",
                                           locked.name, locked.version, target, unpacked.error()));
    }

    return {};
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }

    return text;
}

/// A table header's name, like "dependencies" for "[dependencies] # ...", or nullopt for a line
/// that isn't one.
std::optional<std::string_view> header_of(const std::string_view line) {
    const std::string_view text = trim(line);
    if (!text.starts_with('[') || text.starts_with("[[")) {
        return text.starts_with("[[") ? std::optional<std::string_view>("") : std::nullopt;
    }

    const std::size_t end = text.find(']');
    if (end == std::string_view::npos) {
        return std::nullopt;
    }

    return trim(text.substr(1, end - 1));
}

/// The key a line of a table sets, bare or quoted, and what follows it, or nullopt.
std::optional<std::pair<std::string_view, std::string_view>> key_of(const std::string_view line) {
    std::string_view text = trim(line);
    if (text.empty() || text.starts_with('#')) {
        return std::nullopt;
    }

    std::string_view key;
    if (text.starts_with('"')) {
        const std::size_t end = text.find('"', 1);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }

        key = text.substr(1, end - 1);
        text = text.substr(end + 1);
    } else {
        const std::size_t end = text.find_first_of(" \t=.");
        key = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end);
    }

    return std::pair(key, trim(text));
}

} // namespace

std::optional<std::string> registry_index(const std::optional<std::string>& registry,
                                          const std::vector<registry_entry_t>& registries,
                                          const std::filesystem::path& base) {
    const std::string_view name = registry ? std::string_view(*registry) : default_registry;
    const auto named = std::ranges::find(registries, name, &registry_entry_t::name);
    if (named != registries.end()) {
        return canonical_index(named->index, base);
    }

    if (name == default_registry) {
        return std::string(default_index);
    }

    return std::nullopt;
}

bool is_unpacked(const locked_package_t& locked, const std::string_view target) {
    const std::filesystem::path folder =
        package_cache_folder(locked.source, locked.name, locked.version);
    if (!has_marker(folder, "source", locked.checksum)) {
        return false;
    }

    const auto own = locked.targets.find(target);
    return own == locked.targets.end() || has_marker(folder, target, own->second);
}

std::expected<sync_result_t, std::string> sync_packages(const std::filesystem::path& root,
                                                        const sync_options_t& options) {
    sync_result_t result;
    wanted_t wanted;
    std::set<std::filesystem::path> seen;
    collect(root, seen, wanted);
    if (wanted.broken) {
        result.broken = true;
        return result;
    }

    const std::filesystem::path lock_path = root / "fb.lock";
    const std::optional<std::string> lock_text = read_text(lock_path);
    lockfile_t before;
    if (lock_text) {
        std::expected<lockfile_t, std::string> read = read_lockfile(*lock_text);
        // `fb update` writes a broken fb.lock anew; anything else stops at it.
        if (!read && !(options.update && options.update_names.empty())) {
            return std::unexpected(std::format("{}: {}", to_utf8(lock_path), read.error()));
        }

        before = read.value_or(lockfile_t{});
    }

    if (!lock_text && wanted.requirements.empty()) {
        return result;
    }

    const std::string target =
        options.target.empty() ? std::string(build_target()) : options.target;
    std::map<std::string, std::unique_ptr<registry_t>, std::less<>> registries;
    const auto registry_of = [&](const std::string_view source) -> registry_t& {
        auto found = registries.find(source);
        if (found == registries.end()) {
            found = registries
                        .emplace(std::string(source),
                                 std::make_unique<registry_t>(std::string(source), options.http,
                                                              options.offline, options.status))
                        .first;
        }

        return *found->second;
    };

    std::optional<lockfile_t> locked;
    if (!options.update) {
        locked = locked_for(wanted.requirements, before);
    }

    if (!locked) {
        preferences_t preferences{
            .locked = before,
            .all = options.update && options.update_names.empty(),
        };
        for (const std::string& name : options.update_names) {
            if (before.find(name) == nullptr) {
                return std::unexpected(std::format("fb.lock has no package {} to update", name));
            }

            preferences.unlocked.insert(name);
        }

        const std::expected<version_t, std::string> fb = parse_version(build_config::version);
        const std::expected<std::vector<resolved_package_t>, std::string> resolved = resolve(
            wanted.requirements,
            [&](const std::string_view name, const std::string_view source) {
                return registry_of(source).versions(name);
            },
            preferences, fb.value_or(version_t{}));
        for (const auto& [source, registry] : registries) {
            std::ranges::copy(registry->notes(), std::back_inserter(result.notes));
        }

        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        locked = lockfile_of(*resolved);
    }

    report_changes(before, *locked, options.status);
    const std::string text = write_lockfile(*locked);
    if (lock_text != text) {
        std::ofstream out(lock_path, std::ios::binary);
        out << text;
        out.close();
        if (!out) {
            return std::unexpected(std::format("couldn't write {}", to_utf8(lock_path)));
        }
    }

    for (const locked_package_t& package : locked->packages) {
        if (is_unpacked(package, target)) {
            continue;
        }

        if (const std::expected<void, std::string> fetched =
                fetch(package, target, registry_of(package.source));
            !fetched) {
            return std::unexpected(fetched.error());
        }
    }

    // A registry used only to download has notes too, but none already given.
    for (const auto& [source, registry] : registries) {
        for (const std::string& note : registry->notes()) {
            if (!std::ranges::contains(result.notes, note)) {
                result.notes.push_back(note);
            }
        }
    }

    result.lockfile = std::move(*locked);
    return result;
}

std::expected<std::string, std::string> with_dependency(const std::string_view text,
                                                        const std::string_view name,
                                                        const std::string_view line) {
    const std::string_view newline = text.contains("\r\n") ? "\r\n" : "\n";
    std::vector<std::string_view> lines;
    for (const auto part : std::views::split(text, '\n')) {
        lines.emplace_back(part.begin(), part.end());
    }

    // A last line break leaves an empty last part, which isn't a line.
    const bool ended = text.ends_with('\n');
    if (ended) {
        lines.pop_back();
    }

    std::optional<std::size_t> table;
    std::optional<std::size_t> last;
    std::optional<std::size_t> found;
    bool inside = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (const std::optional<std::string_view> header = header_of(lines[i])) {
            inside = *header == "dependencies";
            if (inside) {
                table = i;
                last = i;
            }

            if (*header == std::format("dependencies.{}", name)) {
                return std::unexpected(std::format(
                    "project.toml has {} as a table of its own, [dependencies.{}], which fb add "
                    "doesn't change; change it there",
                    name, name));
            }

            continue;
        }

        if (!inside) {
            continue;
        }

        if (!trim(lines[i]).empty() && !trim(lines[i]).starts_with('#')) {
            last = i;
        }

        const auto key = key_of(lines[i]);
        if (!key || key->first != name) {
            continue;
        }

        // Only a line that's the whole entry is replaced.
        const bool assigned = key->second.starts_with('=');
        const std::string_view value = trim(key->second.substr(assigned ? 1 : 0));
        const bool whole = assigned && ((value.starts_with('"') && !value.starts_with(R"(""")")) ||
                                        (value.starts_with('{') && value.contains('}')));
        if (!whole) {
            return std::unexpected(std::format(
                "project.toml's line for {} in [dependencies] isn't one fb add can change; "
                "change it there",
                name));
        }

        found = i;
    }

    std::string out;
    const auto add_line = [&](const std::string_view each) {
        out += each;
        out += each.ends_with('\r') ? "\n" : newline;
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (found == i) {
            add_line(line);
            continue;
        }

        add_line(lines[i]);
        if (!found && last == i) {
            add_line(line);
        }
    }

    if (!table) {
        if (!out.empty()) {
            out += newline;
        }

        out += "[dependencies]";
        out += newline;
        out += line;
        out += newline;
    }

    return out;
}

} // namespace fb
