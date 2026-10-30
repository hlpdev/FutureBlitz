#include "fb/resolver.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <format>
#include <map>
#include <optional>
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

    /// A version whose one dependency is from `registry`, as its index line writes it.
    packages_t& add_from(const std::string_view name, const std::string_view version,
                         const std::string_view dependency, const std::string_view req,
                         const std::string_view registry) {
        add(name, version, { { std::string(dependency), std::string(req) } });
        m_entries[std::string(name)].back().dependencies.back().registry = std::string(registry);
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
                                const fb::preferences_t& preferences = fb::preferences_t(),
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

TEST_CASE("the resolver jumps back to the choice that conflicts, past unrelated ones",
          "[resolver]") {
    // resolve-1: physics 1.5.0 asks for shapes 0.4, which tools' 0.3 conflicts with, only once
    // each of the libraries between them is picked. Going back past them to physics, rather than
    // through every combination of theirs, finds physics 1.0.0 at once.
    packages_t packages;
    packages.add("shapes", "0.3.0")
        .add("shapes", "0.4.0")
        .add("physics", "1.0.0", { { "shapes", "0.3" } })
        .add("physics", "1.5.0", { { "shapes", "0.4" } })
        .add("tools", "2.0.0", { { "shapes", "0.3" } });
    std::vector<fb::requirement_t> asked{ wants("physics", "1") };
    picks_t expected{ "physics 1.0.0", "shapes 0.3.0", "tools 2.0.0" };
    for (int lib = 0; lib < 8; ++lib) {
        const std::string name = std::format("lib{}", lib);
        for (int minor = 0; minor < 10; ++minor) {
            packages.add(name, std::format("1.{}.0", minor));
        }

        asked.push_back(wants(name, "1"));
        expected.push_back(std::format("{} 1.9.0", name));
    }

    asked.push_back(wants("tools", "2"));
    std::ranges::sort(expected);
    CHECK(picked(packages, asked) == expected);

    // Unsolvable, it says so as soon, naming the conflict.
    packages.add("tools", "3.0.0", { { "shapes", "0.5" } });
    asked.back() = wants("tools", "3");
    const picks_t none = picked(packages, asked);
    REQUIRE(none.size() == 1);
    CHECK(none.front().starts_with("no version of shapes fits everything that asks for it"));
    CHECK(none.front().contains("tools 3.0.0 asks for 0.5"));

    // A conflict deep in one library's dependencies goes back to that library, not past it.
    packages_t deep;
    deep.add("a", "1.0.0", { { "c", "1" } })
        .add("a", "2.0.0", { { "b", "1" } })
        .add("b", "1.0.0", { { "c", "2" } })
        .add("c", "1.0.0")
        .add("c", "2.0.0")
        .add("d", "1.0.0", { { "c", "1" } });
    std::vector<fb::requirement_t> deep_asked{ wants("a", "*") };
    for (int lib = 0; lib < 6; ++lib) {
        const std::string name = std::format("x{}", lib);
        for (int minor = 0; minor < 10; ++minor) {
            deep.add(name, std::format("1.{}.0", minor));
        }

        deep_asked.push_back(wants(name, "1"));
    }

    deep_asked.push_back(wants("d", "1"));
    const picks_t found = picked(deep, deep_asked);
    REQUIRE(found.size() == 9);
    CHECK(found[0] == "a 1.0.0");
    CHECK(found[1] == "c 1.0.0");
}

TEST_CASE("a conflict says which ranges conflict, and who asks for each", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.2.3", { { "shapes", "0.3" } })
        .add("shapes", "0.3.0")
        .add("shapes", "0.4.0");
    CHECK(picked(packages, { wants("shapes", "0.4"), wants("physics", "1") }) ==
          picks_t{ "no version of shapes fits everything that asks for it: game asks for 0.4, "
                   "and physics 1.2.3 asks for 0.3" });
    // resolve-5: of two versions that conflict, the newest is named, as it'd be picked.
    packages_t two;
    two.add("shapes", "0.2.0")
        .add("shapes", "0.3.0")
        .add("shapes", "0.4.0")
        .add("body", "1.0.0", { { "shapes", "0.2" } })
        .add("body", "1.2.3", { { "shapes", "0.3" } });
    CHECK(picked(two, { wants("shapes", "0.4"), wants("body", "1") }) ==
          picks_t{ "no version of shapes fits everything that asks for it: game asks for 0.4, "
                   "and body 1.2.3 asks for 0.3" });
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
    const fb::lockfile_t lockfile = locked({ { "physics", "1.0.0" }, { "shapes", "0.3.0" } });
    const fb::preferences_t preferences{ .locked = lockfile };
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

    // resolve-3: fb update resolves anew, so a yanked version it unlocks isn't kept.
    fb::preferences_t update{ .locked = locked({ { "physics", "1.1.0" } }), .all = true };
    CHECK(picked(packages, { wants("physics", "1") }, update) == picks_t{ "physics 1.0.0" });
    update.all = false;
    update.unlocked = { "physics" };
    CHECK(picked(packages, { wants("physics", "1") }, update) == picks_t{ "physics 1.0.0" });
}

TEST_CASE("an index line's registry is its canonical index, through the program's mirror",
          "[resolver]") {
    // resolve-8: a dependency's registry is compared as canonical_index() writes it, and
    // futureblitz.dev's is the mirror [registries] names for it.
    packages_t packages;
    packages.add("shapes", "0.3.0")
        .add_from("tools", "1.0.0", "shapes", "0.3", "https://index.futureblitz.dev/")
        .add_from("tool2", "1.0.0", "shapes", "0.3", "https://index.example.com");
    const fb::preferences_t mirrored{
        .mirrors = { { "https://index.futureblitz.dev/", std::string(index) } },
    };
    CHECK(picked(packages, { wants("tools", "1"), wants("shapes", "0.3") }, mirrored) ==
          picks_t{ "shapes 0.3.0", "tools 1.0.0" });
    CHECK(picked(packages, { wants("tool2", "1"), wants("shapes", "0.3") }) ==
          picks_t{ "shapes 0.3.0", "tool2 1.0.0" });
    // Without the mirror, futureblitz.dev's is another registry.
    CHECK(picked(packages, { wants("tools", "1"), wants("shapes", "0.3") }).front() ==
          "shapes comes from two registries: https://index.example.com/ and "
          "https://index.futureblitz.dev/");
}

TEST_CASE("versions whose fb range this fb isn't in are skipped", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0", {}, false, ">=0.1")
        .add("physics", "1.1.0", {}, false, ">=0.5")
        .add("physics", "1.2.0", {}, false, "not a range");
    CHECK(picked(packages, { wants("physics", "1") }) == picks_t{ "physics 1.0.0" });
    CHECK(picked(packages, { wants("physics", "1") }, fb::preferences_t(), "0.6.0") ==
          picks_t{ "physics 1.1.0" });
}

TEST_CASE("pre-releases are picked only when a range names one", "[resolver]") {
    packages_t packages;
    packages.add("physics", "1.0.0").add("physics", "1.1.0-beta.1");
    CHECK(picked(packages, { wants("physics", "1") }) == picks_t{ "physics 1.0.0" });
    CHECK(picked(packages, { wants("physics", "1.1.0-beta.1") }) ==
          picks_t{ "physics 1.1.0-beta.1" });
}
