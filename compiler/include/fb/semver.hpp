#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// A package's version, as semver 2.0 has it: three numbers, and maybe a pre-release, like
/// 1.0.0-beta.2. Build metadata isn't taken, since two versions that differ only there would be
/// one version to a registry.
struct version_t {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    /// The pre-release's identifiers, like { "beta", "2" }. Empty for a release.
    std::vector<std::string> pre;

    /// As semver orders versions: by their numbers, then a pre-release before its release, and
    /// pre-releases by their identifiers, numbers by their values and before words.
    [[nodiscard]] std::strong_ordering operator<=>(const version_t& other) const;
    [[nodiscard]] bool operator==(const version_t& other) const = default;

    /// Like "1.0.0-beta.2".
    [[nodiscard]] std::string text() const;
};

/// A version from its text, like "1.2.3" or "1.0.0-rc.1", or why it isn't one.
[[nodiscard]] std::expected<version_t, std::string> parse_version(std::string_view text);

/// One condition of a range, on a whole version.
struct comparator_t {
    enum class op_t : std::uint8_t {
        EQUAL,
        GREATER,
        GREATER_EQUAL,
        LESS,
        LESS_EQUAL,
    };

    op_t op = op_t::EQUAL;
    version_t version;
};

/// A range of versions, as Cargo writes them: "1.2" is ^1.2, ">=1.2.0, <2.0.0", and "~1.2",
/// "=1.2.3", ">=1.2, <1.5", and "1.*" are the rest. Its comparators all hold for a version in it.
struct version_range_t {
    /// As written, for messages.
    std::string text;
    /// What the text means, as comparators of whole versions. Empty for any version.
    std::vector<comparator_t> comparators;

    /// Whether `version` is in the range. A pre-release is only when a comparator names a
    /// pre-release of the same three numbers, so "1" never picks 2.0.0-beta.1 or 1.5.0-rc.1.
    [[nodiscard]] bool matches(const version_t& version) const;
};

/// A range from its text, or why it isn't one.
[[nodiscard]] std::expected<version_range_t, std::string> parse_range(std::string_view text);

} // namespace fb
