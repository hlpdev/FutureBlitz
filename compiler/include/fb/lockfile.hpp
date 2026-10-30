#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// One registry package fb.lock pins: its exact version, the index it's from, and the SHA-256 of
/// each of its archives.
struct locked_package_t {
    std::string name;
    std::string version;
    /// Its registry's index, as canonical_index() writes it, like
    /// "https://index.futureblitz.dev/".
    std::string source;
    /// Its own archive's SHA-256, as 64 hex digits.
    std::string checksum;
    /// The names of the packages it depends on, which fb.lock has too.
    std::vector<std::string> dependencies;
    /// Each target's archive's SHA-256, for each target its version has.
    std::map<std::string, std::string, std::less<>> targets;

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

/// fb.lock's text, with its packages sorted by name.
[[nodiscard]] std::string write_lockfile(lockfile_t lockfile);

/// fb.lock, read, or why it can't be: it isn't TOML, a package lacks what it needs, two have one
/// name, or it's of another format's version.
[[nodiscard]] std::expected<lockfile_t, std::string> read_lockfile(std::string_view text);

} // namespace fb
