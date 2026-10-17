#include "fb/dump.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

// The --show views: tokens, the AST as S-expressions (compact for tests, indented for people),
// the indented AST with types, and colored LLVM IR. The MIR view is in mir.cpp.

namespace fb {
namespace {

// Tokens

/// Indexed by token_kind_t: whether each kind is a keyword, from the same list as the kinds.
constexpr std::array is_keyword = {
#define FB_TOKEN(name, description) false,
#define FB_KEYWORD(name, spelling)  true,
#include "fb/token_kinds.def"
};

tone_t token_tone(const token_kind_t kind) {
    if (is_keyword[std::to_underlying(kind)]) {
        return tone_t::KEYWORD;
    }

    switch (kind) {
    case token_kind_t::IDENTIFIER:
        return tone_t::NAME;
    case token_kind_t::INTEGER:
    case token_kind_t::DECIMAL:
    case token_kind_t::CHARACTER:
    case token_kind_t::STRING:
    case token_kind_t::STRING_START:
    case token_kind_t::STRING_MIDDLE:
    case token_kind_t::STRING_END:
        return tone_t::LITERAL;
    case token_kind_t::INVALID:
        return tone_t::ERROR;
    default:
        return tone_t::DIM; // punctuation, newlines, and the end of the file
    }
}

/// Keywords and punctuation already show their spelling in their description.
bool shows_text(const token_kind_t kind) {
    return !describe(kind).starts_with('\'') && kind != token_kind_t::NEWLINE &&
           kind != token_kind_t::END_OF_FILE;
}

/// Escapes line breaks and tabs, so every token stays on one line.
std::string escape_controls(const std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }

    return out;
}

// Trees

/// `text` the way source code writes it: in double quotes, with escapes.
std::string quote(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        default:
            out += c;
            break;
        }
    }

    return out + "\"";
}

/// An atom like "x" or "Int32", or a parenthesized list whose first item is its head.
struct node_t {
    std::string text;
    tone_t tone = tone_t::PLAIN;
    std::vector<node_t> items;
    bool list = false;
    bool block = false; // a "(block ...)": the tree puts each statement on its own line
    /// The expression this node shows, which the types view annotates.
    std::optional<expr_id_t> expr;
};

node_t atom(std::string text, const tone_t tone = tone_t::PLAIN) {
    return { .text = std::move(text), .tone = tone };
}

node_t list(const std::string_view head, std::vector<node_t> items = {}) {
    items.insert(items.begin(), atom(std::string(head), tone_t::KEYWORD));
    return { .items = std::move(items), .list = true };
}

/// Whether a block is anywhere inside, which makes the tree view split the node over lines.
bool has_block(const node_t& node) {
    return node.block || std::ranges::any_of(node.items, has_block);
}

/// Turns the AST into nodes. In the tree, literals are plain atoms: "1" instead of "(int 1)".
class builder_t {
public:
    builder_t(const ast_t& ast, const bool tree) : m_ast(ast), m_tree(tree) {}

    [[nodiscard]] node_t expr(const expr_id_t id) const {
        node_t node =
            std::visit([this](const auto& item) { return this->build(item); }, m_ast.get(id).node);
        node.expr = id;
        return node;
    }

    [[nodiscard]] node_t stmt(const stmt_id_t id) const {
        return std::visit([this](const auto& item) { return this->build(item); },
                          m_ast.get(id).node);
    }

private:
    const ast_t& m_ast;
    /// For the tree views, rather than the compact form the parser tests compare.
    bool m_tree;

    [[nodiscard]] node_t literal(const std::string_view kind, std::string value) const {
        if (m_tree) {
            return atom(std::move(value), tone_t::LITERAL);
        }

        return list(kind, { atom(std::move(value), tone_t::LITERAL) });
    }

    [[nodiscard]] node_t block(const block_t& body) const {
        node_t out = list("block");
        out.block = true;
        for (const stmt_id_t id : body) {
            out.items.push_back(stmt(id));
        }

        return out;
    }

    static void add_label(node_t& node, const std::optional<name_t>& label) {
        if (label) {
            node.items.push_back(atom(std::format("{}:", label->text), tone_t::NAME));
        }
    }

    [[nodiscard]] node_t type(const type_expr_id_t id) const {
        return atom(type_text(id), tone_t::TYPE);
    }

    [[nodiscard]] std::string type_text(const type_expr_id_t id) const {
        return std::visit([this](const auto& item) { return this->type_text(item); },
                          m_ast.get(id).node);
    }

    // Types

    static std::string type_text(const error_type_t& /*node*/) {
        return "(error)";
    }

    static std::string type_text(const named_type_t& node) {
        return std::string(node.name);
    }

    [[nodiscard]] std::string type_text(const nullable_type_t& node) const {
        return type_text(node.inner) + "?";
    }

    /// "Float[64, 64]". The sizes show the way the tree shows expressions.
    [[nodiscard]] std::string type_text(const array_type_t& node) const {
        std::string sizes;
        const builder_t tree(m_ast, true);
        for (std::size_t i = 0; i < node.sizes.size(); ++i) {
            sizes += i == 0 ? "" : ", ";
            sizes += plain(tree.expr(node.sizes[i]));
        }

        return std::format("{}[{}]", type_text(node.element), sizes);
    }

    /// A node as unstyled text on one line.
    static std::string plain(const node_t& node) {
        if (!node.list) {
            return node.text;
        }

        std::string out = "(";
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            out += (i == 0 ? "" : " ") + plain(node.items[i]);
        }

        return out + ")";
    }

    // Expressions

    static node_t build(const error_expr_t& /*node*/) {
        return list("error");
    }

    [[nodiscard]] node_t build(const integer_expr_t& node) const {
        return literal("int", std::format("{}", node.value));
    }

    [[nodiscard]] node_t build(const decimal_expr_t& node) const {
        std::string value = std::format("{}", node.value);
        if (m_tree && !value.contains('.') && !value.contains('e')) {
            value += ".0"; // so 3.0 doesn't look like an integer
        }

        return literal("decimal", std::move(value));
    }

    [[nodiscard]] node_t build(const character_expr_t& node) const {
        const auto value = static_cast<std::uint32_t>(node.value);
        // Only visible ASCII shows as itself. Anything else, space included, shows its code point.
        if (value > 0x20 && value < 0x7F) {
            return literal("char", std::format("'{}'", static_cast<char>(value)));
        }

        return literal("char", std::format("U+{:04X}", value));
    }

    [[nodiscard]] node_t build(const string_expr_t& node) const {
        const bool plain =
            node.parts.size() == 1 && std::holds_alternative<std::string>(node.parts.front());
        // In the tree, a string without interpolation is just its quoted text.
        if (m_tree && (plain || node.parts.empty())) {
            return atom(quote(plain ? std::get<std::string>(node.parts.front()) : ""),
                        tone_t::LITERAL);
        }

        node_t out = list("string");
        for (const string_part_t& part : node.parts) {
            if (const auto* text = std::get_if<std::string>(&part)) {
                out.items.push_back(atom(quote(*text), tone_t::LITERAL));
            } else {
                out.items.push_back(expr(std::get<expr_id_t>(part)));
            }
        }

        return out;
    }

    [[nodiscard]] node_t build(const bool_expr_t& node) const {
        return literal("bool", node.value ? "True" : "False");
    }

    [[nodiscard]] node_t build(const null_expr_t& /*node*/) const {
        return m_tree ? atom("Null", tone_t::LITERAL) : list("null");
    }

    static node_t build(const name_expr_t& node) {
        return atom(std::string(node.name), tone_t::NAME);
    }

    [[nodiscard]] node_t build(const unary_expr_t& node) const {
        return list(spelling(node.op), { expr(node.operand) });
    }

    [[nodiscard]] node_t build(const binary_expr_t& node) const {
        return list(spelling(node.op), { expr(node.left), expr(node.right) });
    }

    [[nodiscard]] node_t build(const ternary_expr_t& node) const {
        return list("?", { expr(node.condition), expr(node.if_true), expr(node.if_false) });
    }

    [[nodiscard]] node_t build(const call_expr_t& node) const {
        node_t out = list("call", { expr(node.callee) });
        for (const argument_t& argument : node.arguments) {
            if (argument.name) {
                out.items.push_back(atom(std::format("{}:", argument.name->text), tone_t::NAME));
            }

            if (argument.by_ref) {
                out.items.push_back(atom("ByRef", tone_t::KEYWORD));
            }

            out.items.push_back(expr(argument.value));
        }

        return out;
    }

    [[nodiscard]] node_t build(const member_expr_t& node) const {
        return list(node.null_safe ? "?." : ".",
                    { expr(node.object), atom(std::string(node.member.text), tone_t::NAME) });
    }

    [[nodiscard]] node_t build(const index_expr_t& node) const {
        node_t out = list("index", { expr(node.object) });
        for (const expr_id_t index : node.indices) {
            out.items.push_back(expr(index));
        }

        return out;
    }

    [[nodiscard]] node_t build(const cast_expr_t& node) const {
        return list(node.checked ? "as?" : "as", { expr(node.value), type(node.type) });
    }

    [[nodiscard]] node_t build(const is_expr_t& node) const {
        return list("is", { expr(node.value), type(node.type) });
    }

    static node_t build(const self_expr_t& /*node*/) {
        return atom("Self", tone_t::KEYWORD);
    }

    static node_t build(const super_expr_t& /*node*/) {
        return atom("Super", tone_t::KEYWORD);
    }

    [[nodiscard]] node_t build(const array_expr_t& node) const {
        node_t out = list("array");
        for (const expr_id_t element : node.elements) {
            out.items.push_back(expr(element));
        }

        return out;
    }

    /// Abstract, Final, and Override, as keywords after the node's head.
    static void add_modifiers(node_t& out, const modifiers_t& modifiers) {
        if (modifiers.is_abstract) {
            out.items.push_back(atom("Abstract", tone_t::KEYWORD));
        }

        if (modifiers.is_final) {
            out.items.push_back(atom("Final", tone_t::KEYWORD));
        }

        if (modifiers.is_override) {
            out.items.push_back(atom("Override", tone_t::KEYWORD));
        }
    }

    // Statements

    static node_t build(const error_stmt_t& /*node*/) {
        return list("error");
    }

    [[nodiscard]] node_t build(const expression_stmt_t& node) const {
        return expr(node.expression);
    }

    [[nodiscard]] node_t build(const assign_stmt_t& node) const {
        const std::string op = node.op ? std::format("{}=", spelling(*node.op)) : "=";
        return list(op, { expr(node.target), expr(node.value) });
    }

    [[nodiscard]] node_t build(const increment_stmt_t& node) const {
        return list(node.decrement ? "--" : "++", { expr(node.target) });
    }

    [[nodiscard]] node_t build(const variable_stmt_t& node) const {
        std::string_view kind = "const";
        if (node.kind == variable_kind_t::LOCAL) {
            kind = "local";
        } else if (node.kind == variable_kind_t::GLOBAL) {
            kind = "global";
        } else if (node.kind == variable_kind_t::FIELD) {
            kind = "field";
        }

        // "hp:" when a type follows, as in "(local hp: Int = 100)".
        const std::string name = std::format("{}{}", node.name.text, node.type ? ":" : "");
        node_t out = list(kind, { atom(name, tone_t::NAME) });
        if (node.type) {
            out.items.push_back(type(*node.type));
        }

        if (node.value) {
            out.items.push_back(atom("="));
            out.items.push_back(expr(*node.value));
        }

        return out;
    }

    /// "(function Name ...)", "(method ...)", "(byref-method ...)", or "(new ...)", which has no
    /// name of its own.
    [[nodiscard]] node_t build(const function_stmt_t& node) const {
        std::string_view kind = "function";
        if (node.kind == function_kind_t::METHOD) {
            kind = "method";
        } else if (node.kind == function_kind_t::BYREF_METHOD) {
            kind = "byref-method";
        } else if (node.kind == function_kind_t::CONSTRUCTOR) {
            kind = "new";
        }

        node_t out = list(kind);
        add_modifiers(out, node.modifiers);
        if (node.kind != function_kind_t::CONSTRUCTOR) {
            out.items.push_back(atom(std::string(node.name.text), tone_t::NAME));
        }

        // Each parameter is a list without a head, like "(ByRef n: Int = 1)".
        for (const parameter_t& parameter : node.parameters) {
            node_t item{ .list = true };
            if (parameter.by_ref) {
                item.items.push_back(atom("ByRef", tone_t::KEYWORD));
            }

            item.items.push_back(atom(std::format("{}:", parameter.name.text), tone_t::NAME));
            item.items.push_back(type(parameter.type));
            if (parameter.default_value) {
                item.items.push_back(atom("="));
                item.items.push_back(expr(*parameter.default_value));
            }

            out.items.push_back(std::move(item));
        }

        if (node.return_type) {
            out.items.push_back(atom("->"));
            out.items.push_back(type(*node.return_type));
        }

        out.items.push_back(block(node.body));
        return out;
    }

    [[nodiscard]] node_t build(const if_stmt_t& node) const {
        node_t out = list(node.single_line ? "if-then" : "if");
        for (std::size_t i = 0; i < node.branches.size(); ++i) {
            const if_branch_t& branch = node.branches[i];
            if (i == 0) {
                out.items.push_back(expr(branch.condition));
                out.items.push_back(block(branch.body));
            } else {
                out.items.push_back(list("elseif", { expr(branch.condition), block(branch.body) }));
            }
        }

        if (node.else_body) {
            out.items.push_back(list("else", { block(*node.else_body) }));
        }

        return out;
    }

    [[nodiscard]] node_t build(const while_stmt_t& node) const {
        node_t out = list("while");
        add_label(out, node.label);
        out.items.push_back(expr(node.condition));
        out.items.push_back(block(node.body));
        return out;
    }

    [[nodiscard]] node_t build(const repeat_stmt_t& node) const {
        node_t out = list("repeat");
        add_label(out, node.label);
        out.items.push_back(block(node.body));
        out.items.push_back(atom("until", tone_t::KEYWORD));
        out.items.push_back(expr(node.condition));
        return out;
    }

    [[nodiscard]] node_t build(const loop_stmt_t& node) const {
        node_t out = list("loop");
        add_label(out, node.label);
        out.items.push_back(block(node.body));
        return out;
    }

    [[nodiscard]] node_t build(const for_stmt_t& node) const {
        node_t out = list("for");
        add_label(out, node.label);
        out.items.push_back(atom(std::string(node.variable.text), tone_t::NAME));
        out.items.push_back(expr(node.start));
        out.items.push_back(atom(node.inclusive ? "to" : "until", tone_t::KEYWORD));
        out.items.push_back(expr(node.end));
        if (node.step) {
            out.items.push_back(atom("step", tone_t::KEYWORD));
            out.items.push_back(expr(*node.step));
        }

        out.items.push_back(block(node.body));
        return out;
    }

    [[nodiscard]] node_t build(const for_in_stmt_t& node) const {
        node_t out = list("for-in");
        add_label(out, node.label);
        if (node.index) {
            out.items.push_back(atom(std::string(node.index->text), tone_t::NAME));
        }

        out.items.push_back(atom(std::string(node.value.text), tone_t::NAME));
        out.items.push_back(expr(node.sequence));
        out.items.push_back(block(node.body));
        return out;
    }

    [[nodiscard]] node_t build(const select_stmt_t& node) const {
        node_t out = list("select", { expr(node.subject) });
        for (const case_t& branch : node.cases) {
            node_t item = list("case");
            if (branch.match) {
                item.items.push_back(atom(std::string(branch.match->name.text), tone_t::NAME));
                item.items.push_back(atom("As", tone_t::KEYWORD));
                item.items.push_back(type(branch.match->type));
            }

            for (const case_value_t& value : branch.values) {
                if (value.range_end) {
                    item.items.push_back(
                        list("range", { expr(value.value), expr(*value.range_end) }));
                } else {
                    item.items.push_back(expr(value.value));
                }
            }

            item.items.push_back(block(branch.body));
            out.items.push_back(std::move(item));
        }

        if (node.default_body) {
            out.items.push_back(list("default", { block(*node.default_body) }));
        }

        return out;
    }

    static node_t build(const break_stmt_t& node) {
        node_t out = list("break");
        if (node.label) {
            out.items.push_back(atom(std::string(node.label->text), tone_t::NAME));
        }

        return out;
    }

    static node_t build(const continue_stmt_t& node) {
        node_t out = list("continue");
        if (node.label) {
            out.items.push_back(atom(std::string(node.label->text), tone_t::NAME));
        }

        return out;
    }

    [[nodiscard]] node_t build(const return_stmt_t& node) const {
        node_t out = list("return");
        if (node.value) {
            out.items.push_back(expr(*node.value));
        }

        return out;
    }

    [[nodiscard]] node_t build(const defer_stmt_t& node) const {
        return list("defer", { stmt(node.statement) });
    }

    /// "(enum State: UInt8 Idle (= Attack 10))", with "@Flags" before the name.
    [[nodiscard]] node_t build(const enum_stmt_t& node) const {
        node_t out = list("enum");
        if (node.flags) {
            out.items.push_back(atom("@Flags", tone_t::KEYWORD));
        }

        out.items.push_back(
            atom(std::format("{}{}", node.name.text, node.type ? ":" : ""), tone_t::NAME));
        if (node.type) {
            out.items.push_back(type(*node.type));
        }

        for (const enum_member_t& member : node.members) {
            node_t name = atom(std::string(member.name.text), tone_t::NAME);
            if (member.value) {
                out.items.push_back(list("=", { std::move(name), expr(*member.value) }));
            } else {
                out.items.push_back(std::move(name));
            }
        }

        return out;
    }

    /// "(struct Name (block members...))", or "(class ...)", with "Extends Base" after the name.
    /// In the tree, each member gets its own line.
    [[nodiscard]] node_t build(const type_stmt_t& node) const {
        node_t out = list(node.is_class ? "class" : "struct");
        add_modifiers(out, node.modifiers);
        out.items.push_back(atom(std::string(node.name.text), tone_t::NAME));
        if (node.base) {
            out.items.push_back(atom("Extends", tone_t::KEYWORD));
            out.items.push_back(atom(std::string(node.base->text), tone_t::NAME));
        }

        out.items.push_back(block(node.members));
        return out;
    }
};

/// Prints nodes on one line, or as an indented tree. With an analysis, expressions show their
/// types.
class renderer_t {
public:
    renderer_t(const style_t style, const analysis_t* analysis)
        : m_style(style), m_analysis(analysis) {}

    [[nodiscard]] std::string flat(const node_t& node) const {
        if (!node.list) {
            return m_style.paint(node.text, node.tone) + annotation(node);
        }

        std::string out = "(";
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            out += (i == 0 ? "" : " ");
            out += flat(node.items[i]);
        }

        return out + ")" + annotation(node);
    }

    /// Statements in a block go on their own lines, one level deeper. So does anything holding a
    /// block, like an "(else ...)", and the item after a block or one of those.
    [[nodiscard]] std::string tree(const node_t& node, const std::size_t depth) const {
        if (!has_block(node)) {
            return flat(node);
        }

        const std::string newline = "\n" + std::string((depth + 1) * 2, ' ');
        std::string out = "(";
        bool fresh_line = false;
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            const node_t& item = node.items[i];
            if (item.block) {
                // The statements go straight in, without the "block" head.
                for (std::size_t j = 1; j < item.items.size(); ++j) {
                    out += newline + tree(item.items[j], depth + 1);
                }

                fresh_line = true;
            } else if (fresh_line || has_block(item)) {
                out += newline + tree(item, depth + 1);
                fresh_line = has_block(item);
            } else {
                out += (i == 0 ? "" : " ");
                out += flat(item);
            }
        }

        return out + ")" + annotation(node);
    }

private:
    style_t m_style;
    const analysis_t* m_analysis;

    /// ": Int32", then " → Int64" for a conversion and " = 3" for a known value.
    [[nodiscard]] std::string annotation(const node_t& node) const {
        if (m_analysis == nullptr || !node.expr ||
            std::to_underlying(*node.expr) >= m_analysis->exprs.size()) {
            return "";
        }

        const expr_info_t& info = m_analysis->expr(*node.expr);
        // No type to show: an error, a function's name, or a call that returns nothing.
        if (info.type == type_id_t::INVALID || info.type == type_id_t::NONE) {
            return "";
        }

        std::string out = m_style.paint(":", tone_t::DIM) + " " +
                          m_style.paint(m_analysis->types.name(info.type), tone_t::TYPE);
        if (info.converted != info.type && info.converted != type_id_t::INVALID) {
            out += " → " + m_style.paint(m_analysis->types.name(info.converted), tone_t::TYPE);
        }

        // A literal already shows its value.
        if (info.value && node.tone != tone_t::LITERAL) {
            const std::string value = info.value->type == type_id_t::STRING
                                          ? quote(std::get<std::string>(info.value->data))
                                          : to_text(*info.value);
            out += " = " + m_style.paint(value, tone_t::LITERAL);
        }

        return out;
    }
};

/// The tree view, one top-level item per line. With an analysis, it's the types view.
std::string render_tree(const ast_t& ast, const style_t& style, const analysis_t* analysis) {
    const builder_t builder(ast, true);
    const renderer_t renderer(style, analysis);
    std::string out;
    for (const stmt_id_t id : ast.items()) {
        out += renderer.tree(builder.stmt(id), 0) + "\n";
    }

    return out;
}

// LLVM IR

/// Bare words that aren't keywords. Integer types like "i32" are recognized by their shape.
constexpr std::array<std::string_view, 4> llvm_types = { "void", "ptr", "float", "double" };
constexpr std::array<std::string_view, 3> llvm_constants = { "true", "false", "null" };

bool is_digit(const char c) {
    return c >= '0' && c <= '9';
}

/// Part of a word or a name. Names like "@fb.score" have dots in them.
bool is_word(const char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}

/// Types and constants by name. Any other bare word, like "define" or "add", is a keyword.
tone_t llvm_word_tone(const std::string_view word) {
    const bool integer_type =
        word.size() > 1 && word[0] == 'i' && std::ranges::all_of(word.substr(1), is_digit);
    if (integer_type || std::ranges::contains(llvm_types, word)) {
        return tone_t::TYPE;
    }

    if (std::ranges::contains(llvm_constants, word)) {
        return tone_t::LITERAL;
    }

    return tone_t::KEYWORD;
}

/// Where the token at `begin` ends, and its tone.
std::pair<std::size_t, tone_t> llvm_token(const std::string_view ir, const std::size_t begin) {
    const char c = ir[begin];
    const char next = begin + 1 < ir.size() ? ir[begin + 1] : '\0';
    const auto scan = [&](std::size_t end, const auto& matches) {
        while (end < ir.size() && matches(ir[end], ir[end - 1])) {
            ++end;
        }

        return end;
    };

    // A comment runs to the end of the line.
    if (c == ';') {
        return { std::min(ir.find('\n', begin), ir.size()), tone_t::DIM };
    }

    // A string, or a byte array like c"hi\00".
    if (c == '"' || (c == 'c' && next == '"')) {
        const std::size_t close = ir.find('"', ir.find('"', begin) + 1);
        return { std::min(close, ir.size() - 1) + 1, tone_t::LITERAL };
    }

    // Names like %x and @f stand out, and metadata like !0 and attribute groups like #0 don't. A
    // quoted name, like @"fb.Add(Int32, Int32)", ends where its string does.
    if (c == '%' || c == '@' || c == '!' || c == '#') {
        const tone_t tone = c == '%' || c == '@' ? tone_t::NAME : tone_t::DIM;
        if (next == '"') {
            return { llvm_token(ir, begin + 1).first, tone };
        }

        return { scan(begin + 1,
                      [](const char d, char /*previous*/) {
                          return is_word(d) || d == '-' || d == '$';
                      }),
                 tone };
    }

    if (is_digit(c) || (c == '-' && is_digit(next))) {
        // Hex and floats like "1.500000e+00" included.
        const auto number = [](const char d, const char previous) {
            return is_word(d) || ((d == '+' || d == '-') && previous == 'e');
        };
        return { scan(begin + 1, number), tone_t::LITERAL };
    }

    if (is_word(c) && c != '.') {
        const std::size_t end =
            scan(begin + 1, [](const char d, char /*previous*/) { return is_word(d); });
        return { end, llvm_word_tone(ir.substr(begin, end - begin)) };
    }

    return { begin + 1, tone_t::PLAIN };
}

} // namespace

std::string dump_tokens(const source_file_t& file, const std::span<const token_t> tokens,
                        const style_t& style) {
    // Measure the columns first, so they line up.
    std::vector<std::string> positions;
    std::size_t position_width = 0;
    std::size_t kind_width = 0;
    for (const token_t& token : tokens) {
        const location_t at = file.location_of(token.span.begin, column_unit_t::CODEPOINT);
        positions.push_back(std::format("{}:{}", at.line + 1, at.column + 1));
        position_width = std::max(position_width, positions.back().size());
        if (shows_text(token.kind)) {
            kind_width = std::max(kind_width, describe(token.kind).size());
        }
    }

    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const token_t& token = tokens[i];
        const std::string_view kind = describe(token.kind);
        out += style.paint(std::format("{:<{}}", positions[i], position_width), tone_t::DIM);
        out += "  " + style.paint(kind, token_tone(token.kind));
        if (shows_text(token.kind)) {
            out += std::string(kind_width - kind.size() + 2, ' ');
            out += escape_controls(file.slice(token.span));
        }

        out += '\n';
    }

    return out;
}

std::string dump_ast(const ast_t& ast) {
    const builder_t builder(ast, false);
    const renderer_t renderer(style_t(), nullptr);
    std::string out;
    for (const stmt_id_t id : ast.items()) {
        if (!out.empty()) {
            out += '\n';
        }

        out += renderer.flat(builder.stmt(id));
    }

    return out;
}

std::string dump_tree(const ast_t& ast, const style_t& style) {
    return render_tree(ast, style, nullptr);
}

std::string dump_types(const ast_t& ast, const analysis_t& analysis, const style_t& style) {
    return render_tree(ast, style, &analysis);
}

std::string highlight_llvm(const std::string_view ir, const style_t& style) {
    if (!style.color) {
        return std::string(ir);
    }

    std::string out;
    std::size_t begin = 0;
    while (begin < ir.size()) {
        auto [end, tone] = llvm_token(ir, begin);

        // A block's label, like "entry:" or "3:", starts its line.
        const bool line_start = begin == 0 || ir[begin - 1] == '\n';
        if (line_start && tone != tone_t::PLAIN && end < ir.size() && ir[end] == ':') {
            ++end;
            tone = tone_t::BOLD;
        }

        out += style.paint(ir.substr(begin, end - begin), tone);
        begin = end;
    }

    return out;
}

} // namespace fb
