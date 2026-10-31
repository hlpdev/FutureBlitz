#pragma once

#include "fb/source.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace fb {

enum class severity_t : std::uint8_t {
    ERROR,
    WARNING,
    NOTE,
};

/// Another span a diagnostic points at, with its own message, like "first declared here".
struct label_t {
    span_t span;
    std::string message;
};

/// A change to the source that fixes what a diagnostic says, which an editor can make: the text
/// `span` covers becomes `replacement`. An empty span inserts it.
struct fix_t {
    span_t span;
    std::string replacement;
};

/// One way to fix what a diagnostic says: the changes that together make it, which don't
/// overlap, and what they do, as the LSP's code action and fb check's help name it, like "Import
/// std.collections".
struct suggestion_t {
    std::string title;
    std::vector<fix_t> fixes;
};

/// One error, warning, or note for the user.
struct diagnostic_t {
    severity_t severity = severity_t::ERROR;
    /// An optional code like "E0102", shown as "error[E0102]". Empty for none.
    std::string code;
    std::string message;
    /// Where the problem is.
    span_t span;
    std::vector<label_t> labels;
    /// Extra lines shown after the snippets, as "= note: ...".
    std::vector<std::string> notes;
    /// The ways to fix it, the likeliest first, which the LSP offers as code actions and fb
    /// check shows as help.
    std::vector<suggestion_t> suggestions;

    diagnostic_t& with_label(const span_t at, std::string text) {
        labels.push_back({ .span = at, .message = std::move(text) });
        return *this;
    }

    diagnostic_t& with_note(std::string text) {
        notes.push_back(std::move(text));
        return *this;
    }

    diagnostic_t& with_suggestion(std::string title, std::vector<fix_t> fixes) {
        suggestions.push_back({ .title = std::move(title), .fixes = std::move(fixes) });
        return *this;
    }
};

/// Collects the diagnostics of a compilation, in the order they're reported.
class diagnostic_bag_t {
public:
    /// Returns the stored diagnostic, for adding labels and notes. The reference is only valid
    /// until the next report.
    diagnostic_t& report(diagnostic_t diagnostic);

    /// Reports an error with a message formatted like std::format.
    template <typename... Args>
    diagnostic_t& error(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        return report({
            .severity = severity_t::ERROR,
            .message = std::format(fmt, std::forward<Args>(args)...),
            .span = span,
        });
    }

    /// Reports a warning with a message formatted like std::format.
    template <typename... Args>
    diagnostic_t& warning(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        return report({
            .severity = severity_t::WARNING,
            .message = std::format(fmt, std::forward<Args>(args)...),
            .span = span,
        });
    }

    /// The diagnostic at `index` among diagnostics(), to add to.
    [[nodiscard]] diagnostic_t& at(const std::size_t index) {
        return m_diagnostics[index];
    }

    [[nodiscard]] const std::vector<diagnostic_t>& diagnostics() const {
        return m_diagnostics;
    }

    /// Errors only; warnings and notes don't count.
    [[nodiscard]] std::size_t error_count() const {
        return m_error_count;
    }

    [[nodiscard]] bool has_errors() const {
        return m_error_count > 0;
    }

    /// Orders by file, then position. Diagnostics at the same position keep their order.
    void sort();

    void clear();

private:
    std::vector<diagnostic_t> m_diagnostics;
    std::size_t m_error_count = 0;
};

} // namespace fb
