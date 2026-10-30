#include "fb/semver.hpp"

#include <algorithm>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

bool is_digit(const char c) {
    return c >= '0' && c <= '9';
}

bool is_digits(const std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, is_digit);
}

/// A pre-release identifier: ASCII letters, digits, and '-'.
bool is_identifier(const std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](const char c) {
        return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    });
}

/// A version's number: digits without a leading zero, which fit in 64 bits.
std::optional<std::uint64_t> number_of(const std::string_view text) {
    if (!is_digits(text) || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }

    return value;
}

/// Compares pre-release identifiers as semver does: numbers by value, before words, which
/// compare by their bytes.
std::strong_ordering compare_identifiers(const std::string_view left,
                                         const std::string_view right) {
    const bool left_number = is_digits(left);
    const bool right_number = is_digits(right);
    if (left_number && right_number) {
        // Without leading zeros, a longer number is a larger one.
        if (left.size() != right.size()) {
            return left.size() <=> right.size();
        }

        return left.compare(right) <=> 0;
    }

    if (left_number != right_number) {
        return left_number ? std::strong_ordering::less : std::strong_ordering::greater;
    }

    return left.compare(right) <=> 0;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }

    return text;
}

/// A version a comparator writes, which may leave out its minor and patch numbers, or end in a
/// `*` for them.
struct partial_t {
    std::uint64_t major = 0;
    std::optional<std::uint64_t> minor;
    std::optional<std::uint64_t> patch;
    std::vector<std::string> pre;
    /// It ended in `*`, like "1.*", or was only "*".
    bool wildcard = false;
    /// It was only "*".
    bool any = false;
};

std::expected<partial_t, std::string> parse_partial(const std::string_view text) {
    const std::string wrong = std::format(
        "\"{}\" isn't a version, which is up to three numbers, like 1.2 or 1.2.3", text);
    if (text == "*") {
        return partial_t{ .wildcard = true, .any = true };
    }

    const std::size_t dash = text.find('-');
    const std::string_view numbers = text.substr(0, dash);
    if (text.contains('+')) {
        return std::unexpected(
            std::format("\"{}\" has build metadata, after its '+', which a registry's versions "
                        "don't have",
                        text));
    }

    partial_t partial;
    std::size_t index = 0;
    for (const auto part : std::views::split(numbers, '.')) {
        const std::string_view piece(part.begin(), part.end());
        if (index > 2) {
            return std::unexpected(wrong);
        }

        if (piece == "*" && index > 0) {
            if (partial.wildcard) {
                return std::unexpected(wrong);
            }

            partial.wildcard = true;
            ++index;
            continue;
        }

        const std::optional<std::uint64_t> number = number_of(piece);
        if (!number || partial.wildcard) {
            return std::unexpected(wrong);
        }

        if (index == 0) {
            partial.major = *number;
        } else if (index == 1) {
            partial.minor = number;
        } else {
            partial.patch = number;
        }

        ++index;
    }

    if (dash != std::string_view::npos) {
        if (!partial.patch) {
            return std::unexpected(std::format(
                "\"{}\" has a pre-release, which needs all three numbers before it, like "
                "1.0.0-beta.1",
                text));
        }

        const std::expected<version_t, std::string> whole = parse_version(text);
        if (!whole) {
            return std::unexpected(whole.error());
        }

        partial.pre = whole->pre;
    }

    return partial;
}

/// One past `number`, or nullopt when nothing's above it.
std::optional<std::uint64_t> next(const std::uint64_t number) {
    if (number == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    return number + 1;
}

using op_t = comparator_t::op_t;

void add(std::vector<comparator_t>& out, const op_t op, const version_t& version) {
    out.push_back({ .op = op, .version = version });
}

/// The first version a partial one names: the numbers it leaves out 0.
version_t lowest(const partial_t& at) {
    return {
        .major = at.major,
        .minor = at.minor.value_or(0),
        .patch = at.patch.value_or(0),
        .pre = at.pre,
    };
}

/// `<` the version of these numbers, unless one of them can't be one past its own.
void add_below(std::vector<comparator_t>& out, const std::optional<std::uint64_t> major,
               const std::uint64_t minor, const std::uint64_t patch) {
    if (major) {
        add(out, op_t::LESS, { .major = *major, .minor = minor, .patch = patch });
    }
}

/// `<` the next minor version after `major.minor`, or the next major one when the minor number
/// is the last.
void add_below_minor(std::vector<comparator_t>& out, const std::uint64_t major,
                     const std::uint64_t minor) {
    if (const std::optional<std::uint64_t> above = next(minor)) {
        add_below(out, major, *above, 0);
    } else {
        add_below(out, next(major), 0, 0);
    }
}

/// The comparators of a caret range, `^1.2.3` or a bare `1.2.3`: what's compatible by semver,
/// where below 1.0 the leftmost number that isn't 0 breaks.
void add_caret(std::vector<comparator_t>& out, const partial_t& at) {
    const std::uint64_t minor = at.minor.value_or(0);
    const std::uint64_t patch = at.patch.value_or(0);
    add(out, op_t::GREATER_EQUAL, lowest(at));
    if (at.major > 0 || !at.minor) {
        add_below(out, next(at.major), 0, 0);
    } else if (minor > 0 || !at.patch) {
        add_below_minor(out, at.major, minor);
    } else if (const std::optional<std::uint64_t> above = next(patch)) {
        add_below(out, at.major, 0, *above);
    } else {
        add_below(out, at.major, 1, 0);
    }
}

/// The comparators of `~1.2.3`, `~1.2`, or `~1`: the patches, or for `~1`, the minor versions.
void add_tilde(std::vector<comparator_t>& out, const partial_t& at) {
    add(out, op_t::GREATER_EQUAL, lowest(at));
    if (!at.minor) {
        add_below(out, next(at.major), 0, 0);
    } else {
        add_below_minor(out, at.major, *at.minor);
    }
}

/// The comparators of `=1.2.3`, or `=1.2` and `=1`, which are every version that starts so, as
/// `1.2.*` and `1.*` are.
void add_exact(std::vector<comparator_t>& out, const partial_t& at) {
    if (at.patch) {
        add(out, op_t::EQUAL, lowest(at));
        return;
    }

    add(out, op_t::GREATER_EQUAL, { .major = at.major, .minor = at.minor.value_or(0) });
    if (!at.minor) {
        add_below(out, next(at.major), 0, 0);
    } else {
        add_below_minor(out, at.major, *at.minor);
    }
}

/// `>` or `<=` a partial version, which is past every version that starts so: `>1.2` is
/// `>=1.3.0`, and `<=1.2` is `<1.3.0`.
void add_past(std::vector<comparator_t>& out, const op_t op, const partial_t& at) {
    if (at.patch) {
        add(out, op, lowest(at));
        return;
    }

    std::optional<std::uint64_t> major = at.major;
    std::uint64_t minor = 0;
    if (at.minor) {
        const std::optional<std::uint64_t> above = next(*at.minor);
        if (above) {
            minor = *above;
        } else {
            major = next(at.major);
        }
    } else {
        major = next(at.major);
    }

    constexpr std::uint64_t last = std::numeric_limits<std::uint64_t>::max();
    if (op == op_t::GREATER && major) {
        add(out, op_t::GREATER_EQUAL, { .major = *major, .minor = minor });
    } else if (op == op_t::GREATER) {
        // Nothing is past the last version.
        add(out, op_t::GREATER, { .major = at.major, .minor = last, .patch = last });
    } else if (major) {
        add(out, op_t::LESS, { .major = *major, .minor = minor });
    }
}

/// Whether `version` meets one comparator, by semver's order.
bool meets(const comparator_t& comparator, const version_t& version) {
    switch (comparator.op) {
    case op_t::EQUAL:
        return version == comparator.version;
    case op_t::GREATER:
        return version > comparator.version;
    case op_t::GREATER_EQUAL:
        return version >= comparator.version;
    case op_t::LESS:
        return version < comparator.version;
    case op_t::LESS_EQUAL:
        return version <= comparator.version;
    }

    std::unreachable();
}

} // namespace

std::strong_ordering version_t::operator<=>(const version_t& other) const {
    if (const auto order =
            std::tie(major, minor, patch) <=> std::tie(other.major, other.minor, other.patch);
        order != 0) {
        return order;
    }

    // A pre-release comes before its release.
    if (pre.empty() != other.pre.empty()) {
        return pre.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
    }

    for (std::size_t i = 0; i < std::min(pre.size(), other.pre.size()); ++i) {
        if (const auto order = compare_identifiers(pre[i], other.pre[i]); order != 0) {
            return order;
        }
    }

    return pre.size() <=> other.pre.size();
}

std::string version_t::text() const {
    std::string out = std::format("{}.{}.{}", major, minor, patch);
    for (std::size_t i = 0; i < pre.size(); ++i) {
        out += i == 0 ? '-' : '.';
        out += pre[i];
    }

    return out;
}

std::expected<version_t, std::string> parse_version(const std::string_view text) {
    const std::string wrong = std::format(
        "\"{}\" isn't a version, which is three numbers, like 1.2.3, and maybe a pre-release, "
        "like 1.0.0-beta.1",
        text);
    if (text.contains('+')) {
        return std::unexpected(
            std::format("\"{}\" has build metadata, after its '+', which a package's version "
                        "can't have",
                        text));
    }

    const std::size_t dash = text.find('-');
    const std::string_view numbers = text.substr(0, dash);
    version_t version;
    std::size_t index = 0;
    for (const auto part : std::views::split(numbers, '.')) {
        const std::optional<std::uint64_t> number =
            number_of(std::string_view(part.begin(), part.end()));
        if (!number || index > 2) {
            return std::unexpected(wrong);
        }

        if (index == 0) {
            version.major = *number;
        } else if (index == 1) {
            version.minor = *number;
        } else {
            version.patch = *number;
        }

        ++index;
    }

    if (index != 3) {
        return std::unexpected(wrong);
    }

    if (dash == std::string_view::npos) {
        return version;
    }

    // An empty pre-release splits into no identifiers at all.
    if (dash + 1 == text.size()) {
        return std::unexpected(wrong);
    }

    for (const auto part : std::views::split(text.substr(dash + 1), '.')) {
        const std::string_view identifier(part.begin(), part.end());
        // A number in a pre-release has no leading zero either.
        if (!is_identifier(identifier) ||
            (is_digits(identifier) && identifier.size() > 1 && identifier.front() == '0')) {
            return std::unexpected(wrong);
        }

        version.pre.emplace_back(identifier);
    }

    return version;
}

bool version_range_t::matches(const version_t& version) const {
    if (!version.pre.empty() && std::ranges::none_of(comparators, [&](const comparator_t& each) {
            return !each.version.pre.empty() && each.version.major == version.major &&
                   each.version.minor == version.minor && each.version.patch == version.patch;
        })) {
        return false;
    }

    return std::ranges::all_of(comparators,
                               [&](const comparator_t& each) { return meets(each, version); });
}

std::expected<version_range_t, std::string> parse_range(const std::string_view text) {
    version_range_t range{ .text = std::string(trim(text)) };
    if (range.text.empty()) {
        return std::unexpected(
            "a range of versions can't be empty; \"1.2\" is 1.2 or any later 1.x, and \"*\" is "
            "any version");
    }

    for (const auto part : std::views::split(std::string_view(range.text), ',')) {
        std::string_view written = trim(std::string_view(part.begin(), part.end()));
        if (written.empty()) {
            return std::unexpected(
                std::format("\"{}\" has an empty comparator between its commas", range.text));
        }

        std::string_view op;
        for (const std::string_view each : { ">=", "<=", ">", "<", "=", "^", "~" }) {
            if (written.starts_with(each)) {
                op = each;
                break;
            }
        }

        written = trim(written.substr(op.size()));
        const std::expected<partial_t, std::string> at = parse_partial(written);
        if (!at) {
            return std::unexpected(at.error());
        }

        if (at->wildcard && !op.empty()) {
            return std::unexpected(
                std::format("\"{}{}\" puts a '*' after '{}', which only works alone, like 1.*", op,
                            written, op));
        }

        if (at->any) {
            continue;
        }

        if ((op.empty() && !at->wildcard) || op == "^") {
            add_caret(range.comparators, *at);
        } else if (op.empty() || op == "=") {
            add_exact(range.comparators, *at);
        } else if (op == "~") {
            add_tilde(range.comparators, *at);
        } else if (op == ">=" || op == "<") {
            add(range.comparators, op == ">=" ? op_t::GREATER_EQUAL : op_t::LESS, lowest(*at));
        } else {
            add_past(range.comparators, op == ">" ? op_t::GREATER : op_t::LESS_EQUAL, *at);
        }
    }

    return range;
}

} // namespace fb
