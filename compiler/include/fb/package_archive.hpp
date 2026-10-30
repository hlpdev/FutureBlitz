#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// The targets a package can ship prebuilt files for, as `{target}` names them.
inline constexpr std::array<std::string_view, 5> package_targets = {
    // clang-format packs the names on one line, which the comma keeps from being aligned.
    // NOLINTNEXTLINE(readability-trailing-comma)
    "windows-x64", "linux-x64", "linux-arm64", "macos-arm64", "macos-x64",
};

/// The most files an archive can have, and the longest path, in bytes, as the registry takes
/// them.
inline constexpr std::size_t most_archive_files = 20'000;
inline constexpr std::size_t longest_archive_path = 255;

/// The most a package's own archive, and a target's, can be, as the registry takes them, and the
/// most each can unpack to: the registry's 8 times its limit for a package's own, and for a
/// target's, whose limit the registry raises for a package that asks, 4 GB.
inline constexpr std::uint64_t most_source_archive = std::uint64_t{ 10 } << 20U;
inline constexpr std::uint64_t most_target_archive = std::uint64_t{ 200 } << 20U;
inline constexpr std::uint64_t most_source_unpacked = most_source_archive * 8;
inline constexpr std::uint64_t most_target_unpacked = std::uint64_t{ 4 } << 30U;

/// A file in an archive: its path in the package, with '/'s, and its bytes.
struct archive_file_t {
    std::string path;
    std::string bytes;
};

/// The SHA-256 of `bytes`, as 64 lowercase hex digits.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

/// Why a path can't be in an archive, or nullopt when it can, as docs/registry-api.md's checks
/// say: it's relative, UTF-8, at most longest_archive_path bytes, with no empty, ".", or ".."
/// part, no control characters or `\ : < > " | ? *`, and no part Windows can't name, like NUL, or
/// one ending in '.' or ' '.
[[nodiscard]] std::optional<std::string> archive_path_error(std::string_view path);

/// A .fbpkg of `files`: a POSIX ustar tar, with a pax header for a path ustar can't hold,
/// compressed with gzip, of the files in their paths' byte order, each with mode 0644, no owner,
/// and the time 0, so the same files make the same tar. Fails for a path archive_path_error()
/// refuses.
[[nodiscard]] std::expected<std::string, std::string>
write_archive(std::vector<archive_file_t> files);

/// The files of a .fbpkg, checked as unpacking them would be: only regular files (and folders,
/// which are left out), each path one archive_path_error() takes, no two the same ignoring
/// letter case, and at most most_archive_files of them and `most` bytes together. Fails, saying
/// why, for anything else, or for bytes that aren't a .fbpkg.
[[nodiscard]] std::expected<std::vector<archive_file_t>, std::string>
read_archive(std::string_view bytes, std::uint64_t most);

/// Unpacks `bytes`, a .fbpkg, into `folder`, which it makes, checked as read_archive() checks it:
/// into a folder beside it first, renamed once it's whole, so a folder that's there is whole.
/// Then it writes `marker` in it, a file `.fbpkg-<marker>` holding `checksum`. A folder that's
/// there already is left as it is, and has another marker added, which is how a target's archive
/// joins the package's own.
[[nodiscard]] std::expected<void, std::string>
unpack_archive(std::string_view bytes, const std::filesystem::path& folder, std::string_view marker,
               std::string_view checksum, std::uint64_t most);

/// What goes in a package's archives, by their paths in it: `source` for
/// `<name>-<version>.fbpkg`, and for each target that has files, `targets[target]` for
/// `<name>-<version>-<target>.fbpkg`, the files in its `lib/<target>/` and `native/<target>/`.
/// Neither has build/, tests/, fb.lock, hidden files and folders, or folders with a project.toml
/// of their own. Moving it can allocate, as its map's comparator is moved, which clang-tidy
/// reports as an exception escaping.
struct package_files_t { // NOLINT(bugprone-exception-escape)
    std::vector<std::string> source;
    std::map<std::string, std::vector<std::string>, std::less<>> targets;
};

/// The files of the package in `root`, sorted, for its archives.
[[nodiscard]] package_files_t package_files(const std::filesystem::path& root);

} // namespace fb
