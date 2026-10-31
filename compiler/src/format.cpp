#include "fb/format.hpp"

#include "fb/ast.hpp"
#include "fb/dump.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"
#include "fb/token.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace fb {
namespace {

constexpr std::uint32_t indent_width = 4;

/// How many columns text shows in: 2 for each East Asian wide character, and none for a
/// combining mark.
std::uint32_t width_of(const std::string_view text) {
    std::uint32_t width = 0;
    for (std::size_t at = 0; at < text.size();) {
        const decoded_t decoded = decode_utf8(text, at);
        width += static_cast<std::uint32_t>(display_width(decoded.code_point));
        at += decoded.length;
    }

    return width;
}

/// Whether a comment's text is a doc comment's line: "///", but not "////", or "//!".
bool is_doc_comment(const std::string_view text) {
    return (text.starts_with("///") && !text.starts_with("////")) || text.starts_with("//!");
}

constexpr bool is_opener(const token_kind_t kind) {
    return kind == token_kind_t::LEFT_PAREN || kind == token_kind_t::LEFT_BRACKET ||
           kind == token_kind_t::LEFT_BRACE;
}

constexpr bool is_closer(const token_kind_t kind) {
    return kind == token_kind_t::RIGHT_PAREN || kind == token_kind_t::RIGHT_BRACKET ||
           kind == token_kind_t::RIGHT_BRACE;
}

/// Keywords that end a block when they start a line, as the parser has them.
constexpr bool is_block_end(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::KW_END:
    case token_kind_t::KW_ELSEIF:
    case token_kind_t::KW_ELSE:
    case token_kind_t::KW_UNTIL:
    case token_kind_t::KW_CASE:
    case token_kind_t::KW_DEFAULT:
    case token_kind_t::KW_CATCH:
    case token_kind_t::KW_FINALLY:
        return true;
    default:
        return false;
    }
}

/// '=', a compound assignment, or "=>": what an expression starts after.
constexpr bool is_assignment(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::EQUAL:
    case token_kind_t::PLUS_EQUAL:
    case token_kind_t::MINUS_EQUAL:
    case token_kind_t::STAR_EQUAL:
    case token_kind_t::SLASH_EQUAL:
    case token_kind_t::PERCENT_EQUAL:
    case token_kind_t::AMP_EQUAL:
    case token_kind_t::PIPE_EQUAL:
    case token_kind_t::CARET_EQUAL:
    case token_kind_t::LESS_LESS_EQUAL:
    case token_kind_t::ARROW:
        return true;
    default:
        return false;
    }
}

/// Keywords whose expression a line that goes on lines up with, as "If a ||" and "Throws A,"
/// do.
constexpr bool leads_expression(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::KW_IMPLEMENTS:
    case token_kind_t::KW_THROWS:
    case token_kind_t::KW_IN:
    case token_kind_t::KW_RETURN:
    case token_kind_t::KW_IF:
    case token_kind_t::KW_ELSEIF:
    case token_kind_t::KW_WHILE:
    case token_kind_t::KW_UNTIL:
    case token_kind_t::KW_THROW:
    case token_kind_t::KW_CASE:
    case token_kind_t::KW_SELECT:
    case token_kind_t::KW_LOCK:
        return true;
    default:
        return false;
    }
}

/// Keywords before a declaration's own keyword.
constexpr bool is_modifier(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::KW_PUBLIC:
    case token_kind_t::KW_PRIVATE:
    case token_kind_t::KW_PACKAGE:
    case token_kind_t::KW_PROTECTED:
    case token_kind_t::KW_STATIC:
    case token_kind_t::KW_OVERRIDE:
    case token_kind_t::KW_ABSTRACT:
    case token_kind_t::KW_FINAL:
    case token_kind_t::KW_ASYNC:
    case token_kind_t::KW_SHARED:
    case token_kind_t::KW_UNSAFE:
    case token_kind_t::KW_CONST:
    case token_kind_t::KW_READONLY:
        return true;
    default:
        return false;
    }
}

/// How much a break after a binary operator is wanted, best first: the loosest operators.
constexpr std::uint32_t operator_rank(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::QUESTION_QUESTION:
        return 1;
    case token_kind_t::PIPE_PIPE:
        return 2;
    case token_kind_t::AMP_AMP:
        return 3;
    case token_kind_t::EQUAL_EQUAL:
    case token_kind_t::BANG_EQUAL:
        return 4;
    case token_kind_t::LESS:
    case token_kind_t::LESS_EQUAL:
    case token_kind_t::GREATER:
    case token_kind_t::GREATER_EQUAL:
        return 5;
    case token_kind_t::PIPE:
        return 6;
    case token_kind_t::CARET:
        return 7;
    case token_kind_t::AMP:
        return 8;
    case token_kind_t::LESS_LESS:
        return 9;
    case token_kind_t::PLUS:
    case token_kind_t::MINUS:
        return 10;
    case token_kind_t::STAR:
    case token_kind_t::SLASH:
    case token_kind_t::PERCENT:
        return 11;
    default:
        return 12;
    }
}

/// A token or a comment, on a line.
struct item_t {
    std::string_view text;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    token_kind_t kind = token_kind_t::INVALID;
    bool comment = false;
    std::optional<token_role_t> role;

    [[nodiscard]] bool has(const token_role_t wanted) const {
        return role == wanted;
    }

    [[nodiscard]] bool generic_open() const {
        return kind == token_kind_t::LESS && has(token_role_t::GENERIC);
    }

    [[nodiscard]] bool generic_close() const {
        return kind == token_kind_t::GREATER && has(token_role_t::GENERIC);
    }

    /// A '?' or "??" after a type, or after As or Try, rather than an operator.
    [[nodiscard]] bool postfix_question() const {
        return (kind == token_kind_t::QUESTION && !has(token_role_t::TERNARY)) ||
               (kind == token_kind_t::QUESTION_QUESTION && !has(token_role_t::BINARY));
    }

    [[nodiscard]] bool unary() const {
        return has(token_role_t::UNARY) ||
               (!has(token_role_t::BINARY) &&
                (kind == token_kind_t::BANG || kind == token_kind_t::TILDE));
    }

    [[nodiscard]] bool multi_line() const {
        return text.find_first_of("\r\n") != std::string_view::npos;
    }
};

/// One line of the file as written: a line of code, a comment on its own, or the lines from an
/// #If to its #End If, which are copied as they are.
struct unit_t {
    std::vector<item_t> items;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    /// How many blocks a line of code is in.
    std::uint32_t depth = 0;
    bool code = false;
    /// A line of code that goes on with the statement before it.
    bool continuation = false;
    bool conditional = false;

    /// The first token, which a line of code has.
    [[nodiscard]] const item_t& first_token() const {
        return *std::ranges::find_if(items, [](const item_t& item) { return !item.comment; });
    }
};

/// A bracket that's open, and where the lines inside it go.
struct bracket_t {
    /// Where what follows it on its line starts.
    std::uint32_t align = 0;
    /// The indentation of the line it's on.
    std::uint32_t line_indent = 0;
    /// It ends its line, so the lines inside it go a step deeper than that line instead.
    bool hanging = false;
    /// A declaration's parameters, which go two steps deeper, past its body's.
    bool signature = false;
    bool generic = false;
    /// Where its last ternary's '?' is, which a line starting with the ':' lines up with.
    std::optional<std::uint32_t> question;
};

/// A statement being laid out, with the brackets open before it.
struct segment_t {
    std::uint32_t depth = 0;
    /// How many brackets were open when it started: ones it doesn't own.
    std::size_t base = 0;
    std::uint32_t indent = 0;
    /// Where its expression starts, which a line after an operator outside brackets lines up
    /// with. None when it starts on a line of its own, or there's none to line up with.
    std::optional<std::uint32_t> expression;
    /// The first line's expression is the next token's.
    bool expression_next = false;
    /// Its first line is laid out.
    bool started = false;
    /// It's a declaration, whose first '(' after its keyword, which begins at `keyword`, holds
    /// its parameters.
    bool declaration = false;
    std::uint32_t keyword = 0;
    bool seen_paren = false;
};

/// What laying out lines changes, which a statement that doesn't fit when broken puts back.
struct state_t {
    std::vector<bracket_t> brackets;
    std::vector<segment_t> segments;
    /// Where the last unit written ends in the source, and whether it was code.
    std::optional<std::uint32_t> previous_end;
    bool after_code = false;
    /// The depth of the statement written last.
    std::uint32_t statement_depth = 0;
};

/// Where a line may break after a token, and how much the break is wanted.
struct candidate_t {
    std::size_t item = 0;
    std::size_t length = 0;
    std::uint32_t tier = 0;
    std::uint32_t depth = 0;
    std::uint32_t rank = 0;
    std::vector<bracket_t> brackets;
    segment_t segment;
    std::uint32_t line_depth = 0;
};

/// What each item of a line is, before the line is laid out.
struct item_info_t {
    bool space = false;
    bool breakable = false;
    std::uint32_t tier = 0;
    std::uint32_t depth = 0;
    std::uint32_t rank = 0;
};

class formatter_t {
public:
    formatter_t(const source_file_t& file, const std::span<const token_t> tokens,
                const trivia_t& trivia, const layout_t& layout, const std::uint32_t width)
        : m_file(file), m_text(file.text()), m_tokens(tokens), m_trivia(trivia), m_layout(layout),
          m_width(width) {}

    std::string run() {
        build_units();
        const std::size_t line_break = m_text.find_first_of("\r\n");
        m_eol =
            line_break != std::string_view::npos && m_text.substr(line_break).starts_with("\r\n")
                ? "\r\n"
                : "\n";

        std::size_t index = 0;
        while (index < m_units.size()) {
            const unit_t& unit = m_units[index];
            if (!unit.code || (unit.continuation && m_state.segments.empty())) {
                emit_loose(index);
                ++index;
                continue;
            }

            const std::size_t end = group_end(index);
            emit_group(index, end);
            index = end;
        }

        return std::move(m_out);
    }

private:
    const source_file_t& m_file;
    std::string_view m_text;
    std::span<const token_t> m_tokens;
    const trivia_t& m_trivia;
    const layout_t& m_layout;
    std::uint32_t m_width;
    std::string_view m_eol = "\n";
    std::vector<unit_t> m_units;
    state_t m_state;
    std::string m_out;

    [[nodiscard]] std::uint32_t line_of(const std::uint32_t offset) const {
        return m_file.location_of(offset, column_unit_t::UTF8).line;
    }

    [[nodiscard]] std::string_view slice(const std::uint32_t begin, const std::uint32_t end) const {
        return m_text.substr(begin, end - begin);
    }

    [[nodiscard]] std::optional<token_role_t> role_at(const std::uint32_t begin) const {
        const auto found = m_layout.roles.find(begin);
        return found != m_layout.roles.end() ? std::optional(found->second) : std::nullopt;
    }

    // Units

    void build_units() {
        unit_t current;
        bool continuation = false;
        std::uint32_t depth = 0;
        std::size_t comment = 0;
        std::size_t conditional = 0;
        const std::vector<span_t>& comments = m_trivia.comments;
        const std::vector<span_t>& conditionals = m_trivia.conditionals;

        const auto flush = [&] {
            if (!current.items.empty()) {
                current.begin = current.items.front().begin;
                current.end = current.items.back().end;
                m_units.push_back(std::move(current));
            }

            current = unit_t();
        };

        for (const token_t& token : m_tokens) {
            while (true) {
                const bool has_comment =
                    comment < comments.size() && comments[comment].begin < token.span.begin;
                const bool has_conditional = conditional < conditionals.size() &&
                                             conditionals[conditional].begin < token.span.begin;
                if (!has_comment && !has_conditional) {
                    break;
                }

                if (has_conditional &&
                    (!has_comment || conditionals[conditional].begin < comments[comment].begin)) {
                    flush();
                    const span_t lines = conditionals[conditional++];
                    m_units.push_back({
                        .begin = lines.begin,
                        .end = lines.end,
                        .conditional = true,
                    });
                    continue;
                }

                place_comment(comments[comment++], token, current, flush);
            }

            if (token.kind == token_kind_t::NEWLINE) {
                flush();
                continuation = !m_layout.line_ends.contains(token.span.begin);
                continue;
            }

            if (token.kind == token_kind_t::END_OF_FILE) {
                break;
            }

            if (!current.code) {
                const auto found = m_layout.depths.find(token.span.begin);
                depth = found != m_layout.depths.end() ? found->second : depth;
                current.code = true;
                current.continuation = continuation;
                current.depth = depth;
            }

            add_token(token, current);
        }

        flush();
    }

    template <typename Flush>
    void place_comment(const span_t span, const token_t& next, unit_t& current, Flush& flush) {
        // A comment runs to its line break, which the formatter writes itself. A doc comment's
        // spaces at its end are its Markdown's, a line break, or its example's code.
        std::string_view text = slice(span.begin, span.end);
        while (!is_doc_comment(text) && (text.ends_with(' ') || text.ends_with('\t'))) {
            text.remove_suffix(1);
        }

        const item_t item{ .text = text, .begin = span.begin, .end = span.end, .comment = true };
        const std::uint32_t line = line_of(span.begin);
        if (!current.items.empty() && line_of(current.items.back().end) == line) {
            current.items.push_back(item);
            return;
        }

        if (current.items.empty() && next.kind != token_kind_t::NEWLINE &&
            next.kind != token_kind_t::END_OF_FILE &&
            line_of(next.span.begin) == line_of(span.end)) {
            current.items.push_back(item);
            return;
        }

        flush();
        // Another comment on the line of one of its own goes with it.
        if (!m_units.empty() && !m_units.back().code && !m_units.back().conditional &&
            line_of(m_units.back().end) == line) {
            m_units.back().items.push_back(item);
            m_units.back().end = span.end;
            return;
        }

        m_units.push_back({ .items = { item }, .begin = span.begin, .end = span.end });
    }

    void add_token(const token_t& token, unit_t& current) const {
        const std::uint32_t begin = token.span.begin;
        const std::uint32_t end = token.span.end;
        const std::optional<token_role_t> role = role_at(begin);
        // An Operator "<<" with type parameters is its '<' and the '<' that opens them.
        if (token.kind == token_kind_t::LESS_LESS && role_at(begin + 1) == token_role_t::GENERIC) {
            current.items.push_back({
                .text = slice(begin, begin + 1),
                .begin = begin,
                .end = begin + 1,
                .kind = token_kind_t::LESS,
            });
            current.items.push_back({
                .text = slice(begin + 1, end),
                .begin = begin + 1,
                .end = end,
                .kind = token_kind_t::LESS,
                .role = token_role_t::GENERIC,
            });
            return;
        }

        // A ">=" that closes type arguments is their '>' and a '='.
        if (token.kind == token_kind_t::GREATER_EQUAL && role == token_role_t::GENERIC) {
            current.items.push_back({
                .text = slice(begin, begin + 1),
                .begin = begin,
                .end = begin + 1,
                .kind = token_kind_t::GREATER,
                .role = role,
            });
            current.items.push_back({
                .text = slice(begin + 1, end),
                .begin = begin + 1,
                .end = end,
                .kind = token_kind_t::EQUAL,
            });
            return;
        }

        current.items.push_back({
            .text = slice(begin, end),
            .begin = begin,
            .end = end,
            .kind = token.kind,
            .role = role,
        });
    }

    /// The units from `begin`, a statement's first line, up to the next statement's: the
    /// statement's lines, and the comments and #If lines between them.
    [[nodiscard]] std::size_t group_end(const std::size_t begin) const {
        std::size_t end = begin + 1;
        std::size_t last_code = begin;
        while (end < m_units.size() && (!m_units[end].code || m_units[end].continuation)) {
            if (m_units[end].code) {
                last_code = end;
            }

            ++end;
        }

        return last_code + 1;
    }

    // Writing

    void newline(std::string& out) const {
        out += m_eol;
    }

    /// Whether a blank line from the source goes before `unit`: at most one in a row, and none
    /// at the start of the file or of a block, or before a block's end.
    [[nodiscard]] bool blank_before(const unit_t& unit, const std::uint32_t depth,
                                    const bool block_end) const {
        if (!m_state.previous_end || block_end) {
            return false;
        }

        std::uint32_t breaks = 0;
        const std::string_view between = slice(*m_state.previous_end, unit.begin);
        for (std::size_t i = 0; i < between.size(); ++i) {
            if (between[i] == '\n' ||
                (between[i] == '\r' && !between.substr(i).starts_with("\r\n"))) {
                ++breaks;
            }
        }

        return breaks >= 2 && (!m_state.after_code || depth <= m_state.statement_depth);
    }

    /// The next line of code from `index` on, if any.
    [[nodiscard]] const unit_t* next_code(const std::size_t index) const {
        for (std::size_t i = index; i < m_units.size(); ++i) {
            if (m_units[i].code) {
                return &m_units[i];
            }
        }

        return nullptr;
    }

    /// The depth a comment or #If lines before `next` are at.
    [[nodiscard]] std::uint32_t depth_before(const unit_t* next) const {
        if (next == nullptr) {
            return 0;
        }

        return next->continuation ? m_state.statement_depth : next->depth;
    }

    void finish_unit(const unit_t& unit) {
        m_state.previous_end = unit.end;
        m_state.after_code = unit.code;
        if (unit.code && !unit.continuation) {
            m_state.statement_depth = unit.depth;
        }
    }

    void write_comment(const unit_t& unit, const std::uint32_t indent, std::string& out) const {
        out.append(indent, ' ');
        for (const item_t& item : unit.items) {
            out += &item == unit.items.data() ? "" : " ";
            out += item.text;
        }

        newline(out);
    }

    /// A comment or #If lines outside any statement.
    void emit_loose(const std::size_t index) {
        const unit_t& unit = m_units[index];
        const unit_t* next = next_code(index + 1);
        const bool before_end =
            next != nullptr && !next->continuation && is_block_end(next->first_token().kind);
        // Right after the line that opens the block a block's end closes, it's at the block's
        // start, even with nothing but comments or #If lines in the block.
        const bool opened =
            m_state.after_code && before_end && next->depth == m_state.statement_depth;
        std::uint32_t depth = depth_before(next);
        // Before a block's end, a comment written inside the block stays inside it: at the depth
        // it's written at, as deep as the last line of code before it at most, as a comment at
        // the end of a Select's last Case is.
        const std::uint32_t column = m_file.location_of(unit.begin, column_unit_t::UTF8).column;
        if (before_end && column > m_file.location_of(next->begin, column_unit_t::UTF8).column) {
            depth = std::clamp(column / indent_width, next->depth + 1,
                               std::max(next->depth + 1, m_state.statement_depth));
        }

        if (!opened && blank_before(unit, depth, false)) {
            newline(m_out);
        }

        // #If lines, or a line that goes on with nothing, which only broken code has, as written.
        if (unit.conditional || unit.code) {
            m_out += slice(unit.begin, unit.end);
            newline(m_out);
        } else {
            write_comment(unit, depth * indent_width, m_out);
        }

        finish_unit(unit);
    }

    /// A statement and what's between its lines, broken to fit the width where it can be, and
    /// as written where it can't.
    void emit_group(const std::size_t begin, const std::size_t end) {
        const state_t saved = m_state;
        std::string text;
        if (!emit_units(begin, end, true, text)) {
            m_state = saved;
            text.clear();
            emit_units(begin, end, false, text);
        }

        m_out += text;
    }

    bool emit_units(const std::size_t begin, const std::size_t end, const bool fit,
                    std::string& out) {
        for (std::size_t index = begin; index < end; ++index) {
            const unit_t& unit = m_units[index];
            if (unit.code) {
                const bool block_end = !unit.continuation && is_block_end(unit.first_token().kind);
                const std::uint32_t depth =
                    unit.continuation ? m_state.statement_depth : unit.depth;
                if (blank_before(unit, depth, block_end)) {
                    newline(out);
                }

                std::vector<std::string> lines;
                if (!layout_code(unit, fit, lines)) {
                    return false;
                }

                for (const std::string& line : lines) {
                    out += line;
                    newline(out);
                }
            } else {
                if (blank_before(unit, m_state.statement_depth, false)) {
                    newline(out);
                }

                if (unit.conditional) {
                    out += slice(unit.begin, unit.end);
                    newline(out);
                } else {
                    const unit_t* next = next_code(index + 1);
                    write_comment(unit, next != nullptr ? next_indent(*next) : 0, out);
                }
            }

            finish_unit(unit);
        }

        return true;
    }

    // Lines of code

    /// Where a line that goes on with the statement starts: inside the innermost bracket open,
    /// or lined up with the expression, or a step deeper than the statement.
    [[nodiscard]] std::uint32_t continuation_indent(const item_t& first) const {
        const segment_t& segment = m_state.segments.back();
        if (m_state.brackets.size() > segment.base) {
            const bracket_t& bracket = m_state.brackets.back();
            if (is_closer(first.kind) || first.generic_close()) {
                return bracket.line_indent;
            }

            if (first.kind == token_kind_t::COLON && first.has(token_role_t::TERNARY) &&
                bracket.question) {
                return *bracket.question;
            }

            if (bracket.hanging) {
                return bracket.line_indent + ((bracket.signature ? 2 : 1) * indent_width);
            }

            return bracket.align;
        }

        return segment.expression.value_or(segment.indent + indent_width);
    }

    /// The indentation `unit` gets, a continuation line's only when the lines before it are
    /// laid out.
    [[nodiscard]] std::uint32_t next_indent(const unit_t& unit) const {
        return unit.continuation ? continuation_indent(unit.first_token())
                                 : unit.depth * indent_width;
    }

    /// Where a declaration's keyword begins, after its modifiers and attributes, or nullopt
    /// when the line isn't a Function's, Method's, New's, or Operator's.
    [[nodiscard]] static std::optional<std::uint32_t> declaration_keyword(const unit_t& unit) {
        const std::vector<item_t>& items = unit.items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const item_t& item = items[i];
            if (item.comment || is_modifier(item.kind)) {
                continue;
            }

            // "@Name", "@Module.Name", or either with arguments in parentheses.
            if (item.kind == token_kind_t::AT) {
                while (i + 1 < items.size() && (items[i + 1].kind == token_kind_t::IDENTIFIER ||
                                                items[i + 1].kind == token_kind_t::DOT)) {
                    ++i;
                }

                if (i + 1 < items.size() && items[i + 1].kind == token_kind_t::LEFT_PAREN) {
                    std::uint32_t depth = 0;
                    do {
                        ++i;
                        depth += is_opener(items[i].kind) ? 1U : 0U;
                        depth -= is_closer(items[i].kind) && depth > 0 ? 1U : 0U;
                    } while (depth > 0 && i + 1 < items.size());
                }

                continue;
            }

            if (item.kind == token_kind_t::KW_FUNCTION || item.kind == token_kind_t::KW_METHOD ||
                item.kind == token_kind_t::KW_NEW || item.kind == token_kind_t::KW_OPERATOR) {
                return item.begin;
            }

            return std::nullopt;
        }

        return std::nullopt;
    }

    void start_segment(const unit_t& unit) {
        std::vector<segment_t>& segments = m_state.segments;
        // A block's end goes on with the statement that opened it, as "End Function)" does with
        // the call a lambda is in.
        const bool resumes = is_block_end(unit.first_token().kind);
        while (!segments.empty() && (segments.back().depth > unit.depth ||
                                     (!resumes && segments.back().depth == unit.depth))) {
            m_state.brackets.resize(std::min(m_state.brackets.size(), segments.back().base));
            segments.pop_back();
        }

        // Its brackets stay open, but its line is a new one, with an expression of its own.
        if (resumes && !segments.empty() && segments.back().depth == unit.depth) {
            segments.back() = {
                .depth = unit.depth,
                .base = segments.back().base,
                .indent = segments.back().indent,
            };
            return;
        }

        const std::optional<std::uint32_t> keyword = declaration_keyword(unit);
        segments.push_back({
            .depth = unit.depth,
            .base = m_state.brackets.size(),
            .indent = unit.depth * indent_width,
            .declaration = keyword.has_value(),
            .keyword = keyword.value_or(0),
        });
    }

    /// Whether there's a space between two items on a line. `operator_name` is set while an
    /// Operator's name, which is written together, is read.
    [[nodiscard]] static bool spaced(const item_t& a, const item_t& b, const bool operator_name) {
        using enum token_kind_t;
        if (a.comment || a.kind == KW_OPERATOR) {
            return true;
        }

        if (b.comment) {
            return !is_opener(a.kind);
        }

        if (operator_name) {
            return false;
        }

        // An interpolation's braces next to a map's would make "{{" or "}}", which a string
        // takes as a brace.
        if ((a.kind == STRING_START || a.kind == STRING_MIDDLE) && b.kind == LEFT_BRACE) {
            return true;
        }

        if (a.kind == RIGHT_BRACE && (b.kind == STRING_MIDDLE || b.kind == STRING_END)) {
            return true;
        }

        if (a.kind == STRING_START || a.kind == STRING_MIDDLE || b.kind == STRING_MIDDLE ||
            b.kind == STRING_END || b.kind == FORMAT_SPEC) {
            return false;
        }

        if (b.kind == COMMA || is_closer(b.kind)) {
            return false;
        }

        if (a.kind == COMMA) {
            return true;
        }

        if (is_opener(a.kind)) {
            return false;
        }

        if (a.kind == DOT || a.kind == QUESTION_DOT || b.kind == DOT || b.kind == QUESTION_DOT ||
            a.kind == DOT_DOT || b.kind == DOT_DOT || a.kind == AT) {
            return false;
        }

        if (b.kind == DOT_DOT_DOT) {
            return a.kind == ARROW || a.kind == EQUAL;
        }

        if (b.kind == COLON) {
            return b.has(token_role_t::TERNARY);
        }

        if (a.kind == COLON) {
            return true;
        }

        if (b.kind == QUESTION || b.kind == QUESTION_QUESTION) {
            return !b.postfix_question();
        }

        if (b.generic_open() || a.generic_open() || b.generic_close()) {
            return false;
        }

        if (a.postfix_question() || a.generic_close()) {
            return !(b.kind == LEFT_BRACKET || b.kind == LEFT_PAREN || b.generic_close() ||
                     b.postfix_question()) ||
                   (a.postfix_question() && b.kind == LEFT_PAREN);
        }

        // ">>", ">=", and ">>=" are pieces that touch, and must.
        if (a.kind == GREATER && a.end == b.begin &&
            (b.kind == GREATER || b.kind == GREATER_EQUAL || b.kind == EQUAL)) {
            return false;
        }

        // "- -a" written together would be "--a", another token.
        if (a.unary()) {
            return a.kind == MINUS && b.text.starts_with('-');
        }

        if (b.kind == PLUS_PLUS || b.kind == MINUS_MINUS) {
            return false;
        }

        const bool operand = a.kind == IDENTIFIER || a.kind == RIGHT_PAREN ||
                             a.kind == RIGHT_BRACKET || a.kind == KW_SELF || a.kind == KW_SUPER;
        if (b.kind == LEFT_PAREN) {
            return !operand && a.kind != KW_NEW && a.kind != KW_FUNCTION && a.kind != KW_FUNC &&
                   a.kind != KW_CALLBACK;
        }

        if (b.kind == LEFT_BRACKET) {
            return !operand && a.kind != STRING && a.kind != STRING_END && a.kind != KW_FUNCTION;
        }

        return true;
    }

    /// Each item's space before it and where the line may break after it.
    [[nodiscard]] std::vector<item_info_t> describe_items(const unit_t& unit) const {
        const std::vector<item_t>& items = unit.items;
        std::vector<item_info_t> info(items.size());
        const segment_t& segment = m_state.segments.back();
        auto depth = static_cast<std::uint32_t>(m_state.brackets.size() - segment.base);
        auto generic = static_cast<std::uint32_t>(
            std::ranges::count_if(std::span(m_state.brackets).subspan(segment.base),
                                  [](const bracket_t& bracket) { return bracket.generic; }));
        std::uint32_t interpolation = 0;
        bool operator_name = false;
        bool multi_line = false;
        // Which bracket each item is directly in, by its opener, or the line's own level, and
        // which of them hold a ternary, whose operands an operator's break would split.
        std::vector<std::ptrdiff_t> groups{ -1 };
        std::vector<std::ptrdiff_t> group_of(items.size(), -1);
        std::vector<std::ptrdiff_t> ternaries;
        // Whether each open group has had a ternary's '?', so what follows in it, and in the
        // brackets inside, is the ternary's operands.
        std::vector<bool> asked{ false };
        std::vector<bool> operand(items.size(), false);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const item_t& item = items[i];
            group_of[i] = groups.back();
            operand[i] = std::ranges::contains(asked, true);
            if (item.kind == token_kind_t::QUESTION && item.has(token_role_t::TERNARY)) {
                ternaries.push_back(groups.back());
                asked.back() = true;
            }

            if (is_opener(item.kind) || item.generic_open()) {
                groups.push_back(static_cast<std::ptrdiff_t>(i));
                asked.push_back(false);
            } else if ((is_closer(item.kind) || item.generic_close()) && groups.size() > 1) {
                groups.pop_back();
                asked.pop_back();
            }
        }

        // In a single-line If, the line breaks only inside the brackets of its statement.
        std::optional<std::size_t> then;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == token_kind_t::KW_THEN && group_of[i] == -1) {
                then = i;
                break;
            }
        }

        for (std::size_t i = 0; i < items.size(); ++i) {
            const item_t& item = items[i];
            if (i > 0) {
                info[i].space = spaced(items[i - 1], item, operator_name);
                if (items[i - 1].kind == token_kind_t::KW_OPERATOR) {
                    operator_name = true;
                }
            }

            if (item.kind == token_kind_t::LEFT_PAREN || item.generic_open()) {
                operator_name = false;
            }

            multi_line = multi_line || item.multi_line();
            const std::uint32_t before = depth;
            if (is_opener(item.kind) || item.generic_open()) {
                ++depth;
            } else if ((is_closer(item.kind) || item.generic_close()) && depth > 0) {
                --depth;
            }

            generic += item.generic_open() ? 1U : 0U;
            generic -= item.generic_close() && generic > 0 ? 1U : 0U;
            interpolation += item.kind == token_kind_t::STRING_START ? 1U : 0U;
            interpolation -= item.kind == token_kind_t::STRING_END && interpolation > 0 ? 1U : 0U;
            if (item.comment || i + 1 == items.size() || items[i + 1].comment ||
                interpolation > 0 || generic > 0 || multi_line) {
                continue;
            }

            const item_t& next = items[i + 1];
            const bool inside = before > 0 || is_opener(item.kind);
            // A '>' that touches another, or a ">=", is the first piece of one operator.
            const bool piece =
                item.kind == token_kind_t::GREATER && item.end == next.begin &&
                (next.kind == token_kind_t::GREATER || next.kind == token_kind_t::GREATER_EQUAL);
            // An operator's or a bracket's break in a ternary's level would split its operands.
            const bool in_ternary = std::ranges::contains(ternaries, group_of[i]) || operand[i];
            const bool binary = item.has(token_role_t::BINARY) && !piece && !in_ternary;
            // Before a ternary's ':', which then lines up with its '?', inside brackets.
            const bool ternary =
                next.kind == token_kind_t::COLON && next.has(token_role_t::TERNARY) && depth > 0;
            std::optional<std::uint32_t> tier;
            if (item.kind == token_kind_t::COMMA || binary || ternary) {
                tier = 0;
            } else if (is_opener(item.kind) && !is_closer(next.kind) && !in_ternary) {
                tier = 1;
            } else if (is_assignment(item.kind)) {
                tier = 2;
            }

            // In a single-line If, only at a ',' in its statement's brackets.
            const bool allowed = ternary || inside || m_layout.breaks.contains(item.end);
            if (!tier || !allowed ||
                (then && (i <= *then || item.kind != token_kind_t::COMMA || before == 0))) {
                continue;
            }

            info[i].breakable = true;
            info[i].tier = *tier;
            info[i].depth = ternary ? depth : before;
            if (item.kind == token_kind_t::COMMA) {
                info[i].rank = 0;
            } else if (ternary) {
                info[i].rank = 1;
            } else {
                info[i].rank = operator_rank(item.kind) + 1;
            }
        }

        return info;
    }

    /// Opens or closes a bracket, and notes where the statement's expression starts.
    void place(const item_t& item, const std::uint32_t start, const std::uint32_t column,
               const std::uint32_t indent, std::uint32_t& line_depth, const bool first_token) {
        segment_t& segment = m_state.segments.back();
        // Outside the statement's brackets, on any of its lines, as a Throws after a
        // declaration's parameters is.
        if (m_state.brackets.size() == segment.base && !item.comment) {
            if (segment.expression_next ||
                (!segment.started && first_token && !segment.expression)) {
                segment.expression = start;
                segment.expression_next = false;
            }

            if (is_assignment(item.kind) || leads_expression(item.kind)) {
                segment.expression = std::nullopt;
                segment.expression_next = true;
            }
        }

        // A ternary's '?' in the bracket it's in, which a ':' starting a line lines up with.
        if (m_state.brackets.size() > segment.base && item.kind == token_kind_t::QUESTION &&
            item.has(token_role_t::TERNARY)) {
            m_state.brackets.back().question = start;
        }

        if (is_opener(item.kind) || item.generic_open()) {
            // An attribute's arguments come before the keyword, and aren't its parameters.
            const bool parameters = !segment.started && line_depth == 0 && segment.declaration &&
                                    item.begin > segment.keyword &&
                                    item.kind == token_kind_t::LEFT_PAREN;
            const bool signature = parameters && !segment.seen_paren;
            segment.seen_paren = segment.seen_paren || parameters;

            m_state.brackets.push_back({
                .align = column,
                .line_indent = indent,
                .signature = signature,
                .generic = item.generic_open(),
            });
            ++line_depth;
        } else if ((is_closer(item.kind) || item.generic_close()) &&
                   m_state.brackets.size() > segment.base) {
            m_state.brackets.pop_back();
            line_depth -= line_depth > 0 ? 1 : 0;
        }
    }

    /// What ending a written line after `item` means: a bracket it opens hangs, and an
    /// expression that would start after it starts on the next line.
    void end_line(const item_t& item) {
        if ((is_opener(item.kind) || item.generic_open()) &&
            m_state.brackets.size() > m_state.segments.back().base) {
            m_state.brackets.back().hanging = true;
        }

        // An expression that would start after the line's last token starts on the next.
        segment_t& segment = m_state.segments.back();
        segment.started = true;
        segment.expression_next = false;
    }

    /// Lays out one line of code as one line or more. With `fit`, a line too wide breaks where
    /// it may, and false means it doesn't fit at any of its breaks.
    bool layout_code(const unit_t& unit, const bool fit, std::vector<std::string>& lines) {
        if (!unit.continuation) {
            start_segment(unit);
        }

        const std::vector<item_t>& items = unit.items;
        const std::vector<item_info_t> info = describe_items(unit);
        // How wide what follows each item is, to the end of the line.
        std::vector<std::uint32_t> after(items.size() + 1, 0);
        for (std::size_t k = items.size(); k-- > 1;) {
            after[k - 1] = after[k] + width_of(items[k].text) + (info[k].space ? 1 : 0);
        }

        const line_t line{
            .items = items,
            .info = info,
            .after = after,
            .first_token = static_cast<std::size_t>(&unit.first_token() - items.data()),
        };
        std::uint32_t tries = max_tries;
        return layout_from(line, 0, next_indent(unit), 0, fit, lines, tries);
    }

    /// A line of code being laid out.
    struct line_t {
        std::span<const item_t> items;
        std::span<const item_info_t> info;
        std::span<const std::uint32_t> after;
        std::size_t first_token = 0;
    };

    /// How many breaks one line of code tries before it's left as written.
    static constexpr std::uint32_t max_tries = 256;

    /// Lays out the line's items from `i` on, on a line at `indent` and the lines after it. Where
    /// one is too wide, each break on it is tried, the best first, until the rest fits.
    bool layout_from(const line_t& line, std::size_t i, const std::uint32_t indent,
                     std::uint32_t line_depth, const bool fit, std::vector<std::string>& lines,
                     std::uint32_t& tries) {
        const std::span<const item_t> items = line.items;
        std::string text(indent, ' ');
        std::uint32_t column = indent;
        std::vector<candidate_t> candidates;
        const std::size_t line_begin = i;
        for (; i < items.size(); ++i) {
            const item_t& item = items[i];
            const bool first = i == line_begin;
            const std::uint32_t space = !first && line.info[i].space ? 1 : 0;
            const std::size_t line_break = item.text.find_first_of("\r\n");
            const std::uint32_t width = width_of(item.text.substr(0, line_break));
            if (fit && column + space + width > m_width) {
                // A line that starts too wide doesn't fit at any break.
                if (first) {
                    return false;
                }

                std::ranges::sort(candidates, {}, [](const candidate_t& candidate) {
                    return std::tuple(candidate.tier, candidate.depth, candidate.rank,
                                      ~candidate.item);
                });
                for (const candidate_t& candidate : candidates) {
                    if (tries == 0) {
                        return false;
                    }

                    --tries;
                    m_state.brackets = candidate.brackets;
                    m_state.segments.back() = candidate.segment;
                    end_line(items[candidate.item]);
                    const std::size_t mark = lines.size();
                    lines.push_back(text.substr(0, candidate.length));
                    const std::uint32_t next = continuation_indent(items[candidate.item + 1]);
                    if (layout_from(line, candidate.item + 1, next, candidate.line_depth, fit,
                                    lines, tries)) {
                        return true;
                    }

                    lines.resize(mark);
                }

                return false;
            }

            text.append(space, ' ');
            text += item.text;
            const std::uint32_t start = column + space;
            if (line_break == std::string_view::npos) {
                column = start + width;
            } else {
                const std::size_t last = item.text.find_last_of("\r\n");
                column = width_of(item.text.substr(last + 1));
            }

            place(item, start, column, indent, line_depth, i == line.first_token);
            if (fit && line.info[i].breakable) {
                // After '=' or "=>", when the rest fits on the next line, comes before an
                // opening bracket, and one the rest fits after before one it doesn't, so a line of
                // brackets in brackets breaks once, not at each in turn. After '=' or "=>"
                // otherwise comes last.
                const std::uint32_t rest =
                    line.after[i] - (i + 1 < items.size() && line.info[i + 1].space ? 1 : 0);
                std::uint32_t tier = 0;
                if (line.info[i].tier == 1) {
                    const bracket_t& bracket = m_state.brackets.back();
                    const std::uint32_t inside =
                        bracket.line_indent + ((bracket.signature ? 2 : 1) * indent_width);
                    tier = inside + rest <= m_width ? 2 : 3;
                } else if (line.info[i].tier == 2) {
                    const bool whole =
                        line.info[i].depth == 0 &&
                        m_state.segments.back().indent + indent_width + rest <= m_width;
                    tier = whole ? 1 : 4;
                }

                candidates.push_back({
                    .item = i,
                    .length = text.size(),
                    .tier = tier,
                    .depth = line.info[i].depth,
                    .rank = line.info[i].rank,
                    .brackets = m_state.brackets,
                    .segment = m_state.segments.back(),
                    .line_depth = line_depth,
                });
            }
        }

        end_line(*std::ranges::find_if(items.rbegin(), items.rend(),
                                       [](const item_t& item) { return !item.comment; }));
        lines.push_back(std::move(text));
        return true;
    }
};

/// What a file means, to check formatting kept it: its tokens, and its tree without spans.
std::string meaning(const source_file_t& file, diagnostic_bag_t& diagnostics) {
    trivia_t trivia;
    const std::vector<token_t> tokens = lex_layout(file, diagnostics, trivia);
    layout_t layout;
    const ast_t ast = parse_layout(file, tokens, diagnostics, layout);
    std::string out;
    for (const token_t& token : tokens) {
        // The parser reads a ">=" as a '>' and a '=', which type arguments can end with, as in
        // "List<Int>= []", so it means what they do apart.
        if (token.kind == token_kind_t::GREATER_EQUAL) {
            out += "'>' >\n'=' =\n";
        } else if (token.kind != token_kind_t::NEWLINE) {
            out += describe(token.kind);
            out += ' ';
            out += file.slice(token.span);
            out += '\n';
        }
    }

    // And the doc comments, whose text is Markdown and examples' code.
    for (const span_t comment : trivia.comments) {
        if (is_doc_comment(file.slice(comment))) {
            out += std::format("doc {}\n", file.slice(comment));
        }
    }

    out += dump_ast(ast);
    return out;
}

/// The errors in a bag, which stop formatting.
std::vector<diagnostic_t> errors_of(const diagnostic_bag_t& diagnostics) {
    std::vector<diagnostic_t> errors;
    std::ranges::copy_if(
        diagnostics.diagnostics(), std::back_inserter(errors),
        [](const diagnostic_t& diagnostic) { return diagnostic.severity == severity_t::ERROR; });
    return errors;
}

/// Whether the file parses as some build reads it, with its #If branches.
bool parses_in_a_build(const source_file_t& file) {
    static constexpr std::array<std::array<std::string_view, 2>, 3> builds = {
        {
            { "DEBUG", "PLATFORM_WINDOWS" },
            { "RELEASE", "PLATFORM_LINUX" },
            { "DEBUG", "PLATFORM_MACOS" },
        },
    };
    return std::ranges::any_of(builds, [&](const std::array<std::string_view, 2>& defined) {
        diagnostic_bag_t diagnostics;
        const std::vector<token_t> tokens = lex(file, diagnostics, defined);
        static_cast<void>(parse(file, tokens, diagnostics));
        return !diagnostics.has_errors();
    });
}

} // namespace

bool same_meaning(const source_file_t& file, const std::string_view formatted) {
    const source_file_t after_file(file.id(), file.path(), std::string(formatted));
    diagnostic_bag_t before;
    diagnostic_bag_t after;
    return meaning(file, before) == meaning(after_file, after) && !after.has_errors();
}

std::expected<std::string, std::vector<diagnostic_t>> format_unchecked(const source_file_t& file,
                                                                       const std::uint32_t width) {
    diagnostic_bag_t diagnostics;
    trivia_t trivia;
    const std::vector<token_t> tokens = lex_layout(file, diagnostics, trivia);
    layout_t layout;
    static_cast<void>(parse_layout(file, tokens, diagnostics, layout));
    // The formatter leaves #If lines as they are and lays out the rest, which doesn't parse
    // when an #If holds part of a statement, as a block's first line, though each build does.
    if (diagnostics.has_errors() && !trivia.conditionals.empty() && parses_in_a_build(file)) {
        const std::uint32_t error = errors_of(diagnostics).front().span.begin;
        span_t region = trivia.conditionals.front();
        for (const span_t lines : trivia.conditionals) {
            region = lines.begin <= error ? lines : region;
        }

        const std::uint32_t line_end = std::min(
            region.end, static_cast<std::uint32_t>(std::min(
                            file.text().find_first_of("\r\n", region.begin), file.text().size())));
        diagnostic_bag_t split;
        split.error({ .file = file.id(), .begin = region.begin, .end = line_end },
                    "fb fmt leaves an #If's lines as they are, so it formats a file only when each "
                    "#If holds whole statements, and this one holds part of one");
        return std::unexpected(errors_of(split));
    }

    if (diagnostics.has_errors()) {
        return std::unexpected(errors_of(diagnostics));
    }

    return formatter_t(file, tokens, trivia, layout, width).run();
}

std::expected<std::string, std::vector<diagnostic_t>> format_source(const source_file_t& file,
                                                                    const std::uint32_t width) {
    std::expected<std::string, std::vector<diagnostic_t>> text = format_unchecked(file, width);
    if (!text) {
        return text;
    }

    // Formatting changes only the space between tokens, so what the file means is the same:
    // anything else is a bug, and the file is left as it is.
    if (!same_meaning(file, *text)) {
        diagnostic_bag_t bug;
        bug.error({ .file = file.id(), .begin = 0, .end = 0 },
                  "fb fmt would change what this file means, which is a bug in it; the file is "
                  "left as it is");
        return std::unexpected(errors_of(bug));
    }

    return text;
}

} // namespace fb
