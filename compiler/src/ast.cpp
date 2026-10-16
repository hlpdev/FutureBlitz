#include "fb/ast.hpp"

#include <cassert>
#include <utility>

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
    m_types.push_back(type);
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

} // namespace fb
