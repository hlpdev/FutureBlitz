#include "fb/lint.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace fb {
namespace {

constexpr auto all_lints = std::to_array<lint_info_t>({
    {
        .lint = lint_t::UNUSED_LOCAL,
        .name = "unused_local",
        .summary = "a Local, or a Const in a function, that's never read",
        .inspection = true,
    },
    {
        .lint = lint_t::UNUSED_PARAMETER,
        .name = "unused_parameter",
        .summary = "a parameter its function's code never uses",
        .inspection = true,
    },
    {
        .lint = lint_t::UNUSED_IMPORT,
        .name = "unused_import",
        .summary = "an Import, or a From line's name, that nothing in its file uses",
        .inspection = true,
    },
    {
        .lint = lint_t::UNUSED_PRIVATE,
        .name = "unused_private",
        .summary = "a Private declaration that nothing in its file uses",
        .inspection = true,
    },
    {
        .lint = lint_t::UNREACHABLE,
        .name = "unreachable",
        .summary = "code after what always leaves its block",
        .inspection = true,
    },
    {
        .lint = lint_t::SHADOWED,
        .name = "shadowed",
        .summary = "a Local or parameter named as a Global, Const, Function, type, or Field of "
                   "Self",
        .inspection = true,
    },
    {
        .lint = lint_t::REDUNDANT_AS,
        .name = "redundant_as",
        .summary = "an As whose value has its type already",
        .inspection = true,
    },
    {
        .lint = lint_t::REDUNDANT_NULL,
        .name = "redundant_null",
        .summary = "'?.', '?\?', or a comparison with Null on what can't be Null",
        .inspection = false,
    },
    {
        .lint = lint_t::CONSTANT_CONDITION,
        .name = "constant_condition",
        .summary = "a condition that's always True or always False",
        .inspection = false,
    },
    {
        .lint = lint_t::REDUNDANT_TRY,
        .name = "redundant_try",
        .summary = "a Try around calls that don't throw",
        .inspection = false,
    },
    {
        .lint = lint_t::DEPRECATED,
        .name = "deprecated",
        .summary = "a use of what @Deprecated marks",
        .inspection = false,
    },
    {
        .lint = lint_t::CONSTRAINT_STYLE,
        .name = "constraint_style",
        .summary = "a constraint inline that goes on a Where line, or the other way round",
        .inspection = false,
    },
    {
        .lint = lint_t::SELF_CYCLE,
        .name = "self_cycle",
        .summary = "a lambda that captures Self, kept in a field of Self",
        .inspection = false,
    },
});

static_assert(all_lints.size() == std::tuple_size_v<decltype(lints_t::levels)>);

} // namespace

std::span<const lint_info_t> lints() {
    return all_lints;
}

const lint_info_t& lint_info(const lint_t lint) {
    return all_lints[static_cast<std::size_t>(lint)];
}

std::optional<lint_t> lint_named(const std::string_view name) {
    const auto found = std::ranges::find(all_lints, name, &lint_info_t::name);
    return found != all_lints.end() ? std::optional(found->lint) : std::nullopt;
}

bool lints_t::on(const lint_t lint) const {
    return level(lint) != lint_level_t::OFF && (inspect || !lint_info(lint).inspection);
}

} // namespace fb
