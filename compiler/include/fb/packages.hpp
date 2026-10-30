#pragma once

#include "fb/http_client.hpp"
#include "fb/lockfile.hpp"
#include "fb/manifest.hpp"
#include "fb/registry.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// How sync_packages() works.
struct sync_options_t {
    /// Use only the cache, never the network.
    bool offline = false;
    /// `fb update`: resolve anew, from the newest versions: of every package, or of those
    /// `update_names` names and what they need.
    bool update = false;
    std::vector<std::string> update_names;
    /// What sends HTTP requests: send_http(), or a test's.
    http_client_t http = send_http;
    /// What it says as it works.
    status_t status;
    /// The target whose prebuilt archives it fetches, build_target()'s unless a test says.
    std::string target;
};

/// What sync_packages() did.
struct sync_result_t {
    /// The package's project.toml has mistakes, so nothing was done; a compile reports them.
    bool broken = false;
    /// fb.lock, as it is now.
    lockfile_t lockfile;
    /// What it used from the cache because a registry couldn't be reached.
    std::vector<std::string> notes;
};

/// For the package in `root`: reads its project.toml and its path dependencies', and fb.lock, and
/// when fb.lock doesn't have what they ask for, resolves their registry dependencies again,
/// keeping what fb.lock has where it can, and writes fb.lock; then makes sure each locked
/// package, and its archive for the target, is unpacked in the cache, downloading and checking
/// what isn't. The status says what's added, updated, removed, and downloaded. Fails, saying why,
/// when the ranges can't be met, a registry can't be read and the cache has nothing of it, or a
/// download isn't what fb.lock says.
[[nodiscard]] std::expected<sync_result_t, std::string>
sync_packages(const std::filesystem::path& root, const sync_options_t& options);

/// A registry's canonical index, as `registry` names it in a project.toml in `base` whose
/// [registries] are `registries`: futureblitz.dev's for nullopt or "futureblitz", unless
/// [registries] names that one too. nullopt for a name it doesn't have.
[[nodiscard]] std::optional<std::string>
registry_index(const std::optional<std::string>& registry,
               const std::vector<registry_entry_t>& registries, const std::filesystem::path& base);

/// Whether the cache has the package fb.lock pins as `locked` unpacked whole: its own archive,
/// and its archive for `target`, when its version has one.
[[nodiscard]] bool is_unpacked(const locked_package_t& locked, std::string_view target);

/// `text`, a project.toml, with `line`, like `physics = "1.2.3"`, as the dependency `name` in
/// [dependencies]: in place of a line of its own for it, or after the table's last line, or in a
/// new table at the end. Its other lines stay as they are. Fails when its line for `name` isn't
/// one line, like a table over several.
[[nodiscard]] std::expected<std::string, std::string>
with_dependency(std::string_view text, std::string_view name, std::string_view line);

} // namespace fb
