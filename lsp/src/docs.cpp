#include "fb/lsp/docs.hpp"

#include "fb/diagnostic.hpp"
#include "fb/lexer.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace fb::lsp {
namespace {

/// `text` without the spaces and tabs at its start and end.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }

    return text;
}

/// A comment's text without its markers: a line comment's "//" or "///" and the space after it,
/// and a block's "/*" and "*/", with the " * " that starts each of its lines.
std::string comment_text(const std::string_view comment) {
    if (comment.starts_with("//")) {
        std::string_view text = comment.substr(comment.starts_with("///") ? 3 : 2);
        if (text.starts_with(' ')) {
            text.remove_prefix(1);
        }

        return std::string(trimmed(text).empty() ? std::string_view() : text);
    }

    std::string_view inside = comment.substr(2);
    if (inside.ends_with("*/")) {
        inside.remove_suffix(2);
    }

    std::string text;
    std::size_t begin = 0;
    while (begin <= inside.size()) {
        const std::size_t end = std::min(inside.find('\n', begin), inside.size());
        std::string_view line = trimmed(inside.substr(begin, end - begin));
        if (line.starts_with('*')) {
            line.remove_prefix(1);
            if (line.starts_with(' ')) {
                line.remove_prefix(1);
            }
        }

        if (!text.empty() || !line.empty()) {
            text += std::string(line) + "\n";
        }

        begin = end + 1;
    }

    while (text.ends_with('\n')) {
        text.pop_back();
    }

    return text;
}

} // namespace

std::string doc_comment(const source_file_t& file, const std::uint32_t begin) {
    diagnostic_bag_t ignored;
    trivia_t trivia;
    static_cast<void>(lex_layout(file, ignored, trivia));
    const std::uint32_t line = file.location_of(begin, column_unit_t::UTF8).line;

    // The comments that fill their lines, by the line they end on.
    const auto alone = [&](const span_t comment) {
        const location_t start = file.location_of(comment.begin, column_unit_t::UTF8);
        const location_t end = file.location_of(comment.end, column_unit_t::UTF8);
        const std::string_view first = file.line_text(start.line);
        const std::string_view last = file.line_text(end.line);
        return trimmed(first.substr(0, start.column)).empty() &&
               trimmed(last.substr(std::min<std::size_t>(end.column, last.size()))).empty();
    };

    std::vector<std::string> parts;
    std::uint32_t above = line;
    for (const span_t& comment : trivia.comments | std::views::reverse) {
        const std::uint32_t ends = file.location_of(comment.end, column_unit_t::UTF8).line;
        if (comment.begin >= begin || ends >= above) {
            continue;
        }

        if (ends + 1 != above || !alone(comment)) {
            break;
        }

        parts.push_back(comment_text(file.slice(comment)));
        above = file.location_of(comment.begin, column_unit_t::UTF8).line;
    }

    if (parts.empty()) {
        // One after the code on the declaration's line.
        for (const span_t& comment : trivia.comments) {
            if (comment.begin > begin &&
                file.location_of(comment.begin, column_unit_t::UTF8).line == line) {
                return comment_text(file.slice(comment));
            }
        }

        return {};
    }

    std::string text;
    for (const std::string& part : parts | std::views::reverse) {
        text += (text.empty() ? "" : "\n") + part;
    }

    return text;
}

} // namespace fb::lsp
