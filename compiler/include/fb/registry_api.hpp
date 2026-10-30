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
/// the registry checks them. Moving it can allocate, as its maps' comparators are moved, which
/// clang-tidy reports as an exception escaping.
struct prepared_package_t { // NOLINT(bugprone-exception-escape)
    manifest_t manifest;
    index_entry_t entry;
    built_archive_t source;
    std::vector<built_archive_t> targets;
};

/// Why `name` can't be a registry's package, or nullopt when it can be: lowercase ASCII letters,
/// digits, and '_', starting with a letter, at most 64, and not std, fb, futureblitz, apt, rpm, or
/// releases, even with '_'s.
[[nodiscard]] std::optional<std::string> registry_name_error(std::string_view name);

/// Why a target's archive, `file`, of `size` bytes, can't be published, or nullopt when it can:
/// a registry on disk takes up to 200 MB, and one on the network decides, as it raises its limit
/// for a package that asks.
[[nodiscard]] std::optional<std::string> target_size_error(std::string_view file,
                                                           std::uint64_t size, bool on_disk);

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
    /// The most an upload is in one request; a larger one goes in parts, as the registry takes
    /// them over 64 MB.
    std::uint64_t most_single_upload = std::uint64_t{ 64 } << 20U;
    /// The registry's API's URL, from its config.json, which a session fetches once.
    mutable std::string api;
    /// For trusted publishing's token, the packages the registry said it publishes, which
    /// publish_package() checks before it uploads anything.
    std::optional<std::vector<std::string>> trusted_packages;
    /// Waits as long as a registry that's limiting or unavailable asks, before a request is sent
    /// again; a test's waits for nothing.
    std::function<void(std::chrono::seconds)> wait;
};

/// The most docs JSON can be.
inline constexpr std::uint64_t most_docs_bytes = std::uint64_t{ 25 } << 20U;

/// A size as people read it, in the registry's units, where a KB is a thousand bytes and an MB a
/// million, as its limits are: "812 B", "48.2 KB", or "9.9 MB".
[[nodiscard]] std::string size_text(std::uint64_t bytes);

/// Publishes `package`, with `docs`, fb doc's JSON: for a registry on disk, its archives into its
/// `dl` and its line into its index file; for one on the network, each archive and the docs
/// uploaded, in parts when they're large, then the version published with them. Fails, saying
/// why, when the version is there already, or the registry refuses it, and for trusted
/// publishing's token, before anything's uploaded, when the package isn't one it publishes: a new
/// package's first version comes from a token of a person's. Gives the registry's warnings.
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

/// A pending invite to own a package: who it's for, and when it expires, as the registry writes
/// it, like "2026-11-13T09:30:00Z", or empty when it didn't say.
struct owner_invite_t {
    std::string invitee;
    std::string expires_at;
};

/// A package's owners, and the invites to own it that aren't accepted yet.
struct package_owners_t {
    std::vector<package_owner_t> owners;
    std::vector<owner_invite_t> invites;
};

/// A package's owners and pending invites, which the registry gives without a token.
[[nodiscard]] std::expected<package_owners_t, std::string>
list_owners(std::string_view name, const registry_session_t& session);

/// What change_owner() did: a verb, "Invited", "Added", or "Removed", and the rest of the line,
/// like "alice to own physics, which they accept on the package site".
struct owner_change_t {
    std::string verb;
    std::string message;
};

/// Invites a user, or adds a team, to own a package; or with `remove`, takes one off. `who` is a
/// user's name, or a team's as "team:<name>". Gives what the registry said it did.
[[nodiscard]] std::expected<owner_change_t, std::string>
change_owner(std::string_view name, std::string_view who, bool remove,
             const registry_session_t& session);

/// A package search found.
struct found_package_t {
    std::string name;
    std::string version;
    std::string description;
};

/// The packages whose names, descriptions, or keywords have every one of `words`, the most
/// relevant first, at most `limit` of them, and how many there are in all. A registry on disk has
/// only its packages' names to search, each of which has to have every word.
[[nodiscard]] std::expected<std::pair<std::vector<found_package_t>, std::size_t>, std::string>
search_packages(std::string_view words, std::size_t limit, const registry_session_t& session);

/// What fb login does that a test changes: opening the browser, and waiting between polls.
struct login_hooks_t {
    /// Opens `url` in the user's browser; false when it can't.
    std::function<bool(std::string_view url)> open;
    /// Waits as long as the registry asked, between polls.
    std::function<void(std::chrono::seconds)> wait;
    /// Shows the person the code the page shows, to check it's theirs, and the URL, before the
    /// page is opened; `opening` is false when it isn't, as it isn't https:// or on this machine.
    std::function<void(std::string_view code, std::string_view url, bool opening)> show;
};

/// A sign-in's token, and the account it's for, which is empty when the registry didn't say.
struct signed_in_t {
    std::string token;
    std::string username;
};

/// `fb login`'s sign-in: asks the registry for a sign-in named `name`, like "fb on hunter-pc",
/// opens its page, and polls until the person approves it there, then gives the token. Polls
/// that are rate limited, or that fail, are tried again until the sign-in would have expired.
/// Fails when they deny it, or it expires.
[[nodiscard]] std::expected<signed_in_t, std::string>
login(std::string_view name, const registry_session_t& session, const login_hooks_t& hooks);

/// The username of the account the session's token is for, which GET /me gives. Fails, with the
/// registry's words, for a token it doesn't take: one that's mistyped, revoked, or expired.
[[nodiscard]] std::expected<std::string, std::string> account_of(const registry_session_t& session);

/// Trusted publishing's token, and the packages the registry said it publishes, or nullopt when
/// it didn't say.
struct trusted_token_t {
    std::string token;
    std::optional<std::vector<std::string>> packages;
};

/// In a GitHub workflow, trusted publishing: asks GitHub for an OIDC token for the registry's
/// audience, through ACTIONS_ID_TOKEN_REQUEST_URL and ACTIONS_ID_TOKEN_REQUEST_TOKEN, and
/// exchanges it for a short-lived token. nullopt outside a workflow that can ask for one.
[[nodiscard]] std::optional<std::expected<trusted_token_t, std::string>>
trusted_publishing_token(const registry_session_t& session);

/// Revokes the token the session has, as trusted publishing does when it's done.
[[nodiscard]] std::expected<void, std::string> revoke_token(const registry_session_t& session);

/// The value of the environment variable `name`, as UTF-8, or nullopt when it isn't set.
[[nodiscard]] std::optional<std::string> environment_variable(std::string_view name);

} // namespace fb
