#include "fb/semver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace {

fb::version_t version(const std::string_view text) {
    const std::expected<fb::version_t, std::string> parsed = fb::parse_version(text);
    INFO(text);
    REQUIRE(parsed);
    return *parsed;
}

/// Whether `range` takes `text`.
bool takes(const std::string_view range, const std::string_view text) {
    const std::expected<fb::version_range_t, std::string> parsed = fb::parse_range(range);
    INFO(range);
    REQUIRE(parsed);
    return parsed->matches(version(text));
}

std::string range_error(const std::string_view range) {
    const std::expected<fb::version_range_t, std::string> parsed = fb::parse_range(range);
    return parsed ? std::string() : parsed.error();
}

} // namespace

TEST_CASE("a version is three numbers and maybe a pre-release", "[semver]") {
    const fb::version_t release = version("1.20.3");
    CHECK(release.major == 1);
    CHECK(release.minor == 20);
    CHECK(release.patch == 3);
    CHECK(release.pre.empty());
    CHECK(release.text() == "1.20.3");
    CHECK(version("1.0.0-beta.2").pre == std::vector<std::string>{ "beta", "2" });
    CHECK(version("1.0.0-beta.2").text() == "1.0.0-beta.2");
    CHECK(version("0.0.0-x-y.0").text() == "0.0.0-x-y.0");

    const std::vector<std::string_view> wrongs{
        "1.2",           "1.2.3.4", "01.2.3", "1.02.3", "1.2.3-",    "1.2.3-beta..1",
        "1.2.3-beta.01", "1.2.x",   "",       "v1.2.3", "1.2.3-b@d", "99999999999999999999.0.0",
    };
    for (const std::string_view wrong : wrongs) {
        INFO(wrong);
        CHECK_FALSE(fb::parse_version(wrong));
    }

    CHECK(fb::parse_version("1.2.3+build.5").error().contains("build metadata"));
}

TEST_CASE("versions order as semver orders them", "[semver]") {
    const std::vector<std::string_view> ordered{
        "0.9.9",      "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta",
        "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",
        "1.0.0",      "1.0.1",        "1.10.0",        "2.0.0",
    };
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        INFO(ordered[i - 1] << " < " << ordered[i]);
        CHECK(version(ordered[i - 1]) < version(ordered[i]));
        CHECK(version(ordered[i]) > version(ordered[i - 1]));
    }

    CHECK(version("1.0.0") == version("1.0.0"));
}

TEST_CASE("a bare range is a caret range, of what's compatible", "[semver]") {
    CHECK(takes("1.2", "1.2.0"));
    CHECK(takes("1.2", "1.9.7"));
    CHECK_FALSE(takes("1.2", "1.1.9"));
    CHECK_FALSE(takes("1.2", "2.0.0"));
    CHECK(takes("1.2.3", "1.2.3"));
    CHECK_FALSE(takes("1.2.3", "1.2.2"));
    CHECK(takes("^1.2.3", "1.4.0"));
    CHECK(takes("1", "1.99.0"));
    CHECK_FALSE(takes("1", "2.0.0"));

    // Below 1.0, the leftmost number that isn't 0 breaks.
    CHECK(takes("0.4", "0.4.9"));
    CHECK_FALSE(takes("0.4", "0.5.0"));
    CHECK(takes("0.4.1", "0.4.2"));
    CHECK_FALSE(takes("0.4.1", "0.4.0"));
    CHECK(takes("0.0.3", "0.0.3"));
    CHECK_FALSE(takes("0.0.3", "0.0.4"));
    CHECK(takes("0.0", "0.0.7"));
    CHECK_FALSE(takes("0.0", "0.1.0"));
    CHECK(takes("0", "0.9.0"));
    CHECK_FALSE(takes("0", "1.0.0"));
}

TEST_CASE("tilde, exact, comparison, and wildcard ranges", "[semver]") {
    CHECK(takes("~1.2.3", "1.2.9"));
    CHECK_FALSE(takes("~1.2.3", "1.3.0"));
    CHECK_FALSE(takes("~1.2.3", "1.2.2"));
    CHECK(takes("~1.2", "1.2.0"));
    CHECK(takes("~1", "1.5.0"));
    CHECK_FALSE(takes("~1", "2.0.0"));

    CHECK(takes("=1.2.3", "1.2.3"));
    CHECK_FALSE(takes("=1.2.3", "1.2.4"));
    CHECK(takes("=1.2", "1.2.8"));
    CHECK_FALSE(takes("=1.2", "1.3.0"));

    CHECK(takes(">=1.2", "1.2.0"));
    CHECK(takes(">=1.2", "5.0.0"));
    CHECK_FALSE(takes(">1.2", "1.2.9"));
    CHECK(takes(">1.2", "1.3.0"));
    CHECK(takes(">1.2.3", "1.2.4"));
    CHECK_FALSE(takes("<2", "2.0.0"));
    CHECK(takes("<2", "1.9.9"));
    CHECK(takes("<=1.2", "1.2.9"));
    CHECK_FALSE(takes("<=1.2", "1.3.0"));
    CHECK(takes("<=1.4.0", "1.4.0"));
    CHECK(takes(">= 1.2, < 1.5", "1.4.9"));
    CHECK_FALSE(takes(">=1.2, <1.5", "1.5.0"));

    CHECK(takes("*", "0.0.1"));
    CHECK(takes("*", "99.0.0"));
    CHECK(takes("1.*", "1.7.0"));
    CHECK_FALSE(takes("1.*", "2.0.0"));
    CHECK(takes("1.2.*", "1.2.5"));
    CHECK_FALSE(takes("1.2.*", "1.3.0"));
}

TEST_CASE("a range takes a pre-release only when it names one of the same numbers", "[semver]") {
    CHECK_FALSE(takes("1", "2.0.0-beta.1"));
    CHECK_FALSE(takes("1", "1.5.0-rc.1"));
    CHECK_FALSE(takes("*", "1.0.0-alpha"));
    CHECK(takes("^1.2.0-beta.1", "1.2.0-beta.2"));
    CHECK(takes("^1.2.0-beta.1", "1.2.1"));
    CHECK_FALSE(takes("^1.2.0-beta.1", "1.2.0-alpha"));
    CHECK_FALSE(takes("^1.2.0-beta.1", "1.3.0-beta.1"));
    CHECK(takes(">=1.0.0-beta.0", "1.0.0-beta.1"));
    CHECK_FALSE(takes(">=0.9", "1.0.0-beta.1"));
}

TEST_CASE("a range that isn't one says why", "[semver]") {
    CHECK(range_error("").contains("can't be empty"));
    CHECK(range_error("1.2,").contains("empty comparator"));
    CHECK(range_error(">=1.*").contains("'*'"));
    CHECK(range_error("1.2.3.4").contains("isn't a version"));
    CHECK(range_error("abc").contains("isn't a version"));
    CHECK(range_error("1.2-beta").contains("pre-release"));
    CHECK(range_error("1.2.3+x").contains("build metadata"));
    CHECK(range_error("1 || 2").contains("isn't a version"));
    CHECK(range_error("1.*.3").contains("isn't a version"));
    CHECK(fb::parse_range(" 1.2 ")->text == "1.2");
}

TEST_CASE("a range near the largest numbers has no upper bound past them", "[semver]") {
    CHECK(takes("18446744073709551615", "18446744073709551615.3.0"));
    CHECK(takes("~1.18446744073709551615", "1.18446744073709551615.4"));
    CHECK_FALSE(takes("~1.18446744073709551615", "2.0.0"));
    CHECK_FALSE(takes("0.18446744073709551615", "1.0.0"));
    CHECK_FALSE(takes("0.0.18446744073709551615", "0.1.0"));
    CHECK_FALSE(takes(">18446744073709551615", "18446744073709551615.9.9"));
    CHECK(takes("<=18446744073709551615", "18446744073709551615.9.9"));
}
