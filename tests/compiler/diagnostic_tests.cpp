#include "fb/diagnostic.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

using fb::diagnostic_bag_t;
using fb::severity_t;
using fb::span_t;

namespace {

/// A one-byte span at `begin` in the file with id `file`.
span_t span_at(const std::uint32_t file, const std::uint32_t begin) {
    return { .file = fb::file_id_t{ file }, .begin = begin, .end = begin + 1 };
}

std::vector<std::string> messages_of(const diagnostic_bag_t& bag) {
    std::vector<std::string> messages;
    for (const auto& diagnostic : bag.diagnostics()) {
        messages.push_back(diagnostic.message);
    }
    return messages;
}

} // namespace

TEST_CASE("an empty bag has no errors", "[diagnostic]") {
    const diagnostic_bag_t bag;
    CHECK(bag.diagnostics().empty());
    CHECK(bag.error_count() == 0);
    CHECK_FALSE(bag.has_errors());
}

TEST_CASE("error formats its message and counts as an error", "[diagnostic]") {
    diagnostic_bag_t bag;
    bag.error(span_at(0, 4), "expected {} arguments, got '{}'", 2, "x");

    REQUIRE(bag.diagnostics().size() == 1);
    const auto& diagnostic = bag.diagnostics().front();
    CHECK(diagnostic.severity == severity_t::ERROR);
    CHECK(diagnostic.message == "expected 2 arguments, got 'x'");
    CHECK(diagnostic.span == span_at(0, 4));
    CHECK(bag.error_count() == 1);
    CHECK(bag.has_errors());
}

TEST_CASE("warnings don't count as errors", "[diagnostic]") {
    diagnostic_bag_t bag;
    bag.warning(span_at(0, 0), "unused variable '{}'", "hp");

    REQUIRE(bag.diagnostics().size() == 1);
    CHECK(bag.diagnostics().front().severity == severity_t::WARNING);
    CHECK(bag.error_count() == 0);
    CHECK_FALSE(bag.has_errors());
}

TEST_CASE("labels and notes chain onto a diagnostic", "[diagnostic]") {
    diagnostic_bag_t bag;
    bag.error(span_at(0, 10), "'{}' is already declared", "hp")
        .with_label(span_at(1, 2), "first declared here")
        .with_note("rename one of them");

    const auto& diagnostic = bag.diagnostics().front();
    REQUIRE(diagnostic.labels.size() == 1);
    CHECK(diagnostic.labels.front().span == span_at(1, 2));
    CHECK(diagnostic.labels.front().message == "first declared here");
    CHECK(diagnostic.notes == std::vector<std::string>{ "rename one of them" });
}

TEST_CASE("sort orders by file, then position", "[diagnostic]") {
    diagnostic_bag_t bag;
    bag.error(span_at(1, 0), "d");
    bag.error(span_at(0, 5), "b");
    bag.warning(span_at(0, 2), "a");
    bag.error(span_at(0, 5), "c"); // same position as "b", so it stays after "b"
    bag.sort();

    CHECK(messages_of(bag) == std::vector<std::string>{ "a", "b", "c", "d" });
}

TEST_CASE("clear empties the bag", "[diagnostic]") {
    diagnostic_bag_t bag;
    bag.error(span_at(0, 0), "unexpected character");
    bag.clear();

    CHECK(bag.diagnostics().empty());
    CHECK_FALSE(bag.has_errors());
}
