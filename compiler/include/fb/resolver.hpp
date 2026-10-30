#pragma once

#include "fb/lockfile.hpp"
#include "fb/registry.hpp"
#include "fb/semver.hpp"

#include <expected>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// What a package asks of another: a range of its versions, from a registry.
struct requirement_t {
    std::string name;
    version_range_t range;
    /// The registry's canonical index.
    std::string source;
    /// Who asks, for messages: "game", or "physics 1.2.3".
    std::string who;
};

/// A version the resolver picked.
struct resolved_package_t {
    std::string source;
    index_entry_t entry;
};

/// The versions of a package in a registry, oldest first, as registry_t::versions() gives them.
using versions_of_t = std::function<std::expected<std::vector<index_entry_t>, std::string>(
    std::string_view name, std::string_view source)>;

/// What the resolver keeps of fb.lock, and what it can't.
struct preferences_t {
    /// The versions fb.lock has, which go first while every range allows them, and which can be
    /// yanked.
    lockfile_t locked;
    /// The packages `fb update` names, whose locked versions don't count. Every one with `all`.
    std::set<std::string, std::less<>> unlocked;
    bool all = false;
};

/// One version of each package that every range on it allows, from `requirements` and those of
/// each version picked, newest first, but fb.lock's first while it fits, skipping yanked versions
/// fb.lock doesn't have and those whose `fb` range `fb_version` isn't in. Sorted by name. Fails,
/// saying which ranges conflict and who asks for each, when nothing fits.
[[nodiscard]] std::expected<std::vector<resolved_package_t>, std::string>
resolve(const std::vector<requirement_t>& requirements, const versions_of_t& versions_of,
        const preferences_t& preferences, const version_t& fb_version);

} // namespace fb
