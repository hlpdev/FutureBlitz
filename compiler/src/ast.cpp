#include "fb/ast.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace fb {

std::string_view spelling(unary_op_t op) {
    switch (op) {
    case unary_op_t::NEGATE:
        return "-";
    case unary_op_t::NOT:
        return "!";
    case unary_op_t::BIT_NOT:
        return "~";
    }

    std::unreachable();
}

std::string_view spelling(binary_op_t op) {
    switch (op) {
    case binary_op_t::ADD:
        return "+";
    case binary_op_t::SUBTRACT:
        return "-";
    case binary_op_t::MULTIPLY:
        return "*";
    case binary_op_t::DIVIDE:
        return "/";
    case binary_op_t::REMAINDER:
        return "%";
    case binary_op_t::POWER:
        return "**";
    case binary_op_t::SHIFT_LEFT:
        return "<<";
    case binary_op_t::SHIFT_RIGHT:
        return ">>";
    case binary_op_t::BIT_AND:
        return "&";
    case binary_op_t::BIT_OR:
        return "|";
    case binary_op_t::BIT_XOR:
        return "^";
    case binary_op_t::EQUAL:
        return "==";
    case binary_op_t::NOT_EQUAL:
        return "!=";
    case binary_op_t::LESS:
        return "<";
    case binary_op_t::LESS_EQUAL:
        return "<=";
    case binary_op_t::GREATER:
        return ">";
    case binary_op_t::GREATER_EQUAL:
        return ">=";
    case binary_op_t::AND:
        return "&&";
    case binary_op_t::OR:
        return "||";
    case binary_op_t::COALESCE:
        return "??";
    }

    std::unreachable();
}

std::string_view spelling(const visibility_t visibility) {
    switch (visibility) {
    case visibility_t::PRIVATE:
        return "Private";
    case visibility_t::PROTECTED:
        return "Protected";
    case visibility_t::PACKAGE:
        return "Package";
    case visibility_t::PUBLIC:
        return "Public";
    }

    std::unreachable();
}

expr_id_t ast_t::add(expr_t expr) {
    const auto id = static_cast<expr_id_t>(m_exprs.size());
    m_exprs.push_back(std::move(expr));
    return id;
}

stmt_id_t ast_t::add(stmt_t stmt) {
    const auto id = static_cast<stmt_id_t>(m_stmts.size());
    m_stmts.push_back(std::move(stmt));
    return id;
}

type_expr_id_t ast_t::add(type_expr_t type) {
    const auto id = static_cast<type_expr_id_t>(m_types.size());
    m_types.push_back(std::move(type));
    return id;
}

const expr_t& ast_t::get(const expr_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_exprs.size());
    return m_exprs[index];
}

const stmt_t& ast_t::get(const stmt_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_stmts.size());
    return m_stmts[index];
}

const type_expr_t& ast_t::get(const type_expr_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_types.size());
    return m_types[index];
}

std::optional<name_t> declared_name(const stmt_node_t& node) {
    return std::visit(
        [](const auto& declared) -> std::optional<name_t> {
            using node_t = std::decay_t<decltype(declared)>;
            if constexpr (std::is_same_v<node_t, function_stmt_t> ||
                          std::is_same_v<node_t, type_stmt_t> ||
                          std::is_same_v<node_t, enum_stmt_t> ||
                          std::is_same_v<node_t, variable_stmt_t> ||
                          std::is_same_v<node_t, property_stmt_t> ||
                          std::is_same_v<node_t, alias_stmt_t>) {
                return declared.name;
            } else {
                return std::nullopt;
            }
        },
        node);
}

namespace {

/// A doc comment's key: where the name it's on starts, its file in the high 32 bits.
std::uint64_t doc_key(const span_t name) {
    return (std::uint64_t{ std::to_underlying(name.file) } << 32U) | name.begin;
}

} // namespace

void ast_t::set_doc(const span_t name, doc_comment_t doc) {
    m_docs.insert_or_assign(doc_key(name), std::move(doc));
}

const doc_comment_t* ast_t::doc(const span_t name) const {
    const auto found = m_docs.find(doc_key(name));
    return found != m_docs.end() ? &found->second : nullptr;
}

const doc_comment_t* ast_t::doc(const stmt_id_t id) const {
    const std::optional<name_t> name = declared_name(get(id).node);
    return name ? doc(name->span) : nullptr;
}

std::vector<const doc_comment_t*> ast_t::docs_in(const file_id_t file) const {
    std::vector<const doc_comment_t*> found;
    for (const auto& [key, doc] : m_docs) {
        if (key >> 32U == std::to_underlying(file)) {
            found.push_back(&doc);
        }
    }

    return found;
}

void ast_t::set_module_doc(const file_id_t file, doc_comment_t doc) {
    m_module_docs.insert_or_assign(file, std::move(doc));
}

const doc_comment_t* ast_t::module_doc(const file_id_t file) const {
    const auto found = m_module_docs.find(file);
    return found != m_module_docs.end() ? &found->second : nullptr;
}

void ast_t::sort_items() {
    // A file the table doesn't have goes last, like a main module's.
    std::ranges::stable_sort(m_items, {}, [this](const stmt_id_t id) {
        const std::optional<module_id_t> module = m_modules.of(get(id).span.file);
        return module ? std::to_underlying(*module) : std::numeric_limits<std::uint32_t>::max();
    });
}

} // namespace fb
