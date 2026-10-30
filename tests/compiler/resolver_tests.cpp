#include "fb/resolver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view index = "https://index.example.com/";

/// A registry's packages, by name: each version, with its dependencies' ranges.
class packages_t {
public:
    packages_t& add(const std::string_view name, const std::string_view version,
                    const std::vector<std::pair<std::string, std::string>>& dependencies = {},
                    const bool yanked = false, std::optional<std::string> fb = std::nullopt) {
        fb::index_entry_t entry{
            .name = std::string(name),
            .version = std::string(version),
            .fb = std::move(fb),
            .source = { .sha256 = std::string(64, 'a'), .size = 1 },
            .yanked = yanked,
        };
        for (const auto& [dependency, req] : dependencies) {
            entry.dependencies.push_back({ .name = dependency, .req = req });
        }

        m_entries[std::string(name)].push_back(std::move(entry));
        return *this;
    }

    [[nodiscard]] fb::versions_of_t versions_of() {
        return [this](const std::string_view name, const std::string_view source)
                   -> std::expected<std::vector<fb::index_entry_t>, std::string> {
            ++asked;
            if (source != index) {
                return std::unexpected(std::format("{} can't be reached", source));
            }

            const auto found = m_entries.find(name);
            return found != m_entries.end() ? found->second : std::vector<fb::index_entry_t>{};
        };
    }

    int asked = 0;

private:
    std::map<std::string, std::vector<fb::index_entry_t>, std::less<>> m_entries;
};

fb::requirement_t wants(const std::string_view name, const std::string_view range,
                        const std::string_view who = "game") {
    return {
        .name = std::string(name),
        .range = *fb::parse_range(range),
        .source = std::string(index),
        .who = std::string(who),
    };
}

/// What the resolver picks, as "name version" each, or its error.
std::vector<std::string> picked(packages_t& packages, const std::vector<fb::requirement_t>& asked,
                                const fb::preferences_t& preferences = {},
                                const std::string_view fb_version = "0.1.0") {
    const std::expected<std::vector<fb::resolved_package_t>, std::string> resolved =
        fb::resolve(asked, packages.versions_of(), preferences, *fb::parse_version(fb_version));
    if (!resolved) {
        return { resolved.error() };
    }

    std::vector<std::string> out;
    for (const fb::resolved_package_t& package : *resolved) {
        out.push_back(std::format("{} {}", package.entry.name, package.entry.version));
    }

    return out;
}

using picks_t = std::vector<std::string>;

fb::lockfile_t locked(const std::vector<std::pair<std::string, std::string>>& versions) {
    fb::lockfile_t lockfile;
    for (const auto& [name, version] : versions) {
        lockfile.packages.push_back(
            { .name = name, .version = version, .source = std::string(index) });
    }

    return lockfile;
}

} // namespace

TEST_CASE("the resolver picks the newest version every range allows", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0", { { "shapes", "0.3" } })
        .add("physics", "1.2.0", { { "shapes", "0.3.2" } })
        .add("physics", "2.0.0", { { "shapes", "0.4" } })
        .add("shapes", "0.3.1")
        .add("shapes", "0.3.5")
        .add("shapes", "0.4.0");
    CHECK(picked(packages, { wants("physics", "1") }) ==
          picks_t{ "physics 1.2.0", "shapes 0.3.5" });
    CHECK(picked(packages, { wants("physics", "*") }) ==
          picks_t{ "physics 2.0.0", "shapes 0.4.0" });
    // Each registry's versions of a package are asked for once.
    packages.asked = 0;
    CHECK(picked(packages, { wants("physics", "1"), wants("shapes", "0.3") }) ==
          picks_t{ "physics 1.2.0", "shapes 0.3.5" });
    CHECK(packages.asked == 2);
}

TEST_CASE("the resolver backtracks to an older version when a newer one conflicts", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0", { { "shapes", "0.3" } })
        .add("physics", "1.1.0", { { "shapes", "0.4" } })
        .add("physics", "1.2.0", { { "shapes", "9" } })
        .add("shapes", "0.3.0")
        .add("shapes", "0.4.0");
    // physics 1.2.0's shapes doesn't exist, and 1.1.0's conflicts with the game's own.
    CHECK(picked(packages, { wants("physics", "1"), wants("shapes", "0.3") }) ==
          picks_t{ "physics 1.0.0", "shapes 0.3.0" });
}

TEST_CASE("a conflict says which ranges conflict, and who asks for each", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.2.3", { { "shapes", "0.3" } })
        .add("shapes", "0.3.0")
        .add("shapes", "0.4.0");
    CHECK(picked(packages, { wants("shapes", "0.4"), wants("physics", "1") }) ==
          picks_t{ "no version of shapes fits everything that asks for it: game asks for 0.4, "
                   "and physics 1.2.3 asks for 0.3" });
    CHECK(picked(packages, { wants("nope", "1") }) ==
          picks_t{ "there's no package nope in https://index.example.com/, which game asks for" });
    CHECK(picked(packages, { wants("shapes", "2") }) ==
          picks_t{ "no version of shapes fits 2, which game asks for; its newest are 0.4.0, "
                   "0.3.0, and this fb is 0.1.0" });

    fb::requirement_t elsewhere = wants("shapes", "0.3");
    elsewhere.source = "https://other.example.com/";
    CHECK(picked(packages, { wants("shapes", "0.3"), elsewhere }) ==
          picks_t{ "shapes comes from two registries: https://index.example.com/ and "
                   "https://other.example.com/" });
    CHECK(picked(packages, { elsewhere }) ==
          picks_t{ "https://other.example.com/ can't be reached" });
}

TEST_CASE("the resolver keeps fb.lock's versions while every range allows them", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0")
        .add("physics", "1.4.0")
        .add("shapes", "0.3.0")
        .add("shapes", "0.3.9");
    const fb::preferences_t preferences{ .locked = locked(
                                             { { "physics", "1.0.0" }, { "shapes", "0.3.0" } }) };
    CHECK(picked(packages, { wants("physics", "1"), wants("shapes", "0.3") }, preferences) ==
          picks_t{ "physics 1.0.0", "shapes 0.3.0" });
    // A range the locked version is outside picks anew.
    CHECK(picked(packages, { wants("physics", "1.1"), wants("shapes", "0.3") }, preferences) ==
          picks_t{ "physics 1.4.0", "shapes 0.3.0" });

    // fb update unlocks what it names, or everything.
    fb::preferences_t update = preferences;
    update.unlocked = { "shapes" };
    CHECK(picked(packages, { wants("physics", "1"), wants("shapes", "0.3") }, update) ==
          picks_t{ "physics 1.0.0", "shapes 0.3.9" });
    update.all = true;
    CHECK(picked(packages, { wants("physics", "1"), wants("shapes", "0.3") }, update) ==
          picks_t{ "physics 1.4.0", "shapes 0.3.9" });
}

TEST_CASE("yanked versions are only for a lock that has them", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0").add("physics", "1.1.0", {}, true);
    CHECK(picked(packages, { wants("physics", "1") }) == picks_t{ "physics 1.0.0" });
    CHECK(picked(packages, { wants("physics", "1") },
                 { .locked = locked({ { "physics", "1.1.0" } }) }) == picks_t{ "physics 1.1.0" });
    CHECK(picked(packages, { wants("physics", "1.1") }).front().contains("1.1.0 (yanked)"));
}

TEST_CASE("versions whose fb range this fb isn't in are skipped", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0", {}, false, ">=0.1")
        .add("physics", "1.1.0", {}, false, ">=0.5")
        .add("physics", "1.2.0", {}, false, "not a range");
    CHECK(picked(packages, { wants("physics", "1") }) == picks_t{ "physics 1.0.0" });
    CHECK(picked(packages, { wants("physics", "1") }, {}, "0.6.0") == picks_t{ "physics 1.1.0" });
}

TEST_CASE("pre-releases are picked only when a range names one", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0").add("physics", "1.1.0-beta.1");
    CHECK(picked(packages, { wants("physics", "1") }) == picks_t{ "physics 1.0.0" });
    CHECK(picked(packages, { wants("physics", "1.1.0-beta.1") }) ==
          picks_t{ "physics 1.1.0-beta.1" });
}
