#pragma once

#include "fb/http_client.hpp"
#include "fb/manifest.hpp"
#include "fb/registry.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// An archive fb publish built: its file's name, the target it's for, or empty for the package's
/// own, its bytes, and the paths in it.
struct built_archive_t {
    std::string file;
    std::string target;
    std::string bytes;
    archive_ref_t ref;
    std::vector<std::string> paths;
};

/// A package ready to publish: its project.toml, its index line, and its archives, checked as
/// the registry checks them.
struct prepared_package_t {
    manifest_t manifest;
    index_entry_t entry;
    built_archive_t source;
    std::vector<built_archive_t> targets;
};

/// Why `name` can't be a registry's package, or nullopt when it can be: lowercase ASCII letters,
/// digits, and '_', starting with a letter, at most 64, and not std, fb, or futureblitz, even
/// with '_'s.
[[nodiscard]] std::optional<std::string> registry_name_error(std::string_view name);

/// Builds the archives of the package in `root` for the registry whose canonical index is
/// `index`, which `public_registry` says is futureblitz.dev's, and checks it as the registry
/// does (docs/registry-api.md): a library whose name the registry takes, with a description, an
/// SPDX license, a license file, and a readme of UTF-8 under 1 MB; dependencies only by their
/// versions, from its own registry for futureblitz.dev; paths an archive can hold; archives within
/// the size limits; and a prebuilt package's builds, debug and release, agreeing with its .fbi for
/// each target. It doesn't ask the registry whether the version is new.
[[nodiscard]] std::expected<prepared_package_t, std::string>
prepare_package(const std::filesystem::path& root, std::string_view index, bool public_registry);

/// What publish_package() and the other API calls need.
struct registry_session_t {
    /// The registry's canonical index.
    std::string index;
    http_client_t http = send_http;
    /// The API token, for a registry on the network.
    std::string token;
    status_t status;
};

/// The most docs JSON can be.
inline constexpr std::uint64_t most_docs_bytes = std::uint64_t{ 25 } << 20U;

/// Publishes `package`, with `docs`, fb doc's JSON: for a registry on disk, its archives into its
/// `dl` and its line into its index file; for one on the network, each archive and the docs
/// uploaded, in parts when they're large, then the version published with them. Fails, saying
/// why, when the version is there already, or the registry refuses it. Gives the registry's
/// warnings.
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
publish_package(const prepared_package_t& package, std::string_view docs,
                const registry_session_t& session);

/// Yanks a version of a package, or with `undo`, takes its yank back.
[[nodiscard]] std::expected<void, std::string> yank_version(std::string_view name,
                                                            std::string_view version, bool undo,
                                                            const registry_session_t& session);

/// An owner of a package: a user's or a team's name.
struct package_owner_t {
    std::string kind;
    std::string name;
};

/// A package's owners.
[[nodiscard]] std::expected<std::vector<package_owner_t>, std::string>
list_owners(std::string_view name, const registry_session_t& session);

/// Invites a user, or adds a team, to own a package; or with `remove`, takes one off. `who` is a
/// user's name, or a team's as "team:<name>". Gives what the registry said it did.
[[nodiscard]] std::expected<std::string, std::string>
change_owner(std::string_view name, std::string_view who, bool remove,
             const registry_session_t& session);

/// A package search found.
struct found_package_t {
    std::string name;
    std::string version;
    std::string description;
};

/// The packages whose names, descriptions, or keywords have `words`, the most relevant first, at
/// most `limit` of them, and how many there are in all.
[[nodiscard]] std::expected<std::pair<std::vector<found_package_t>, std::size_t>, std::string>
search_packages(std::string_view words, std::size_t limit, const registry_session_t& session);

/// What fb login does that a test changes: opening the browser, and waiting between polls.
struct login_hooks_t {
    /// Opens `url` in the user's browser; false when it can't, and fb says to open it.
    std::function<bool(std::string_view url)> open;
    /// Waits as long as the registry asked, between polls.
    std::function<void(std::chrono::seconds)> wait;
    /// Shows the person the code the page shows, to check it's theirs, and the URL.
    std::function<void(std::string_view code, std::string_view url, bool opened)> show;
};

/// `fb login`'s sign-in: asks the registry for a sign-in named `name`, like "fb on hunter-pc",
/// opens its page, and polls until the person approves it there, then gives the token. Fails
/// when they deny it, it expires, or the registry can't be reached.
[[nodiscard]] std::expected<std::string, std::string>
login(std::string_view name, const registry_session_t& session, const login_hooks_t& hooks);

/// In a GitHub workflow, trusted publishing: asks GitHub for an OIDC token for the registry's
/// audience, through ACTIONS_ID_TOKEN_REQUEST_URL and ACTIONS_ID_TOKEN_REQUEST_TOKEN, and
/// exchanges it for a short-lived token. nullopt outside a workflow that can ask for one.
[[nodiscard]] std::optional<std::expected<std::string, std::string>>
trusted_publishing_token(const registry_session_t& session);

/// Revokes the token the session has, as trusted publishing does when it's done.
[[nodiscard]] std::expected<void, std::string> revoke_token(const registry_session_t& session);

/// The value of the environment variable `name`, as UTF-8, or nullopt when it isn't set.
[[nodiscard]] std::optional<std::string> environment_variable(std::string_view name);

} // namespace fb
