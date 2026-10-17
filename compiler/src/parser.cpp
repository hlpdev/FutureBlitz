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
    case token_kind_t::KW_ASYNC:
    case token_kind_t::KW_PACKAGE:
    case token_kind_t::KW_PARALLEL:
    case token_kind_t::KW_PRIVATE:
    case token_kind_t::KW_PROTECTED:
    case token_kind_t::KW_PUBLIC:
    case token_kind_t::KW_SHARED:
        return unsupported_t::PREFIX;
    case token_kind_t::KW_IMPORT:
        return unsupported_t::LINE;
    case token_kind_t::KW_EXTEND:
    case token_kind_t::KW_EXTERN:
    case token_kind_t::KW_INTERFACE:
    case token_kind_t::KW_LOCK:
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
        // @Flags is the one attribute M2 has.
        if (at(token_kind_t::AT) && peek(1).kind == token_kind_t::IDENTIFIER &&
            text(peek(1)) == "Flags") {
            return parse_flags();
        }

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
        case token_kind_t::KW_OPERATOR:
            return parse_function(function_kind_t::OPERATOR);
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
            return parse_type_declaration({});
        case token_kind_t::KW_ABSTRACT:
        case token_kind_t::KW_FINAL:
        case token_kind_t::KW_OVERRIDE:
        case token_kind_t::KW_STATIC:
        case token_kind_t::KW_READONLY:
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
    stmt_id_t parse_flags() {
        const std::uint32_t begin = advance().span.begin;
        const token_t& name = advance();
        if (at(token_kind_t::KW_ENUM)) {
            return parse_enum(true, begin);
        }

        error(name.span, "@Flags only goes before an Enum");
        return parse_statement();
    }

    /// "Enum Name: Type", its members, then "End Enum". Members are separated by commas, which
    /// can end a line, or by lines.
    stmt_id_t parse_enum(const bool flags, const std::uint32_t begin) {
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
            while (at(token_kind_t::IDENTIFIER)) {
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
        return add_stmt(span_from(begin), enum_stmt_t{
                                              .name = name,
                                              .type = type,
                                              .members = std::move(members),
                                              .flags = flags,
                                          });
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
        default:
            return nullptr;
        }
    }

    /// Abstract, Final, Override, Static, and ReadOnly, in any order, then the declaration
    /// they're on. Analysis checks which ones it can have.
    stmt_id_t parse_modified() {
        modifiers_t modifiers;
        while (bool* flag = modifier_flag(modifiers, peek().kind)) {
            const token_t& token = advance();
            if (*flag) {
                error(token.span, "{} is written twice", describe(token.kind));
            }

            *flag = true;
        }

        switch (peek().kind) {
        case token_kind_t::KW_STRUCT:
        case token_kind_t::KW_CLASS:
            return parse_type_declaration(modifiers);
        case token_kind_t::KW_FUNCTION:
            return parse_function(function_kind_t::FUNCTION, modifiers);
        case token_kind_t::KW_OPERATOR:
            return parse_function(function_kind_t::OPERATOR, modifiers);
        case token_kind_t::KW_METHOD:
            return parse_function(function_kind_t::METHOD, modifiers);
        case token_kind_t::KW_NEW:
            return parse_function(function_kind_t::CONSTRUCTOR, modifiers);
        case token_kind_t::KW_FIELD:
        case token_kind_t::KW_LOCAL:
        case token_kind_t::KW_GLOBAL:
        case token_kind_t::KW_CONST:
            return parse_variable(modifiers);
        case token_kind_t::KW_PROPERTY:
            return parse_property(modifiers);
        default:
            break;
        }

        if (at(token_kind_t::KW_BYREF) && peek(1).kind == token_kind_t::KW_METHOD) {
            return parse_function(function_kind_t::BYREF_METHOD, modifiers);
        }

        error(peek().span, "{} doesn't take modifiers like Static or Final", describe_current());
        return parse_statement();
    }

    /// "Property Name: Type", then "Get => value" or a Get block, and maybe "Set(value)" and its
    /// block, then "End Property". Get and Set are names everywhere else.
    stmt_id_t parse_property(const modifiers_t modifiers) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t name = expect_name();
        const type_expr_id_t type = expect(token_kind_t::COLON)
                                        ? parse_type()
                                        : add_type(point(peek().span.begin), error_type_t{});
        finish_line(start);
        m_blocks.push_back(keyword.kind);
        std::optional<stmt_id_t> getter;
        std::optional<stmt_id_t> setter;
        while (!is_block_end(peek().kind)) {
            const std::size_t line = m_pos;
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

    stmt_id_t parse_variable(const modifiers_t modifiers = {}) {
        const token_t& keyword = advance();
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
                                                           .modifiers = modifiers,
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
    /// the keyword is its name. An Abstract Method is only the line that declares it.
    stmt_id_t parse_function(const function_kind_t kind, const modifiers_t modifiers = {}) {
        const std::size_t start = m_pos;
        const std::uint32_t begin = peek().span.begin;
        if (kind == function_kind_t::BYREF_METHOD) {
            advance(); // ByRef
        }

        const token_t& keyword = advance();
        name_t name{ .text = text(keyword), .span = keyword.span };
        if (kind == function_kind_t::OPERATOR) {
            name = parse_operator_name();
        } else if (kind != function_kind_t::CONSTRUCTOR) {
            name = expect_name();
        }

        std::vector<name_t> type_parameters;
        if (at(token_kind_t::LESS)) {
            if (kind == function_kind_t::CONSTRUCTOR) {
                error(peek().span, "New can't have type parameters; its Struct or Class can");
            }

            type_parameters = parse_type_parameters();
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
        if (modifiers.is_abstract) {
            // The line ends without a body, and the caller finishes it.
        } else if (accept(token_kind_t::ARROW)) {
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
                                              .type_parameters = std::move(type_parameters),
                                              .parameters = std::move(parameters),
                                              .return_type = return_type,
                                              .throws = std::move(throws),
                                              .body = std::move(body),
                                              .modifiers = modifiers,
                                          });
    }

    /// The operator after "Operator", which names it: one a type can define, like "+" or "==".
    /// ">>" is two '>' that touch.
    name_t parse_operator_name() {
        const token_t& token = peek();
        switch (token.kind) {
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
        default:
            // A name where the operator goes is skipped, so the rest of the line still parses.
            report_expected("an operator, like + or ==");
            if (at(token_kind_t::IDENTIFIER)) {
                advance();
            }

            return { .text = {}, .span = point(token.span.begin) };
        }
    }

    /// "Struct Name" or "Class Name Extends Base", the members, then the matching End. Members
    /// parse like statements; analysis checks that each one is a Field, Method, or New.
    /// Interfaces come later.
    stmt_id_t parse_type_declaration(const modifiers_t modifiers) {
        const std::size_t start = m_pos;
        const token_t& keyword = advance();
        const name_t name = expect_name();
        std::vector<name_t> type_parameters;
        if (at(token_kind_t::LESS)) {
            type_parameters = parse_type_parameters();
        }

        std::optional<name_t> base;
        if (accept(token_kind_t::KW_EXTENDS)) {
            base = expect_name();
            if (at(token_kind_t::LESS)) {
                error(peek().span, "extending a generic Class isn't supported yet");
                (void)parse_type_arguments();
            }
        }

        if (at(token_kind_t::KW_IMPLEMENTS)) {
            error(peek().span, "{} isn't supported yet", describe(peek().kind));
            skip_rest_of_line();
        }

        finish_line(start);
        block_t members = parse_body(keyword);
        return add_stmt(span_from(keyword.span.begin),
                        type_stmt_t{
                            .is_class = keyword.kind == token_kind_t::KW_CLASS,
                            .name = name,
                            .type_parameters = std::move(type_parameters),
                            .members = std::move(members),
                            .base = base,
                            .modifiers = modifiers,
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
        case token_kind_t::KW_TRY:
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

        return std::holds_alternative<call_expr_t>(node);
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
        if (token.kind == token_kind_t::KW_TRY) {
            return parse_try_expression();
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
        const expr_id_t operand = parse_unary();
        return add_expr(span_from(token.span.begin), unary_expr_t{ .op = *op, .operand = operand });
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

        const expr_id_t operand = parse_unary();
        return add_expr(span_from(keyword.span.begin),
                        try_expr_t{ .operand = operand, .optional = optional });
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
            case token_kind_t::COMMA:
            case token_kind_t::QUESTION:
            case token_kind_t::LEFT_BRACKET:
            case token_kind_t::RIGHT_BRACKET:
            case token_kind_t::INTEGER:
            case token_kind_t::KW_FUNC:
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
            arguments.push_back(parse_type());
            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::GREATER, open.span);
        --m_bracket_depth;
        return arguments;
    }

    /// "<T, U>" after the name of a generic Function, Method, Struct, or Class. Constraints come
    /// with interfaces.
    std::vector<name_t> parse_type_parameters() {
        const token_t& open = advance();
        ++m_bracket_depth;
        std::vector<name_t> names;
        while (!at(token_kind_t::GREATER) && !at(token_kind_t::END_OF_FILE)) {
            names.push_back(expect_name());
            if (at(token_kind_t::COLON)) {
                error(peek().span, "constraints on type parameters aren't supported yet");
                advance();
                (void)parse_type();
            }

            if (!accept(token_kind_t::COMMA)) {
                break;
            }
        }

        expect_closing(token_kind_t::GREATER, open.span);
        --m_bracket_depth;
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
        case token_kind_t::KW_FUNCTION:
            return parse_lambda();
        case token_kind_t::KW_NEW:
        case token_kind_t::KW_AWAIT:
        case token_kind_t::LEFT_BRACE:
            error(token.span, "{} isn't supported yet", describe(token.kind));
            return missing_expr();
        default:
            report_expected("an expression");
            return missing_expr();
        }
    }

    /// "Function(x: Int) => x * 2", or a block lambda: "Function(x)", its body on the lines after
    /// it, then "End Function". Wherever it is, even in a call's brackets, its body ends
    /// statements at line breaks.
    expr_id_t parse_lambda() {
        const token_t& keyword = advance();
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
        };
        if (accept(token_kind_t::ARROW)) {
            skip_continuation_newlines();
            lambda.value = parse_expression();
            return add_expr(span_from(keyword.span.begin), std::move(lambda));
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
        return add_expr(span_from(keyword.span.begin), std::move(lambda));
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
        if (token.kind == token_kind_t::LEFT_PAREN) {
            type = parse_tuple_type();
        } else if (token.kind == token_kind_t::IDENTIFIER) {
            type = parse_named_type();
        } else if (token.kind == token_kind_t::KW_FUNC) {
            type = parse_function_type();
        } else {
            if (token.kind == token_kind_t::KW_CALLBACK) {
                error(token.span, "{} isn't supported yet", describe(token.kind));
            } else {
                report_expected("a type");
            }

            return add_type(point(token.span.begin), error_type_t{});
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

    /// A type by name, like "Int", or "Weak<Node>".
    type_expr_id_t parse_named_type() {
        const token_t& token = advance();
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

    /// "Func(A, B): R", or "Func(A)" for a function that returns nothing. The return type takes
    /// any "?" after it, so a Func that can be Null is written "(Func(): Int)?".
    type_expr_id_t parse_function_type() {
        const token_t& keyword = advance();
        std::vector<type_expr_id_t> parameters;
        if (at(token_kind_t::LEFT_PAREN)) {
            const token_t& open = advance();
            ++m_bracket_depth;
            while (!at(token_kind_t::RIGHT_PAREN) && !at(token_kind_t::END_OF_FILE)) {
                if (at(token_kind_t::KW_BYREF)) {
                    error(peek().span, "a Func's parameters can't be ByRef");
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

        return add_type(span_from(keyword.span.begin),
                        function_type_t{ .parameters = std::move(parameters), .result = result });
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
