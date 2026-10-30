#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// An archive fb.lock pins: its SHA-256, as 64 hex digits, and its size, 0 when fb.lock doesn't
/// say.
struct locked_archive_t {
    std::string checksum;
    std::uint64_t size = 0;

    [[nodiscard]] bool operator==(const locked_archive_t& other) const = default;
};

/// One registry package fb.lock pins: its exact version, the index it's from, and each of its
/// archives. Moving it can allocate, as its map's comparator is moved, which clang-tidy reports
/// as an exception escaping.
struct locked_package_t { // NOLINT(bugprone-exception-escape)
    std::string name;
    std::string version;
    /// Its registry's index, as canonical_index() writes it, like
    /// "https://index.futureblitz.dev/", or for a registry on disk, its folder's file:// URL.
    std::string source;
    /// Its own archive.
    locked_archive_t archive;
    /// The names of the packages it depends on, which fb.lock has too.
    std::vector<std::string> dependencies;
    /// Each target's archive, for each target its version has.
    std::map<std::string, locked_archive_t, std::less<>> targets;

    [[nodiscard]] bool operator==(const locked_package_t& other) const = default;
};

/// fb.lock: the exact versions a package builds with, sorted by name.
struct lockfile_t {
    std::vector<locked_package_t> packages;

    /// The one named `name`, or null.
    [[nodiscard]] const locked_package_t* find(std::string_view name) const;

    [[nodiscard]] bool operator==(const lockfile_t& other) const = default;
};

/// The version of fb.lock's format fb writes and reads.
inline constexpr int lockfile_version = 1;

/// fb.lock's text, with its packages sorted by name, for the package in `root`. A registry on
/// disk is written as its folder from `root`, "path+../registry/index/", so the lock pins the
/// same versions wherever the package is.
[[nodiscard]] std::string write_lockfile(lockfile_t lockfile, const std::filesystem::path& root);

/// fb.lock of the package in `root`, read, or why it can't be: it isn't TOML, a package lacks
/// what it needs or has a broken list of targets or dependencies, two have one name, or it's of
/// another format's version. A registry on disk's folder is made a file:// URL again, from `root`.
[[nodiscard]] std::expected<lockfile_t, std::string>
read_lockfile(std::string_view text, const std::filesystem::path& root);

} // namespace fb
