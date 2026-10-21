#include "fb/ast.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
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

void ast_t::sort_items() {
    // A file the table doesn't have goes last, like a main module's.
    std::ranges::stable_sort(m_items, {}, [this](const stmt_id_t id) {
        const std::optional<module_id_t> module = m_modules.of(get(id).span.file);
        return module ? std::to_underlying(*module) : std::numeric_limits<std::uint32_t>::max();
    });
}

} // namespace fb
