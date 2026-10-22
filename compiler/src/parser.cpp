#include "fb/parser.hpp"

#include "fb/literal.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

/// From docs/spec/03-operators.md. Higher binds tighter; unary and postfix operators are handled
/// by the structure of the parser instead.
namespace precedence {
constexpr std::uint32_t ternary = 1;
constexpr std::uint32_t coalesce = 2;
constexpr std::uint32_t logical_or = 3;
constexpr std::uint32_t logical_and = 4;
constexpr std::uint32_t equality = 5;
constexpr std::uint32_t comparison = 6;
constexpr std::uint32_t type_test = 7;
constexpr std::uint32_t bit_or = 8;
constexpr std::uint32_t bit_xor = 9;
constexpr std::uint32_t bit_and = 10;
constexpr std::uint32_t shift = 11;
constexpr std::uint32_t additive = 12;
constexpr std::uint32_t multiplicative = 13;
constexpr std::uint32_t power = 14;
} // namespace precedence

/// A binary operator found at the current token.
struct binary_t {
    binary_op_t op;
    std::uint32_t precedence;
    std::uint32_t tokens = 1; // ">>" is two '>' tokens
    bool right_associative = false;
};

/// An assignment operator found at the current token. `op` is empty for a plain '=', and holds
/// the operator of a compound one, like ADD for "+=".
struct assignment_t {
    std::optional<binary_op_t> op;
    std::uint32_t tokens = 1; // ">>=" is '>' then ">="
};

/// What an Extern block's Functions take from it while it's parsed: how they're called, and
/// the block's visibility, if it has one.
struct extern_block_t {
    external_t external;
    std::optional<visibility_t> visibility;
};

/// The visibility a keyword says, or nullopt when it isn't Private, Protected, Package, or
/// Public.
constexpr std::optional<visibility_t> visibility_of(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::KW_PRIVATE:
        return visibility_t::PRIVATE;
    case token_kind_t::KW_PROTECTED:
        return visibility_t::PROTECTED;
    case token_kind_t::KW_PACKAGE:
        return visibility_t::PACKAGE;
    case token_kind_t::KW_PUBLIC:
        return visibility_t::PUBLIC;
    default:
        return std::nullopt;
    }
}

/// Keywords that end a block when they start a line.
constexpr bool is_block_end(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::END_OF_FILE:
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

/// True when nothing, not even a space, separates the tokens. ">>", "As?", and "T?" must be
/// written that way (docs/spec/03-operators.md).
constexpr bool touches(const token_t& left, const token_t& right) {
    return left.span.end == right.span.begin;
}

/// Whether text could be an identifier: ASCII letters, digits, and '_', not starting with a
/// digit, and not a keyword.
bool is_name(const std::string_view text) {
    const auto letter = [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto letter_or_digit = [&](const char c) { return letter(c) || (c >= '0' && c <= '9'); };
    return !text.empty() && letter(text.front()) && std::ranges::all_of(text, letter_or_digit) &&
           !keyword_kind(text);
}

/// Recursive descent for statements, with precedence climbing for binary operators.
/// A NEWLINE token ends a statement. Inside brackets, peek() and advance() skip NEWLINE tokens,
/// and after an operator, '=', ',', or "=>", skip_continuation_newlines() does.
/// Errors don't stop the parse: each line reports at most one, the rest of the line is skipped,
/// and parsing picks up on the next line. A block's statements stop at any block-ending keyword,
/// so a missing End can't swallow the enclosing block. Missing pieces become error nodes or
/// empty names, so later stages always get a whole tree.
class parser_t {
public:
    parser_t(const source_file_t& file, const std::span<const token_t> tokens,
             diagnostic_bag_t& diagnostics, ast_t& ast)
        : m_file(file), m_tokens(tokens), m_diagnostics(diagnostics), m_ast(ast) {
        assert(!m_tokens.empty() && m_tokens.back().kind == token_kind_t::END_OF_FILE);
    }

    void run() {
        while (!at(token_kind_t::END_OF_FILE)) {
            const std::size_t start = m_pos;
            // No block is open at the top level, so a block-ending keyword has nothing to end.
            if (is_block_end(peek().kind)) {
                error(peek().span, "unexpected {}", describe_current());
            } else {
                m_ast.add_item(parse_statement());
            }

            finish_line(start);
        }
    }

private:
    const source_file_t& m_file;
    std::span<const token_t> m_tokens;
    diagnostic_bag_t& m_diagnostics;
    ast_t& m_ast;
    std::vector<token_kind_t> m_blocks; // keywords of the open blocks, innermost last
    std::size_t m_pos = 0;
    std::uint32_t m_previous_end = 0; // where the last consumed token ends
    /// Open brackets and string interpolations. While it's above 0, NEWLINE tokens are skipped.
    std::uint32_t m_bracket_depth = 0;
    bool m_line_failed = false; // the current line already has an error
    /// How many expressions and blocks the one being parsed is inside, which is how deep the
    /// compiler's passes go into it. Past max_nesting it's an error, not a crash, and the rest of
    /// the file's errors would be its fallout.
    std::uint32_t m_nesting = 0;
    static constexpr std::uint32_t max_nesting = 1000;
    bool m_too_deep = false;
    /// The Extern block whose Functions are being parsed.
    std::optional<extern_block_t> m_extern;

    // Tokens

    /// Inside brackets line breaks don't end anything, so NEWLINE tokens are stepped over.
    [[nodiscard]] std::size_t skip_newlines(std::size_t index) const {
        while (m_bracket_depth > 0 && m_tokens[index].kind == token_kind_t::NEWLINE) {
            ++index;
        }

        return index;
    }

    [[nodiscard]] const token_t& peek(const std::size_t ahead = 0) const {
        std::size_t index = skip_newlines(m_pos);
        for (std::size_t i = 0; i < ahead && m_tokens[index].kind != token_kind_t::END_OF_FILE;
             ++i) {
            index = skip_newlines(index + 1);
        }

        return m_tokens[index];
    }

    /// Consumes and returns the current token. END_OF_FILE is never consumed.
    const token_t& advance() {
        const std::size_t index = skip_newlines(m_pos);
        const token_t& token = m_tokens[index];
        if (token.kind != token_kind_t::END_OF_FILE) {
            m_pos = index + 1;
            m_previous_end = token.span.end;
        }

        return token;
    }

    [[nodiscard]] bool at(const token_kind_t kind) const {
        return peek().kind == kind;
    }

    bool accept(const token_kind_t kind) {
        if (!at(kind)) {
            return false;
        }

        advance();
        return true;
    }

    /// Consumes a `kind` token, or reports "expected ..." without consuming anything.
    bool expect(const token_kind_t kind) {
        if (accept(kind)) {
            return true;
        }

        report_expected(describe(kind));
        return false;
    }

    /// Like expect(), for a closing bracket. The error also points at the `opening` one.
    void expect_closing(const token_kind_t kind, const span_t opening) {
        if (accept(kind)) {
            return;
        }

        if (diagnostic_t* diagnostic = report_expected(describe(kind))) {
            diagnostic->with_label(opening, "opened here");
        }
    }

    [[nodiscard]] std::string_view text(const token_t& token) const {
        return m_file.slice(token.span);
    }

    /// Consumes the current token as a name. The caller has checked that it's an identifier.
    name_t take_name() {
        const token_t& token = advance();
        return { .text = text(token), .span = token.span };
    }

    /// On error, the name is empty; later stages skip empty names.
    name_t expect_name() {
        if (at(token_kind_t::IDENTIFIER)) {
            return take_name();
        }

        report_expected(describe(token_kind_t::IDENTIFIER));
        // A keyword used as a name is skipped, so the rest of the declaration still parses.
        const token_kind_t next = peek(1).kind;
        if (keyword_kind(text(peek())) &&
            (next == token_kind_t::EQUAL || next == token_kind_t::COLON ||
             next == token_kind_t::LEFT_PAREN)) {
            const token_t& keyword = advance();
            return { .text = {}, .span = point(keyword.span.begin) };
        }

        return { .text = {}, .span = point(peek().span.begin) };
    }

    // Lines

    [[nodiscard]] bool at_line_end() const {
        const token_kind_t kind = peek().kind;
        return kind == token_kind_t::NEWLINE || kind == token_kind_t::END_OF_FILE;
    }

    [[nodiscard]] bool at_line_start() const {
        return m_pos == 0 || m_tokens[m_pos - 1].kind == token_kind_t::NEWLINE;
    }

    /// After an operator, '=', ',', or "=>" the statement goes on past a line break.
    void skip_continuation_newlines() {
        while (m_tokens[m_pos].kind == token_kind_t::NEWLINE) {
            ++m_pos;
        }
    }

    /// Whether the line ends after the next `ahead` tokens.
    [[nodiscard]] bool line_ends(const std::size_t ahead) const {
        const token_kind_t kind = peek(ahead).kind;
        return kind == token_kind_t::NEWLINE || kind == token_kind_t::END_OF_FILE;
    }

    void skip_rest_of_line() {
        while (!at_line_end()) {
            advance();
        }
    }

    /// Ends the statement that began at token `start`. Anything else on its line is an error, and
    /// after any error the rest of the line is skipped.
    void finish_line(const std::size_t start) {
        if (m_pos != start && at_line_start()) {
            return; // a block that ended early left its terminator for the enclosing block
        }

        if (!at_line_end()) {
            report_expected("end of line");
            skip_rest_of_line();
        }

        accept(token_kind_t::NEWLINE);
        m_line_failed = false;
    }

    // Errors

    /// An empty span at `offset`, for something that's missing.
    [[nodiscard]] span_t point(const std::uint32_t offset) const {
        return { .file = m_file.id(), .begin = offset, .end = offset };
    }

    /// From `begin` to the end of the last consumed token.
    [[nodiscard]] span_t span_from(const std::uint32_t begin) const {
        return { .file = m_file.id(), .begin = begin, .end = std::max(begin, m_previous_end) };
    }

    /// One error per line: later errors on a line that already has one are usually fallout.
    template <typename... Args>
    diagnostic_t* error(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        if (m_line_failed || m_too_deep) {
            return nullptr;
        }

        m_line_failed = true;
        return &m_diagnostics.error(span, fmt, std::forward<Args>(args)...);
    }

    /// "expected X, found Y" at the current token. Inside brackets, a line break before it means
    /// the closing bracket was probably forgotten, so the error goes at the end of the line.
    diagnostic_t* report_expected(const std::string_view what) {
        if (peek().kind == token_kind_t::INVALID) {
            m_line_failed = true; // the lexer already reported it
            return nullptr;
        }

        if (m_bracket_depth > 0 && m_tokens[m_pos].kind == token_kind_t::NEWLINE) {
            return error(point(m_previous_end), "expected {}, found newline", what);
        }

        return error(peek().span, "expected {}, found {}", what, describe_current());
    }

    /// The current token as messages show it. "End" includes the keyword after it.
    [[nodiscard]] std::string describe_current() const {
        const token_t& token = peek();
        const token_t& next = peek(1);
        if (token.kind == token_kind_t::KW_END && next.kind != token_kind_t::NEWLINE &&
            next.kind != token_kind_t::END_OF_FILE) {
            const std::uint32_t length = next.span.end - token.span.begin;
            return std::format("'{}'", m_file.text().substr(token.span.begin, length));
        }

        return std::string(describe(token.kind));
    }

    // Nodes

    expr_id_t add_expr(const span_t span, expr_node_t node) {
        return m_ast.add(expr_t{ .span = span, .node = std::move(node) });
    }

    stmt_id_t add_stmt(const span_t span, stmt_node_t node) {
        return m_ast.add(stmt_t{ .span = span, .node = std::move(node) });
    }

    type_expr_id_t add_type(const span_t span, const type_expr_node_t& node) {
        return m_ast.add(type_expr_t{ .span = span, .node = node });
    }

    [[nodiscard]] span_t span_of(const expr_id_t id) const {
        return m_ast.get(id).span;
    }

    /// Stands in for an expression that's missing or not supported yet.
    expr_id_t missing_expr() {
        return add_expr(point(peek().span.begin), error_expr_t{});
    }

    // Blocks

    /// Statements up to the next block-ending keyword.
    block_t parse_block() {
        block_t block;
        if (m_nesting >= max_nesting) {
            report_too_deep();
            return block;
        }

        ++m_nesting;
        while (!is_block_end(peek().kind)) {
            const std::size_t start = m_pos;
            block.push_back(parse_statement());
            finish_line(start);
        }

        --m_nesting;
        return block;
    }

    /// The body of a block opened by `opening`, then its "End <keyword>".
    block_t parse_body(const token_t& opening) {
        m_blocks.push_back(opening.kind);
        block_t body = parse_block();
        expect_end(opening);
        m_blocks.pop_back();
        return body;
    }

    /// Code nested past max_nesting, which the compiler's passes would follow until they ran out
    /// of stack.
    void report_too_deep() {
        error(peek().span,
              "this is nested more than {} deep, deeper than the compiler goes; split it up with "
              "Locals or Functions",
              max_nesting);
        m_too_deep = true;
    }

    /// True when a block around the innermost one was opened by `kind`.
    [[nodiscard]] bool closes_outer_block(const token_kind_t kind) const {
        return std::ranges::contains(std::span(m_blocks).first(m_blocks.size() - 1), kind);
    }

    /// A missing End is left for whichever block it belongs to. A wrong End that closes nothing
    /// else is taken as this block's.
    void expect_end(const token_t& opening) {
        if (at(token_kind_t::KW_END) && peek(1).kind == opening.kind) {
            advance();
            advance();
            return;
        }

        const bool stray_end = at(token_kind_t::KW_END) && !closes_outer_block(peek(1).kind);
        if (diagnostic_t* diagnostic = error(peek().span, "expected 'End {}', found {}",
                                             text(opening), describe_current())) {
            diagnostic->with_label(opening.span, "opened here");
        }

        if (stray_end) {
            advance();
            if (!at_line_end()) {
                advance();
            }
        }
    }

    // Statements

    /// One statement, without its line break: the caller ends the line with finish_line().
    /// Block statements end their own header line and parse through to the end of the block.
    stmt_id_t parse_statement() {
        if (at(token_kind_t::AT)) {
            return parse_attributed();
        }

        if (at(token_kind_t::IDENTIFIER) && text(peek()) == "Attribute" &&
            peek(1).kind == token_kind_t::IDENTIFIER) {
            return parse_attribute_declaration();
        }

        // "Unsafe" alone on its line opens a block, and before a declaration it's a modifier.
        if (at(token_kind_t::KW_UNSAFE)) {
            return line_ends(1) ? parse_unsafe() : parse_modified();
        }

        // "From" is a keyword only where it starts an import line, before a module.
        if (at(token_kind_t::IDENTIFIER) && text(peek()) == "From" &&
            (peek(1).kind == token_kind_t::IDENTIFIER || peek(1).kind == token_kind_t::STRING ||
             peek(1).kind == token_kind_t::STRING_START)) {
            return parse_import();
        }

        switch (peek().kind) {
        case token_kind_t::KW_IMPORT:
            return parse_import();
        case token_kind_t::KW_CONST:
            if (modifiers_lead_to(m_pos + 1, token_kind_t::KW_FUNCTION)) {
                return parse_modified();
            }

            return parse_variable();
        case token_kind_t::KW_LOCAL:
        case token_kind_t::KW_GLOBAL:
        case token_kind_t::KW_FIELD:
            return parse_variable();
        case token_kind_t::KW_FUNCTION:
            return parse_function(function_kind_t::FUNCTION);
        case token_kind_t::KW_OPERATOR:
            return parse_function(function_kind_t::OPERATOR);
        // Struct members parse anywhere; analysis reports the ones outside a Struct.
        case token_kind_t::KW_METHOD:
            return parse_function(function_kind_t::METHOD);
        case token_kind_t::KW_NEW:
            return parse_function(function_kind_t::CONSTRUCTOR);
        case token_kind_t::KW_BYREF:
            if (modifiers_lead_to(m_pos + 1, token_kind_t::KW_METHOD)) {
                return parse_modified();
            }

            return parse_simple();
        case token_kind_t::KW_STRUCT:
        case token_kind_t::KW_CLASS:
        case token_kind_t::KW_INTERFACE:
            return parse_type_declaration({});
        case token_kind_t::KW_EXTEND:
            return parse_extend();
        case token_kind_t::KW_EXTERN:
            return parse_extern({});
        case token_kind_t::KW_WHERE: {
            const token_t& keyword = advance();
            error(keyword.span,
                  "a Where line goes at the top of a generic Function, Method, Struct, or Class, "
                  "before the rest of its body");
            skip_rest_of_line();
            return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
        }
        case token_kind_t::KW_ABSTRACT:
        case token_kind_t::KW_FINAL:
        case token_kind_t::KW_OVERRIDE:
        case token_kind_t::KW_STATIC:
        case token_kind_t::KW_READONLY:
        case token_kind_t::KW_SHARED:
        case token_kind_t::KW_ASYNC:
        case token_kind_t::KW_PRIVATE:
        case token_kind_t::KW_PROTECTED:
        case token_kind_t::KW_PACKAGE:
        case token_kind_t::KW_PUBLIC:
            return parse_modified();
        case token_kind_t::KW_PROPERTY:
            return parse_property({});
        case token_kind_t::KW_ENUM:
            return parse_enum(false, peek().span.begin);
        case token_kind_t::KW_DEFER: {
            // Analysis checks which statements can be deferred.
            const token_t& keyword = advance();
            const stmt_id_t statement = parse_statement();
            return add_stmt(span_from(keyword.span.begin), defer_stmt_t{ .statement = statement });
        }
        case token_kind_t::KW_THROW: {
            const token_t& keyword = advance();
            const expr_id_t error = parse_expression();
            return add_stmt(span_from(keyword.span.begin), throw_stmt_t{ .error = error });
        }
        case token_kind_t::KW_TRY:
            if (opens_try_block()) {
                return parse_try();
            }

            return parse_simple();
        case token_kind_t::KW_IF:
            return parse_if();
        case token_kind_t::KW_WHILE:
            return parse_while({});
        case token_kind_t::KW_REPEAT:
            return parse_repeat({});
        case token_kind_t::KW_LOOP:
            return parse_loop({});
        case token_kind_t::KW_FOR:
            return parse_for({});
        case token_kind_t::KW_PARALLEL:
            return parse_parallel({});
        case token_kind_t::KW_SELECT:
            return parse_select();
        case token_kind_t::KW_LOCK:
            return parse_lock();
        case token_kind_t::KW_BREAK:
        case token_kind_t::KW_CONTINUE:
            return parse_jump();
        case token_kind_t::KW_RETURN:
            return parse_return();
        case token_kind_t::IDENTIFIER:
            if (peek(1).kind == token_kind_t::COLON) {
                return parse_labeled();
            }

            return parse_simple();
        default:
            return parse_simple();
        }
    }

    /// "Try" alone on its line opens a Try block. "Try F()" is a call with Try before it.
    [[nodiscard]] bool opens_try_block() const {
        const token_kind_t next = peek(1).kind;
        return next == token_kind_t::NEWLINE || next == token_kind_t::END_OF_FILE;
    }

    /// "Try", its body, each "Catch e As Type" or "Catch e" with its code, then "Finally" with its
    /// code, and "End Try". It needs a Catch or a Finally.
    stmt_id_t parse_try() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        finish_line(start);
        m_blocks.push_back(keyword.kind);
        block_t body = parse_block();
        std::vector<catch_t> catches;
        while (at(token_kind_t::KW_CATCH)) {
            const std::size_t line = m_pos;
            advance();
            const name_t name = expect_name();
            std::optional<type_expr_id_t> type;
            if (accept(token_kind_t::KW_AS)) {
                type = parse_type();
            }

            finish_line(line);
            catches.push_back({ .name = name, .type = type, .body = parse_block() });
        }

        std::optional<block_t> finally_body;
        if (at(token_kind_t::KW_FINALLY)) {
            const std::size_t line = m_pos;
            advance();
            finish_line(line);
            finally_body = parse_block();
        }

        if (catches.empty() && !finally_body) {
            error(keyword.span, "a Try needs a Catch or a Finally");
        }

        expect_end(keyword);
        m_blocks.pop_back();
        return add_stmt(span_from(keyword.span.begin), try_stmt_t{
                                                           .body = std::move(body),
                                                           .catches = std::move(catches),
                                                           .finally_body = std::move(finally_body),
                                                       });
    }

    /// "@Flags Enum ...". On anything else, @Flags is an error, and the statement still parses.
    /// "Enum Name: Type", its members, then "End Enum". Members are separated by commas, which
    /// can end a line, or by lines.
    stmt_id_t parse_enum(const bool flags, const std::uint32_t begin, modifiers_t modifiers = {}) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t name = expect_name();
        std::optional<type_expr_id_t> type;
        if (accept(token_kind_t::COLON)) {
            type = parse_type();
        }

        finish_line(start);
        m_blocks.push_back(keyword.kind);
        std::vector<enum_member_t> members;
        while (!is_block_end(peek().kind)) {
            const std::size_t line = m_pos;
            while (at(token_kind_t::IDENTIFIER) || visibility_of(peek().kind)) {
                if (const std::optional<visibility_t> written = visibility_of(peek().kind)) {
                    error(advance().span,
                          "an Enum's members have its visibility, so they take no {}{}",
                          spelling(*written),
                          *written == visibility_t::PROTECTED ? "" : "; put it before the Enum");
                    continue;
                }

                const name_t member = take_name();
                std::optional<expr_id_t> value;
                if (accept(token_kind_t::EQUAL)) {
                    value = parse_expression();
                }

                members.push_back({ .name = member, .value = value });
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }

                skip_continuation_newlines();
            }

            if (m_pos == line) {
                report_expected("a member's name");
            }

            finish_line(line);
        }

        expect_end(keyword);
        m_blocks.pop_back();
        enum_stmt_t declared{
            .name = name,
            .type = type,
            .members = std::move(members),
            .flags = flags,
            .modifiers = std::move(modifiers),
        };
        return add_stmt(span_from(begin), std::move(declared));
    }

    /// Whether a statement that starts with `kind` declares something, which modifiers and
    /// attributes can go before.
    static bool starts_declaration(const token_kind_t kind) {
        modifiers_t ignored;
        switch (kind) {
        case token_kind_t::KW_STRUCT:
        case token_kind_t::KW_CLASS:
        case token_kind_t::KW_INTERFACE:
        case token_kind_t::KW_FUNCTION:
        case token_kind_t::KW_OPERATOR:
        case token_kind_t::KW_METHOD:
        case token_kind_t::KW_NEW:
        case token_kind_t::KW_CONST:
        case token_kind_t::KW_FIELD:
        case token_kind_t::KW_LOCAL:
        case token_kind_t::KW_GLOBAL:
        case token_kind_t::KW_PROPERTY:
        case token_kind_t::KW_BYREF:
            return true;
        default:
            return visibility_of(kind) || modifier_flag(ignored, kind) != nullptr;
        }
    }

    /// Whether the visibilities and modifiers from token `index` on come before a `kind`, as in
    /// "Const Public Function", where they're between the words of one.
    [[nodiscard]] bool modifiers_lead_to(std::size_t index, const token_kind_t kind) const {
        modifiers_t ignored;
        while (visibility_of(m_tokens[index].kind) ||
               modifier_flag(ignored, m_tokens[index].kind) != nullptr) {
            ++index;
        }

        return m_tokens[index].kind == kind;
    }

    /// Private, Protected, Package, or Public at the current token, into `modifiers`. A
    /// declaration has one.
    void take_visibility(modifiers_t& modifiers) {
        const token_t& token = advance();
        if (modifiers.visibility_span) {
            error(token.span, "this declaration is {} already, and has only one visibility",
                  spelling(modifiers.visibility));
            return;
        }

        modifiers.visibility = visibility_of(token.kind).value_or(visibility_t::PRIVATE);
        modifiers.visibility_span = token.span;
    }

    /// Which of `modifiers` a keyword sets, or null when it isn't one.
    static bool* modifier_flag(modifiers_t& modifiers, const token_kind_t kind) {
        switch (kind) {
        case token_kind_t::KW_ABSTRACT:
            return &modifiers.is_abstract;
        case token_kind_t::KW_FINAL:
            return &modifiers.is_final;
        case token_kind_t::KW_OVERRIDE:
            return &modifiers.is_override;
        case token_kind_t::KW_STATIC:
            return &modifiers.is_static;
        case token_kind_t::KW_READONLY:
            return &modifiers.is_read_only;
        case token_kind_t::KW_SHARED:
            return &modifiers.is_shared;
        case token_kind_t::KW_ASYNC:
            return &modifiers.is_async;
        case token_kind_t::KW_UNSAFE:
            return &modifiers.is_unsafe;
        default:
            return nullptr;
        }
    }

    /// Abstract, Final, Override, Static, ReadOnly, Shared, and a visibility like Public, in any
    /// order, then the declaration they're on, which may be a "Const Function", or with only a
    /// visibility, an Enum or an Attribute. Analysis checks which ones it can have, but Shared
    /// only goes before a type.
    stmt_id_t parse_modified(modifiers_t modifiers = {}) {
        std::optional<span_t> shared;
        std::optional<span_t> async;
        std::optional<span_t> unsafe;
        const token_t* unsafe_keyword = nullptr;
        bool flagged = false;
        bool by_ref = false;
        while (true) {
            if (visibility_of(peek().kind)) {
                take_visibility(modifiers);
                continue;
            }

            // "Const Function" and "ByRef Method" can have the others between their words.
            const bool is_const = at(token_kind_t::KW_CONST) &&
                                  modifiers_lead_to(m_pos + 1, token_kind_t::KW_FUNCTION);
            if (is_const || (at(token_kind_t::KW_BYREF) &&
                             modifiers_lead_to(m_pos + 1, token_kind_t::KW_METHOD))) {
                const token_t& token = advance();
                if (is_const ? modifiers.is_const : by_ref) {
                    error(token.span, "{} is written twice", describe(token.kind));
                }

                (is_const ? modifiers.is_const : by_ref) = true;
                flagged = true;
                continue;
            }

            bool* flag = modifier_flag(modifiers, peek().kind);
            if (flag == nullptr) {
                break;
            }

            flagged = true;
            const token_t& token = advance();
            if (*flag) {
                error(token.span, "{} is written twice", describe(token.kind));
            }

            if (token.kind == token_kind_t::KW_SHARED) {
                shared = token.span;
            } else if (token.kind == token_kind_t::KW_ASYNC) {
                async = token.span;
            } else if (token.kind == token_kind_t::KW_UNSAFE) {
                unsafe = token.span;
                unsafe_keyword = &token;
            }

            *flag = true;
        }

        if (shared && !at(token_kind_t::KW_CLASS) && !at(token_kind_t::KW_STRUCT) &&
            !at(token_kind_t::KW_INTERFACE)) {
            error(*shared, "Shared only goes before a Class, whose objects threads share");
        }

        if (async && !at(token_kind_t::KW_FUNCTION) && !at(token_kind_t::KW_METHOD) &&
            !at(token_kind_t::KW_BYREF)) {
            error(*async, "Async only goes before a Function or Method, whose code can Await");
        }

        // Analysis says which Functions can't be Unsafe, like an Operator or a Const Function.
        const bool function_follows =
            at(token_kind_t::KW_FUNCTION) || at(token_kind_t::KW_METHOD) ||
            at(token_kind_t::KW_BYREF) || at(token_kind_t::KW_OPERATOR) ||
            (at(token_kind_t::KW_CONST) && peek(1).kind == token_kind_t::KW_FUNCTION);
        if (unsafe && !function_follows) {
            error(*unsafe, "Unsafe goes before a Function or Method, or alone on a line, where it "
                           "starts an Unsafe block");
            // Alone on its line after other modifiers, it's still a block, whose body and End
            // are taken as one.
            if (at_line_end() && unsafe_keyword != nullptr) {
                finish_line(m_pos);
                block_t body = parse_body(*unsafe_keyword);
                return add_stmt(span_from(unsafe_keyword->span.begin),
                                unsafe_stmt_t{ .body = std::move(body) });
            }
        }

        switch (peek().kind) {
        case token_kind_t::KW_STRUCT:
        case token_kind_t::KW_CLASS:
        case token_kind_t::KW_INTERFACE:
            return parse_type_declaration(modifiers);
        case token_kind_t::KW_FUNCTION:
            return parse_function(function_kind_t::FUNCTION, modifiers);
        case token_kind_t::KW_OPERATOR:
            return parse_function(function_kind_t::OPERATOR, modifiers);
        case token_kind_t::KW_METHOD:
            return parse_function(by_ref ? function_kind_t::BYREF_METHOD : function_kind_t::METHOD,
                                  modifiers);
        case token_kind_t::KW_NEW:
            return parse_function(function_kind_t::CONSTRUCTOR, modifiers);
        case token_kind_t::KW_CONST:
        case token_kind_t::KW_FIELD:
        case token_kind_t::KW_LOCAL:
        case token_kind_t::KW_GLOBAL:
            return parse_variable(modifiers);
        case token_kind_t::KW_PROPERTY:
            return parse_property(modifiers);
        case token_kind_t::KW_EXTERN:
            if (flagged) {
                error(peek().span, "an Extern block takes only a visibility, like 'Public Extern'");
            }

            return parse_extern(modifiers);
        default:
            break;
        }

        if (!flagged && modifiers.visibility_span) {
            return parse_visible(modifiers);
        }

        // Lows 71, 78, 96, 120, 123: attributes come first, then the modifiers.
        if (at(token_kind_t::AT)) {
            error(peek().span,
                  "attributes go before a declaration's modifiers, as in @Inline Static Function");
            return parse_attributed();
        }

        error(peek().span, "{} doesn't take modifiers like Static or Final", describe_current());
        return parse_statement();
    }

    /// The rest of a statement after its visibility, which isn't one parse_modified() takes: an
    /// Enum or an Attribute declaration. Anything else is reported, then parsed.
    stmt_id_t parse_visible(const modifiers_t& modifiers) {
        if (at(token_kind_t::KW_ENUM)) {
            return parse_enum(false, peek().span.begin, modifiers);
        }

        if (at(token_kind_t::IDENTIFIER) && text(peek()) == "Attribute" &&
            peek(1).kind == token_kind_t::IDENTIFIER) {
            return parse_attribute_declaration(modifiers);
        }

        const span_t written = modifiers.visibility_span.value_or(peek().span);
        if (at(token_kind_t::AT)) {
            error(written, "{0} goes after a declaration's attributes, as in @Inline {0} Function",
                  spelling(modifiers.visibility));
            return parse_attributed();
        }

        if (at(token_kind_t::KW_EXTEND)) {
            error(written, "an Extend has no visibility of its own; put {} before its members",
                  spelling(modifiers.visibility));
        } else {
            error(written, "{} only goes before a declaration, like a Function or a Field",
                  spelling(modifiers.visibility));
        }

        return parse_statement();
    }

    /// "Property Name: Type", then "Get => value" or a Get block, and maybe "Set(value)" and its
    /// block, then "End Property". Get and Set are names everywhere else. In an Interface, or with
    /// Abstract, it's the first line alone, unless a Get or Set follows.
    stmt_id_t parse_property(const modifiers_t& modifiers) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t name = expect_name();
        const type_expr_id_t type = expect(token_kind_t::COLON)
                                        ? parse_type()
                                        : add_type(point(peek().span.begin), error_type_t{});
        if ((modifiers.is_abstract || in_interface()) && !next_line_is_accessor()) {
            // The caller finishes the line.
            return add_stmt(span_from(keyword.span.begin), property_stmt_t{
                                                               .name = name,
                                                               .type = type,
                                                               .modifiers = modifiers,
                                                               .required = true,
                                                           });
        }

        finish_line(start);
        m_blocks.push_back(keyword.kind);
        std::optional<stmt_id_t> getter;
        std::optional<stmt_id_t> setter;
        while (!is_block_end(peek().kind)) {
            const std::size_t line = m_pos;
            if (const std::optional<visibility_t> written = visibility_of(peek().kind)) {
                error(advance().span,
                      "a Property's Get and Set have its visibility, so they take no {}",
                      spelling(*written));
            }

            const bool word = at(token_kind_t::IDENTIFIER);
            if (word && text(peek()) == "Get" && !getter) {
                getter = parse_getter(name, type);
            } else if (word && text(peek()) == "Set" && !setter) {
                setter = parse_setter(name, type);
            } else {
                report_expected(getter ? "'Set'" : "'Get'");
            }

            finish_line(line);
        }

        if (!getter) {
            error(name.span, "a Property needs a Get");
        }

        expect_end(keyword);
        m_blocks.pop_back();
        return add_stmt(span_from(keyword.span.begin), property_stmt_t{
                                                           .name = name,
                                                           .type = type,
                                                           .getter = getter,
                                                           .setter = setter,
                                                           .modifiers = modifiers,
                                                       });
    }

    /// Whether the members being parsed are an Interface's.
    [[nodiscard]] bool in_interface() const {
        return !m_blocks.empty() && m_blocks.back() == token_kind_t::KW_INTERFACE;
    }

    /// The first token of the line after this one.
    [[nodiscard]] const token_t& next_line_start() const {
        std::size_t index = m_pos;
        while (m_tokens[index].kind != token_kind_t::NEWLINE &&
               m_tokens[index].kind != token_kind_t::END_OF_FILE) {
            ++index;
        }

        return m_tokens[index].kind == token_kind_t::NEWLINE ? m_tokens[index + 1]
                                                             : m_tokens[index];
    }

    /// Whether the next line starts with Get or Set, so a Property has its parts.
    [[nodiscard]] bool next_line_is_accessor() const {
        const token_t& next = next_line_start();
        return next.kind == token_kind_t::IDENTIFIER &&
               (text(next) == "Get" || text(next) == "Set");
    }

    /// Whether the next line starts an Interface Method's body, rather than another member, maybe
    /// after its attributes, or the Interface's End: a Method without one is only its first line.
    [[nodiscard]] bool next_line_is_body() const {
        const token_t& next = next_line_start();
        switch (next.kind) {
        case token_kind_t::AT:
        case token_kind_t::KW_METHOD:
        case token_kind_t::KW_BYREF:
        case token_kind_t::KW_STATIC:
        case token_kind_t::KW_OPERATOR:
        case token_kind_t::KW_PROPERTY:
        case token_kind_t::KW_FUNCTION:
        case token_kind_t::KW_FIELD:
        case token_kind_t::KW_NEW:
        case token_kind_t::KW_ABSTRACT:
        case token_kind_t::KW_FINAL:
        case token_kind_t::KW_OVERRIDE:
        case token_kind_t::KW_READONLY:
        case token_kind_t::KW_PRIVATE:
        case token_kind_t::KW_PROTECTED:
        case token_kind_t::KW_PACKAGE:
        case token_kind_t::KW_PUBLIC:
        case token_kind_t::END_OF_FILE:
            return false;
        case token_kind_t::KW_END: {
            const auto after = static_cast<std::size_t>(&next - m_tokens.data()) + 1;
            return after >= m_tokens.size() || m_tokens[after].kind != token_kind_t::KW_INTERFACE;
        }
        // "Unsafe Method" is a member, and "Unsafe" alone starts a block in a body.
        case token_kind_t::KW_UNSAFE: {
            const auto after = static_cast<std::size_t>(&next - m_tokens.data()) + 1;
            return after >= m_tokens.size() || m_tokens[after].kind == token_kind_t::NEWLINE ||
                   m_tokens[after].kind == token_kind_t::END_OF_FILE;
        }
        default:
            return true;
        }
    }

    /// "Get => value", or "Get", its block, and "End Get": a Method that returns the Property's
    /// value.
    stmt_id_t parse_getter(const name_t& property, const type_expr_id_t type) {
        const std::size_t line = m_pos;
        const token_t& word = advance();
        block_t body;
        if (accept(token_kind_t::ARROW)) {
            skip_continuation_newlines();
            const expr_id_t value = parse_expression();
            body.push_back(add_stmt(span_of(value), return_stmt_t{ .value = value }));
        } else {
            finish_line(line);
            body = parse_accessor_body(word);
        }

        return add_stmt(span_from(word.span.begin),
                        function_stmt_t{
                            .kind = function_kind_t::METHOD,
                            .name = { .text = property.text, .span = word.span },
                            .return_type = type,
                            .body = std::move(body),
                        });
    }

    /// "Set(value)", its block, and "End Set": a Method that takes the Property's new value.
    stmt_id_t parse_setter(const name_t& property, const type_expr_id_t type) {
        const std::size_t line = m_pos;
        const token_t& word = advance();
        expect(token_kind_t::LEFT_PAREN);
        const name_t value = expect_name();
        expect(token_kind_t::RIGHT_PAREN);
        finish_line(line);
        block_t body = parse_accessor_body(word);
        return add_stmt(span_from(word.span.begin),
                        function_stmt_t{
                            .kind = function_kind_t::METHOD,
                            .name = { .text = property.text, .span = word.span },
                            .parameters = { { .name = value, .by_ref = false, .type = type } },
                            .body = std::move(body),
                        });
    }

    /// A Get's or Set's block, then "End Get" or "End Set", whose second word is a name.
    block_t parse_accessor_body(const token_t& word) {
        block_t body = parse_block();
        if (at(token_kind_t::KW_END) && peek(1).kind == token_kind_t::IDENTIFIER &&
            text(peek(1)) == text(word)) {
            advance();
            advance();
        } else if (diagnostic_t* diagnostic = error(peek().span, "expected 'End {}', found {}",
                                                    text(word), describe_current())) {
            diagnostic->with_label(word.span, "opened here");
        }

        return body;
    }

    /// "Unsafe ... End Unsafe".
    stmt_id_t parse_unsafe() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        finish_line(start);
        block_t body = parse_body(keyword);
        return add_stmt(span_from(keyword.span.begin), unsafe_stmt_t{ .body = std::move(body) });
    }

    /// "@Inline", "@Deprecated("why")", and "@Flags", then the declaration they're on.
    /// Analysis checks which declarations can have them.
    stmt_id_t parse_attributed() {
        const std::uint32_t begin = peek().span.begin;
        modifiers_t modifiers;
        bool flags = false;
        span_t flags_span;
        span_t c_layout_span;
        while (at(token_kind_t::AT)) {
            advance();
            name_t name = expect_name();
            // "@C.Rename": an Attribute after the qualifier its module's Import gives.
            std::optional<name_t> qualifier;
            if (accept(token_kind_t::DOT)) {
                qualifier = name;
                name = expect_name();
            }

            std::optional<span_t> span;
            std::vector<argument_t> arguments = parse_attribute_arguments(span);
            const bool builtin =
                !qualifier && (name.text == "Flags" || name.text == "Inline" ||
                               name.text == "Deprecated" || name.text == "CLayout" ||
                               name.text == "PanicsAtCaller" || name.text == "MainThread");
            const bool repeated =
                !qualifier && ((name.text == "Flags" && flags) ||
                               (name.text == "Inline" && modifiers.is_inline) ||
                               (name.text == "CLayout" && modifiers.is_c_layout) ||
                               (name.text == "MainThread" && modifiers.is_main_thread) ||
                               (name.text == "PanicsAtCaller" && modifiers.panics_at_caller) ||
                               (name.text == "Deprecated" && modifiers.deprecated));
            if (qualifier) {
                modifiers.attributes.push_back({
                    .qualifier = qualifier,
                    .name = name,
                    .arguments = std::move(arguments),
                });
            } else if (repeated) {
                error(name.span, "@{} is written twice", name.text);
            } else if (name.text == "Flags") {
                flags = true;
                flags_span = name.span;
            } else if (name.text == "Inline") {
                modifiers.is_inline = true;
            } else if (name.text == "MainThread") {
                modifiers.is_main_thread = true;
            } else if (name.text == "PanicsAtCaller") {
                modifiers.panics_at_caller = true;
            } else if (name.text == "Deprecated") {
                modifiers.deprecated = deprecation(arguments, span);
            } else if (name.text == "CLayout") {
                modifiers.is_c_layout = true;
                c_layout_span = name.span;
            } else if (name.text == "Serialize") {
                error(name.span, "@{} isn't supported yet", name.text);
            } else if (!name.text.empty()) {
                // One an Attribute declaration declares, which analysis finds.
                modifiers.attributes.push_back({ .name = name, .arguments = std::move(arguments) });
            }

            if (span && builtin && name.text != "Deprecated") {
                error(*span, "@{} takes no arguments", name.text);
            }

            // An attribute can be on its own line, before the declaration.
            accept(token_kind_t::NEWLINE);
        }

        // "@Flags Public Enum Layer": the visibility comes after the attributes.
        while (visibility_of(peek().kind)) {
            take_visibility(modifiers);
        }

        // "@CLayout Shared Struct" is before a Struct too, which the Shared error is about.
        const bool before_struct =
            at(token_kind_t::KW_STRUCT) ||
            (at(token_kind_t::KW_SHARED) && peek(1).kind == token_kind_t::KW_STRUCT);
        if (modifiers.is_c_layout && !before_struct) {
            error(c_layout_span, "@CLayout only goes before a Struct, which it lays out as C does");
            modifiers.is_c_layout = false;
        }

        if (at(token_kind_t::KW_ENUM)) {
            return parse_enum(flags, begin, std::move(modifiers));
        }

        if (flags) {
            error(flags_span, "@Flags only goes before an Enum");
        }

        if (at(token_kind_t::KW_EXTERN)) {
            error(peek().span,
                  "an Extern block takes no attributes; put them before its Functions");
            return parse_statement();
        }

        if (!starts_declaration(peek().kind)) {
            error(peek().span, "attributes go before a declaration, like a Function or a Struct");
            return parse_statement();
        }

        return parse_modified(std::move(modifiers));
    }

    /// An attribute's arguments in parentheses, and in `span`, where they are. None without
    /// parentheses. Like a call's, they can be named.
    std::vector<argument_t> parse_attribute_arguments(std::optional<span_t>& span) {
        std::vector<argument_t> arguments;
        if (!at(token_kind_t::LEFT_PAREN)) {
            return arguments;
        }

        const token_t& open = advance();
        ++m_bracket_depth;
        while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
            std::optional<name_t> name;
            if (at(token_kind_t::IDENTIFIER) && peek(1).kind == token_kind_t::COLON) {
                name = take_name();
                advance(); // ':'
            }

            const bool by_ref = accept(token_kind_t::KW_BYREF);
            arguments.push_back({ .name = name, .by_ref = by_ref, .value = parse_expression() });
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::RIGHT_PAREN, open.span);
        --m_bracket_depth;
        span = span_from(open.span.begin);
        return arguments;
    }

    /// @Deprecated's message: a string without interpolation, or nothing.
    std::string deprecation(const std::vector<argument_t>& arguments,
                            const std::optional<span_t> span) {
        if (arguments.empty()) {
            return {};
        }

        const argument_t& first = arguments.front();
        const auto* text = std::get_if<string_expr_t>(&m_ast.get(first.value).node);
        const bool plain = arguments.size() == 1 && !first.name && !first.by_ref &&
                           text != nullptr && text->parts.size() <= 1 &&
                           std::ranges::all_of(text->parts, [](const string_part_t& part) {
                               return std::holds_alternative<std::string>(part);
                           });
        if (!plain) {
            error(arguments.size() == 1 ? span_of(first.value) : span.value_or(span_t{}),
                  "@Deprecated takes a message, a string without interpolation");
            return {};
        }

        return text->parts.empty() ? "" : std::get<std::string>(text->parts.front());
    }

    /// "Attribute Rename(Name: String)", which declares an attribute: a Struct whose fields are
    /// its parameters, which have its visibility. "Attribute" is only a keyword here.
    stmt_id_t parse_attribute_declaration(const modifiers_t& modifiers = {}) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t name = expect_name();
        block_t fields;
        if (at(token_kind_t::LEFT_PAREN)) {
            const token_t& open = advance();
            ++m_bracket_depth;
            while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
                const parameter_t parameter = parse_parameter();
                if (parameter.by_ref || parameter.variadic) {
                    error(parameter.name.span, "an Attribute's parameters are plain values");
                }

                fields.push_back(add_stmt(parameter.name.span,
                                          variable_stmt_t{
                                              .kind = variable_kind_t::FIELD,
                                              .name = parameter.name,
                                              .type = parameter.type,
                                              .value = parameter.default_value,
                                              .modifiers = { .visibility = modifiers.visibility },
                                          }));
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }
            }

            expect_closing(token_kind_t::RIGHT_PAREN, open.span);
            --m_bracket_depth;
        }

        finish_line(start);
        return add_stmt(span_from(keyword.span.begin), type_stmt_t{
                                                           .is_attribute = true,
                                                           .name = name,
                                                           .members = std::move(fields),
                                                           .modifiers = modifiers,
                                                       });
    }

    /// "Import std.collections", "Import "./ui/hud"", either with "As C" after it, or "From
    /// std.math Import Vec3, Lerp". Analysis checks that it comes before the file's other
    /// statements.
    stmt_id_t parse_import() {
        const token_t& keyword = advance();
        import_stmt_t node{ .from = keyword.kind != token_kind_t::KW_IMPORT };
        if (!parse_import_target(node)) {
            skip_rest_of_line();
            return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
        }

        if (node.from) {
            if (!expect(token_kind_t::KW_IMPORT)) {
                return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
            }

            node.names.push_back(expect_name());
            while (accept(token_kind_t::COMMA)) {
                skip_continuation_newlines();
                node.names.push_back(expect_name());
            }
        } else if (accept(token_kind_t::KW_AS)) {
            node.alias = expect_name();
        }

        return add_stmt(span_from(keyword.span.begin), std::move(node));
    }

    /// The module an Import or From names: a dotted name, like "std.collections", or a quoted
    /// path, like "./ui/hud", which is plain text. False after an error.
    bool parse_import_target(import_stmt_t& node) {
        const token_t& first = peek();
        if (first.kind == token_kind_t::STRING_START) {
            error(first.span, "an Import's path is plain text, without {{...}}");
            return false;
        }

        if (first.kind == token_kind_t::STRING) {
            advance();
            const std::array pieces{ text(first) };
            std::vector<std::string> decoded = decode_string(pieces);
            node.target = decoded.empty() ? std::string() : std::move(decoded.front());
            node.quoted = true;
            node.target_span = first.span;
            node.last = path_name(first);
            return true;
        }

        if (first.kind != token_kind_t::IDENTIFIER) {
            report_expected("a module, like std.collections, or a quoted path, like \"./hud\"");
            return false;
        }

        node.last = take_name();
        node.target = node.last.text;
        while (accept(token_kind_t::DOT)) {
            if (!at(token_kind_t::IDENTIFIER)) {
                report_expected(describe(token_kind_t::IDENTIFIER));
                return false;
            }

            node.last = take_name();
            node.target += '.';
            node.target += node.last.text;
        }

        node.target_span = span_from(first.span.begin);
        return true;
    }

    /// The file's name in a quoted path, when the path spells it plainly and it could be a
    /// name: "hud" in "./ui/hud". Empty otherwise.
    [[nodiscard]] name_t path_name(const token_t& token) const {
        const std::string_view written = text(token);
        const std::size_t close = written.size() - 1;
        const std::size_t slash = written.rfind('/');
        const std::size_t begin =
            slash == std::string_view::npos ? written.find('"') + 1 : slash + 1;
        const std::string_view stem = begin < close ? written.substr(begin, close - begin) : "";
        if (!is_name(stem)) {
            return { .text = {}, .span = point(token.span.begin) };
        }

        return {
            .text = stem,
            .span = {
                .file = token.span.file,
                .begin = token.span.begin + static_cast<std::uint32_t>(begin),
                .end = token.span.begin + static_cast<std::uint32_t>(close),
            },
        };
    }

    /// A labeled loop, like "Outer: For ...". Break and Continue can name the label.
    stmt_id_t parse_labeled() {
        const name_t label = take_name();
        advance(); // ':'
        switch (peek().kind) {
        case token_kind_t::KW_WHILE:
            return parse_while(label);
        case token_kind_t::KW_REPEAT:
            return parse_repeat(label);
        case token_kind_t::KW_LOOP:
            return parse_loop(label);
        case token_kind_t::KW_FOR:
            return parse_for(label);
        case token_kind_t::KW_PARALLEL:
            return parse_parallel(label);
        default:
            error(label.span, "a label must be followed by a loop");
            return parse_statement();
        }
    }

    /// "Parallel For i = 0 Until n": a counting loop whose passes run at once.
    stmt_id_t parse_parallel(const std::optional<name_t>& label) {
        const token_t& keyword = advance();
        if (!at(token_kind_t::KW_FOR)) {
            error(keyword.span,
                  "Parallel only goes before a For, like 'Parallel For i = 0 Until n'");
            return parse_statement();
        }

        return parse_for(label, keyword.span);
    }

    stmt_id_t parse_variable(const modifiers_t& modifiers = {}) {
        const token_t& keyword = advance();
        modifiers_t written = modifiers;
        if (const std::optional<visibility_t> visibility = visibility_of(peek().kind)) {
            error(peek().span, "{0} goes before {1}, as in {0} {1} x", spelling(*visibility),
                  text(keyword));
            take_visibility(written);
        }

        const name_t name = expect_name();
        std::optional<type_expr_id_t> type;
        if (accept(token_kind_t::COLON)) {
            type = parse_type();
        }

        if (at(token_kind_t::COMMA)) {
            return parse_destructure(keyword, name, type);
        }

        std::optional<expr_id_t> value;
        if (accept(token_kind_t::EQUAL)) {
            skip_continuation_newlines();
            value = parse_expression();
        }

        // expect_name() reported a name that's missing.
        if (name.text.empty()) {
            return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
        }

        variable_kind_t kind = variable_kind_t::CONSTANT;
        if (keyword.kind == token_kind_t::KW_LOCAL) {
            kind = variable_kind_t::LOCAL;
        } else if (keyword.kind == token_kind_t::KW_GLOBAL) {
            kind = variable_kind_t::GLOBAL;
        } else if (keyword.kind == token_kind_t::KW_FIELD) {
            kind = variable_kind_t::FIELD;
        }

        return add_stmt(span_from(keyword.span.begin), variable_stmt_t{
                                                           .kind = kind,
                                                           .name = name,
                                                           .type = type,
                                                           .value = value,
                                                           .modifiers = written,
                                                       });
    }

    /// "Local lo, hi = value", which takes a tuple apart. The names get their elements' types, so
    /// they have none of their own.
    stmt_id_t parse_destructure(const token_t& keyword, const name_t first,
                                const std::optional<type_expr_id_t> first_type) {
        if (keyword.kind != token_kind_t::KW_LOCAL) {
            error(keyword.span, "only a Local can take a tuple apart");
        }

        const auto report_type = [this](const type_expr_id_t type) {
            error(m_ast.get(type).span,
                  "the names that take a tuple apart get their types from its elements");
        };

        if (first_type) {
            report_type(*first_type);
        }

        std::vector<name_t> names{ first };
        while (accept(token_kind_t::COMMA)) {
            names.push_back(expect_name());
            if (accept(token_kind_t::COLON)) {
                report_type(parse_type());
            }
        }

        if (!expect(token_kind_t::EQUAL)) {
            return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
        }

        skip_continuation_newlines();
        const expr_id_t value = parse_expression();
        return add_stmt(span_from(keyword.span.begin),
                        destructure_stmt_t{ .names = std::move(names), .value = value });
    }

    /// A Function, or a Struct's Method, "ByRef Method", or New. New has no name of its own:
    /// the keyword is its name. An Abstract Method is only the line that declares it, and so is
    /// an Interface's Method without a body. Where lines start the body.
    stmt_id_t parse_function(const function_kind_t kind, const modifiers_t& modifiers = {}) {
        const std::size_t start = m_pos;
        const std::uint32_t begin = peek().span.begin;
        // parse_modified() takes ByRef where modifiers come between it and Method.
        if (kind == function_kind_t::BYREF_METHOD && at(token_kind_t::KW_BYREF)) {
            advance();
        }

        const token_t& keyword = advance();
        name_t name{ .text = text(keyword), .span = keyword.span };
        if (kind == function_kind_t::OPERATOR) {
            name = parse_operator_name();
        } else if (kind != function_kind_t::CONSTRUCTOR) {
            name = expect_name();
        }

        std::vector<name_t> type_parameters;
        std::vector<std::optional<type_expr_id_t>> value_types;
        std::vector<constraint_t> constraints;
        if (at(token_kind_t::LESS)) {
            if (kind == function_kind_t::CONSTRUCTOR) {
                error(peek().span, "New can't have type parameters; its Struct or Class can");
            }

            type_parameters = parse_type_parameters(constraints, value_types);
        }

        std::vector<parameter_t> parameters;
        if (at(token_kind_t::LEFT_PAREN)) {
            const token_t& open = advance();
            ++m_bracket_depth;
            while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
                parameters.push_back(parse_parameter());
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }
            }

            expect_closing(token_kind_t::RIGHT_PAREN, open.span);
            --m_bracket_depth;
        } else {
            report_expected(describe(token_kind_t::LEFT_PAREN));
        }

        std::optional<type_expr_id_t> return_type;
        if (at(token_kind_t::COLON)) {
            if (kind == function_kind_t::CONSTRUCTOR) {
                error(peek().span, "New can't have a return type; it makes its Struct");
            }

            advance();
            return_type = parse_type();
        }

        // "Throws FileError, ParseError": the Classes its errors extend.
        std::vector<type_expr_id_t> throws;
        if (accept(token_kind_t::KW_THROWS)) {
            while (true) {
                throws.push_back(parse_type());
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }
            }
        }

        // "Function F(): Int => x" is short for a body that returns x.
        block_t body;
        bool required = false;
        std::optional<external_t> external;
        modifiers_t own = modifiers;
        if (m_extern && kind == function_kind_t::FUNCTION) {
            // An Extern block's Function is its first line alone, maybe with "= "symbol"" after
            // it, and the caller finishes the line.
            external = m_extern->external;
            if (!own.visibility_span && m_extern->visibility) {
                own.visibility = *m_extern->visibility;
            }

            if (accept(token_kind_t::EQUAL)) {
                if (at(token_kind_t::STRING)) {
                    const token_t& written = advance();
                    external->symbol = string_text(written);
                    external->symbol_span = written.span;
                } else {
                    report_expected("the C symbol's name, like \"JPH_CreateBody\"");
                }
            } else if (at(token_kind_t::ARROW)) {
                error(peek().span, "an Extern block's Function has no body; C code defines it");
            } else if (at_line_end() && extern_body_follows()) {
                // One written with a body anyway is skipped through its End Function.
                error(name.span, "an Extern block's Function has no body; C code defines it");
                finish_line(start);
                (void)parse_body(keyword);
            }
        } else if (modifiers.is_abstract) {
            // The line ends without a body, and the caller finishes it.
        } else if (accept(token_kind_t::ARROW)) {
            skip_continuation_newlines();
            const expr_id_t value = parse_expression();
            body.push_back(add_stmt(span_of(value), return_stmt_t{ .value = value }));
        } else if (in_interface() && !next_line_is_body()) {
            required = true;
        } else {
            finish_line(start);
            parse_where_lines(constraints);
            body = parse_body(keyword);
        }

        return add_stmt(span_from(begin), function_stmt_t{
                                              .kind = kind,
                                              .name = name,
                                              .type_parameters = std::move(type_parameters),
                                              .value_types = std::move(value_types),
                                              .constraints = std::move(constraints),
                                              .parameters = std::move(parameters),
                                              .return_type = return_type,
                                              .throws = std::move(throws),
                                              .body = std::move(body),
                                              .modifiers = own,
                                              .required = required,
                                              .external = std::move(external),
                                          });
    }

    /// Whether the lines after an Extern block's Function are a body, as if it were an ordinary
    /// Function: an End Function comes before the block's next Function or its End Extern.
    [[nodiscard]] bool extern_body_follows() const {
        modifiers_t ignored;
        for (auto at = static_cast<std::size_t>(&next_line_start() - m_tokens.data());
             at < m_tokens.size();) {
            // The line's first word, after any visibility and modifiers.
            std::size_t word = at;
            while (word < m_tokens.size() &&
                   (visibility_of(m_tokens[word].kind) ||
                    modifier_flag(ignored, m_tokens[word].kind) != nullptr ||
                    m_tokens[word].kind == token_kind_t::KW_CONST)) {
                ++word;
            }

            const token_kind_t kind = m_tokens[word].kind;
            if (kind == token_kind_t::END_OF_FILE || kind == token_kind_t::KW_FUNCTION ||
                kind == token_kind_t::AT) {
                return false;
            }

            // An End of a block in the body, like End If, goes on to the next line.
            if (kind == token_kind_t::KW_END && word + 1 < m_tokens.size()) {
                const token_kind_t ended = m_tokens[word + 1].kind;
                if (ended == token_kind_t::KW_FUNCTION || ended == token_kind_t::KW_EXTERN) {
                    return ended == token_kind_t::KW_FUNCTION;
                }
            }

            while (at < m_tokens.size() && m_tokens[at].kind != token_kind_t::NEWLINE &&
                   m_tokens[at].kind != token_kind_t::END_OF_FILE) {
                ++at;
            }

            ++at;
        }

        return false;
    }

    /// A plain string token's text, decoded.
    [[nodiscard]] std::string string_text(const token_t& token) const {
        const std::array pieces{ text(token) };
        std::vector<std::string> decoded = decode_string(pieces);
        return decoded.empty() ? std::string() : std::move(decoded.front());
    }

    /// "Extern "C" Lib "joltc"", its Functions, each its first line alone, maybe with
    /// "= "symbol"" after it, then "End Extern". It goes at a file's top level, and takes only a
    /// visibility, which its Functions take unless they have their own.
    stmt_id_t parse_extern(const modifiers_t& modifiers) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        if (!m_blocks.empty()) {
            error(keyword.span, "an Extern block goes at the top level of a file");
        }

        extern_stmt_t node{ .modifiers = modifiers };
        if (at(token_kind_t::STRING)) {
            const token_t& written = advance();
            node.convention_span = written.span;
            const std::string convention = string_text(written);
            if (convention == "stdcall") {
                node.convention = convention_t::STDCALL;
            } else if (convention == "runtime") {
                node.convention = convention_t::RUNTIME;
            } else if (convention != "C") {
                error(written.span,
                      "an Extern block's calling convention is \"C\", \"stdcall\", or "
                      "\"runtime\", not {}",
                      text(written));
            }
        } else if (at(token_kind_t::STRING_START)) {
            error(peek().span, "a calling convention is plain text, without {{...}}");
            skip_rest_of_line();
        }

        if (at(token_kind_t::IDENTIFIER) && text(peek()) == "Lib") {
            advance();
            if (at(token_kind_t::STRING)) {
                const token_t& written = advance();
                node.library = string_text(written);
                node.library_span = written.span;
            } else {
                report_expected("a library's name, like \"joltc\"");
            }
        }

        finish_line(start);
        m_blocks.push_back(keyword.kind);
        const std::optional<extern_block_t> outer = std::exchange(
            m_extern,
            extern_block_t{
                .external = { .convention = node.convention, .library = node.library },
                .visibility =
                    modifiers.visibility_span ? std::optional(modifiers.visibility) : std::nullopt,
            });
        while (!is_block_end(peek().kind)) {
            const std::size_t line = m_pos;
            const stmt_id_t id = parse_statement();
            const stmt_node_t& parsed = m_ast.get(id).node;
            const auto* function = std::get_if<function_stmt_t>(&parsed);
            if (function != nullptr && function->kind == function_kind_t::FUNCTION) {
                node.functions.push_back(id);
            } else if (!std::holds_alternative<error_stmt_t>(parsed)) {
                m_diagnostics.error(m_ast.get(id).span, "an Extern block holds only Functions");
            }

            finish_line(line);
        }

        m_extern = outer;
        expect_end(keyword);
        m_blocks.pop_back();
        return add_stmt(span_from(keyword.span.begin), std::move(node));
    }

    /// The operator after "Operator", which names it: one a type can define, like "+" or "==".
    /// ">>" is two '>' that touch, and "[]" and "[]=", which index, are their tokens touching.
    name_t parse_operator_name() {
        const token_t& token = peek();
        switch (token.kind) {
        case token_kind_t::LEFT_BRACKET: {
            advance();
            if (!at(token_kind_t::RIGHT_BRACKET) || !touches(token, peek())) {
                report_expected("']' right after '[', as in Operator [] or Operator []=");
                // A spaced-out "[ ]" or "[ ]=" is taken as meant, so the rest still parses.
                if (accept(token_kind_t::RIGHT_BRACKET)) {
                    return { .text = accept(token_kind_t::EQUAL) ? "[]=" : "[]",
                             .span = token.span };
                }

                return { .text = {}, .span = point(token.span.begin) };
            }

            const token_t* last = &advance();
            if (at(token_kind_t::EQUAL) && touches(*last, peek())) {
                last = &advance();
            }

            const span_t span{
                .file = token.span.file,
                .begin = token.span.begin,
                .end = last->span.end,
            };
            return { .text = m_file.slice(span), .span = span };
        }
        case token_kind_t::PLUS:
        case token_kind_t::MINUS:
        case token_kind_t::STAR:
        case token_kind_t::STAR_STAR:
        case token_kind_t::SLASH:
        case token_kind_t::PERCENT:
        case token_kind_t::AMP:
        case token_kind_t::PIPE:
        case token_kind_t::CARET:
        case token_kind_t::TILDE:
        case token_kind_t::BANG:
        case token_kind_t::LESS_LESS:
        case token_kind_t::EQUAL_EQUAL:
        case token_kind_t::BANG_EQUAL:
        case token_kind_t::LESS:
        case token_kind_t::LESS_EQUAL:
        case token_kind_t::GREATER_EQUAL:
            advance();
            return { .text = text(token), .span = token.span };
        case token_kind_t::GREATER: {
            advance();
            if (at(token_kind_t::GREATER) && touches(token, peek())) {
                const token_t& second = advance();
                const span_t span{
                    .file = token.span.file,
                    .begin = token.span.begin,
                    .end = second.span.end,
                };
                return { .text = m_file.slice(span), .span = span };
            }

            return { .text = text(token), .span = token.span };
        }
        case token_kind_t::AMP_AMP:
        case token_kind_t::PIPE_PIPE:
        case token_kind_t::QUESTION_QUESTION:
            advance();
            error(token.span, "{} always means what it does, so it can't be an Operator",
                  describe(token.kind));
            return { .text = {}, .span = point(token.span.begin) };
        default:
            // Whatever is where the operator goes is skipped, so the rest of the line still
            // parses.
            report_expected("an operator, like + or ==");
            if (!at(token_kind_t::LEFT_PAREN) && !at_line_end()) {
                advance();
            }

            return { .text = {}, .span = point(token.span.begin) };
        }
    }

    /// "Struct Name Implements A", "Class Name Extends Base Implements A, B", or "Interface
    /// Name Extends A, B", any Where lines, the members, then the matching End. Members parse
    /// like statements; analysis checks what each one can be.
    stmt_id_t parse_type_declaration(const modifiers_t& modifiers) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const bool is_interface = keyword.kind == token_kind_t::KW_INTERFACE;
        const name_t name = expect_name();
        std::vector<name_t> type_parameters;
        std::vector<std::optional<type_expr_id_t>> value_types;
        std::vector<constraint_t> constraints;
        if (at(token_kind_t::LESS)) {
            type_parameters = parse_type_parameters(constraints, value_types);
        }

        // An Interface extends any number of Interfaces, and a Class one Class.
        std::optional<name_t> base;
        std::vector<type_expr_id_t> base_arguments;
        std::vector<type_expr_id_t> interfaces;
        std::optional<name_t> base_qualifier;
        if (is_interface && accept(token_kind_t::KW_EXTENDS)) {
            interfaces = parse_type_list();
        } else if (accept(token_kind_t::KW_EXTENDS)) {
            base = expect_name();
            // "Extends shapes.Base": a Class another module declares.
            if (accept(token_kind_t::DOT)) {
                base_qualifier = base;
                base = expect_name();
            }

            if (at(token_kind_t::LESS)) {
                base_arguments = parse_type_arguments();
            }
        }

        if (at(token_kind_t::KW_IMPLEMENTS)) {
            const token_t& word = advance();
            if (is_interface) {
                error(word.span, "an Interface extends other Interfaces; write 'Extends'");
            }

            std::ranges::copy(parse_type_list(), std::back_inserter(interfaces));
        }

        finish_line(start);
        parse_where_lines(constraints);
        block_t members = parse_body(keyword);
        return add_stmt(span_from(keyword.span.begin),
                        type_stmt_t{
                            .is_class = keyword.kind == token_kind_t::KW_CLASS,
                            .is_interface = is_interface,
                            .name = name,
                            .type_parameters = std::move(type_parameters),
                            .value_types = std::move(value_types),
                            .constraints = std::move(constraints),
                            .members = std::move(members),
                            .base = base,
                            .base_arguments = std::move(base_arguments),
                            .base_qualifier = base_qualifier,
                            .interfaces = std::move(interfaces),
                            .modifiers = modifiers,
                        });
    }

    /// "Extend Type Implements A, B", any Where lines, the members, then "End Extend". Members
    /// parse like statements; analysis checks what each one can be.
    stmt_id_t parse_extend() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const type_expr_id_t type = parse_type();
        std::vector<type_expr_id_t> interfaces;
        if (accept(token_kind_t::KW_IMPLEMENTS)) {
            interfaces = parse_type_list();
        }

        finish_line(start);
        std::vector<constraint_t> constraints;
        parse_where_lines(constraints);
        block_t members = parse_body(keyword);
        return add_stmt(span_from(keyword.span.begin), extend_stmt_t{
                                                           .type = type,
                                                           .interfaces = std::move(interfaces),
                                                           .constraints = std::move(constraints),
                                                           .members = std::move(members),
                                                       });
    }

    /// "A, B": the types after Implements, or after an Interface's Extends.
    std::vector<type_expr_id_t> parse_type_list() {
        std::vector<type_expr_id_t> types{ parse_type() };
        while (accept(token_kind_t::COMMA)) {
            types.push_back(parse_type());
        }

        return types;
    }

    /// "Hashable & Equatable": the Interfaces a type parameter implements.
    std::vector<type_expr_id_t> parse_constraint() {
        std::vector<type_expr_id_t> interfaces{ parse_type() };
        while (accept(token_kind_t::AMP)) {
            interfaces.push_back(parse_type());
        }

        return interfaces;
    }

    /// "Where T: Hashable & Equatable" lines at the top of a body, each for one type parameter.
    void parse_where_lines(std::vector<constraint_t>& constraints) {
        while (at(token_kind_t::KW_WHERE)) {
            const std::size_t line = m_pos;
            advance();
            const name_t parameter = expect_name();
            if (expect(token_kind_t::COLON)) {
                constraints.push_back(
                    { .parameter = parameter, .interfaces = parse_constraint(), .where = true });
            }

            finish_line(line);
        }
    }

    parameter_t parse_parameter() {
        const bool by_ref = accept(token_kind_t::KW_BYREF);
        const name_t name = expect_name();
        const type_expr_id_t type = expect(token_kind_t::COLON)
                                        ? parse_type()
                                        : add_type(point(peek().span.begin), error_type_t{});
        const bool variadic = accept(token_kind_t::DOT_DOT_DOT);
        std::optional<expr_id_t> default_value;
        if (accept(token_kind_t::EQUAL)) {
            default_value = parse_expression();
        }

        return {
            .name = name,
            .by_ref = by_ref,
            .type = type,
            .variadic = variadic,
            .default_value = default_value,
        };
    }

    stmt_id_t parse_if() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const expr_id_t condition = parse_expression();
        // Only the single-line form uses Then.
        if (accept(token_kind_t::KW_THEN)) {
            return parse_single_line_if(keyword, condition);
        }

        finish_line(start);
        m_blocks.push_back(keyword.kind);
        std::vector<if_branch_t> branches;
        branches.push_back({ .condition = condition, .body = parse_block() });
        while (at(token_kind_t::KW_ELSEIF)) {
            const std::size_t line = m_pos;
            advance();
            const expr_id_t branch_condition = parse_expression();
            finish_line(line);
            branches.push_back({ .condition = branch_condition, .body = parse_block() });
        }

        std::optional<block_t> else_body;
        if (at(token_kind_t::KW_ELSE)) {
            const std::size_t line = m_pos;
            advance();
            finish_line(line);
            else_body = parse_block();
        }

        expect_end(keyword);
        m_blocks.pop_back();
        return add_stmt(span_from(keyword.span.begin), if_stmt_t{
                                                           .branches = std::move(branches),
                                                           .else_body = std::move(else_body),
                                                           .single_line = false,
                                                       });
    }

    stmt_id_t parse_single_line_if(const token_t& keyword, const expr_id_t condition) {
        const stmt_id_t then_stmt = parse_single_line_body();
        std::optional<block_t> else_body;
        if (accept(token_kind_t::KW_ELSE)) {
            else_body = block_t{ parse_single_line_body() };
        }

        return add_stmt(span_from(keyword.span.begin),
                        if_stmt_t{
                            .branches = { { .condition = condition, .body = { then_stmt } } },
                            .else_body = std::move(else_body),
                            .single_line = true,
                        });
    }

    /// The statement after Then or Else. Block statements need lines of their own, though "Try
    /// F()" is a call.
    stmt_id_t parse_single_line_body() {
        if (at(token_kind_t::KW_TRY) && !opens_try_block()) {
            return parse_statement();
        }

        switch (peek().kind) {
        case token_kind_t::KW_IF:
        case token_kind_t::KW_WHILE:
        case token_kind_t::KW_REPEAT:
        case token_kind_t::KW_LOOP:
        case token_kind_t::KW_FOR:
        case token_kind_t::KW_SELECT:
        case token_kind_t::KW_FUNCTION:
        case token_kind_t::KW_STRUCT:
        case token_kind_t::KW_CLASS:
        case token_kind_t::KW_INTERFACE:
        case token_kind_t::KW_EXTEND:
        case token_kind_t::KW_TRY:
        case token_kind_t::KW_LOCK:
        case token_kind_t::KW_PARALLEL:
            error(peek().span, "a single-line If can't contain {}", describe(peek().kind));
            return add_stmt(point(peek().span.begin), error_stmt_t{});
        default:
            return parse_statement();
        }
    }

    /// Where a loop statement starts: at its label, if it has one.
    [[nodiscard]] static std::uint32_t begin_of(const std::optional<name_t>& label,
                                                const token_t& keyword) {
        return label ? label->span.begin : keyword.span.begin;
    }

    stmt_id_t parse_while(const std::optional<name_t>& label) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const expr_id_t condition = parse_expression();
        finish_line(start);
        block_t body = parse_body(keyword);
        return add_stmt(
            span_from(begin_of(label, keyword)),
            while_stmt_t{ .label = label, .condition = condition, .body = std::move(body) });
    }

    /// Repeat ends with "Until <condition>" instead of an End.
    stmt_id_t parse_repeat(const std::optional<name_t>& label) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        finish_line(start);
        m_blocks.push_back(keyword.kind);
        block_t body = parse_block();
        m_blocks.pop_back();

        expr_id_t condition{};
        if (accept(token_kind_t::KW_UNTIL)) {
            condition = parse_expression();
        } else {
            if (diagnostic_t* diagnostic = report_expected(describe(token_kind_t::KW_UNTIL))) {
                diagnostic->with_label(keyword.span, "opened here");
            }

            condition = missing_expr();
        }

        return add_stmt(
            span_from(begin_of(label, keyword)),
            repeat_stmt_t{ .label = label, .body = std::move(body), .condition = condition });
    }

    /// "Lock m As s": the Mutex is an expression without 'As' or 'Is' in it, and s names its
    /// value in the body.
    stmt_id_t parse_lock() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const expr_id_t mutex = parse_expression(precedence::bit_or);
        name_t name{ .span = point(peek().span.begin) };
        if (accept(token_kind_t::KW_AS)) {
            name = expect_name();
        } else {
            report_expected("'As' and a name for the Mutex's value");
        }

        finish_line(start);
        block_t body = parse_body(keyword);
        return add_stmt(span_from(keyword.span.begin),
                        lock_stmt_t{ .mutex = mutex, .name = name, .body = std::move(body) });
    }

    stmt_id_t parse_loop(const std::optional<name_t>& label) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        finish_line(start);
        block_t body = parse_body(keyword);
        return add_stmt(span_from(begin_of(label, keyword)),
                        loop_stmt_t{ .label = label, .body = std::move(body) });
    }

    stmt_id_t parse_for(const std::optional<name_t>& label,
                        const std::optional<span_t> parallel = std::nullopt) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const auto counts = [&] {
            if (parallel) {
                error(*parallel, "a Parallel For counts, like 'Parallel For i = 0 Until n'");
            }
        };

        if (at(token_kind_t::KW_FIELD) || at(token_kind_t::KW_METHOD) ||
            at(token_kind_t::KW_PROPERTY) ||
            (at(token_kind_t::KW_STATIC) && peek(1).kind == token_kind_t::KW_FIELD)) {
            counts();
            return parse_for_member(label, start, keyword);
        }

        const name_t first = expect_name();

        // "For x In items" or "For i, x In items". Anything else is a counting loop.
        if (at(token_kind_t::COMMA) || at(token_kind_t::KW_IN)) {
            counts();
            std::optional<name_t> index;
            name_t value = first;
            if (accept(token_kind_t::COMMA)) {
                index = first;
                value = expect_name();
            }

            expect(token_kind_t::KW_IN);
            const expr_id_t sequence = parse_expression();
            finish_line(start);
            block_t body = parse_body(keyword);
            return add_stmt(span_from(begin_of(label, keyword)), for_in_stmt_t{
                                                                     .label = label,
                                                                     .index = index,
                                                                     .value = value,
                                                                     .sequence = sequence,
                                                                     .body = std::move(body),
                                                                 });
        }

        expect(token_kind_t::EQUAL);
        const expr_id_t from = parse_expression();
        // To includes the end; Until stops before it.
        bool inclusive = true;
        if (accept(token_kind_t::KW_UNTIL)) {
            inclusive = false;
        } else if (!accept(token_kind_t::KW_TO)) {
            report_expected("'To' or 'Until'");
        }

        const expr_id_t to = parse_expression();
        // Step is a contextual keyword, an ordinary name everywhere else.
        std::optional<expr_id_t> step;
        if (at(token_kind_t::IDENTIFIER) && text(peek()) == "Step") {
            advance();
            step = parse_expression();
        }

        finish_line(start);
        block_t body = parse_body(keyword);
        // A label comes first, then Parallel.
        std::uint32_t begin = parallel ? parallel->begin : keyword.span.begin;
        if (label) {
            begin = label->span.begin;
        }

        return add_stmt(span_from(begin), for_stmt_t{
                                              .label = label,
                                              .variable = first,
                                              .start = from,
                                              .end = to,
                                              .inclusive = inclusive,
                                              .step = step,
                                              .body = std::move(body),
                                              .parallel = parallel,
                                          });
    }

    /// "For Field f: F In T", "For Method m In T", "For Property p: P In T", or "For Static
    /// Field s: S In T", with Where lines on F at the top of the body.
    stmt_id_t parse_for_member(const std::optional<name_t>& label, const std::size_t start,
                               const token_t& keyword) {
        member_kind_t kind = member_kind_t::FIELD;
        if (accept(token_kind_t::KW_STATIC)) {
            kind = member_kind_t::STATIC_FIELD;
        } else if (at(token_kind_t::KW_METHOD)) {
            kind = member_kind_t::METHOD;
        } else if (at(token_kind_t::KW_PROPERTY)) {
            kind = member_kind_t::PROPERTY;
        }

        advance(); // Field, Method, or Property
        const name_t variable = expect_name();
        std::optional<name_t> type_name;
        if (accept(token_kind_t::COLON)) {
            type_name = expect_name();
        }

        expect(token_kind_t::KW_IN);
        const type_expr_id_t type = parse_type();
        finish_line(start);
        std::vector<constraint_t> constraints;
        parse_where_lines(constraints);
        block_t body = parse_body(keyword);
        return add_stmt(span_from(begin_of(label, keyword)),
                        for_member_stmt_t{
                            .label = label,
                            .kind = kind,
                            .variable = variable,
                            .type_name = type_name,
                            .type = type,
                            .constraints = std::move(constraints),
                            .body = std::move(body),
                        });
    }

    stmt_id_t parse_select() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const expr_id_t subject = parse_expression();
        finish_line(start);
        m_blocks.push_back(keyword.kind);

        // Only Case and Default can come before the first Case.
        while (!at(token_kind_t::KW_CASE) && !is_block_end(peek().kind)) {
            const std::size_t line = m_pos;
            report_expected(describe(token_kind_t::KW_CASE));
            finish_line(line);
        }

        std::vector<case_t> cases;
        while (at(token_kind_t::KW_CASE)) {
            const std::size_t line = m_pos;
            advance();
            // "Case e As Boss" is a type match, unlike "Case e As? Boss".
            const bool checked =
                peek(2).kind == token_kind_t::QUESTION && touches(peek(1), peek(2));
            if (at(token_kind_t::IDENTIFIER) && peek(1).kind == token_kind_t::KW_AS && !checked) {
                const name_t name = take_name();
                advance(); // As
                const type_expr_id_t type = parse_type();
                finish_line(line);
                cases.push_back({
                    .match = type_case_t{ .name = name, .type = type },
                    .body = parse_block(),
                });
                continue;
            }

            // Values and inclusive ranges, separated by commas: "Case 1, 3 To 5".
            std::vector<case_value_t> values;
            while (true) {
                const expr_id_t value = parse_expression();
                std::optional<expr_id_t> range_end;
                if (accept(token_kind_t::KW_TO)) {
                    range_end = parse_expression();
                }

                values.push_back({ .value = value, .range_end = range_end });
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }

                skip_continuation_newlines();
            }

            finish_line(line);
            cases.push_back({ .values = std::move(values), .body = parse_block() });
        }

        std::optional<block_t> default_body;
        if (at(token_kind_t::KW_DEFAULT)) {
            const std::size_t line = m_pos;
            advance();
            finish_line(line);
            default_body = parse_block();
        }

        expect_end(keyword);
        m_blocks.pop_back();
        return add_stmt(span_from(keyword.span.begin), select_stmt_t{
                                                           .subject = subject,
                                                           .cases = std::move(cases),
                                                           .default_body = std::move(default_body),
                                                       });
    }

    /// Break or Continue, with an optional loop label.
    stmt_id_t parse_jump() {
        const token_t& keyword = advance();
        std::optional<name_t> label;
        if (at(token_kind_t::IDENTIFIER)) {
            label = take_name();
        }

        if (keyword.kind == token_kind_t::KW_BREAK) {
            return add_stmt(span_from(keyword.span.begin), break_stmt_t{ .label = label });
        }

        return add_stmt(span_from(keyword.span.begin), continue_stmt_t{ .label = label });
    }

    stmt_id_t parse_return() {
        const token_t& keyword = advance();
        // "If done Then Return Else Continue" returns no value.
        std::optional<expr_id_t> value;
        if (!at_line_end() && !at(token_kind_t::KW_ELSE)) {
            value = parse_expression();
        }

        return add_stmt(span_from(keyword.span.begin), return_stmt_t{ .value = value });
    }

    /// Assignments, ++ and --, and calls. Any other expression on its own does nothing.
    stmt_id_t parse_simple() {
        const std::uint32_t begin = peek().span.begin;
        // '=' is never an operator in an expression, so an assignment's target stops before it.
        expr_id_t target = parse_expression();
        // "a, b = (b, a)" assigns each element of a tuple, like "(a, b) = (b, a)".
        if (at(token_kind_t::COMMA)) {
            std::vector<expr_id_t> targets{ target };
            while (accept(token_kind_t::COMMA)) {
                targets.push_back(parse_expression());
            }

            target = add_expr(span_from(begin), tuple_expr_t{ .elements = std::move(targets) });
        }

        if (const std::optional<assignment_t> assignment = assignment_at_current()) {
            for (std::uint32_t i = 0; i < assignment->tokens; ++i) {
                advance();
            }

            skip_continuation_newlines();
            const expr_id_t value = parse_expression();
            // "grid[x] += v" sets what "grid[x] + v" gives.
            const expr_id_t set = assignment->op ? add_expr(span_from(begin),
                                                            binary_expr_t{
                                                                .op = *assignment->op,
                                                                .left = target,
                                                                .right = value,
                                                            })
                                                 : value;
            return add_stmt(span_from(begin), assign_stmt_t{
                                                  .op = assignment->op,
                                                  .target = target,
                                                  .value = value,
                                                  .index_set = index_set(target, set),
                                              });
        }

        if (at(token_kind_t::PLUS_PLUS) || at(token_kind_t::MINUS_MINUS)) {
            const token_t& token = advance();
            const bool decrement = token.kind == token_kind_t::MINUS_MINUS;
            std::optional<expr_id_t> set;
            if (std::holds_alternative<index_expr_t>(m_ast.get(target).node)) {
                const expr_id_t one = add_expr(token.span, integer_expr_t{ .value = 1 });
                set = index_set(
                    target, add_expr(span_from(begin),
                                     binary_expr_t{
                                         .op = decrement ? binary_op_t::SUBTRACT : binary_op_t::ADD,
                                         .left = target,
                                         .right = one,
                                     }));
            }

            return add_stmt(span_from(begin), increment_stmt_t{
                                                  .target = target,
                                                  .decrement = decrement,
                                                  .index_set = set,
                                              });
        }

        const expr_node_t& node = m_ast.get(target).node;
        const bool is_call = calls(target);
        const bool is_name = std::holds_alternative<name_expr_t>(node);
        const bool is_error = std::holds_alternative<error_expr_t>(node);
        const span_t span = span_of(target);
        // A name with more after it looks like a Blitz call without parentheses: "Print x".
        if (is_name && !at_line_end() && !at(token_kind_t::KW_ELSE)) {
            error(span, "calls need parentheses, like {}(...)", m_file.slice(span));
        } else if (!is_call && !is_error) {
            error(span, "expression has no effect");
        }

        if (is_error) {
            return add_stmt(span_from(begin), error_stmt_t{});
        }

        return add_stmt(span_from(begin), expression_stmt_t{ .expression = target });
    }

    /// Whether an expression is a call, or one with Try or Try? before it.
    [[nodiscard]] bool calls(const expr_id_t id) const {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* tried = std::get_if<try_expr_t>(&node)) {
            return calls(tried->operand);
        }

        // Awaiting is an effect of its own: the code waits there.
        return std::holds_alternative<call_expr_t>(node) ||
               std::holds_alternative<await_expr_t>(node);
    }

    [[nodiscard]] std::optional<assignment_t> assignment_at_current() const {
        const token_t& token = peek();
        switch (token.kind) {
        case token_kind_t::EQUAL:
            return assignment_t{};
        case token_kind_t::PLUS_EQUAL:
            return assignment_t{ .op = binary_op_t::ADD };
        case token_kind_t::MINUS_EQUAL:
            return assignment_t{ .op = binary_op_t::SUBTRACT };
        case token_kind_t::STAR_EQUAL:
            return assignment_t{ .op = binary_op_t::MULTIPLY };
        case token_kind_t::SLASH_EQUAL:
            return assignment_t{ .op = binary_op_t::DIVIDE };
        case token_kind_t::PERCENT_EQUAL:
            return assignment_t{ .op = binary_op_t::REMAINDER };
        case token_kind_t::AMP_EQUAL:
            return assignment_t{ .op = binary_op_t::BIT_AND };
        case token_kind_t::PIPE_EQUAL:
            return assignment_t{ .op = binary_op_t::BIT_OR };
        case token_kind_t::CARET_EQUAL:
            return assignment_t{ .op = binary_op_t::BIT_XOR };
        case token_kind_t::LESS_LESS_EQUAL:
            return assignment_t{ .op = binary_op_t::SHIFT_LEFT };
        case token_kind_t::GREATER:
            if (peek(1).kind == token_kind_t::GREATER_EQUAL && touches(token, peek(1))) {
                return assignment_t{ .op = binary_op_t::SHIFT_RIGHT, .tokens = 2 };
            }

            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    // Expressions

    /// Precedence climbing: a unary operand, then each operator that binds at least as tightly as
    /// `min_precedence`. An operator's right side only takes operators that bind tighter than it,
    /// so "a - b - c" is "(a - b) - c".
    expr_id_t parse_expression(const std::uint32_t min_precedence = precedence::ternary) {
        if (m_nesting >= max_nesting) {
            report_too_deep();
            return missing_expr();
        }

        // Each operator around `left` puts it a level deeper, as each call in here does.
        const std::uint32_t outer = m_nesting++;
        expr_id_t left = parse_unary();
        while (m_nesting < max_nesting) {
            const token_kind_t kind = peek().kind;
            // The ternary and Is/As aren't plain binary operators: one takes two more operands,
            // the other a type.
            if (kind == token_kind_t::QUESTION && min_precedence <= precedence::ternary) {
                left = parse_ternary(left);
            } else if ((kind == token_kind_t::KW_AS || kind == token_kind_t::KW_IS) &&
                       min_precedence <= precedence::type_test) {
                left = parse_type_test(left);
            } else if (const std::optional<binary_t> binary = binary_at_current();
                       binary && binary->precedence >= min_precedence) {
                left = parse_binary(left, *binary);
            } else {
                m_nesting = outer;
                return left;
            }

            ++m_nesting;
        }

        report_too_deep();
        m_nesting = outer;
        return left;
    }

    [[nodiscard]] std::optional<binary_t> binary_at_current() const {
        using enum binary_op_t;
        const token_t& token = peek();
        switch (token.kind) {
        case token_kind_t::PLUS:
            return binary_t{ .op = ADD, .precedence = precedence::additive };
        case token_kind_t::MINUS:
            return binary_t{ .op = SUBTRACT, .precedence = precedence::additive };
        case token_kind_t::STAR:
            return binary_t{ .op = MULTIPLY, .precedence = precedence::multiplicative };
        case token_kind_t::SLASH:
            return binary_t{ .op = DIVIDE, .precedence = precedence::multiplicative };
        case token_kind_t::PERCENT:
            return binary_t{ .op = REMAINDER, .precedence = precedence::multiplicative };
        case token_kind_t::STAR_STAR:
            return binary_t{
                .op = POWER,
                .precedence = precedence::power,
                .right_associative = true,
            };
        case token_kind_t::LESS_LESS:
            return binary_t{ .op = SHIFT_LEFT, .precedence = precedence::shift };
        case token_kind_t::AMP:
            return binary_t{ .op = BIT_AND, .precedence = precedence::bit_and };
        case token_kind_t::CARET:
            return binary_t{ .op = BIT_XOR, .precedence = precedence::bit_xor };
        case token_kind_t::PIPE:
            return binary_t{ .op = BIT_OR, .precedence = precedence::bit_or };
        case token_kind_t::LESS:
            return binary_t{ .op = LESS, .precedence = precedence::comparison };
        case token_kind_t::LESS_EQUAL:
            return binary_t{ .op = LESS_EQUAL, .precedence = precedence::comparison };
        case token_kind_t::GREATER_EQUAL:
            return binary_t{ .op = GREATER_EQUAL, .precedence = precedence::comparison };
        case token_kind_t::GREATER:
            // The lexer never joins '>' tokens, so "List<List<Int>>" can close two generics.
            // Touching ones make a shift here.
            if (touches(token, peek(1)) && peek(1).kind == token_kind_t::GREATER) {
                return binary_t{
                    .op = SHIFT_RIGHT,
                    .precedence = precedence::shift,
                    .tokens = 2,
                };
            }

            if (touches(token, peek(1)) && peek(1).kind == token_kind_t::GREATER_EQUAL) {
                return std::nullopt; // ">>=" is an assignment
            }

            return binary_t{ .op = GREATER, .precedence = precedence::comparison };
        case token_kind_t::EQUAL_EQUAL:
            return binary_t{ .op = EQUAL, .precedence = precedence::equality };
        case token_kind_t::BANG_EQUAL:
            return binary_t{ .op = NOT_EQUAL, .precedence = precedence::equality };
        case token_kind_t::AMP_AMP:
            return binary_t{ .op = AND, .precedence = precedence::logical_and };
        case token_kind_t::PIPE_PIPE:
            return binary_t{ .op = OR, .precedence = precedence::logical_or };
        case token_kind_t::QUESTION_QUESTION:
            return binary_t{
                .op = COALESCE,
                .precedence = precedence::coalesce,
                .right_associative = true,
            };
        default:
            return std::nullopt;
        }
    }

    expr_id_t parse_binary(const expr_id_t left, const binary_t& binary) {
        for (std::uint32_t i = 0; i < binary.tokens; ++i) {
            advance();
        }

        skip_continuation_newlines();
        // A right-associative operator lets its own level into its right side, so "a ** b ** c"
        // is "a ** (b ** c)".
        const std::uint32_t next =
            binary.right_associative ? binary.precedence : binary.precedence + 1;
        const expr_id_t right = parse_expression(next);
        return add_expr(merge(span_of(left), span_of(right)),
                        binary_expr_t{ .op = binary.op, .left = left, .right = right });
    }

    expr_id_t parse_ternary(const expr_id_t condition) {
        advance(); // '?'
        // Both branches take a whole expression, so "a ? b : c ? d : e" groups to the right.
        const expr_id_t if_true = parse_expression();
        expect(token_kind_t::COLON);
        const expr_id_t if_false = parse_expression();
        return add_expr(
            merge(span_of(condition), span_of(if_false)),
            ternary_expr_t{ .condition = condition, .if_true = if_true, .if_false = if_false });
    }

    /// "x Is T", "x As T", or the checked cast "x As? T", written without a space before '?'.
    expr_id_t parse_type_test(const expr_id_t value) {
        const token_t& keyword = advance();
        if (keyword.kind == token_kind_t::KW_IS) {
            const type_expr_id_t type = parse_type();
            return add_expr(span_from(span_of(value).begin),
                            is_expr_t{ .value = value, .type = type });
        }

        const bool checked = at(token_kind_t::QUESTION) && touches(keyword, peek());
        if (checked) {
            advance();
        }

        const type_expr_id_t type = parse_type();
        return add_expr(span_from(span_of(value).begin),
                        cast_expr_t{ .value = value, .type = type, .checked = checked });
    }

    /// Prefix operators bind tighter than any binary operator, even '**': "-a ** b" is
    /// "(-a) ** b".
    expr_id_t parse_unary() {
        if (m_nesting >= max_nesting) {
            report_too_deep();
            return missing_expr();
        }

        const token_t& token = peek();
        if (token.kind == token_kind_t::KW_TRY) {
            return parse_try_expression();
        }

        // "Await x" takes what comes before the next binary operator, like Try.
        if (token.kind == token_kind_t::KW_AWAIT) {
            advance();
            const expr_id_t operand = nested_unary();
            return add_expr(span_from(token.span.begin), await_expr_t{ .operand = operand });
        }

        std::optional<unary_op_t> op;
        switch (token.kind) {
        case token_kind_t::MINUS:
            op = unary_op_t::NEGATE;
            break;
        case token_kind_t::BANG:
            op = unary_op_t::NOT;
            break;
        case token_kind_t::TILDE:
            op = unary_op_t::BIT_NOT;
            break;
        default:
            return parse_postfix(parse_primary());
        }

        advance();
        const expr_id_t operand = nested_unary();
        return add_expr(span_from(token.span.begin), unary_expr_t{ .op = *op, .operand = operand });
    }

    /// The operand of a prefix operator, Try, or Await, a level deeper.
    expr_id_t nested_unary() {
        ++m_nesting;
        const expr_id_t operand = parse_unary();
        --m_nesting;
        return operand;
    }

    /// "Try operand" or "Try? operand", written without a space before '?'. Like a prefix
    /// operator, it takes what comes before the next binary operator, so "Try? Parse(s) ?? 0" is
    /// "(Try? Parse(s)) ?? 0".
    expr_id_t parse_try_expression() {
        const token_t& keyword = advance();
        const bool optional = at(token_kind_t::QUESTION) && touches(keyword, peek());
        if (optional) {
            advance();
        }

        const expr_id_t operand = nested_unary();
        return add_expr(span_from(keyword.span.begin),
                        try_expr_t{ .operand = operand, .optional = optional });
    }

    /// Member access, calls, and indexing after `expr`. They bind tightest of all, and each puts
    /// what's before it a level deeper.
    expr_id_t parse_postfix(expr_id_t expr) {
        const std::uint32_t outer = m_nesting;
        while (m_nesting < max_nesting) {
            switch (peek().kind) {
            case token_kind_t::DOT:
            case token_kind_t::QUESTION_DOT:
                expr = parse_member(expr);
                break;
            case token_kind_t::LEFT_PAREN:
                expr = parse_call(expr);
                break;
            case token_kind_t::LEFT_BRACKET:
                expr = parse_index(expr);
                break;
            default:
                m_nesting = outer;
                return expr;
            }

            ++m_nesting;
        }

        report_too_deep();
        m_nesting = outer;
        return expr;
    }

    /// "object.name". The name can be New, for "Super.New()", or a tuple element's number.
    expr_id_t parse_member(const expr_id_t object) {
        const bool null_safe = advance().kind == token_kind_t::QUESTION_DOT;
        if (at(token_kind_t::INTEGER) || at(token_kind_t::DECIMAL)) {
            return parse_element(object, null_safe);
        }

        if (!at(token_kind_t::IDENTIFIER) && !at(token_kind_t::KW_NEW)) {
            report_expected(describe(token_kind_t::IDENTIFIER));
            return add_expr(span_from(span_of(object).begin), error_expr_t{});
        }

        const name_t member = take_name();
        std::vector<type_expr_id_t> type_arguments;
        if (type_arguments_follow()) {
            type_arguments = parse_type_arguments();
        }

        return add_expr(span_from(span_of(object).begin),
                        member_expr_t{
                            .object = object,
                            .member = member,
                            .null_safe = null_safe,
                            .type_arguments = std::move(type_arguments),
                        });
    }

    /// Whether a '<' here starts type arguments, as in "Max<Float>(1, 2.5)": what follows it, up
    /// to the '>' that closes it, could be types, and that '>' is followed by '(', '.', ')', or
    /// ','. Otherwise the '<' compares.
    [[nodiscard]] bool type_arguments_follow() const {
        std::size_t index = skip_newlines(m_pos);
        if (m_tokens[index].kind != token_kind_t::LESS) {
            return false;
        }

        std::uint32_t depth = 0;
        for (; m_tokens[index].kind != token_kind_t::END_OF_FILE;
             index = skip_newlines(index + 1)) {
            switch (m_tokens[index].kind) {
            case token_kind_t::LESS:
                ++depth;
                break;
            case token_kind_t::GREATER: {
                --depth;
                if (depth > 0) {
                    break;
                }

                const token_kind_t next = m_tokens[skip_newlines(index + 1)].kind;
                return next == token_kind_t::LEFT_PAREN || next == token_kind_t::DOT ||
                       next == token_kind_t::RIGHT_PAREN || next == token_kind_t::COMMA;
            }
            case token_kind_t::IDENTIFIER:
            case token_kind_t::KW_SELF:
            case token_kind_t::DOT:
            case token_kind_t::COMMA:
            case token_kind_t::QUESTION:
            case token_kind_t::LEFT_BRACKET:
            case token_kind_t::RIGHT_BRACKET:
            case token_kind_t::INTEGER:
            case token_kind_t::DECIMAL:
            case token_kind_t::MINUS:
            case token_kind_t::PLUS:
            case token_kind_t::STAR:
            case token_kind_t::KW_FUNC:
            case token_kind_t::KW_CALLBACK:
            case token_kind_t::LEFT_PAREN:
            case token_kind_t::RIGHT_PAREN:
            case token_kind_t::COLON:
                break;
            default:
                return false;
            }
        }

        return false;
    }

    /// "<Int, String>", after a generic name.
    std::vector<type_expr_id_t> parse_type_arguments() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<type_expr_id_t> arguments;
        while (!at(token_kind_t::GREATER) && !at(token_kind_t::END_OF_FILE)) {
            arguments.push_back(parse_type_argument());
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::GREATER, open.span);
        --m_bracket_depth;
        return arguments;
    }

    /// A type, or a Const type parameter's argument. One that goes on with + or *, like
    /// "N * 2", is a value computed from Const type parameters, which takes no comparisons, so
    /// the '>' still ends the list.
    type_expr_id_t parse_type_argument() {
        const std::size_t start = m_pos;
        const type_expr_id_t type = parse_type();
        if (!at(token_kind_t::PLUS) && !at(token_kind_t::STAR)) {
            return type;
        }

        m_pos = start;
        const expr_id_t value = parse_value_sum();
        return add_type(span_of(value), value_type_t{ .value = value });
    }

    /// Products added together, each of unary operands.
    expr_id_t parse_value_sum() {
        expr_id_t left = parse_value_product();
        while (accept(token_kind_t::PLUS)) {
            const expr_id_t right = parse_value_product();
            left = add_expr(span_from(span_of(left).begin),
                            binary_expr_t{ .op = binary_op_t::ADD, .left = left, .right = right });
        }

        return left;
    }

    expr_id_t parse_value_product() {
        expr_id_t left = parse_unary();
        while (accept(token_kind_t::STAR)) {
            const expr_id_t right = parse_unary();
            left = add_expr(
                span_from(span_of(left).begin),
                binary_expr_t{ .op = binary_op_t::MULTIPLY, .left = left, .right = right });
        }

        return left;
    }

    /// "<T, U: Comparable, Const N: Int>" after the name of a generic Function, Method, Struct,
    /// or Class. A constraint after a ':' goes in `constraints`, and a Const one's type in
    /// `value_types`.
    std::vector<name_t>
    parse_type_parameters(std::vector<constraint_t>& constraints,
                          std::vector<std::optional<type_expr_id_t>>& value_types) {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<name_t> names;
        while (!at(token_kind_t::GREATER) && !at(token_kind_t::END_OF_FILE)) {
            const bool is_value = accept(token_kind_t::KW_CONST);
            names.push_back(expect_name());
            if (is_value) {
                value_types.resize(names.size());
                value_types.back() = expect(token_kind_t::COLON)
                                         ? parse_type()
                                         : add_type(point(peek().span.begin), error_type_t{});
            } else if (accept(token_kind_t::COLON)) {
                constraints.push_back(
                    { .parameter = names.back(), .interfaces = parse_constraint() });
            }

            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::GREATER, open.span);
        --m_bracket_depth;
        if (!value_types.empty()) {
            value_types.resize(names.size());
        }

        return names;
    }

    /// "pair.0": a tuple's element, by its number. In "t.0.1", the numbers lex as one decimal,
    /// which names an element of an element.
    expr_id_t parse_element(const expr_id_t object, const bool null_safe) {
        const token_t& token = advance();
        const std::string_view digits = text(token);
        const std::size_t dot = digits.find('.');
        const std::string_view first = digits.substr(0, dot);
        const std::string_view second =
            dot == std::string_view::npos ? std::string_view{} : digits.substr(dot + 1);
        const auto plain = [](const std::string_view number) {
            return !number.empty() &&
                   std::ranges::all_of(number, [](const char c) { return c >= '0' && c <= '9'; });
        };
        if (!plain(first) || (dot != std::string_view::npos && !plain(second))) {
            error(token.span, "a tuple's elements are numbered, like .0 and .1");
            return add_expr(span_from(span_of(object).begin), error_expr_t{});
        }

        const std::uint32_t begin = span_of(object).begin;
        const span_t first_span{
            .file = token.span.file,
            .begin = token.span.begin,
            .end = token.span.begin + static_cast<std::uint32_t>(first.size()),
        };
        const expr_id_t element =
            add_expr({ .file = token.span.file, .begin = begin, .end = first_span.end },
                     member_expr_t{
                         .object = object,
                         .member = { .text = first, .span = first_span },
                         .null_safe = null_safe,
                     });
        if (dot == std::string_view::npos) {
            return element;
        }

        const span_t second_span{
            .file = token.span.file,
            .begin = first_span.end + 1,
            .end = token.span.end,
        };
        return add_expr(span_from(begin), member_expr_t{
                                              .object = element,
                                              .member = { .text = second, .span = second_span },
                                              .null_safe = false,
                                          });
    }

    expr_id_t parse_call(const expr_id_t callee) {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<argument_t> arguments;
        while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
            // "name: value" is a named argument.
            std::optional<name_t> name;
            if (at(token_kind_t::IDENTIFIER) && peek(1).kind == token_kind_t::COLON) {
                name = take_name();
                advance(); // ':'
            }

            const bool by_ref = accept(token_kind_t::KW_BYREF);
            arguments.push_back({ .name = name, .by_ref = by_ref, .value = parse_expression() });
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::RIGHT_PAREN, open.span);
        --m_bracket_depth;
        return add_expr(span_from(span_of(callee).begin),
                        call_expr_t{ .callee = callee, .arguments = std::move(arguments) });
    }

    /// "a[i]", "grid[x, y]", or a slice, "a[from..to]", where either can be left out.
    expr_id_t parse_index(const expr_id_t object) {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<expr_id_t> indices;
        while (!at(token_kind_t::RIGHT_BRACKET) && !at(token_kind_t::END_OF_FILE) &&
               !at(token_kind_t::DOT_DOT)) {
            indices.push_back(parse_expression());
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        if (indices.size() <= 1 && accept(token_kind_t::DOT_DOT)) {
            std::optional<expr_id_t> to;
            if (!at(token_kind_t::RIGHT_BRACKET)) {
                to = parse_expression();
            }

            expect_closing(token_kind_t::RIGHT_BRACKET, open.span);
            --m_bracket_depth;
            const std::optional<expr_id_t> from =
                indices.empty() ? std::nullopt : std::optional(indices.front());
            return add_expr(span_from(span_of(object).begin),
                            slice_expr_t{ .object = object, .from = from, .to = to });
        }

        expect_closing(token_kind_t::RIGHT_BRACKET, open.span);
        --m_bracket_depth;
        // What a type with an Operator [] calls, at the brackets.
        std::vector<argument_t> arguments;
        arguments.reserve(indices.size());
        for (const expr_id_t index : indices) {
            arguments.push_back({ .by_ref = false, .value = index });
        }

        const expr_id_t call =
            index_call(object, "[]", span_from(open.span.begin), std::move(arguments));
        return add_expr(
            span_from(span_of(object).begin),
            index_expr_t{ .object = object, .indices = std::move(indices), .call = call });
    }

    /// "object.[](indices)" or "object.[]=(indices, value)", which an index's Operator makes of
    /// it, at `brackets`.
    expr_id_t index_call(const expr_id_t object, const std::string_view name, const span_t brackets,
                         std::vector<argument_t> arguments) {
        const expr_id_t callee =
            add_expr(brackets, member_expr_t{
                                   .object = object,
                                   .member = { .text = name, .span = brackets },
                                   .null_safe = false,
                               });
        return add_expr(brackets,
                        call_expr_t{ .callee = callee, .arguments = std::move(arguments) });
    }

    /// For an assignment to an index, like "grid[x, y] = value": the call of its Operator []=,
    /// which gets `value` after the indices.
    std::optional<expr_id_t> index_set(const expr_id_t target, const expr_id_t value) {
        const auto* index = std::get_if<index_expr_t>(&m_ast.get(target).node);
        if (index == nullptr) {
            return std::nullopt;
        }

        std::vector<argument_t> arguments;
        arguments.reserve(index->indices.size() + 1);
        for (const expr_id_t argument : index->indices) {
            arguments.push_back({ .by_ref = false, .value = argument });
        }

        arguments.push_back({ .by_ref = false, .value = value });
        return index_call(index->object, "[]=", span_of(index->call), std::move(arguments));
    }

    expr_id_t parse_primary() {
        const token_t& token = peek();
        switch (token.kind) {
        case token_kind_t::INTEGER:
            // The lexer already reported a literal that doesn't fit in 64 bits.
            advance();
            return add_expr(token.span,
                            integer_expr_t{ .value = parse_integer(text(token)).value_or(0) });
        case token_kind_t::DECIMAL: {
            advance();
            const std::optional<double> value = parse_decimal(text(token));
            if (!value) {
                error(token.span, "decimal literal is out of range");
            }

            return add_expr(token.span, decimal_expr_t{ .value = value.value_or(0.0) });
        }
        case token_kind_t::CHARACTER:
            advance();
            return add_expr(token.span, character_expr_t{ .value = decode_character(text(token)) });
        case token_kind_t::STRING:
        case token_kind_t::STRING_START:
            return parse_string();
        case token_kind_t::KW_TRUE:
        case token_kind_t::KW_FALSE:
            advance();
            return add_expr(token.span,
                            bool_expr_t{ .value = token.kind == token_kind_t::KW_TRUE });
        case token_kind_t::KW_NULL:
            advance();
            return add_expr(token.span, null_expr_t{});
        case token_kind_t::IDENTIFIER: {
            advance();
            name_expr_t name{ .name = text(token) };
            if (type_arguments_follow()) {
                name.type_arguments = parse_type_arguments();
            }

            return add_expr(span_from(token.span.begin), std::move(name));
        }
        case token_kind_t::LEFT_PAREN:
            return parse_parenthesized();
        case token_kind_t::KW_SELF:
            advance();
            return add_expr(token.span, self_expr_t{});
        case token_kind_t::KW_SUPER:
            advance();
            return add_expr(token.span, super_expr_t{});
        case token_kind_t::LEFT_BRACKET:
            return parse_array();
        case token_kind_t::LEFT_BRACE:
            return parse_map();
        case token_kind_t::KW_FUNCTION:
            return parse_lambda();
        case token_kind_t::KW_ASYNC:
            if (peek(1).kind == token_kind_t::KW_FUNCTION) {
                return parse_lambda(advance().span.begin);
            }

            report_expected("an expression");
            return missing_expr();
        case token_kind_t::KW_NEW:
            error(token.span, "{} isn't supported yet", describe(token.kind));
            return missing_expr();
        default:
            report_expected("an expression");
            return missing_expr();
        }
    }

    /// "Function(x: Int) => x * 2", or a block lambda: "Function(x)", its body on the lines after
    /// it, then "End Function". Wherever it is, even in a call's brackets, its body ends
    /// statements at line breaks. After "Async", which starts at `async`, it's an Async lambda.
    expr_id_t parse_lambda(const std::optional<std::uint32_t> async = std::nullopt) {
        const token_t& keyword = advance();
        const std::uint32_t begin = async.value_or(keyword.span.begin);
        std::vector<name_t> weak;
        if (at(token_kind_t::LEFT_BRACKET)) {
            weak = parse_capture_list();
        }

        std::vector<lambda_parameter_t> parameters;
        if (at(token_kind_t::LEFT_PAREN)) {
            const token_t& open = advance();
            ++m_bracket_depth;
            while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
                parameters.push_back(parse_lambda_parameter());
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }
            }

            expect_closing(token_kind_t::RIGHT_PAREN, open.span);
            --m_bracket_depth;
        } else {
            report_expected(describe(token_kind_t::LEFT_PAREN));
        }

        std::optional<type_expr_id_t> return_type;
        if (accept(token_kind_t::COLON)) {
            return_type = parse_type();
        }

        lambda_expr_t lambda{
            .weak = std::move(weak),
            .parameters = std::move(parameters),
            .return_type = return_type,
            .is_async = async.has_value(),
        };
        if (accept(token_kind_t::ARROW)) {
            skip_continuation_newlines();
            lambda.value = parse_expression();
            return add_expr(span_from(begin), std::move(lambda));
        }

        const std::uint32_t depth = std::exchange(m_bracket_depth, 0);
        if (!at_line_end()) {
            report_expected("'=>' or the end of the line");
            skip_rest_of_line();
        }

        accept(token_kind_t::NEWLINE);
        m_line_failed = false;
        lambda.body = parse_body(keyword);
        m_bracket_depth = depth;
        return add_expr(span_from(begin), std::move(lambda));
    }

    /// "[Weak Self, Weak node]": what a lambda refers to weakly, which is all a capture list says.
    std::vector<name_t> parse_capture_list() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<name_t> names;
        while (!at(token_kind_t::RIGHT_BRACKET) && !at(token_kind_t::END_OF_FILE)) {
            const bool weak = at(token_kind_t::IDENTIFIER) && text(peek()) == "Weak";
            if (weak) {
                advance();
            } else {
                error(peek().span, "a capture list only has Weak captures, like [Weak Self]");
            }

            const name_t name = at(token_kind_t::KW_SELF) ? take_name() : expect_name();
            if (weak) {
                names.push_back(name);
            }

            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::RIGHT_BRACKET, open.span);
        --m_bracket_depth;
        return names;
    }

    /// "x" or "x: Int". A lambda's parameters are never ByRef, and have no defaults.
    lambda_parameter_t parse_lambda_parameter() {
        if (at(token_kind_t::KW_BYREF)) {
            error(peek().span, "a lambda's parameters can't be ByRef");
            advance();
        }

        const name_t name = expect_name();
        std::optional<type_expr_id_t> type;
        if (accept(token_kind_t::COLON)) {
            type = parse_type();
        }

        if (at(token_kind_t::EQUAL)) {
            error(peek().span, "a lambda's parameters can't have defaults");
            advance();
            (void)parse_expression();
        }

        return { .name = name, .type = type };
    }

    /// "[a, b, c]". The elements can go over several lines, with a comma after the last one.
    expr_id_t parse_array() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<expr_id_t> elements;
        while (!at(token_kind_t::RIGHT_BRACKET) && !at(token_kind_t::END_OF_FILE)) {
            elements.push_back(parse_expression());
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::RIGHT_BRACKET, open.span);
        --m_bracket_depth;
        return add_expr(span_from(open.span.begin),
                        array_expr_t{ .elements = std::move(elements) });
    }

    /// "{key: value, ...}", a new Map of the entries. A comma can end the last one, as in an
    /// array.
    expr_id_t parse_map() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<map_entry_t> entries;
        while (!at(token_kind_t::RIGHT_BRACE) && !at(token_kind_t::END_OF_FILE)) {
            const expr_id_t key = parse_expression();
            expect(token_kind_t::COLON);
            entries.push_back({ .key = key, .value = parse_expression() });
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::RIGHT_BRACE, open.span);
        --m_bracket_depth;
        return add_expr(span_from(open.span.begin), map_expr_t{ .entries = std::move(entries) });
    }

    /// Parentheses only group: they get no node, and the inner expression's span leaves them out.
    /// With commas inside, they make a tuple, "(a, b)", whose span has them. A comma can end the
    /// last value, as in an array.
    expr_id_t parse_parenthesized() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<expr_id_t> elements{ parse_expression() };
        bool comma = false;
        while (accept(token_kind_t::COMMA)) {
            comma = true;
            if (at(token_kind_t::RIGHT_PAREN) || at(token_kind_t::END_OF_FILE)) {
                break;
            }

            elements.push_back(parse_expression());
        }

        expect_closing(token_kind_t::RIGHT_PAREN, open.span);
        --m_bracket_depth;
        if (elements.size() == 1) {
            if (comma) {
                error(span_from(open.span.begin), "a tuple has at least two values");
            }

            return elements.front();
        }

        return add_expr(span_from(open.span.begin),
                        tuple_expr_t{ .elements = std::move(elements) });
    }

    /// A string literal. An interpolated one comes as STRING_START, then expressions separated by
    /// STRING_MIDDLE pieces, then STRING_END. Text that decodes to nothing gets no part.
    expr_id_t parse_string() {
        const std::uint32_t begin = peek().span.begin;
        std::vector<std::string_view> pieces;
        std::vector<expr_id_t> values;

        const token_t& first = advance();
        pieces.push_back(text(first));
        if (first.kind == token_kind_t::STRING_START) {
            ++m_bracket_depth;
            while (true) {
                values.push_back(parse_expression());
                if (at(token_kind_t::STRING_MIDDLE)) {
                    pieces.push_back(text(advance()));
                } else if (at(token_kind_t::STRING_END)) {
                    pieces.push_back(text(advance()));
                    break;
                } else {
                    report_expected("'}'");
                    break;
                }
            }

            --m_bracket_depth;
        }

        // Decoded together: a multi-line string's indentation comes from its last piece.
        std::vector<std::string> texts = decode_string(pieces);
        std::vector<string_part_t> parts;
        for (std::size_t i = 0; i < texts.size(); ++i) {
            if (!texts[i].empty()) {
                parts.emplace_back(std::move(texts[i]));
            }

            if (i < values.size()) {
                parts.emplace_back(values[i]);
            }
        }

        return add_expr(span_from(begin), string_expr_t{ .parts = std::move(parts) });
    }

    // Types

    /// A type name or a tuple type, followed by any "?", "[]", and "[N]" suffixes, like "Int[]?",
    /// "Float[4, 4]", or "(Int, String)[]".
    type_expr_id_t parse_type() {
        const token_t& token = peek();
        type_expr_id_t type{};
        // A number is a Const type parameter's argument: the 64 in "Grid<Float, 64, 64>".
        const bool number =
            token.kind == token_kind_t::INTEGER || token.kind == token_kind_t::DECIMAL;
        const token_kind_t after = peek(1).kind;
        if (number || (token.kind == token_kind_t::MINUS &&
                       (after == token_kind_t::INTEGER || after == token_kind_t::DECIMAL))) {
            const expr_id_t value = parse_unary();
            return add_type(span_of(value), value_type_t{ .value = value });
        }

        if (token.kind == token_kind_t::LEFT_PAREN) {
            type = parse_tuple_type();
        } else if (token.kind == token_kind_t::IDENTIFIER) {
            type = parse_named_type();
        } else if (token.kind == token_kind_t::KW_SELF) {
            // In an Interface, Self is the type that implements it.
            advance();
            type = add_type(token.span, named_type_t{ .name = text(token) });
        } else if (token.kind == token_kind_t::KW_FUNC || token.kind == token_kind_t::KW_CALLBACK) {
            type = parse_function_type();
        } else {
            report_expected("a type");
            return add_type(point(token.span.begin), error_type_t{});
        }

        while (true) {
            // "T?" only when the '?' touches the type, so "x Is T ? a : b" stays a ternary.
            if (at(token_kind_t::QUESTION) && m_previous_end == peek().span.begin) {
                advance();
                type = add_type(span_from(token.span.begin), nullable_type_t{ .inner = type });
            } else if (at(token_kind_t::QUESTION_QUESTION) && m_previous_end == peek().span.begin) {
                const token_t& marks = advance();
                const span_t first{
                    .file = m_file.id(),
                    .begin = token.span.begin,
                    .end = marks.span.begin + 1,
                };
                type = add_type(first, nullable_type_t{ .inner = type });
                type = add_type(span_from(token.span.begin), nullable_type_t{ .inner = type });
            } else if (at(token_kind_t::LEFT_BRACKET) &&
                       peek(1).kind == token_kind_t::RIGHT_BRACKET) {
                advance();
                advance();
                type = add_type(span_from(token.span.begin), array_type_t{ .element = type });
            } else if (at(token_kind_t::LEFT_BRACKET)) {
                const token_t& open = advance();
                ++m_bracket_depth;
                std::vector<expr_id_t> sizes;
                while (!at(token_kind_t::RIGHT_BRACKET) && !at(token_kind_t::END_OF_FILE)) {
                    sizes.push_back(parse_expression());
                    if (!accept(token_kind_t::COMMA)) {
                        break;
                    }
                }

                expect_closing(token_kind_t::RIGHT_BRACKET, open.span);
                --m_bracket_depth;
                type = add_type(span_from(token.span.begin),
                                array_type_t{ .element = type, .sizes = std::move(sizes) });
            } else {
                return type;
            }
        }
    }

    /// A type by name, like "Int", or "Weak<Node>", or after a module's qualifier, like
    /// "C.Map<K, V>".
    type_expr_id_t parse_named_type() {
        const token_t& token = advance();
        if (accept(token_kind_t::DOT)) {
            const name_t qualifier{ .text = text(token), .span = token.span };
            const name_t name = expect_name();
            std::vector<type_expr_id_t> arguments;
            if (at(token_kind_t::LESS)) {
                arguments = parse_type_arguments();
            }

            return add_type(span_from(token.span.begin), named_type_t{
                                                             .name = name.text,
                                                             .arguments = std::move(arguments),
                                                             .qualifier = qualifier,
                                                         });
        }

        type_expr_id_t type = add_type(token.span, named_type_t{ .name = text(token) });
        // "Weak" is a contextual keyword, a name everywhere but before '<'.
        if (at(token_kind_t::LESS) && text(token) == "Weak") {
            advance();
            const type_expr_id_t inner = parse_type();
            expect(token_kind_t::GREATER);
            type = add_type(span_from(token.span.begin), weak_type_t{ .inner = inner });
        } else if (at(token_kind_t::LESS)) {
            std::vector<type_expr_id_t> arguments = parse_type_arguments();
            type = add_type(span_from(token.span.begin),
                            named_type_t{ .name = text(token), .arguments = std::move(arguments) });
        }

        return type;
    }

    /// "Func(A, B): R", or "Func(A)" for a function that returns nothing, and "Callback(A, B):
    /// R" alike. The return type takes any "?" after it, so a Func that can be Null is written
    /// "(Func(): Int)?".
    type_expr_id_t parse_function_type() {
        const token_t& keyword = advance();
        const bool callback = keyword.kind == token_kind_t::KW_CALLBACK;
        std::vector<type_expr_id_t> parameters;
        if (at(token_kind_t::LEFT_PAREN)) {
            const token_t& open = advance();
            ++m_bracket_depth;
            while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
                if (at(token_kind_t::KW_BYREF)) {
                    error(peek().span, "a {}'s parameters can't be ByRef",
                          callback ? "Callback" : "Func");
                    advance();
                }

                parameters.push_back(parse_type());
                if (!accept(token_kind_t::COMMA)) {
                    break;
                }
            }

            expect_closing(token_kind_t::RIGHT_PAREN, open.span);
            --m_bracket_depth;
        } else {
            report_expected(describe(token_kind_t::LEFT_PAREN));
        }

        std::optional<type_expr_id_t> result;
        if (accept(token_kind_t::COLON)) {
            result = parse_type();
        }

        return add_type(span_from(keyword.span.begin), function_type_t{
                                                           .parameters = std::move(parameters),
                                                           .result = result,
                                                           .callback = callback,
                                                       });
    }

    /// "(A, B)", a tuple type. Parentheses around one type only group it, as in an expression.
    type_expr_id_t parse_tuple_type() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<type_expr_id_t> elements{ parse_type() };
        bool comma = false;
        while (accept(token_kind_t::COMMA)) {
            comma = true;
            if (at(token_kind_t::RIGHT_PAREN) || at(token_kind_t::END_OF_FILE)) {
                break;
            }

            elements.push_back(parse_type());
        }

        expect_closing(token_kind_t::RIGHT_PAREN, open.span);
        --m_bracket_depth;
        if (elements.size() == 1) {
            if (comma) {
                error(span_from(open.span.begin), "a tuple has at least two types");
            }

            return elements.front();
        }

        return add_type(span_from(open.span.begin),
                        tuple_type_t{ .elements = std::move(elements) });
    }
};

} // namespace

ast_t parse(const source_file_t& file, const std::span<const token_t> tokens,
            diagnostic_bag_t& diagnostics) {
    ast_t ast;
    parse_into(file, tokens, diagnostics, ast);
    return ast;
}

void parse_into(const source_file_t& file, const std::span<const token_t> tokens,
                diagnostic_bag_t& diagnostics, ast_t& ast) {
    parser_t(file, tokens, diagnostics, ast).run();
}

} // namespace fb
