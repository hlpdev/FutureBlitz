// The package manager's commands, over the compiler's library's registries, fb.lock, and
// archives (docs/libraries.md's client design, and docs/registry-api.md's API).

#include "fb/package_commands.hpp"

#include "fb/build_config.hpp"
#include "fb/commands.hpp"
#include "fb/credentials.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/doc.hpp"
#include "fb/driver.hpp"
#include "fb/interface.hpp"
#include "fb/manifest.hpp"
#include "fb/package_archive.hpp"
#include "fb/packages.hpp"
#include "fb/process.hpp"
#include "fb/registry.hpp"
#include "fb/registry_api.hpp"
#include "fb/semver.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// wingdi.h's ERROR would be severity_t::ERROR's.
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
// shellapi.h needs windows.h's types first.
#include <shellapi.h>
#else
#include <unistd.h>
#endif

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

bool write_text(const std::filesystem::path& path, const std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    // An ofstream truncates what was there.
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return static_cast<bool>(out);
}

/// The package the current folder is in, or nullopt after saying there's none.
std::optional<std::filesystem::path> current_package(const console_t& console,
                                                     const options_t& options) {
    std::error_code error;
    const std::filesystem::path from =
        options.input.empty() ? std::filesystem::current_path(error) : options.input;
    std::optional<std::filesystem::path> found = find_package(from);
    if (!found) {
        console.error(std::format("there's no project.toml in {} or above it; fb {} works on a "
                                  "package",
                                  options.input.empty() ? "this folder" : to_utf8(options.input),
                                  command_name(options.command)));
    }

    return found;
}

/// The project.toml of the package in `root`, or nullopt when it can't be read or has mistakes.
std::optional<manifest_t> manifest_of(const std::filesystem::path& root) {
    const std::optional<std::string> text = read_text(root / "project.toml");
    if (!text) {
        return std::nullopt;
    }

    const source_file_t file(file_id_t{}, "project.toml", *text);
    diagnostic_bag_t diagnostics;
    return read_manifest(file, diagnostics);
}

/// The registry a command works with: its canonical index, the name [registries] gives it, if it
/// has one, and whether it's futureblitz.dev's.
struct chosen_registry_t {
    std::string index;
    std::optional<std::string> name;
    bool public_registry = false;
};

/// Whether what --registry gives is an index's URL or folder, not a name [registries] gives.
bool is_index(const std::string_view given) {
    return given.contains("://") || given.contains('/') || given.contains('\\') ||
           given.contains('.');
}

/// Whether `written`, an index as --registry or [registries] gives it, looks like a host's name
/// without its URL's scheme, like "index.example.com": letters, digits, '-', and '.'s, with no
/// '/'.
bool looks_like_host(const std::string_view written) {
    return written.contains('.') && !written.starts_with('.') &&
           std::ranges::all_of(written, [](const char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '.';
           });
}

/// Why `index`, a canonical one, written as `written`, isn't a registry fb can use, or nullopt
/// when it is: a folder is a registry on disk only with its config.json. A folder that isn't
/// there, named like a host, is most likely a URL without its https://.
std::optional<std::string> folder_error(const std::string_view index,
                                        const std::string_view written) {
    const std::filesystem::path folder = local_index_folder(index);
    std::error_code error;
    if (folder.empty() || std::filesystem::is_regular_file(folder / "config.json", error)) {
        return std::nullopt;
    }

    std::string out =
        std::format("there's no registry in {}: it has no config.json", to_utf8(folder));
    if (looks_like_host(written) && !std::filesystem::exists(folder, error)) {
        out += std::format("; an index's URL starts with https://, like https://{}/", written);
    }

    return out;
}

/// The registry --registry names, from [registries] of the package in `root`, if there is one,
/// or as an index's URL or folder, or else futureblitz.dev's, or the mirror [registries] names
/// it. A folder has to be a registry on disk's, with its config.json.
std::expected<chosen_registry_t, std::string>
choose_registry(const options_t& options, const std::optional<std::filesystem::path>& root) {
    const std::string given = options.registry.value_or(std::string(default_registry));
    std::error_code error;
    if (is_index(given)) {
        const std::expected<std::string, std::string> written = with_index_scheme(given);
        if (!written) {
            return std::unexpected(written.error());
        }

        if (written->starts_with("http://") && !is_loopback_url(*written)) {
            return std::unexpected(
                "a registry's index is https://, or http:// only on this machine (localhost, "
                "127.0.0.1, or [::1])");
        }

        const std::string index = canonical_index(*written, std::filesystem::current_path(error));
        if (const std::optional<std::string> wrong = folder_error(index, given)) {
            return std::unexpected(*wrong);
        }

        return chosen_registry_t{ .index = index, .public_registry = index == default_index };
    }

    std::vector<registry_entry_t> registries;
    if (root) {
        if (const std::optional<manifest_t> manifest = manifest_of(*root)) {
            registries = manifest->registries;
        }
    }

    const std::optional<std::string> index =
        registry_index(given, registries, root.value_or(std::filesystem::current_path(error)));
    if (!index) {
        return std::unexpected(std::format(
            "there's no registry '{}' in {}'s [registries]; --registry takes a registry's name "
            "there, or its index's URL or folder",
            given, root ? "the package" : "a package"));
    }

    const auto entry = std::ranges::find(registries, given, &registry_entry_t::name);
    if (const std::optional<std::string> wrong =
            folder_error(*index, entry != registries.end() ? entry->index : "")) {
        return std::unexpected(*wrong);
    }

    return chosen_registry_t{
        .index = *index,
        .name = given,
        .public_registry = *index == default_index,
    };
}

/// A token for the registry: FB_REGISTRY_TOKEN, or for a publish in a GitHub workflow, trusted
/// publishing's, which is revoked after, or the one fb login kept.
struct token_t {
    std::string value;
    bool trusted = false;
    /// Trusted publishing's packages, as the registry said them.
    std::optional<std::vector<std::string>> packages;
};

std::expected<token_t, std::string> token_for(const registry_session_t& session,
                                              const bool publishing) {
    if (const std::optional<std::string> given = environment_variable("FB_REGISTRY_TOKEN");
        given && !given->empty()) {
        return token_t{ .value = *given };
    }

    if (publishing) {
        if (const std::optional<std::expected<trusted_token_t, std::string>> trusted =
                trusted_publishing_token(session)) {
            if (!*trusted) {
                return std::unexpected(trusted->error());
            }

            const trusted_token_t& exchanged = **trusted;
            return token_t{
                .value = exchanged.token,
                .trusted = true,
                .packages = exchanged.packages,
            };
        }
    }

    const std::expected<std::optional<std::string>, std::string> kept =
        credential_store()->get(credential_key(session.index));
    if (!kept) {
        return std::unexpected(kept.error());
    }

    if (!*kept) {
        return std::unexpected(
            std::format("fb has no token for {}; fb login signs in, or FB_REGISTRY_TOKEN gives one",
                        session.index));
    }

    return token_t{ .value = **kept };
}

/// A session with the registry, its token found when `authorized`, or nullopt after saying why.
std::optional<registry_session_t> session_for(const console_t& console,
                                              const chosen_registry_t& registry,
                                              const bool authorized, const bool publishing,
                                              bool* trusted = nullptr) {
    registry_session_t session{
        .index = registry.index,
        .status = [&console](const std::string_view verb,
                             const std::string_view message) { console.status(verb, message); },
    };
    if (!authorized || !local_index_folder(registry.index).empty()) {
        return session;
    }

    const std::expected<token_t, std::string> token = token_for(session, publishing);
    if (!token) {
        console.error(token.error());
        return std::nullopt;
    }

    session.token = token->value;
    if (token->trusted) {
        session.trusted_packages = token->packages;
    }

    if (trusted != nullptr) {
        *trusted = token->trusted;
    }

    return session;
}

#if !defined(_WIN32) && !defined(__APPLE__)
/// Where a program is on PATH, or nullopt.
std::optional<std::filesystem::path> on_path(const std::string_view name) {
    const std::optional<std::string> path = environment_variable("PATH");
    if (!path) {
        return std::nullopt;
    }

    std::error_code error;
    for (const auto part : std::views::split(std::string_view(*path), ':')) {
        const std::filesystem::path candidate =
            path_from_utf8(std::string_view(part.begin(), part.end())) / path_from_utf8(name);
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }

    return std::nullopt;
}
#endif

/// Opens `url` in the user's browser, or in the program FB_BROWSER names. False when it
/// couldn't.
bool open_browser(const std::string_view url) {
    if (const std::optional<std::string> browser = environment_variable("FB_BROWSER");
        browser && !browser->empty()) {
        // It's started, not waited for: a browser it starts would keep fb waiting until it
        // closes.
        const std::vector<std::string> arguments{ std::string(url) };
        return start_detached(path_from_utf8(*browser), arguments).has_value();
    }

#ifdef _WIN32
    const std::wstring wide = path_from_utf8(url).wstring();
    // ShellExecuteW gives a number above 32 when it worked, as an HINSTANCE.
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
#ifdef __APPLE__
    const std::optional<std::filesystem::path> opener = std::filesystem::path("/usr/bin/open");
#else
    const std::optional<std::filesystem::path> opener = on_path("xdg-open");
#endif
    if (!opener) {
        return false;
    }

    const std::vector<std::string> arguments{ std::string(url) };
    return start_detached(*opener, arguments).has_value();
#endif
}

/// This computer's name, for the sign-in's name: "fb on hunter-pc".
std::string machine_name() {
#ifdef _WIN32
    std::wstring name(MAX_COMPUTERNAME_LENGTH + 1, L'\0');
    auto size = static_cast<DWORD>(name.size());
    if (GetComputerNameW(name.data(), &size) != FALSE) {
        name.resize(size);
        return to_utf8(std::filesystem::path(name));
    }
#else
    std::string name(256, '\0');
    if (gethostname(name.data(), name.size()) == 0) {
        name.resize(name.find('\0'));
        return name;
    }
#endif
    return "this computer";
}

/// The newest version of `name` in the registry that this fb can build: a release, unless it has
/// only pre-releases, and not yanked.
std::expected<std::string, std::string> newest_version(registry_t& registry,
                                                       const std::string_view name) {
    const std::expected<std::vector<index_entry_t>, std::string> versions = registry.versions(name);
    if (!versions) {
        return std::unexpected(versions.error());
    }

    const std::expected<version_t, std::string> fb = parse_version(build_config::version);
    std::optional<version_t> newest;
    std::optional<version_t> newest_pre;
    for (const index_entry_t& entry : *versions) {
        const std::expected<version_t, std::string> version = parse_version(entry.version);
        if (!version || entry.yanked) {
            continue;
        }

        if (entry.fb) {
            const std::expected<version_range_t, std::string> range = parse_range(*entry.fb);
            if (!range || !fb || !range->matches(*fb)) {
                continue;
            }
        }

        std::optional<version_t>& kept = version->pre.empty() ? newest : newest_pre;
        if (!kept || *version > *kept) {
            kept = *version;
        }
    }

    if (versions->empty()) {
        return std::unexpected(std::format("there's no package {} in {}", name, registry.index()));
    }

    if (!newest && !newest_pre) {
        return std::unexpected(
            std::format("{} has no version this fb ({}) can build that isn't yanked", name,
                        build_config::version));
    }

    return (newest ? *newest : *newest_pre).text();
}

/// Says what a sync didn't reach, and gives its error, if it had one.
bool report_sync(const console_t& console,
                 const std::expected<sync_result_t, std::string>& synced) {
    if (!synced) {
        console.error(synced.error());
        return false;
    }

    for (const std::string& note : synced->notes) {
        console.note(note);
    }

    return true;
}

sync_options_t sync_options(const console_t& console, const options_t& options) {
    return {
        .offline = options.offline,
        .status = [&console](const std::string_view verb,
                             const std::string_view message) { console.status(verb, message); },
    };
}

int add_command(const console_t& console, const options_t& options) {
    const std::optional<std::filesystem::path> root = current_package(console, options);
    if (!root) {
        return usage_exit_code;
    }

    if (options.registry && is_index(*options.registry)) {
        console.error("fb add takes a registry by its name in [registries], which project.toml "
                      "writes the dependency with");
        return usage_exit_code;
    }

    const std::expected<chosen_registry_t, std::string> chosen = choose_registry(options, root);
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    const std::filesystem::path manifest = *root / "project.toml";
    const std::optional<std::string> before = read_text(manifest);
    if (!before || !manifest_of(*root)) {
        console.error("project.toml has mistakes, which fb check shows");
        return compile_errors_exit_code;
    }

    registry_t registry(chosen->index, send_http, options.offline, {});
    std::string text = *before;
    std::vector<std::string> added_lines;
    for (const std::string& spec : options.names) {
        const std::size_t at = spec.find('@');
        const std::string name = spec.substr(0, at);
        if (const std::optional<std::string> wrong = package_name_error(name)) {
            console.error(*wrong);
            return usage_exit_code;
        }

        std::string range;
        if (at != std::string::npos) {
            range = spec.substr(at + 1);
            if (const std::expected<version_range_t, std::string> parsed = parse_range(range);
                !parsed) {
                console.error(parsed.error());
                return usage_exit_code;
            }
        } else {
            const std::expected<std::string, std::string> newest = newest_version(registry, name);
            if (!newest) {
                console.error(newest.error());
                return compile_errors_exit_code;
            }

            range = *newest;
        }

        // The plain form, unless it needs a table for its registry or link.
        const bool own = !chosen->name || *chosen->name == default_registry;
        std::string line = std::format("{} = \"{}\"", name, range);
        if (!own || options.beside) {
            line = std::format("{} = {{ version = \"{}\"{}{} }}", name, range,
                               own ? "" : std::format(", registry = \"{}\"", *chosen->name),
                               options.beside ? ", link = \"beside\"" : "");
        }

        const std::expected<std::string, std::string> added = with_dependency(text, name, line);
        if (!added) {
            console.error(added.error());
            return compile_errors_exit_code;
        }

        text = *added;
        added_lines.push_back(std::format("{} {} to dependencies", name,
                                          at == std::string::npos ? "v" + range : range));
    }

    for (const std::string& note : registry.notes()) {
        console.note(note);
    }

    if (!write_text(manifest, text)) {
        console.error(std::format("couldn't write {}", to_utf8(manifest)));
        return compile_errors_exit_code;
    }

    // What can't be locked is taken out of project.toml again, so it's said to be added only
    // once it's locked.
    if (!report_sync(console, sync_packages(*root, sync_options(console, options)))) {
        (void)write_text(manifest, *before);
        return compile_errors_exit_code;
    }

    for (const std::string& line : added_lines) {
        console.status("Added", line);
    }

    return 0;
}

int update_command(const console_t& console, const options_t& options) {
    const std::optional<std::filesystem::path> root = current_package(console, options);
    if (!root) {
        return usage_exit_code;
    }

    sync_options_t sync = sync_options(console, options);
    sync.update = true;
    sync.update_names = options.names;
    const std::expected<sync_result_t, std::string> synced = sync_packages(*root, sync);
    if (synced && synced->broken) {
        console.error("project.toml has mistakes, which fb check shows");
        return compile_errors_exit_code;
    }

    return report_sync(console, synced) ? 0 : compile_errors_exit_code;
}

int search_command(const console_t& console, const options_t& options) {
    std::error_code error;
    const std::expected<chosen_registry_t, std::string> chosen =
        choose_registry(options, find_package(std::filesystem::current_path(error)));
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    std::string words;
    for (const std::string& word : options.names) {
        words += (words.empty() ? "" : " ") + word;
    }

    const std::optional<registry_session_t> session = session_for(console, *chosen, false, false);
    if (!session) {
        return compile_errors_exit_code;
    }

    const auto found = search_packages(words, options.limit, *session);
    if (!found) {
        console.error(found.error());
        return compile_errors_exit_code;
    }

    const auto& [packages, total] = *found;
    std::size_t width = 0;
    for (const found_package_t& package : packages) {
        width = std::max(width, package.name.size() + package.version.size() + 5);
    }

    for (const found_package_t& package : packages) {
        const std::string entry = std::format("{} = \"{}\"", package.name, package.version);
        console_t::print(package.description.empty()
                             ? entry + "\n"
                             : std::format("{:<{}}  # {}\n", entry, width, package.description));
    }

    if (packages.empty()) {
        console.note(std::format("no packages in {} match '{}'", chosen->index, words));
    } else if (total > packages.size()) {
        console.note(
            std::format("{} more match; --limit shows up to 100", total - packages.size()));
    }

    return 0;
}

/// Whether the registry has the version of each of `manifest`'s dependencies with a path and a
/// version that its folder has, not yanked, which the published package depends on: what the
/// package builds with here is what another computer gets, so it's published first, as Cargo has
/// it. Says what isn't.
bool published_path_versions(const console_t& console, const options_t& options,
                             const std::filesystem::path& root, const manifest_t& manifest) {
    for (const dependency_t& dependency : manifest.dependencies) {
        if (dependency.path.empty() || !dependency.range) {
            continue;
        }

        const std::optional<manifest_t> folder =
            manifest_of(root / path_from_utf8(dependency.path));
        const std::optional<std::string> index =
            registry_index(dependency.registry, manifest.registries, root);
        if (!folder || !index) {
            continue;
        }

        registry_t registry(*index, send_http, options.offline, {});
        const std::expected<std::vector<index_entry_t>, std::string> versions =
            registry.versions(dependency.name);
        if (!versions) {
            console.error(versions.error());
            return false;
        }

        if (std::ranges::none_of(*versions, [&](const index_entry_t& entry) {
                return entry.version == folder->version && !entry.yanked;
            })) {
            console.error(std::format(
                "{} builds with {} v{} from its folder, '{}', which the registry doesn't have, so "
                "no one could build the published package; publish {} v{} first",
                manifest.name, dependency.name, folder->version, dependency.path, dependency.name,
                folder->version));
            return false;
        }
    }

    return true;
}

/// project.toml's text with each registry on disk that [registries] names by a relative folder
/// named by its whole path, from `root`, so the package builds from another folder.
std::string with_whole_registries(const std::string& text, const std::filesystem::path& root) {
    const source_file_t file(file_id_t{}, "project.toml", text);
    diagnostic_bag_t ignored;
    const std::optional<manifest_t> manifest = read_manifest(file, ignored);
    if (!manifest) {
        return text;
    }

    std::string out = text;
    std::vector<const registry_entry_t*> entries;
    for (const registry_entry_t& entry : manifest->registries) {
        if (is_local_index(entry.index) && !entry.index.starts_with("file:") &&
            !path_from_utf8(entry.index).is_absolute()) {
            entries.push_back(&entry);
        }
    }

    // From the last, so the places before stay where they are.
    std::ranges::sort(entries, std::ranges::greater{},
                      [](const registry_entry_t* entry) { return entry->index_span.begin; });
    for (const registry_entry_t* entry : entries) {
        std::error_code error;
        std::string whole =
            to_utf8(std::filesystem::weakly_canonical(root / path_from_utf8(entry->index), error));
        std::ranges::replace(whole, '\\', '/');
        out.replace(entry->index_span.begin, entry->index_span.end - entry->index_span.begin,
                    std::format("\"{}\"", whole));
    }

    return out;
}

/// Builds the package as its archives have it, unpacked in build/package/<name>-<version>/,
/// from the registry's packages alone, as another computer would, so what a path dependency's
/// folder has that its published version doesn't is found before it's published. Says why it
/// doesn't build.
bool verify_package(const console_t& console, const options_t& options,
                    const std::filesystem::path& root, const prepared_package_t& package) {
    const std::string shown =
        std::format("{} v{}", package.manifest.name, package.manifest.version);
    const std::filesystem::path archives = root / "build" / "package";
    const std::filesystem::path folder =
        archives / std::format("{}-{}", package.manifest.name, package.manifest.version);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    console.status("Verifying", std::format("{} as the registry would have it", shown));
    if (const std::expected<void, std::string> unpacked =
            unpack_archive(archives / package.source.file, folder, "source",
                           package.source.ref.sha256, most_source_unpacked);
        !unpacked) {
        console.error(std::format("couldn't unpack {}: {}", package.source.file, unpacked.error()));
        return false;
    }

    for (const built_archive_t& target : package.targets) {
        if (target.target != build_target()) {
            continue;
        }

        if (const std::expected<void, std::string> unpacked =
                unpack_archive(archives / target.file, folder, target.target, target.ref.sha256,
                               most_target_unpacked);
            !unpacked) {
            console.error(std::format("couldn't unpack {}: {}", target.file, unpacked.error()));
            return false;
        }
    }

    const std::optional<std::string> text = read_text(folder / "project.toml");
    if (!text || !write_text(folder / "project.toml", with_whole_registries(*text, root))) {
        console.error(std::format("couldn't write {}", to_utf8(folder / "project.toml")));
        return false;
    }

    if (!report_sync(console, sync_packages(folder, sync_options(console, options)))) {
        return false;
    }

    const std::expected<compilation_t, std::string> compiled = compile_file(folder);
    if (!compiled) {
        console.error(compiled.error());
        return false;
    }

    const std::size_t errors = compiled->diagnostics.error_count();
    for (const diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
        if (diagnostic.severity == severity_t::ERROR) {
            console_t::print_error(
                render_pretty(diagnostic, compiled->sources, console.diagnostics()));
        }
    }

    if (errors > 0) {
        console.error(std::format(
            "{} doesn't build as the registry would have it, from its packages, because of {} "
            "error{}; --no-verify publishes it anyway",
            shown, errors, errors == 1 ? "" : "s"));
        return false;
    }

    return true;
}

int publish_command(const console_t& console, const options_t& options) {
    // --offline keeps fb off the network, which publishing can't be.
    if (options.offline && !options.dry_run) {
        console.error("fb publish sends the package to the registry, so it can't be --offline; "
                      "--dry-run with --offline packages it without the network");
        return usage_exit_code;
    }

    const std::optional<std::filesystem::path> root = current_package(console, options);
    if (!root) {
        return usage_exit_code;
    }

    const std::expected<chosen_registry_t, std::string> chosen = choose_registry(options, root);
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    // The package compiles, with its dependencies, for its docs.
    if (!prepare_packages(console, options, *root)) {
        return compile_errors_exit_code;
    }

    const std::expected<compilation_t, std::string> compiled = compile_docs(*root);
    if (!compiled) {
        console.error(compiled.error());
        return usage_exit_code;
    }

    const std::size_t errors = compiled->diagnostics.error_count();
    for (const diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
        if (diagnostic.severity == severity_t::ERROR) {
            console_t::print_error(
                render_pretty(diagnostic, compiled->sources, console.diagnostics()));
        }
    }

    if (errors > 0) {
        console.error(std::format("couldn't publish the package because of {} error{}", errors,
                                  errors == 1 ? "" : "s"));
        return compile_errors_exit_code;
    }

    diagnostic_bag_t warnings;
    const std::string docs =
        doc_json(document(*compiled, false, build_mode_t::DEBUG, warnings), compiled->sources);
    const std::expected<prepared_package_t, std::string> package =
        prepare_package(*root, chosen->index, chosen->public_registry);
    if (!package) {
        console.error(package.error());
        return compile_errors_exit_code;
    }

    // What a path dependency's folder builds with has to be the registry's, which --offline can't
    // ask.
    if (!options.offline && !published_path_versions(console, options, *root, package->manifest)) {
        return compile_errors_exit_code;
    }

    const std::string shown =
        std::format("{} v{}", package->manifest.name, package->manifest.version);
    console.status("Packaging", std::format("{} ({})", shown, to_utf8(*root)));
    std::vector<const built_archive_t*> archives{ &package->source };
    for (const built_archive_t& target : package->targets) {
        archives.push_back(&target);
    }

    for (const built_archive_t* archive : archives) {
        const std::filesystem::path written = *root / "build" / "package" / archive->file;
        if (!write_text(written, archive->bytes)) {
            console.error(std::format("couldn't write {}", to_utf8(written)));
            return compile_errors_exit_code;
        }

        console.status("Packaged",
                       std::format("{}: {} file{}, {}", archive->file, archive->paths.size(),
                                   archive->paths.size() == 1 ? "" : "s",
                                   size_text(archive->ref.size)));
        if (options.dry_run) {
            for (const std::string& path : archive->paths) {
                console_t::print(std::format("{}  {}\n", archive->file, path));
            }
        }
    }

    if (!options.no_verify && !verify_package(console, options, *root, *package)) {
        return compile_errors_exit_code;
    }

    if (options.dry_run) {
        console.status("Finished", std::format("the dry run of {}; nothing was published to {}",
                                               shown, chosen->index));
        return 0;
    }

    bool trusted = false;
    const std::optional<registry_session_t> session =
        session_for(console, *chosen, true, true, &trusted);
    if (!session) {
        return compile_errors_exit_code;
    }

    console.status("Publishing", std::format("{} to {}", shown, chosen->index));
    const std::expected<std::vector<std::string>, std::string> published =
        publish_package(*package, docs, *session);
    if (trusted) {
        // Trusted publishing's token is only for this publish.
        if (const std::expected<void, std::string> revoked = revoke_token(*session); !revoked) {
            console.warning(
                std::format("the workflow's token couldn't be revoked: {}", revoked.error()));
        }
    }

    if (!published) {
        console.error(published.error());
        return compile_errors_exit_code;
    }

    for (const std::string& warning : *published) {
        console.warning(warning);
    }

    console.status("Published", shown);
    return 0;
}

/// Why `who`, as --add and --remove give it, can't be an owner's name, or nullopt when it can be:
/// a username, or "team:" and a team's, each lowercase letters, digits, and '-', 1 to 39 long, not
/// starting or ending with '-'.
std::optional<std::string> owner_name_error(const std::string_view who) {
    const std::string_view name = who.starts_with("team:") ? who.substr(5) : who;
    const bool fits = !name.empty() && name.size() <= 39 && !name.starts_with('-') &&
                      !name.ends_with('-') && std::ranges::all_of(name, [](const char c) {
                          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                      });
    if (fits) {
        return std::nullopt;
    }

    return std::format("'{}' isn't a username, or team: and a team's name, which are lowercase "
                       "letters, digits, and '-', up to 39",
                       who);
}

int yank_command(const console_t& console, const options_t& options) {
    std::error_code error;
    const std::expected<chosen_registry_t, std::string> chosen =
        choose_registry(options, find_package(std::filesystem::current_path(error)));
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    const std::string& spec = options.names.front();
    const std::string name = spec.substr(0, spec.find('@'));
    const std::string version = spec.substr(spec.find('@') + 1);
    if (const std::optional<std::string> wrong = package_name_error(name)) {
        console.error(*wrong);
        return usage_exit_code;
    }

    if (!parse_version(version)) {
        console.error(std::format("'{}' isn't a version, like physics@1.2.0", version));
        return usage_exit_code;
    }

    const std::optional<registry_session_t> session = session_for(console, *chosen, true, false);
    if (!session) {
        return compile_errors_exit_code;
    }

    if (const std::expected<void, std::string> yanked =
            yank_version(name, version, options.undo, *session);
        !yanked) {
        console.error(yanked.error());
        return compile_errors_exit_code;
    }

    console.status(options.undo ? "Unyanked" : "Yanked", std::format("{} v{}", name, version));
    return 0;
}

int owner_command(const console_t& console, const options_t& options) {
    std::error_code error;
    const std::expected<chosen_registry_t, std::string> chosen =
        choose_registry(options, find_package(std::filesystem::current_path(error)));
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    const std::string& name = options.names.front();
    if (const std::optional<std::string> wrong = package_name_error(name)) {
        console.error(*wrong);
        return usage_exit_code;
    }

    if (options.owner) {
        if (const std::optional<std::string> wrong = owner_name_error(*options.owner)) {
            console.error(*wrong);
            return usage_exit_code;
        }
    }

    // The list is the registry's to show anyone, so only a change needs a token.
    const std::optional<registry_session_t> session =
        session_for(console, *chosen, options.owner.has_value(), false);
    if (!session) {
        return compile_errors_exit_code;
    }

    if (!options.owner) {
        const std::expected<package_owners_t, std::string> owners = list_owners(name, *session);
        if (!owners) {
            console.error(owners.error());
            return compile_errors_exit_code;
        }

        for (const package_owner_t& owner : owners->owners) {
            console_t::print(std::format("{} {}\n", owner.kind, owner.name));
        }

        // An invite's expiry, as the registry writes it, is shown by its date.
        for (const owner_invite_t& invite : owners->invites) {
            const std::string until =
                invite.expires_at.size() >= 10
                    ? std::format(", until {}", std::string_view(invite.expires_at).substr(0, 10))
                    : "";
            console_t::print(std::format("user {} (invited{})\n", invite.invitee, until));
        }

        return 0;
    }

    const std::expected<owner_change_t, std::string> changed =
        change_owner(name, *options.owner, options.remove_owner, *session);
    if (!changed) {
        console.error(changed.error());
        return compile_errors_exit_code;
    }

    console.status(changed->verb, changed->message);
    return 0;
}

int login_command(const console_t& console, const options_t& options) {
    std::error_code error;
    const std::expected<chosen_registry_t, std::string> chosen =
        choose_registry(options, find_package(std::filesystem::current_path(error)));
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    const std::optional<registry_session_t> session = session_for(console, *chosen, false, false);
    if (!session) {
        return compile_errors_exit_code;
    }

    if (!local_index_folder(chosen->index).empty()) {
        console.error("a registry on disk has no accounts to sign in to");
        return usage_exit_code;
    }

    // The store is checked first: a sign-in's token comes once, and one it couldn't keep would
    // be lost.
    const std::unique_ptr<credential_store_t> store = credential_store();
    if (const std::expected<std::optional<std::string>, std::string> kept =
            store->get(credential_key(chosen->index));
        !kept) {
        console.error(std::format("fb can't keep a token: {}", kept.error()));
        return compile_errors_exit_code;
    }

    std::string token;
    std::string username;
    if (options.token) {
        // A token made on the dashboard, from stdin, which a pipe or a paste gives.
        std::string line;
        std::getline(std::cin, line);
        const auto blank = [](const char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while (!line.empty() && blank(line.back())) {
            line.pop_back();
        }

        line.erase(line.begin(), std::ranges::find_if_not(line, blank));
        if (line.empty()) {
            console.error("fb login --token reads the token from stdin, which gave none");
            return usage_exit_code;
        }

        if (const std::optional<std::string> wrong = token_error(line)) {
            console.error(*wrong);
            return usage_exit_code;
        }

        // One the registry doesn't take, mistyped, revoked, or expired, isn't kept over the
        // token there is.
        registry_session_t checking = *session;
        checking.token = line;
        const std::expected<std::string, std::string> account = account_of(checking);
        if (!account) {
            console.error(std::format("the registry didn't take the token: {}", account.error()));
            return compile_errors_exit_code;
        }

        token = line;
        username = *account;
    } else {
        // FB_BROWSER=none, or empty, opens nothing.
        const std::optional<std::string> browser = environment_variable("FB_BROWSER");
        const bool no_browser = browser && (browser->empty() || *browser == "none");
        login_hooks_t hooks{
            .open =
                [&console](const std::string_view url) {
                    const bool opened = open_browser(url);
                    if (!opened) {
                        console.note(
                            "fb couldn't open a browser, so the page is for opening by hand");
                    }

                    return opened;
                },
            .wait =
                [](const std::chrono::seconds seconds) { std::this_thread::sleep_for(seconds); },
            .show =
                [&console](const std::string_view code, const std::string_view url,
                           const bool opening) {
                    console.status(
                        opening ? "Opening" : "Open",
                        std::format("{} to sign in{}", url, opening ? "" : ", in a browser"));
                    console.status("Code", std::format("{}, which the page shows too", code));
                    console.status("Waiting", "for the sign-in to be approved there");
                },
        };
        if (no_browser) {
            hooks.open = nullptr;
        }

        const std::expected<signed_in_t, std::string> signed_in =
            login(std::format("fb on {}", machine_name()), *session, hooks);
        if (!signed_in) {
            console.error(signed_in.error());
            return compile_errors_exit_code;
        }

        token = signed_in->token;
        username = signed_in->username;
    }

    if (const std::expected<void, std::string> kept =
            store->set(credential_key(chosen->index), token);
        !kept) {
        // It isn't lost: the person can keep it themselves.
        console.error(std::format("fb couldn't keep the token: {}", kept.error()));
        console.note(std::format("the token is {}; FB_REGISTRY_TOKEN gives it to fb", token));
        return compile_errors_exit_code;
    }

    console.status("Logged in", username.empty()
                                    ? std::format("to {}", chosen->index)
                                    : std::format("to {} as {}", chosen->index, username));
    if (environment_variable("FB_REGISTRY_TOKEN")) {
        console.note("FB_REGISTRY_TOKEN is set, and fb uses it before the token it kept");
    }

    return 0;
}

int logout_command(const console_t& console, const options_t& options) {
    std::error_code error;
    const std::expected<chosen_registry_t, std::string> chosen =
        choose_registry(options, find_package(std::filesystem::current_path(error)));
    if (!chosen) {
        console.error(chosen.error());
        return usage_exit_code;
    }

    const std::expected<bool, std::string> removed =
        credential_store()->remove(credential_key(chosen->index));
    if (!removed) {
        console.error(removed.error());
        return compile_errors_exit_code;
    }

    if (*removed) {
        console.status("Logged out", std::format("of {}", chosen->index));
    } else {
        console.note(std::format("fb kept no token for {}", chosen->index));
    }

    return 0;
}

} // namespace

bool prepare_packages(const console_t& console, const options_t& options,
                      const std::filesystem::path& input) {
    std::error_code error;
    const std::filesystem::path folder =
        std::filesystem::is_directory(input, error) ? input : input.parent_path();
    const std::optional<std::filesystem::path> root =
        find_package(folder.empty() ? std::filesystem::current_path(error) : folder);
    if (!root) {
        return true;
    }

    return report_sync(console, sync_packages(*root, sync_options(console, options)));
}

int package_command(const options_t& options) {
    const console_t console(options.color);
    switch (options.command) {
    case command_t::ADD:
        return add_command(console, options);
    case command_t::UPDATE:
        return update_command(console, options);
    case command_t::SEARCH:
        return search_command(console, options);
    case command_t::PUBLISH:
        return publish_command(console, options);
    case command_t::YANK:
        return yank_command(console, options);
    case command_t::OWNER:
        return owner_command(console, options);
    case command_t::LOGIN:
        return login_command(console, options);
    case command_t::LOGOUT:
        return logout_command(console, options);
    default:
        return usage_exit_code;
    }
}

} // namespace fb
