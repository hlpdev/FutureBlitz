#include "fb/resolver.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The most versions the resolver tries before it gives up on a program whose ranges it can't
/// settle soon.
constexpr std::size_t most_tries = 100'000;

/// A version it can pick, parsed.
struct candidate_t {
    version_t version;
    const index_entry_t* entry = nullptr;
};

class resolver_t {
public:
    resolver_t(const versions_of_t& versions_of, const preferences_t& preferences,
               const version_t& fb_version)
        : m_versions_of(versions_of), m_preferences(preferences), m_fb_version(fb_version) {}

    std::expected<std::vector<resolved_package_t>, std::string>
    run(std::vector<requirement_t> requirements) {
        m_requirements = std::move(requirements);
        m_roots = m_requirements.size();
        m_origins.assign(m_roots, -1);
        if (solve(0)) {
            if (m_error) {
                return std::unexpected(*m_error);
            }

            return std::unexpected(m_conflict.empty()
                                       ? std::string("the dependencies' ranges can't all be met")
                                       : m_conflict);
        }

        std::vector<resolved_package_t> out;
        out.reserve(m_chosen.size());
        for (const auto& [name, chosen] : m_chosen) {
            out.push_back({ .source = chosen.source, .entry = *chosen.entry });
        }

        return out;
    }

private:
    struct chosen_t {
        std::string source;
        const index_entry_t* entry = nullptr;
        version_t version;
        /// How deep among the picks it was picked.
        int depth = 0;
    };

    const versions_of_t& m_versions_of;
    const preferences_t& m_preferences;
    const version_t& m_fb_version;
    std::vector<requirement_t> m_requirements;
    /// The depth of the pick each requirement came from, or -1 for the program's own.
    std::vector<int> m_origins;
    /// How many picks there are now.
    int m_depth = 0;
    /// How many of them the program and its path dependencies ask for, before those of the
    /// versions picked.
    std::size_t m_roots = 0;
    std::map<std::string, chosen_t, std::less<>> m_chosen;
    /// Each registry's packages' versions, fetched once.
    std::map<std::pair<std::string, std::string>, std::vector<index_entry_t>> m_versions;
    /// A failure that ends the search, like an index that can't be read.
    std::optional<std::string> m_error;
    /// The conflict found furthest into the requirements, which is what's said when nothing
    /// fits.
    std::string m_conflict;
    std::size_t m_furthest = 0;
    std::size_t m_tries = 0;

    void conflict(const std::size_t at, std::string message) {
        // The first at the furthest point is the newest version's, which is tried first.
        if (m_conflict.empty() || at > m_furthest) {
            m_furthest = at;
            m_conflict = std::move(message);
        }
    }

    /// Who asked for `name` before requirement `at`, and for what: "game asks for 0.4, and
    /// physics 1.2.3 asks for 0.3".
    [[nodiscard]] std::string askers(const std::string_view name, const std::size_t at) const {
        std::string out;
        for (std::size_t i = 0; i <= at && i < m_requirements.size(); ++i) {
            const requirement_t& each = m_requirements[i];
            if (each.name == name) {
                out += std::format("{}{} asks for {}", out.empty() ? "" : ", and ", each.who,
                                   each.range.text);
            }
        }

        return out;
    }

    const std::vector<index_entry_t>* versions(const requirement_t& requirement) {
        const std::pair key(requirement.source, requirement.name);
        if (const auto found = m_versions.find(key); found != m_versions.end()) {
            return &found->second;
        }

        std::expected<std::vector<index_entry_t>, std::string> fetched =
            m_versions_of(requirement.name, requirement.source);
        if (!fetched) {
            m_error = fetched.error();
            return nullptr;
        }

        return &m_versions.emplace(key, std::move(*fetched)).first->second;
    }

    /// The versions `requirement` allows, the locked one first, then newest first.
    [[nodiscard]] std::vector<candidate_t> candidates(const requirement_t& requirement,
                                                      const std::vector<index_entry_t>& all) const {
        const locked_package_t* locked = m_preferences.locked.find(requirement.name);
        const bool keep = locked != nullptr && locked->source == requirement.source &&
                          !m_preferences.all && !m_preferences.unlocked.contains(requirement.name);
        std::vector<candidate_t> out;
        for (const index_entry_t& entry : all) {
            const std::expected<version_t, std::string> version = parse_version(entry.version);
            if (!version || !requirement.range.matches(*version)) {
                continue;
            }

            // A yanked version is only for a lock that has it.
            const bool is_locked = locked != nullptr && locked->source == requirement.source &&
                                   locked->version == entry.version;
            if (entry.yanked && !(is_locked && keep)) {
                continue;
            }

            if (entry.fb) {
                const std::expected<version_range_t, std::string> fb = parse_range(*entry.fb);
                if (!fb || !fb->matches(m_fb_version)) {
                    continue;
                }
            }

            out.push_back({ .version = *version, .entry = &entry });
        }

        std::ranges::sort(out, [&](const candidate_t& left, const candidate_t& right) {
            const bool left_locked = keep && left.entry->version == locked->version;
            const bool right_locked = keep && right.entry->version == locked->version;
            if (left_locked != right_locked) {
                return left_locked;
            }

            return left.version > right.version;
        });
        return out;
    }

    /// Why no version of a package fits a requirement by itself.
    [[nodiscard]] std::string none_fits(const requirement_t& requirement,
                                        const std::vector<index_entry_t>& all) const {
        if (all.empty()) {
            return std::format("there's no package {} in {}, which {} asks for", requirement.name,
                               requirement.source, requirement.who);
        }

        std::string listed;
        for (const index_entry_t& entry : all | std::views::reverse | std::views::take(5)) {
            listed += std::format("{}{}{}", listed.empty() ? "" : ", ", entry.version,
                                  entry.yanked ? " (yanked)" : "");
        }

        return std::format("no version of {} fits {}, which {} asks for; its newest are {}, and "
                           "this fb is {}",
                           requirement.name, requirement.range.text, requirement.who, listed,
                           m_fb_version.text());
    }

    /// The choices a failure came from: the depths of the picks whose versions, or whose
    /// requirements, made it. -1, the program's own, isn't one.
    using culprits_t = std::set<int>;

    /// A failure's culprits, without the program's own.
    static culprits_t culprits_of(const std::initializer_list<int> depths) {
        culprits_t out;
        for (const int depth : depths) {
            if (depth >= 0) {
                out.insert(depth);
            }
        }

        return out;
    }

    /// Settles the requirements from `at` on: nullopt when they're met, or else the choices the
    /// failure came from, so a choice that isn't among them is skipped back over, not tried with
    /// each of its other versions (conflict-directed backjumping).
    std::optional<culprits_t> solve(const std::size_t at) {
        if (at == m_requirements.size()) {
            return std::nullopt;
        }

        if (m_error || ++m_tries > most_tries) {
            if (!m_error) {
                m_error = "the dependencies' ranges took too many tries to settle; narrowing "
                          "them in project.toml helps";
            }

            return culprits_t{};
        }

        const requirement_t requirement = m_requirements[at];
        const int origin = m_origins[at];
        if (const auto chosen = m_chosen.find(requirement.name); chosen != m_chosen.end()) {
            if (chosen->second.source != requirement.source) {
                conflict(at,
                         std::format("{} comes from two registries: {} and {}", requirement.name,
                                     chosen->second.source, requirement.source));
                return culprits_of({ origin, chosen->second.depth });
            }

            if (!requirement.range.matches(chosen->second.version)) {
                conflict(at, std::format("no version of {} fits everything that asks for it: {}",
                                         requirement.name, askers(requirement.name, at)));
                return culprits_of({ origin, chosen->second.depth });
            }

            return solve(at + 1);
        }

        const std::vector<index_entry_t>* all = versions(requirement);
        if (all == nullptr) {
            return culprits_t{};
        }

        const std::vector<candidate_t> fits = candidates(requirement, *all);
        if (fits.empty() && at < m_roots) {
            // Nothing else can change what the program itself asks for, so it's what's said.
            m_error = none_fits(requirement, *all);
            return culprits_t{};
        }

        if (fits.empty()) {
            conflict(at, none_fits(requirement, *all));
            return culprits_of({ origin });
        }

        const int depth = m_depth++;
        culprits_t failed = culprits_of({ origin });
        for (const candidate_t& candidate : fits) {
            m_chosen.emplace(requirement.name, chosen_t{
                                                   .source = requirement.source,
                                                   .entry = candidate.entry,
                                                   .version = candidate.version,
                                                   .depth = depth,
                                               });
            const std::size_t before = m_requirements.size();
            bool parsed = true;
            for (const index_dependency_t& dependency : candidate.entry->dependencies) {
                std::expected<version_range_t, std::string> range = parse_range(dependency.req);
                if (!range) {
                    parsed = false;
                    break;
                }

                m_requirements.push_back({
                    .name = dependency.name,
                    .range = std::move(*range),
                    .source = source_of(dependency, requirement.source),
                    .who = std::format("{} {}", requirement.name, candidate.entry->version),
                });
                m_origins.push_back(depth);
            }

            std::optional<culprits_t> result = parsed ? solve(at + 1) : culprits_t{ depth };
            if (!result) {
                return std::nullopt;
            }

            m_requirements.resize(before);
            m_origins.resize(before);
            m_chosen.erase(requirement.name);
            if (m_error) {
                --m_depth;
                return culprits_t{};
            }

            // A failure this choice had no part in fails with any of its versions, so it's
            // passed back to the choice that did.
            if (!result->contains(depth)) {
                --m_depth;
                return result;
            }

            result->erase(depth);
            failed.insert(result->begin(), result->end());
        }

        --m_depth;
        return failed;
    }

    /// A dependency's registry: its package's own, or another's index, canonical, and through
    /// the program's mirror of it, if it has one.
    [[nodiscard]] std::string source_of(const index_dependency_t& dependency,
                                        const std::string& own) const {
        if (!dependency.registry) {
            return own;
        }

        const std::string index = canonical_index(*dependency.registry, "");
        const auto mirror = m_preferences.mirrors.find(index);
        return mirror != m_preferences.mirrors.end() ? mirror->second : index;
    }
};

} // namespace

std::expected<std::vector<resolved_package_t>, std::string>
resolve(const std::vector<requirement_t>& requirements, const versions_of_t& versions_of,
        const preferences_t& preferences, const version_t& fb_version) {
    return resolver_t(versions_of, preferences, fb_version).run(requirements);
}

} // namespace fb
