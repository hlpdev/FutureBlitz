#pragma once

#include "fb/http_client.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// futureblitz.dev's index, the default registry's.
inline constexpr std::string_view default_index = "https://index.futureblitz.dev/";

/// A dependency of a version, as its index line has it.
struct index_dependency_t {
    std::string name;
    /// Its range, as written, like "0.3".
    std::string req;
    /// Another registry's index, as canonical_index() writes it, or nullopt for the index's own.
    std::optional<std::string> registry;

    [[nodiscard]] bool operator==(const index_dependency_t& other) const = default;
};

/// An archive an index line names: its SHA-256, as 64 hex digits, and its size.
struct archive_ref_t {
    std::string sha256;
    std::uint64_t size = 0;

    [[nodiscard]] bool operator==(const archive_ref_t& other) const = default;
};

/// One version of a package, a line of its index file, as docs/registry-api.md has it. Moving
/// it can allocate, as its map's comparator is moved, which clang-tidy reports as an exception
/// escaping.
struct index_entry_t { // NOLINT(bugprone-exception-escape)
    std::string name;
    std::string version;
    /// Sorted by name.
    std::vector<index_dependency_t> dependencies;
    /// The range of fb's versions it builds with, or nullopt for any.
    std::optional<std::string> fb;
    /// Its own archive, `<name>-<version>.fbpkg`.
    archive_ref_t source;
    /// Each target's archive, `<name>-<version>-<target>.fbpkg`, for the targets it has one for.
    std::map<std::string, archive_ref_t, std::less<>> targets;
    bool yanked = false;

    [[nodiscard]] bool operator==(const index_entry_t& other) const = default;
};

/// The version of an index line's format fb reads and writes. A line of a later one is skipped.
inline constexpr int index_format = 1;

/// A package's index file's lines, each one version, read, or why they can't be: a line that
/// isn't JSON, or one that isn't a version of `name`. A line of a later format than fb knows is
/// left out, as if its version weren't there.
[[nodiscard]] std::expected<std::vector<index_entry_t>, std::string>
parse_index(std::string_view text, std::string_view name);

/// A version's line in its package's index file, without its line break.
[[nodiscard]] std::string index_line(const index_entry_t& entry);

/// A registry's config.json: where its archives are, a template of `{name}`, `{version}`, and
/// `{file}`, its API's URL, and the audience a GitHub workflow's OIDC token is for. A registry on
/// disk has only `dl`, a folder or a file:// URL.
struct registry_config_t {
    std::string dl;
    std::string api;
    std::string oidc_audience;
};

/// config.json, read, or why it can't be.
[[nodiscard]] std::expected<registry_config_t, std::string>
parse_registry_config(std::string_view text);

/// Where `dl`, config.json's, has an archive: its `{name}`, `{version}`, and `{file}` filled in,
/// or for one without them, those after it, separated by '/'s.
[[nodiscard]] std::string download_url(std::string_view dl, std::string_view name,
                                       std::string_view version, std::string_view file);

/// What a registry's API's error answer says, from its `{ "errors": [{ "code", "detail" }] }`, or
/// else its status.
[[nodiscard]] std::string api_error(const http_response_t& response);

/// fb's folder for the cache: FB_HOME, or else .futureblitz in the user's home folder.
[[nodiscard]] std::filesystem::path fb_home();

/// An index as fb.lock records it: an http:// or https:// URL ending in '/', or for a registry
/// on disk, a file:// URL of its folder's absolute path, ending in '/'. A folder's path is from
/// `base`.
[[nodiscard]] std::string canonical_index(std::string_view index,
                                          const std::filesystem::path& base);

/// The folder of a registry on disk, from its canonical file:// index. Empty for an http:// one.
[[nodiscard]] std::filesystem::path local_index_folder(std::string_view canonical);

/// A registry's folder in the cache, by its host, or its folder's name, and its URL's hash:
/// "index.futureblitz.dev-1a2b3c4d5e6f7a8b".
[[nodiscard]] std::string registry_folder(std::string_view canonical);

/// Where a version of a package from `source`, a canonical index, is unpacked in the cache.
[[nodiscard]] std::filesystem::path
package_cache_folder(std::string_view source, std::string_view name, std::string_view version);

/// An archive's file name: "<name>-<version>.fbpkg", or with "-<target>" for a target's.
[[nodiscard]] std::string archive_name(std::string_view name, std::string_view version,
                                       std::string_view target = {});

/// What a command says as it works, like ("Downloading", "physics v1.2.3").
using status_t = std::function<void(std::string_view verb, std::string_view message)>;

/// A registry, through its index and archives, cached in fb_home(): the index files fetched
/// with their ETags, and each archive checked against its SHA-256. Offline, or when the
/// registry can't be reached, it uses what's in the cache, and notes that it did.
class registry_t {
public:
    /// `canonical` is from canonical_index(); `http` is unused for a registry on disk.
    registry_t(std::string canonical, http_client_t http, bool offline, status_t status);

    [[nodiscard]] const std::string& index() const {
        return m_index;
    }

    [[nodiscard]] bool local() const {
        return !m_folder.empty();
    }

    /// The versions of `name`, oldest first, or an empty list when the registry has none.
    /// Fails when the registry can't be read and the cache has nothing of it.
    [[nodiscard]] std::expected<std::vector<index_entry_t>, std::string>
    versions(std::string_view name);

    /// Its config.json, fetched once.
    [[nodiscard]] std::expected<registry_config_t, std::string> config();

    /// Where the archive `file` of `name` and `version` is: in the cache, downloaded into it a
    /// piece at a time if it isn't, or a registry on disk's own, whose SHA-256 has to be
    /// `checksum`.
    [[nodiscard]] std::expected<std::filesystem::path, std::string>
    archive(std::string_view name, std::string_view version, std::string_view file,
            std::string_view checksum);

    /// What it used from the cache because it couldn't reach the registry, to tell the user.
    [[nodiscard]] const std::vector<std::string>& notes() const {
        return m_notes;
    }

private:
    std::string m_index;
    /// A registry on disk's folder, or empty.
    std::filesystem::path m_folder;
    http_client_t m_http;
    bool m_offline = false;
    status_t m_status;
    std::optional<registry_config_t> m_config;
    std::vector<std::string> m_notes;
    /// It couldn't be reached once, so it isn't tried again.
    bool m_unreachable = false;
};

} // namespace fb
