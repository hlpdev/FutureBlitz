#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fb {

/// What a warning is about, by which project.toml's [lints] turns it off or makes it an error.
/// The inspections look for what a check of a package's own code finds beyond its errors, and
/// the others are warnings every check gives.
enum class lint_t : std::uint8_t {
    /// A Local, or a Const in a function, that's never read.
    UNUSED_LOCAL,
    /// A parameter a function's code never uses.
    UNUSED_PARAMETER,
    /// An Import or a From line's name that nothing in the file uses.
    UNUSED_IMPORT,
    /// A Private declaration that nothing in its file uses.
    UNUSED_PRIVATE,
    /// Code after what always leaves its block.
    UNREACHABLE,
    /// A Local or a parameter named as a Global, a Const, a Function, a type, or Self's Field.
    SHADOWED,
    /// An As whose value has its type already.
    REDUNDANT_AS,
    /// A '?.', '??', or comparison with Null on what can't be Null.
    REDUNDANT_NULL,
    /// A condition that's always True or always False.
    CONSTANT_CONDITION,
    /// A Try around calls that don't throw.
    REDUNDANT_TRY,
    /// A use of what @Deprecated marks.
    DEPRECATED,
    /// A constraint written where it isn't the custom: inline when it's the declaration's one,
    /// on a Where line when there are more.
    CONSTRAINT_STYLE,
    /// A lambda that captures Self, kept in a field of Self.
    SELF_CYCLE,
};

/// How a lint's warnings are given. A warning is the zero, which each lint has without
/// [lints].
enum class lint_level_t : std::uint8_t {
    WARNING,
    OFF,
    ERROR,
};

/// What [lints] calls a lint, what it's about, and whether it's an inspection.
struct lint_info_t {
    lint_t lint;
    std::string_view name;
    std::string_view summary;
    bool inspection;
};

/// Every lint, in the order lint_t has them.
[[nodiscard]] std::span<const lint_info_t> lints();

[[nodiscard]] const lint_info_t& lint_info(lint_t lint);

/// The lint [lints] names `name`, like UNUSED_LOCAL for "unused_local".
[[nodiscard]] std::optional<lint_t> lint_named(std::string_view name);

/// [lints]'s level for each lint, a warning without one. A package's own code has these, and
/// the standard library's and a registry's packages' have none, as their warnings aren't the
/// program's to fix.
struct lints_t {
    std::array<lint_level_t, 13> levels{};
    /// Whether the inspections look at the code: they do for fb check, build, run, test, and
    /// the LSP, which have these from a project.toml, and don't for compile() in memory, unless
    /// it's asked.
    bool inspect = true;

    [[nodiscard]] lint_level_t level(const lint_t lint) const {
        return levels[static_cast<std::size_t>(lint)];
    }

    void set(const lint_t lint, const lint_level_t level) {
        levels[static_cast<std::size_t>(lint)] = level;
    }

    /// Whether `lint` gives warnings, or errors: it's on, and if it's an inspection, inspections
    /// look.
    [[nodiscard]] bool on(lint_t lint) const;

    friend bool operator==(const lints_t&, const lints_t&) = default;
};

} // namespace fb
