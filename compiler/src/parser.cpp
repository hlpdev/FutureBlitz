#include "fb/parser.hpp"

#include "fb/literal.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// How the parser skips a construct that comes after M1.
enum class unsupported_t : std::uint8_t {
    NONE,
    PREFIX, // report it, then parse the rest of the statement: "Public Function F()"
    LINE,   // report it and skip the line: "Import std.math"
    BLOCK,  // report it and skip to its "End <keyword>": "Class ... End Class"
};

/// How to skip a statement that starts with `kind`. NONE means M1 parses it.
constexpr unsupported_t unsupported(const token_kind_t kind) {
    switch (kind) {
    case token_kind_t::AT:
    case token_kind_t::KW_ABSTRACT:
    case token_kind_t::KW_ASYNC:
    case token_kind_t::KW_DEFER:
    case token_kind_t::KW_FINAL:
    case token_kind_t::KW_OVERRIDE:
    case token_kind_t::KW_PACKAGE:
    case token_kind_t::KW_PARALLEL:
    case token_kind_t::KW_PRIVATE:
    case token_kind_t::KW_PROTECTED:
    case token_kind_t::KW_PUBLIC:
    case token_kind_t::KW_READONLY:
    case token_kind_t::KW_SHARED:
    case token_kind_t::KW_STATIC:
        return unsupported_t::PREFIX;
    case token_kind_t::KW_IMPORT:
    case token_kind_t::KW_THROW:
        return unsupported_t::LINE;
    case token_kind_t::KW_ENUM:
    case token_kind_t::KW_EXTEND:
    case token_kind_t::KW_EXTERN:
    case token_kind_t::KW_INTERFACE:
    case token_kind_t::KW_LOCK:
    case token_kind_t::KW_TRY:
    case token_kind_t::KW_UNSAFE:
        return unsupported_t::BLOCK;
    default:
        return unsupported_t::NONE;
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
             diagnostic_bag_t& diagnostics)
        : m_file(file), m_tokens(tokens), m_diagnostics(diagnostics) {
        assert(!m_tokens.empty() && m_tokens.back().kind == token_kind_t::END_OF_FILE);
    }

    ast_t run() {
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

        return std::move(m_ast);
    }

private:
    const source_file_t& m_file;
    std::span<const token_t> m_tokens;
    diagnostic_bag_t& m_diagnostics;
    ast_t m_ast;
    std::vector<token_kind_t> m_blocks; // keywords of the open blocks, innermost last
    std::size_t m_pos = 0;
    std::uint32_t m_previous_end = 0; // where the last consumed token ends
    /// Open brackets and string interpolations. While it's above 0, NEWLINE tokens are skipped.
    std::uint32_t m_bracket_depth = 0;
    bool m_line_failed = false; // the current line already has an error

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
        if (m_line_failed) {
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
        while (!is_block_end(peek().kind)) {
            const std::size_t start = m_pos;
            block.push_back(parse_statement());
            finish_line(start);
        }

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
        switch (unsupported(peek().kind)) {
        case unsupported_t::PREFIX:
            return parse_unsupported_prefix();
        case unsupported_t::LINE:
            return parse_unsupported_line();
        case unsupported_t::BLOCK:
            return parse_unsupported_block();
        case unsupported_t::NONE:
            break;
        }

        switch (peek().kind) {
        case token_kind_t::KW_CONST:
            // "Const Function" declares a const function, which comes after M1.
            if (peek(1).kind == token_kind_t::KW_FUNCTION) {
                return parse_unsupported_prefix();
            }

            return parse_variable();
        case token_kind_t::KW_LOCAL:
        case token_kind_t::KW_GLOBAL:
        case token_kind_t::KW_FIELD:
            return parse_variable();
        case token_kind_t::KW_FUNCTION:
            return parse_function(function_kind_t::FUNCTION);
        // Struct members parse anywhere; analysis reports the ones outside a Struct.
        case token_kind_t::KW_METHOD:
            return parse_function(function_kind_t::METHOD);
        case token_kind_t::KW_NEW:
            return parse_function(function_kind_t::CONSTRUCTOR);
        case token_kind_t::KW_BYREF:
            if (peek(1).kind == token_kind_t::KW_METHOD) {
                return parse_function(function_kind_t::BYREF_METHOD);
            }

            return parse_simple();
        case token_kind_t::KW_STRUCT:
        case token_kind_t::KW_CLASS:
            return parse_type_declaration();
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
        case token_kind_t::KW_SELECT:
            return parse_select();
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

    /// Reports a modifier or attribute that comes after M1, then parses the statement it's on.
    stmt_id_t parse_unsupported_prefix() {
        const token_t& token = advance();
        if (token.kind == token_kind_t::AT) {
            error(token.span, "attributes aren't supported yet");
            accept(token_kind_t::IDENTIFIER);
            if (at(token_kind_t::LEFT_PAREN)) {
                skip_parenthesized();
            }
        } else {
            error(token.span, "{} isn't supported yet", describe(token.kind));
        }

        return parse_statement();
    }

    /// Skips from '(' to its matching ')', like an attribute's arguments.
    void skip_parenthesized() {
        advance();
        ++m_bracket_depth;
        while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
            if (at(token_kind_t::LEFT_PAREN)) {
                skip_parenthesized();
            } else {
                advance();
            }
        }

        accept(token_kind_t::RIGHT_PAREN);
        --m_bracket_depth;
    }

    stmt_id_t parse_unsupported_line() {
        const token_t& keyword = advance();
        error(keyword.span, "{} isn't supported yet", describe(keyword.kind));
        skip_rest_of_line();
        return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
    }

    /// Skips to the matching "End <keyword>", counting nested blocks of the same kind.
    stmt_id_t parse_unsupported_block() {
        const token_t& keyword = advance();
        error(keyword.span, "{} isn't supported yet", describe(keyword.kind));

        std::uint32_t depth = 1;
        while (depth > 0 && !at(token_kind_t::END_OF_FILE)) {
            if (at(token_kind_t::KW_END) && peek(1).kind == keyword.kind) {
                advance();
                advance();
                --depth;
            } else {
                if (at(keyword.kind)) {
                    ++depth;
                }

                advance();
            }
        }

        return add_stmt(span_from(keyword.span.begin), error_stmt_t{});
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
        default:
            error(label.span, "a label must be followed by a loop");
            return parse_statement();
        }
    }

    stmt_id_t parse_variable() {
        const token_t& keyword = advance();
        const name_t name = expect_name();
        if (at(token_kind_t::COMMA)) {
            error(peek().span, "declaring several variables at once isn't supported yet");
        }

        std::optional<type_expr_id_t> type;
        if (accept(token_kind_t::COLON)) {
            type = parse_type();
        }

        std::optional<expr_id_t> value;
        if (accept(token_kind_t::EQUAL)) {
            skip_continuation_newlines();
            value = parse_expression();
        }

        variable_kind_t kind = variable_kind_t::CONSTANT;
        if (keyword.kind == token_kind_t::KW_LOCAL) {
            kind = variable_kind_t::LOCAL;
        } else if (keyword.kind == token_kind_t::KW_GLOBAL) {
            kind = variable_kind_t::GLOBAL;
        } else if (keyword.kind == token_kind_t::KW_FIELD) {
            kind = variable_kind_t::FIELD;
        }

        return add_stmt(
            span_from(keyword.span.begin),
            variable_stmt_t{ .kind = kind, .name = name, .type = type, .value = value });
    }

    /// A Function, or a Struct's Method, "ByRef Method", or New. New has no name of its own:
    /// the keyword is its name.
    stmt_id_t parse_function(const function_kind_t kind) {
        const std::size_t start = m_pos;
        const std::uint32_t begin = peek().span.begin;
        if (kind == function_kind_t::BYREF_METHOD) {
            advance(); // ByRef
        }

        const token_t& keyword = advance();
        const name_t name = kind == function_kind_t::CONSTRUCTOR
                                ? name_t{ .text = text(keyword), .span = keyword.span }
                                : expect_name();

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

        // "Function F(): Int => x" is short for a body that returns x.
        block_t body;
        if (accept(token_kind_t::ARROW)) {
            skip_continuation_newlines();
            const expr_id_t value = parse_expression();
            body.push_back(add_stmt(span_of(value), return_stmt_t{ .value = value }));
        } else {
            finish_line(start);
            body = parse_body(keyword);
        }

        return add_stmt(span_from(begin), function_stmt_t{
                                              .kind = kind,
                                              .name = name,
                                              .parameters = std::move(parameters),
                                              .return_type = return_type,
                                              .body = std::move(body),
                                          });
    }

    /// "Struct Name" or "Class Name", the members, then the matching End. Members parse like
    /// statements; analysis checks that each one is a Field, Method, or New. A Class's base and
    /// interfaces come later.
    stmt_id_t parse_type_declaration() {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t name = expect_name();
        if (at(token_kind_t::KW_EXTENDS) || at(token_kind_t::KW_IMPLEMENTS)) {
            error(peek().span, "{} isn't supported yet", describe(peek().kind));
            skip_rest_of_line();
        }

        finish_line(start);
        block_t members = parse_body(keyword);
        return add_stmt(span_from(keyword.span.begin),
                        type_stmt_t{
                            .is_class = keyword.kind == token_kind_t::KW_CLASS,
                            .name = name,
                            .members = std::move(members),
                        });
    }

    parameter_t parse_parameter() {
        const bool by_ref = accept(token_kind_t::KW_BYREF);
        const name_t name = expect_name();
        const type_expr_id_t type = expect(token_kind_t::COLON)
                                        ? parse_type()
                                        : add_type(point(peek().span.begin), error_type_t{});
        std::optional<expr_id_t> default_value;
        if (accept(token_kind_t::EQUAL)) {
            default_value = parse_expression();
        }

        return { .name = name, .by_ref = by_ref, .type = type, .default_value = default_value };
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

    /// The statement after Then or Else. Block statements need lines of their own.
    stmt_id_t parse_single_line_body() {
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

    stmt_id_t parse_loop(const std::optional<name_t>& label) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        finish_line(start);
        block_t body = parse_body(keyword);
        return add_stmt(span_from(begin_of(label, keyword)),
                        loop_stmt_t{ .label = label, .body = std::move(body) });
    }

    stmt_id_t parse_for(const std::optional<name_t>& label) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t first = expect_name();

        // "For x In items" or "For i, x In items". Anything else is a counting loop.
        if (at(token_kind_t::COMMA) || at(token_kind_t::KW_IN)) {
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
        return add_stmt(span_from(begin_of(label, keyword)), for_stmt_t{
                                                                 .label = label,
                                                                 .variable = first,
                                                                 .start = from,
                                                                 .end = to,
                                                                 .inclusive = inclusive,
                                                                 .step = step,
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
        const expr_id_t target = parse_expression();

        if (const std::optional<assignment_t> assignment = assignment_at_current()) {
            for (std::uint32_t i = 0; i < assignment->tokens; ++i) {
                advance();
            }

            skip_continuation_newlines();
            const expr_id_t value = parse_expression();
            return add_stmt(
                span_from(begin),
                assign_stmt_t{ .op = assignment->op, .target = target, .value = value });
        }

        if (at(token_kind_t::PLUS_PLUS) || at(token_kind_t::MINUS_MINUS)) {
            const bool decrement = advance().kind == token_kind_t::MINUS_MINUS;
            return add_stmt(span_from(begin),
                            increment_stmt_t{ .target = target, .decrement = decrement });
        }

        const expr_node_t& node = m_ast.get(target).node;
        const bool is_call = std::holds_alternative<call_expr_t>(node);
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
        expr_id_t left = parse_unary();
        while (true) {
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
                return left;
            }
        }
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
        const token_t& token = peek();
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
        const expr_id_t operand = parse_unary();
        return add_expr(span_from(token.span.begin), unary_expr_t{ .op = *op, .operand = operand });
    }

    /// Member access, calls, and indexing after `expr`. They bind tightest of all.
    expr_id_t parse_postfix(expr_id_t expr) {
        while (true) {
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
                return expr;
            }
        }
    }

    expr_id_t parse_member(const expr_id_t object) {
        const bool null_safe = advance().kind == token_kind_t::QUESTION_DOT;
        if (!at(token_kind_t::IDENTIFIER)) {
            report_expected(describe(token_kind_t::IDENTIFIER));
            return add_expr(span_from(span_of(object).begin), error_expr_t{});
        }

        const name_t member = take_name();
        return add_expr(
            span_from(span_of(object).begin),
            member_expr_t{ .object = object, .member = member, .null_safe = null_safe });
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

    expr_id_t parse_index(const expr_id_t object) {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<expr_id_t> indices;
        while (!at(token_kind_t::RIGHT_BRACKET) && !at(token_kind_t::END_OF_FILE)) {
            indices.push_back(parse_expression());
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::RIGHT_BRACKET, open.span);
        --m_bracket_depth;
        return add_expr(span_from(span_of(object).begin),
                        index_expr_t{ .object = object, .indices = std::move(indices) });
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
        case token_kind_t::IDENTIFIER:
            advance();
            return add_expr(token.span, name_expr_t{ .name = text(token) });
        case token_kind_t::LEFT_PAREN:
            return parse_parenthesized();
        case token_kind_t::KW_SELF:
            advance();
            return add_expr(token.span, self_expr_t{});
        case token_kind_t::KW_SUPER:
        case token_kind_t::KW_NEW:
        case token_kind_t::KW_AWAIT:
        case token_kind_t::KW_TRY:
        case token_kind_t::KW_FUNCTION:
        case token_kind_t::LEFT_BRACKET:
        case token_kind_t::LEFT_BRACE:
            error(token.span, "{} isn't supported yet", describe(token.kind));
            return missing_expr();
        default:
            report_expected("an expression");
            return missing_expr();
        }
    }

    /// Parentheses only group: they get no node, and the inner expression's span leaves them out.
    expr_id_t parse_parenthesized() {
        const token_t& open = advance();
        ++m_bracket_depth;
        const expr_id_t inner = parse_expression();
        expect_closing(token_kind_t::RIGHT_PAREN, open.span);
        --m_bracket_depth;
        return inner;
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

    /// A type name followed by any "?" and "[]" suffixes, like "Int[]?".
    type_expr_id_t parse_type() {
        const token_t& token = peek();
        if (token.kind != token_kind_t::IDENTIFIER) {
            if (token.kind == token_kind_t::KW_FUNC || token.kind == token_kind_t::KW_CALLBACK) {
                error(token.span, "{} isn't supported yet", describe(token.kind));
            } else {
                report_expected("a type");
            }

            return add_type(point(token.span.begin), error_type_t{});
        }

        advance();
        type_expr_id_t type = add_type(token.span, named_type_t{ .name = text(token) });
        if (at(token_kind_t::LESS)) {
            error(peek().span, "generic types aren't supported yet");
        }

        while (true) {
            // "T?" only when the '?' touches the type, so "x Is T ? a : b" stays a ternary.
            if (at(token_kind_t::QUESTION) && m_previous_end == peek().span.begin) {
                advance();
                type = add_type(span_from(token.span.begin), nullable_type_t{ .inner = type });
            } else if (at(token_kind_t::LEFT_BRACKET) &&
                       peek(1).kind == token_kind_t::RIGHT_BRACKET) {
                advance();
                advance();
                type = add_type(span_from(token.span.begin), array_type_t{ .element = type });
            } else if (at(token_kind_t::LEFT_BRACKET)) {
                error(peek().span, "fixed-size arrays aren't supported yet");
                return type;
            } else {
                return type;
            }
        }
    }
};

} // namespace

ast_t parse(const source_file_t& file, const std::span<const token_t> tokens,
            diagnostic_bag_t& diagnostics) {
    return parser_t(file, tokens, diagnostics).run();
}

} // namespace fb
