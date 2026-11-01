#include "fb/lsp/refactor.hpp"

#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/lint.hpp"
#include "fb/lsp/query.hpp"
#include "fb/module.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fb::lsp {
namespace {

template <typename Id>
[[nodiscard]] std::size_t index_of(const Id id) {
    return static_cast<std::size_t>(std::to_underlying(id));
}

[[nodiscard]] std::string_view text_of(const query_t& query, const span_t span) {
    return query.compilation().sources.get(span.file).text().substr(span.begin, span.length());
}

[[nodiscard]] const expr_info_t& info(const query_t& query, const expr_id_t id) {
    return query.analysis().exprs[index_of(id)];
}

[[nodiscard]] const symbol_t& symbol_of(const query_t& query, const symbol_id_t id) {
    return query.analysis().symbol(id);
}

[[nodiscard]] span_t span_of(const query_t& query, const expr_id_t id) {
    return query.ast().get(id).span;
}

[[nodiscard]] bool within(const span_t inner, const span_t outer) {
    return inner.file == outer.file && outer.begin <= inner.begin && inner.end <= outer.end;
}

/// Where the line `at` is on starts.
[[nodiscard]] std::uint32_t line_begin(const std::string_view text, const std::uint32_t at) {
    const std::size_t found = at == 0 ? std::string_view::npos : text.rfind('\n', at - 1);
    return found == std::string_view::npos ? 0 : static_cast<std::uint32_t>(found + 1);
}

/// Where the line after the one `at` is on starts, or the text's end.
[[nodiscard]] std::uint32_t next_line(const std::string_view text, const std::uint32_t at) {
    const std::size_t found = text.find('\n', at);
    return found == std::string_view::npos ? static_cast<std::uint32_t>(text.size())
                                           : static_cast<std::uint32_t>(found + 1);
}

/// The spaces and tabs the line `at` is on starts with.
[[nodiscard]] std::string_view indentation(const std::string_view text, const std::uint32_t at) {
    const std::uint32_t begin = line_begin(text, at);
    const std::size_t end = text.find_first_not_of(" \t", begin);
    return text.substr(begin, (end == std::string_view::npos ? text.size() : end) - begin);
}

/// Whether only spaces and tabs come before `at` on its line.
[[nodiscard]] bool starts_line(const std::string_view text, const std::uint32_t at) {
    return indentation(text, at).size() == at - line_begin(text, at);
}

[[nodiscard]] std::string_view line_break(const std::string_view text) {
    return text.contains("\r\n") ? "\r\n" : "\n";
}

/// What a file's code indents a block by: a tab, or its smallest indentation in spaces.
[[nodiscard]] std::string indent_unit(const std::string_view text) {
    std::size_t smallest = 0;
    for (std::uint32_t at = 0; at < text.size(); at = next_line(text, at)) {
        if (text[at] == '\t') {
            return "\t";
        }

        const std::size_t spaces = text.find_first_not_of(' ', at) - at;
        if (spaces > 0 && at + spaces < text.size() && text[at + spaces] != '\n' &&
            text[at + spaces] != '\r' && (smallest == 0 || spaces < smallest)) {
            smallest = spaces;
        }
    }

    std::string unit;
    unit.assign(smallest == 0 ? 4 : smallest, ' ');
    return unit;
}

[[nodiscard]] bool identifier_char(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// Whether `word` is in `text` as a word of its own.
[[nodiscard]] bool has_word(const std::string_view text, const std::string_view word) {
    for (std::size_t at = text.find(word); at != std::string_view::npos;
         at = text.find(word, at + 1)) {
        const bool before = at > 0 && identifier_char(text[at - 1]);
        const bool after =
            at + word.size() < text.size() && identifier_char(text[at + word.size()]);
        if (!before && !after) {
            return true;
        }
    }

    return false;
}

/// `base`, or `base2`, `base3`, and on, the first that `text` doesn't have.
[[nodiscard]] std::string unused_name(const std::string_view text, const std::string_view base) {
    std::string name(base);
    for (int n = 2; has_word(text, name); ++n) {
        name = std::format("{}{}", base, n);
    }

    return name;
}

/// The innermost statement of the query's file that has all of `span`.
[[nodiscard]] std::optional<stmt_id_t> statement_around(const query_t& query, const span_t span) {
    std::optional<stmt_id_t> best;
    for (std::size_t i = 0; i < query.ast().stmt_count(); ++i) {
        const auto id = static_cast<stmt_id_t>(i);
        const span_t around = query.ast().get(id).span;
        if (within(span, around) &&
            (!best || around.length() < query.ast().get(*best).span.length())) {
            best = id;
        }
    }

    return best;
}

/// The innermost statement other than `statement` that has all of it, or nullopt at the top
/// level.
[[nodiscard]] std::optional<stmt_id_t> parent_of(const query_t& query, const stmt_id_t statement) {
    const span_t span = query.ast().get(statement).span;
    std::optional<stmt_id_t> best;
    for (std::size_t i = 0; i < query.ast().stmt_count(); ++i) {
        const auto id = static_cast<stmt_id_t>(i);
        const span_t around = query.ast().get(id).span;
        if (id != statement && within(span, around) && around != span &&
            (!best || around.length() < query.ast().get(*best).span.length())) {
            best = id;
        }
    }

    return best;
}

/// The innermost Function, Method, or New whose declaration has `span`.
[[nodiscard]] std::optional<symbol_id_t> function_around(const query_t& query, const span_t span) {
    std::optional<symbol_id_t> best;
    std::uint32_t length = UINT32_MAX;
    for (const symbol_id_t function : query.analysis().functions) {
        const symbol_t& declared = symbol_of(query, function);
        if (!declared.declaration) {
            continue;
        }

        const span_t around = query.ast().get(*declared.declaration).span;
        if (within(span, around) && around.length() < length) {
            best = function;
            length = around.length();
        }
    }

    return best;
}

/// The expressions of the query's check in `span`.
[[nodiscard]] std::vector<expr_id_t> exprs_in(const query_t& query, const span_t span) {
    std::vector<expr_id_t> found;
    for (std::size_t i = 0; i < query.ast().expr_count(); ++i) {
        const auto id = static_cast<expr_id_t>(i);
        if (within(span_of(query, id), span)) {
            found.push_back(id);
        }
    }

    return found;
}

/// The statements of the query's check in `span`.
[[nodiscard]] std::vector<stmt_id_t> statements_in(const query_t& query, const span_t span) {
    std::vector<stmt_id_t> found;
    for (std::size_t i = 0; i < query.ast().stmt_count(); ++i) {
        const auto id = static_cast<stmt_id_t>(i);
        if (within(query.ast().get(id).span, span)) {
            found.push_back(id);
        }
    }

    return found;
}

/// Whether working out an expression may do what another place's order matters to: a call, an
/// Await, an index or a slice, which may call an Operator or panic, a Property's Get, an
/// operator or conversion an Operator or New does, an interpolation's ToString, or a ThreadLocal
/// Global's read, which runs its initializer.
[[nodiscard]] bool acts(const query_t& query, const expr_id_t id) {
    const expr_node_t& node = query.ast().get(id).node;
    const expr_info_t& found = info(query, id);
    if (std::holds_alternative<call_expr_t>(node) || std::holds_alternative<await_expr_t>(node) ||
        std::holds_alternative<index_expr_t>(node) || std::holds_alternative<slice_expr_t>(node)) {
        return true;
    }

    if (const auto* string = std::get_if<string_expr_t>(&node)) {
        return std::ranges::any_of(string->parts, [](const string_part_t& part) {
            return std::holds_alternative<interpolation_t>(part);
        });
    }

    if (std::holds_alternative<member_expr_t>(node) || std::holds_alternative<name_expr_t>(node)) {
        if (!found.symbol) {
            return false;
        }

        const symbol_t& named = symbol_of(query, *found.symbol);
        return named.kind == symbol_kind_t::PROPERTY ||
               (named.kind == symbol_kind_t::GLOBAL && named.modifiers.is_thread_local);
    }

    return (std::holds_alternative<unary_expr_t>(node) ||
            std::holds_alternative<binary_expr_t>(node) ||
            std::holds_alternative<cast_expr_t>(node)) &&
           found.symbol.has_value();
}

/// Whether an expression, or a part of it, acts.
[[nodiscard]] bool acts_within(const query_t& query, const expr_id_t id) {
    return std::ranges::any_of(exprs_in(query, span_of(query, id)),
                               [&](const expr_id_t part) { return acts(query, part); });
}

/// Whether an expression gives the same wherever and however often it's worked out, doing
/// nothing else: a literal, a Local, parameter, loop variable, or Const that `stable` says
/// nothing changes, and operators on those that can't trap, as integer arithmetic can.
[[nodiscard]] bool steady(const query_t& query, const expr_id_t id,
                          const std::function<bool(symbol_id_t)>& stable) {
    const expr_node_t& node = query.ast().get(id).node;
    const expr_info_t& found = info(query, id);
    if (std::holds_alternative<integer_expr_t>(node) ||
        std::holds_alternative<decimal_expr_t>(node) ||
        std::holds_alternative<character_expr_t>(node) ||
        std::holds_alternative<bool_expr_t>(node) || std::holds_alternative<null_expr_t>(node)) {
        return true;
    }

    if (const auto* string = std::get_if<string_expr_t>(&node)) {
        return std::ranges::all_of(string->parts, [](const string_part_t& part) {
            return std::holds_alternative<std::string>(part);
        });
    }

    if (std::holds_alternative<name_expr_t>(node)) {
        if (!found.symbol) {
            return false;
        }

        const symbol_t& named = symbol_of(query, *found.symbol);
        const bool variable =
            named.kind == symbol_kind_t::LOCAL || named.kind == symbol_kind_t::PARAMETER ||
            named.kind == symbol_kind_t::LOOP_VARIABLE || named.kind == symbol_kind_t::CONSTANT;
        return variable && !named.by_ref && stable(*found.symbol);
    }

    if (const auto* unary = std::get_if<unary_expr_t>(&node)) {
        return !found.symbol && (unary->op != unary_op_t::NEGATE || !is_integer(found.type)) &&
               steady(query, unary->operand, stable);
    }

    if (const auto* binary = std::get_if<binary_expr_t>(&node)) {
        bool safe = false;
        switch (binary->op) {
        case binary_op_t::ADD:
        case binary_op_t::SUBTRACT:
        case binary_op_t::MULTIPLY:
        case binary_op_t::DIVIDE:
        case binary_op_t::POWER:
            safe = !is_integer(info(query, binary->left).type) &&
                   !is_integer(info(query, binary->right).type);
            break;
        case binary_op_t::REMAINDER:
        case binary_op_t::SHIFT_LEFT:
        case binary_op_t::SHIFT_RIGHT:
            safe = false;
            break;
        default:
            safe = true;
            break;
        }

        return safe && !found.symbol && steady(query, binary->left, stable) &&
               steady(query, binary->right, stable);
    }

    if (const auto* ternary = std::get_if<ternary_expr_t>(&node)) {
        return steady(query, ternary->condition, stable) &&
               steady(query, ternary->if_true, stable) && steady(query, ternary->if_false, stable);
    }

    if (const auto* tuple = std::get_if<tuple_expr_t>(&node)) {
        return std::ranges::all_of(tuple->elements, [&](const expr_id_t element) {
            return steady(query, element, stable);
        });
    }

    return false;
}

/// The variables code in `span` sets: by a plain or compound assignment, ++ or --, or passing
/// it ByRef.
[[nodiscard]] std::set<symbol_id_t> assigned_in(const query_t& query, const span_t span) {
    std::set<symbol_id_t> found;
    const auto named = [&](const expr_id_t target) {
        if (std::holds_alternative<name_expr_t>(query.ast().get(target).node)) {
            if (const std::optional<symbol_id_t> id = info(query, target).symbol) {
                found.insert(*id);
            }
        } else if (const auto* tuple = std::get_if<tuple_expr_t>(&query.ast().get(target).node)) {
            for (const expr_id_t element : tuple->elements) {
                if (const std::optional<symbol_id_t> id = info(query, element).symbol) {
                    found.insert(*id);
                }
            }
        }
    };
    for (const stmt_id_t statement : statements_in(query, span)) {
        const stmt_node_t& node = query.ast().get(statement).node;
        if (const auto* assigned = std::get_if<assign_stmt_t>(&node)) {
            named(assigned->target);
        } else if (const auto* increment = std::get_if<increment_stmt_t>(&node)) {
            named(increment->target);
        }
    }

    for (const expr_id_t id : exprs_in(query, span)) {
        if (const auto* call = std::get_if<call_expr_t>(&query.ast().get(id).node)) {
            for (const argument_t& argument : call->arguments) {
                if (argument.by_ref) {
                    named(argument.value);
                }
            }
        }
    }

    return found;
}

/// The variable a member or an index is of, as `c` is `c.items[0].count`'s, or nullopt.
[[nodiscard]] std::optional<symbol_id_t> root_of(const query_t& query, expr_id_t id) {
    while (true) {
        const expr_node_t& node = query.ast().get(id).node;
        if (const auto* member = std::get_if<member_expr_t>(&node)) {
            id = member->object;
        } else if (const auto* indexed = std::get_if<index_expr_t>(&node)) {
            id = indexed->object;
        } else {
            break;
        }
    }

    return std::holds_alternative<name_expr_t>(query.ast().get(id).node) ? info(query, id).symbol
                                                                         : std::nullopt;
}

/// Whether a call is of a ByRef Method, which can change the Struct it's called on.
[[nodiscard]] bool by_ref_method(const query_t& query, const expr_id_t call) {
    const auto* node = std::get_if<call_expr_t>(&query.ast().get(call).node);
    const std::optional<symbol_id_t> called = info(query, call).symbol;
    return node != nullptr &&
           std::holds_alternative<member_expr_t>(query.ast().get(node->callee).node) && called &&
           symbol_of(query, *called).kind == symbol_kind_t::METHOD &&
           symbol_of(query, *called).by_ref;
}

/// The variables code in `span` may change through what it passes ByRef, a member of one among
/// them, or calls a ByRef Method on.
[[nodiscard]] std::set<symbol_id_t> passed_in(const query_t& query, const span_t span) {
    std::set<symbol_id_t> found;
    for (const expr_id_t id : exprs_in(query, span)) {
        const auto* call = std::get_if<call_expr_t>(&query.ast().get(id).node);
        if (call == nullptr) {
            continue;
        }

        for (const argument_t& argument : call->arguments) {
            if (const std::optional<symbol_id_t> root = root_of(query, argument.value);
                root && argument.by_ref) {
                found.insert(*root);
            }
        }

        if (by_ref_method(query, id)) {
            const auto& member = std::get<member_expr_t>(query.ast().get(call->callee).node);
            if (const std::optional<symbol_id_t> root = root_of(query, member.object)) {
                found.insert(*root);
            }
        }
    }

    return found;
}

/// The variables code in `span` changes in any way: those assigned_in() and passed_in() find, and
/// those a member or an index of is set.
[[nodiscard]] std::set<symbol_id_t> changed_in(const query_t& query, const span_t span) {
    std::set<symbol_id_t> found = assigned_in(query, span);
    found.merge(passed_in(query, span));
    for (const stmt_id_t statement : statements_in(query, span)) {
        const stmt_node_t& node = query.ast().get(statement).node;
        std::optional<expr_id_t> target;
        if (const auto* assigned = std::get_if<assign_stmt_t>(&node)) {
            target = assigned->target;
        } else if (const auto* increment = std::get_if<increment_stmt_t>(&node)) {
            target = increment->target;
        }

        if (const std::optional<symbol_id_t> root =
                target ? root_of(query, *target) : std::nullopt) {
            found.insert(*root);
        }
    }

    return found;
}

/// Whether an expression in `statement`'s span, before `span`, may act, so moving `span`'s code
/// before the statement would change their order.
[[nodiscard]] bool acts_before(const query_t& query, const span_t statement, const span_t span) {
    return std::ranges::any_of(exprs_in(query, statement), [&](const expr_id_t id) {
        const span_t other = span_of(query, id);
        return other.end <= span.begin && acts(query, id);
    });
}

/// Whether `span` is worked out only sometimes, or later, inside what has it in `statement`:
/// the right of `&&`, `||`, or `??`, a branch of `?:`, a lambda's code, or the arguments of a
/// call after `?.`.
[[nodiscard]] bool conditional(const query_t& query, const span_t statement, const span_t span) {
    return std::ranges::any_of(exprs_in(query, statement), [&](const expr_id_t id) {
        const span_t around = span_of(query, id);
        if (!within(span, around) || around == span) {
            return false;
        }

        const expr_node_t& node = query.ast().get(id).node;
        if (const auto* binary = std::get_if<binary_expr_t>(&node)) {
            return (binary->op == binary_op_t::AND || binary->op == binary_op_t::OR ||
                    binary->op == binary_op_t::COALESCE) &&
                   within(span, span_of(query, binary->right));
        }

        if (const auto* ternary = std::get_if<ternary_expr_t>(&node)) {
            return !within(span, span_of(query, ternary->condition));
        }

        if (const auto* call = std::get_if<call_expr_t>(&node)) {
            const auto* member = std::get_if<member_expr_t>(&query.ast().get(call->callee).node);
            return member != nullptr && member->null_safe &&
                   !within(span, span_of(query, call->callee));
        }

        return std::holds_alternative<lambda_expr_t>(node);
    });
}

/// Whether a literal or the like takes its type from where it is, so written elsewhere it needs
/// an As to keep `type`: a number whose type isn't its own, Null, an array, a map, or a lambda.
[[nodiscard]] bool needs_as(const query_t& query, const expr_id_t id, const type_id_t type) {
    const expr_node_t& node = query.ast().get(id).node;
    if (std::holds_alternative<integer_expr_t>(node)) {
        return type != type_id_t::INT32;
    }

    if (std::holds_alternative<decimal_expr_t>(node)) {
        return type != type_id_t::FLOAT64;
    }

    if (const auto* unary = std::get_if<unary_expr_t>(&node);
        unary != nullptr && unary->op == unary_op_t::NEGATE) {
        return needs_as(query, unary->operand, type);
    }

    return std::holds_alternative<null_expr_t>(node) ||
           std::holds_alternative<array_expr_t>(node) || std::holds_alternative<map_expr_t>(node) ||
           std::holds_alternative<lambda_expr_t>(node) || info(query, id).type != type;
}

/// An expression's text where another goes, in parentheses unless it's one piece.
[[nodiscard]] std::string placed(const query_t& query, const expr_id_t id, std::string text) {
    const expr_node_t& node = query.ast().get(id).node;
    const bool whole =
        std::holds_alternative<name_expr_t>(node) || std::holds_alternative<call_expr_t>(node) ||
        std::holds_alternative<member_expr_t>(node) || std::holds_alternative<index_expr_t>(node) ||
        std::holds_alternative<integer_expr_t>(node) ||
        std::holds_alternative<decimal_expr_t>(node) ||
        std::holds_alternative<string_expr_t>(node) ||
        std::holds_alternative<character_expr_t>(node) ||
        std::holds_alternative<bool_expr_t>(node) || std::holds_alternative<self_expr_t>(node) ||
        std::holds_alternative<tuple_expr_t>(node) || std::holds_alternative<array_expr_t>(node);
    return whole ? std::move(text) : std::format("({})", text);
}

/// The text of the whole lines from the one `begin` is on through the one `end` is on, with
/// the line break after them.
[[nodiscard]] span_t whole_lines(const query_t& query, const span_t span) {
    const std::string_view text = query.compilation().sources.get(span.file).text();
    return {
        .file = span.file,
        .begin = line_begin(text, span.begin),
        .end = next_line(text, span.end == 0 ? 0 : span.end - 1),
    };
}

/// A declaration's lines, with the `///` comment before it.
[[nodiscard]] span_t declaration_lines(const query_t& query, const stmt_id_t statement) {
    const std::string_view text = query.source().text();
    span_t lines = whole_lines(query, {
                                          .file = query.file(),
                                          .begin = declaration_begin(query.ast().get(statement)),
                                          .end = query.ast().get(statement).span.end,
                                      });
    while (lines.begin > 0) {
        const std::uint32_t before = line_begin(text, lines.begin - 1);
        if (!text.substr(before).substr(indentation(text, before).size()).starts_with("///")) {
            break;
        }

        lines.begin = before;
    }

    return lines;
}

// Extract a Local

std::optional<refactoring_t> extract_local(const query_t& query, const span_t selection) {
    const std::string_view text = query.source().text();
    // The one expression that's the selection, its outermost.
    std::optional<expr_id_t> chosen;
    for (const expr_id_t id : exprs_in(query, selection)) {
        if (span_of(query, id) == selection &&
            (!chosen || within(span_of(query, *chosen), span_of(query, id)))) {
            chosen = id;
        }
    }

    if (!chosen) {
        return std::nullopt;
    }

    const expr_id_t value = *chosen;
    const type_id_t type = info(query, value).type;
    if (type == type_id_t::INVALID || type == type_id_t::NONE ||
        std::holds_alternative<name_expr_t>(query.ast().get(value).node) ||
        !assigned_in(query, selection).empty()) {
        return std::nullopt;
    }

    const std::optional<stmt_id_t> statement = statement_around(query, selection);
    if (!statement) {
        return std::nullopt;
    }

    // What's passed ByRef, or a ByRef Method is called on, or a part of either, is a place, not a
    // value: a copy of it would be what changes.
    for (const expr_id_t id : exprs_in(query, query.ast().get(*statement).span)) {
        const auto* call = std::get_if<call_expr_t>(&query.ast().get(id).node);
        if (call == nullptr) {
            continue;
        }

        for (const argument_t& argument : call->arguments) {
            if (argument.by_ref && within(selection, span_of(query, argument.value))) {
                return std::nullopt;
            }
        }

        if (by_ref_method(query, id) &&
            within(selection,
                   span_of(query,
                           std::get<member_expr_t>(query.ast().get(call->callee).node).object))) {
            return std::nullopt;
        }
    }

    // What's set isn't a value to work out first.
    for (const stmt_id_t id : statements_in(query, query.ast().get(*statement).span)) {
        const stmt_node_t& setting = query.ast().get(id).node;
        const auto* assigned = std::get_if<assign_stmt_t>(&setting);
        const auto* increment = std::get_if<increment_stmt_t>(&setting);
        std::optional<expr_id_t> target;
        if (assigned != nullptr) {
            target = assigned->target;
        } else if (increment != nullptr) {
            target = increment->target;
        }

        if (target && within(selection, span_of(query, *target))) {
            return std::nullopt;
        }
    }

    // Where its statement works it out once, first, before the rest of it.
    const stmt_t& around = query.ast().get(*statement);
    const stmt_node_t& node = around.node;
    bool once =
        std::holds_alternative<expression_stmt_t>(node) ||
        std::holds_alternative<assign_stmt_t>(node) ||
        std::holds_alternative<increment_stmt_t>(node) ||
        std::holds_alternative<return_stmt_t>(node) || std::holds_alternative<throw_stmt_t>(node) ||
        std::holds_alternative<destructure_stmt_t>(node) ||
        std::holds_alternative<for_stmt_t>(node) || std::holds_alternative<for_in_stmt_t>(node) ||
        std::holds_alternative<lock_stmt_t>(node);
    if (const auto* variable = std::get_if<variable_stmt_t>(&node)) {
        once = variable->kind == variable_kind_t::LOCAL ||
               (variable->kind == variable_kind_t::CONSTANT && function_around(query, around.span));
    } else if (const auto* branches = std::get_if<if_stmt_t>(&node)) {
        once = !branches->single_line &&
               within(selection, span_of(query, branches->branches.front().condition));
    } else if (const auto* select = std::get_if<select_stmt_t>(&node)) {
        once = within(selection, span_of(query, select->subject));
    }

    // A loop's assignments to its own variable come before the body: the For's bounds are
    // before the variable is.
    const std::optional<stmt_id_t> parent = parent_of(query, *statement);
    const bool single_line_body =
        parent && std::holds_alternative<if_stmt_t>(query.ast().get(*parent).node) &&
        std::get<if_stmt_t>(query.ast().get(*parent).node).single_line;
    if (!once || single_line_body || !starts_line(text, around.span.begin) ||
        conditional(query, around.span, selection) || acts_before(query, around.span, selection)) {
        return std::nullopt;
    }

    const std::string name = unused_name(text, "value");
    const bool alone = std::holds_alternative<member_expr_t>(query.ast().get(value).node) ||
                       std::holds_alternative<string_expr_t>(query.ast().get(value).node) ||
                       std::holds_alternative<bool_expr_t>(query.ast().get(value).node) ||
                       std::holds_alternative<character_expr_t>(query.ast().get(value).node) ||
                       (std::holds_alternative<call_expr_t>(query.ast().get(value).node) &&
                        info(query, value).type_arguments.empty()) ||
                       !needs_as(query, value, type);
    const std::string written = alone ? std::string() : std::format(": {}", query.type_name(type));
    return refactoring_t{
        .title = "Extract to a Local",
        .kind = "refactor.extract",
        .edits = {
            {
                .span = { .file = query.file(),
                          .begin = line_begin(text, around.span.begin),
                          .end = line_begin(text, around.span.begin), },
                .replacement = std::format("{}Local {}{} = {}{}", indentation(text, around.span.begin),
                                           name, written, text_of(query, selection),
                                           line_break(text)),
            },
            { .span = selection, .replacement = name },
        },
    };
}

// Extract a Function or Method

/// Why the statements in `range` can't be a function of their own: what leaves them, or what
/// they'd lose by moving.
[[nodiscard]] bool movable(const query_t& query, const span_t range) {
    for (const stmt_id_t id : statements_in(query, range)) {
        const stmt_node_t& node = query.ast().get(id).node;
        if (std::holds_alternative<return_stmt_t>(node) ||
            std::holds_alternative<defer_stmt_t>(node) ||
            std::holds_alternative<unsafe_stmt_t>(node) ||
            std::holds_alternative<function_stmt_t>(node) ||
            std::holds_alternative<type_stmt_t>(node)) {
            return false;
        }

        // A Throw or a Break or Continue that goes out of the code.
        const bool leaves = std::holds_alternative<throw_stmt_t>(node) ||
                            std::holds_alternative<break_stmt_t>(node) ||
                            std::holds_alternative<continue_stmt_t>(node);
        if (!leaves) {
            continue;
        }

        const auto labeled = [&]<typename T>(const T* statement) {
            return statement != nullptr && statement->label.has_value();
        };
        if (labeled(std::get_if<break_stmt_t>(&node)) ||
            labeled(std::get_if<continue_stmt_t>(&node))) {
            return false;
        }

        const bool kept =
            std::ranges::any_of(statements_in(query, range), [&](const stmt_id_t other) {
                const stmt_node_t& outer = query.ast().get(other).node;
                const bool holds =
                    within(query.ast().get(id).span, query.ast().get(other).span) && other != id;
                // A Throw is caught in a Try's body with a Catch for every error, not in its
                // Catches or its Finally.
                if (std::holds_alternative<throw_stmt_t>(node)) {
                    const auto* tried = std::get_if<try_stmt_t>(&outer);
                    return holds && tried != nullptr &&
                           std::ranges::any_of(tried->catches,
                                               [](const catch_t& each) { return !each.type; }) &&
                           std::ranges::any_of(tried->body, [&](const stmt_id_t part) {
                               return within(query.ast().get(id).span, query.ast().get(part).span);
                           });
                }

                return holds && (std::holds_alternative<while_stmt_t>(outer) ||
                                 std::holds_alternative<repeat_stmt_t>(outer) ||
                                 std::holds_alternative<loop_stmt_t>(outer) ||
                                 std::holds_alternative<for_stmt_t>(outer) ||
                                 std::holds_alternative<for_in_stmt_t>(outer) ||
                                 std::holds_alternative<for_member_stmt_t>(outer));
            });
        if (!kept) {
            return false;
        }
    }

    return std::ranges::none_of(exprs_in(query, range), [&](const expr_id_t id) {
        const expr_node_t& node = query.ast().get(id).node;
        const auto* tried = std::get_if<try_expr_t>(&node);
        return std::holds_alternative<await_expr_t>(node) ||
               std::holds_alternative<super_expr_t>(node) || (tried != nullptr && !tried->optional);
    });
}

std::vector<refactoring_t> extract_function(const query_t& query, const span_t selection) {
    const std::string_view text = query.source().text();
    // The statements the selection covers, each whole, side by side in one block.
    std::vector<stmt_id_t> covered;
    for (const stmt_id_t id : statements_in(query, selection)) {
        const bool outermost =
            std::ranges::none_of(statements_in(query, selection), [&](const stmt_id_t other) {
                return other != id &&
                       within(query.ast().get(id).span, query.ast().get(other).span) &&
                       query.ast().get(other).span != query.ast().get(id).span;
            });
        if (outermost) {
            covered.push_back(id);
        }
    }

    if (covered.empty()) {
        return {};
    }

    std::ranges::sort(covered, {},
                      [&](const stmt_id_t id) { return query.ast().get(id).span.begin; });
    const span_t first = query.ast().get(covered.front()).span;
    const span_t last = query.ast().get(covered.back()).span;
    const span_t range{ .file = query.file(), .begin = first.begin, .end = last.end };
    const std::optional<stmt_id_t> parent = parent_of(query, covered.front());
    const bool one_block = std::ranges::all_of(covered, [&](const stmt_id_t id) {
        return parent_of(query, id) == parent && starts_line(text, query.ast().get(id).span.begin);
    });
    // No statement is partly in it, and it's only spaces around them.
    const bool whole = std::ranges::all_of(
        std::views::iota(std::size_t{ 0 }, query.ast().stmt_count()), [&](const std::size_t i) {
            const span_t statement = query.ast().get(static_cast<stmt_id_t>(i)).span;
            const bool overlaps = statement.file == range.file && statement.begin < range.end &&
                                  range.begin < statement.end;
            return !overlaps || within(statement, range) || within(range, statement);
        });
    const auto blank = [&](const std::uint32_t from, const std::uint32_t to) {
        return from >= to ||
               text.substr(from, to - from).find_first_not_of(" \t\r\n") == std::string_view::npos;
    };
    if (!one_block || !whole || !blank(selection.begin, range.begin) ||
        !blank(range.end, selection.end) || !movable(query, range)) {
        return {};
    }

    if (parent) {
        if (const auto* branches = std::get_if<if_stmt_t>(&query.ast().get(*parent).node);
            branches != nullptr && branches->single_line) {
            return {};
        }
    }

    // What it's in: a function, or the file's top-level code, whose own variables it gets as
    // parameters.
    const std::optional<symbol_id_t> function = function_around(query, range);
    const span_t scope =
        function
            ? query.ast().get(symbol_of(query, *function).declaration.value_or(stmt_id_t{})).span
            : span_t{
                  .file = query.file(),
                  .begin = 0,
                  .end = static_cast<std::uint32_t>(text.size()),
              };
    const symbol_t* enclosing = function ? &symbol_of(query, *function) : nullptr;
    if (enclosing != nullptr) {
        const auto* declared =
            std::get_if<function_stmt_t>(&query.ast().get(*enclosing->declaration).node);
        const bool generic_owner =
            enclosing->owner != type_id_t::INVALID &&
            (!query.analysis().types.get(enclosing->owner).parameters.empty() ||
             !query.analysis().types.get(enclosing->owner).arguments.empty());
        if (declared == nullptr || !declared->type_parameters.empty() || generic_owner ||
            declared->modifiers.is_unsafe || declared->modifiers.is_async || enclosing->by_ref) {
            return {};
        }
    }

    // A lambda's code it's in would leave the lambda.
    for (std::size_t i = 0; i < query.ast().expr_count(); ++i) {
        const auto id = static_cast<expr_id_t>(i);
        if (std::holds_alternative<lambda_expr_t>(query.ast().get(id).node) &&
            within(range, span_of(query, id)) && within(span_of(query, id), scope)) {
            return {};
        }
    }

    const auto local_to_scope = [&](const symbol_t& variable) {
        const bool kind = variable.kind == symbol_kind_t::LOCAL ||
                          variable.kind == symbol_kind_t::PARAMETER ||
                          variable.kind == symbol_kind_t::LOOP_VARIABLE ||
                          (variable.kind == symbol_kind_t::CONSTANT && variable.declaration &&
                           function_around(query, variable.span));
        if (!kind || variable.span.empty() || !within(variable.span, scope)) {
            return false;
        }

        // At the top level, a Local of the file's code, not one of a function's.
        return function.has_value() || !function_around(query, variable.span);
    };

    std::vector<symbol_id_t> inputs;
    std::vector<symbol_id_t> declared_inside;
    bool self = false;
    const std::vector<named_t> names = query.names();
    for (const named_t& named : names) {
        if (!within(named.span, range)) {
            continue;
        }

        const symbol_t& used = symbol_of(query, named.symbol);
        if (enclosing != nullptr && enclosing->self && named.symbol == *enclosing->self) {
            self = true;
            continue;
        }

        // A member of the Method's type, named alone, is Self's or its type's.
        if (enclosing != nullptr && enclosing->owner != type_id_t::INVALID &&
            used.owner != type_id_t::INVALID &&
            (used.kind == symbol_kind_t::FIELD || used.kind == symbol_kind_t::METHOD ||
             used.kind == symbol_kind_t::PROPERTY || used.kind == symbol_kind_t::GLOBAL)) {
            const std::uint32_t at = named.span.begin;
            const bool qualified = at > 0 && text[at - 1] == '.';
            self = self || !qualified;
        }

        if (!local_to_scope(used)) {
            continue;
        }

        if (within(used.span, range)) {
            if (!std::ranges::contains(declared_inside, named.symbol)) {
                declared_inside.push_back(named.symbol);
            }
        } else if (!std::ranges::contains(inputs, named.symbol)) {
            inputs.push_back(named.symbol);
        }
    }

    for (const expr_id_t id : exprs_in(query, range)) {
        self = self || std::holds_alternative<self_expr_t>(query.ast().get(id).node);
    }

    // It can't change what's outside it: a variable, or a Struct's field through one.
    // Nor through ByRef, or a ByRef Method.
    std::set<symbol_id_t> assigned = assigned_in(query, range);
    assigned.merge(passed_in(query, range));
    if (std::ranges::any_of(inputs, [&](const symbol_id_t id) { return assigned.contains(id); })) {
        return {};
    }

    for (const stmt_id_t id : statements_in(query, range)) {
        const stmt_node_t& node = query.ast().get(id).node;
        std::optional<expr_id_t> target;
        if (const auto* assignment = std::get_if<assign_stmt_t>(&node)) {
            target = assignment->target;
        } else if (const auto* increment = std::get_if<increment_stmt_t>(&node)) {
            target = increment->target;
        }

        // The variable a member or an index of it is set through.
        while (target) {
            const expr_node_t& part = query.ast().get(*target).node;
            if (const auto* member = std::get_if<member_expr_t>(&part)) {
                target = member->object;
            } else if (const auto* indexed = std::get_if<index_expr_t>(&part)) {
                target = indexed->object;
            } else {
                break;
            }

            const std::optional<symbol_id_t> root = info(query, *target).symbol;
            const type_kind_t kind = query.analysis().types.get(info(query, *target).type).kind;
            if (root && std::ranges::contains(inputs, *root) &&
                (kind == type_kind_t::STRUCT || kind == type_kind_t::TUPLE ||
                 kind == type_kind_t::FIXED_ARRAY)) {
                return {};
            }
        }
    }

    // What it declares that the code after it reads, which the new one returns: one at most.
    const span_t after{ .file = query.file(), .begin = range.end, .end = scope.end };
    std::vector<symbol_id_t> outputs;
    for (const symbol_id_t id : declared_inside) {
        const bool read_after = std::ranges::any_of(names, [&](const named_t& named) {
            return named.symbol == id && within(named.span, after);
        });
        if (read_after) {
            outputs.push_back(id);
        }
    }

    const auto is_local_variable = [&](const symbol_id_t id) {
        const symbol_t& output = symbol_of(query, id);
        if (output.kind != symbol_kind_t::LOCAL || !output.declaration) {
            return false;
        }

        const auto* variable =
            std::get_if<variable_stmt_t>(&query.ast().get(*output.declaration).node);
        return variable != nullptr && within(query.ast().get(*output.declaration).span, range) &&
               parent_of(query, *output.declaration) == parent;
    };
    if (outputs.size() > 1 || (outputs.size() == 1 && !is_local_variable(outputs.front()))) {
        return {};
    }

    const bool in_method = enclosing != nullptr && enclosing->kind == symbol_kind_t::METHOD;
    const bool is_static = in_method && enclosing->modifiers.is_static;
    if (self && !in_method) {
        return {};
    }

    std::vector<refactoring_t> found;
    const std::string br(line_break(text));
    const std::string unit = indent_unit(text);
    const std::string name = unused_name(text, "Extracted");
    std::string parameters;
    std::string arguments;
    // Each parameter of the type its first use has there, which a check before the code may
    // have narrowed, as a T? that isn't Null is a T.
    for (const symbol_id_t id : inputs) {
        const symbol_t& input = symbol_of(query, id);
        type_id_t type = input.type;
        std::uint32_t first_use = UINT32_MAX;
        for (const expr_id_t use : exprs_in(query, range)) {
            if (std::holds_alternative<name_expr_t>(query.ast().get(use).node) &&
                info(query, use).symbol == id && span_of(query, use).begin < first_use) {
                first_use = span_of(query, use).begin;
                type = info(query, use).type;
            }
        }

        parameters += std::format("{}{}: {}", parameters.empty() ? "" : ", ", input.name,
                                  query.type_name(type));
        arguments += std::format("{}{}", arguments.empty() ? "" : ", ", input.name);
    }

    const std::optional<symbol_id_t> output =
        outputs.empty() ? std::nullopt : std::optional(outputs.front());
    const std::string result =
        output ? std::format(": {}", query.type_name(symbol_of(query, *output).type)) : "";

    // The statements' lines, indented one step in from the new declaration.
    const span_t lines = whole_lines(query, range);
    const std::string_view moved = text.substr(lines.begin, lines.length());
    std::size_t common = std::string_view::npos;
    for (std::uint32_t at = 0; at < moved.size(); at = next_line(moved, at)) {
        const std::string_view indent = indentation(moved, at);
        const std::uint32_t content = at + static_cast<std::uint32_t>(indent.size());
        if (content < moved.size() && moved[content] != '\n' && moved[content] != '\r') {
            common = std::min(common, indent.size());
        }
    }

    // A Method goes beside the one it's from; a Function after the top-level declaration it's
    // in, or the file's code.
    std::optional<stmt_id_t> top;
    if (enclosing != nullptr) {
        top = *enclosing->declaration;
        while (const std::optional<stmt_id_t> outer = parent_of(query, *top)) {
            top = outer;
        }
    }

    const auto make = [&](const bool method) {
        const std::optional<stmt_id_t> anchor =
            method ? std::optional(*enclosing->declaration) : top;
        const std::string outer =
            anchor ? std::string(indentation(text, query.ast().get(*anchor).span.begin))
                   : std::string();
        std::string body;
        for (std::uint32_t at = 0; at < moved.size(); at = next_line(moved, at)) {
            const std::string_view line = moved.substr(at, next_line(moved, at) - at);
            const std::string_view content = line.substr(std::min(common, line.size()));
            body += content.find_first_not_of(" \t\r\n") == std::string_view::npos
                        ? std::string(content)
                        : outer + unit + std::string(content);
        }

        if (!body.ends_with('\n')) {
            body += br;
        }

        if (output) {
            body += std::format("{}{}Return {}{}", outer, unit, symbol_of(query, *output).name, br);
        }

        const std::string keyword = method ? "Method" : "Function";
        const std::string declaration = std::format(
            "{}Private {}{} {}({}){}{}{}{}End {}", outer, method && is_static ? "Static " : "",
            keyword, name, parameters, result, br, body, outer, keyword);
        const std::uint32_t at =
            anchor ? query.ast().get(*anchor).span.end : static_cast<std::uint32_t>(text.size());
        const std::string placed_text =
            anchor ? br + br + declaration
                   : (text.ends_with('\n') ? br : br + br) + declaration + br;
        const std::string call =
            std::format("{}{}{}({}){}", indentation(text, range.begin),
                        output ? std::format("Local {} = ", symbol_of(query, *output).name) : "",
                        name, arguments, br);
        return refactoring_t{
            .title = method ? "Extract to a Method" : "Extract to a Function",
            .kind = "refactor.extract",
            .edits = {
                { .span = lines, .replacement = call },
                { .span = { .file = query.file(), .begin = at, .end = at }, .replacement = placed_text },
            },
        };
    };

    if (in_method) {
        found.push_back(make(true));
    }

    if (!self) {
        found.push_back(make(false));
    }

    return found;
}

// Inline a Local

std::optional<refactoring_t> inline_local(const query_t& query, const named_t& named) {
    const symbol_t& local = symbol_of(query, named.symbol);
    if (local.kind != symbol_kind_t::LOCAL || !local.declaration) {
        return std::nullopt;
    }

    const stmt_id_t statement = *local.declaration;
    const auto* variable = std::get_if<variable_stmt_t>(&query.ast().get(statement).node);
    const std::string_view text = query.source().text();
    if (variable == nullptr || !variable->value || variable->kind != variable_kind_t::LOCAL ||
        !starts_line(text, query.ast().get(statement).span.begin)) {
        return std::nullopt;
    }

    const std::optional<symbol_id_t> function = function_around(query, local.span);
    const span_t scope =
        function
            ? query.ast().get(symbol_of(query, *function).declaration.value_or(stmt_id_t{})).span
            : span_t{
                  .file = query.file(),
                  .begin = 0,
                  .end = static_cast<std::uint32_t>(text.size()),
              };
    // Nothing changes it, a Struct's copy, nor what its value reads.
    const std::set<symbol_id_t> assigned = changed_in(query, scope);
    if (assigned.contains(named.symbol)) {
        return std::nullopt;
    }

    // Its value reads only what nothing in its code changes, so it gives the same at each use.
    const expr_id_t value = *variable->value;
    if (!steady(query, value, [&](const symbol_id_t id) { return !assigned.contains(id); })) {
        return std::nullopt;
    }

    std::string replacement = std::string(text_of(query, span_of(query, value)));
    if (needs_as(query, value, local.type)) {
        if (std::holds_alternative<null_expr_t>(query.ast().get(value).node)) {
            return std::nullopt;
        }

        replacement = std::format("({} As {})", replacement, query.type_name(local.type));
    } else {
        replacement = placed(query, value, std::move(replacement));
    }

    std::vector<fix_t> edits{
        {
            .span = whole_lines(query, query.ast().get(statement).span),
            .replacement = "",
        },
    };
    for (const named_t& use : query.names()) {
        if (use.symbol == named.symbol && !use.declaration && use.span != local.span) {
            edits.push_back({ .span = use.span, .replacement = replacement });
        }
    }

    return refactoring_t{
        .title = std::format("Inline '{}'", local.name),
        .kind = "refactor.inline",
        .edits = std::move(edits),
    };
}

// Inline a Function

/// The text of a call of `function`, a Function whose code is one Return of a value, with the
/// value in its place: its parameters' uses the arguments, which each have to be steady, or
/// used once.
std::optional<std::string> inlined_call(const query_t& query, const symbol_id_t function,
                                        const expr_id_t call) {
    const symbol_t& declared = symbol_of(query, function);
    const auto& node =
        std::get<function_stmt_t>(query.ast().get(declared.declaration.value_or(stmt_id_t{})).node);
    const auto& returned = std::get<return_stmt_t>(query.ast().get(node.body.front()).node);
    const expr_id_t body = returned.value.value_or(expr_id_t{});
    const span_t body_span = span_of(query, body);
    const expr_info_t& called = info(query, call);
    if (called.arguments.size() != declared.parameters.size() || !called.packed.empty()) {
        return std::nullopt;
    }

    // The call's own code, or the file's: a variable there named as what the body names would
    // hide it.
    const std::optional<symbol_id_t> caller = function_around(query, span_of(query, call));
    const span_t calling =
        caller ? query.ast().get(symbol_of(query, *caller).declaration.value_or(stmt_id_t{})).span
               : span_t{
                     .file = query.file(),
                     .begin = 0,
                     .end = static_cast<std::uint32_t>(query.source().text().size()),
                 };
    const std::vector<named_t> names = query.names();
    const std::set<symbol_id_t> assigned = changed_in(query, calling);
    std::vector<std::pair<span_t, std::string>> replaced;
    std::size_t acting = 0;
    for (const named_t& named : names) {
        if (!within(named.span, body_span)) {
            continue;
        }

        const auto parameter = std::ranges::find(declared.parameters, named.symbol);
        if (parameter == declared.parameters.end()) {
            const bool hidden = std::ranges::any_of(names, [&](const named_t& other) {
                const symbol_t& seen = symbol_of(query, other.symbol);
                return other.declaration &&
                       (seen.kind == symbol_kind_t::LOCAL ||
                        seen.kind == symbol_kind_t::PARAMETER ||
                        seen.kind == symbol_kind_t::LOOP_VARIABLE) &&
                       seen.name == symbol_of(query, named.symbol).name &&
                       within(other.span, calling);
            });
            if (hidden) {
                return std::nullopt;
            }

            continue;
        }

        const std::size_t at = static_cast<std::size_t>(parameter - declared.parameters.begin());
        std::string argument;
        if (const std::optional<expr_id_t> given = called.arguments[at]) {
            // The parameter's type, which the call converts the argument to.
            const type_id_t type = symbol_of(query, *parameter).type;
            argument =
                needs_as(query, *given, type)
                    ? std::format("({} As {})", text_of(query, span_of(query, *given)),
                                  query.type_name(type))
                    : placed(query, *given, std::string(text_of(query, span_of(query, *given))));
            if (!steady(query, *given,
                        [&](const symbol_id_t id) { return !assigned.contains(id); })) {
                ++acting;
            }
        } else if (const std::optional<expr_id_t> fallback = node.parameters[at].default_value) {
            argument =
                placed(query, *fallback, std::string(text_of(query, span_of(query, *fallback))));
        } else {
            return std::nullopt;
        }

        replaced.emplace_back(named.span, std::move(argument));
    }

    // An argument that acts is worked out once, first, as the call works it out: it's the only
    // one, used once, with nothing before it that acts or that's worked out only sometimes.
    for (std::size_t i = 0; i < called.arguments.size(); ++i) {
        const std::optional<expr_id_t> given = called.arguments[i];
        if (!given ||
            steady(query, *given, [&](const symbol_id_t id) { return !assigned.contains(id); })) {
            continue;
        }

        const auto uses = std::ranges::count_if(names, [&](const named_t& named) {
            return named.symbol == declared.parameters[i] && within(named.span, body_span);
        });
        const auto use = std::ranges::find_if(names, [&](const named_t& named) {
            return named.symbol == declared.parameters[i] && within(named.span, body_span);
        });
        if (acting > 1 || uses != 1 || conditional(query, body_span, use->span) ||
            acts_before(query, body_span, use->span)) {
            return std::nullopt;
        }
    }

    std::ranges::sort(replaced, {}, [](const auto& each) { return each.first.begin; });
    std::string out;
    std::uint32_t at = body_span.begin;
    for (const auto& [span, argument] : replaced) {
        out += text_of(query, { .file = body_span.file, .begin = at, .end = span.begin });
        out += argument;
        at = span.end;
    }

    out += text_of(query, { .file = body_span.file, .begin = at, .end = body_span.end });
    if (declared.type != type_id_t::NONE && needs_as(query, body, declared.type)) {
        return std::format("({} As {})", out, query.type_name(declared.type));
    }

    return placed(query, body, std::move(out));
}

/// Whether a Function's code is one Return of a value, which a call can be in place of: not
/// generic, Async, Unsafe, throwing, Extern, recursive, ByRef, or variadic.
[[nodiscard]] bool inlinable(const query_t& query, const symbol_id_t function) {
    const symbol_t& declared = symbol_of(query, function);
    if (declared.kind != symbol_kind_t::FUNCTION || !declared.declaration ||
        !declared.throws.empty() || declared.span.file != query.file()) {
        return false;
    }

    const auto* node = std::get_if<function_stmt_t>(&query.ast().get(*declared.declaration).node);
    if (node == nullptr || node->kind != function_kind_t::FUNCTION ||
        !node->type_parameters.empty() || node->modifiers.is_async || node->modifiers.is_unsafe ||
        node->external || node->prebuilt || node->body.size() != 1) {
        return false;
    }

    const auto* returned = std::get_if<return_stmt_t>(&query.ast().get(node->body.front()).node);
    if (returned == nullptr || !returned->value ||
        std::ranges::any_of(node->parameters, [](const parameter_t& parameter) {
            return parameter.by_ref || parameter.variadic;
        })) {
        return false;
    }

    const span_t body = span_of(query, *returned->value);
    return std::ranges::none_of(query.names(), [&](const named_t& named) {
        return named.symbol == function && within(named.span, body);
    });
}

/// The calls of `function` in the query's file, by their callee's name.
[[nodiscard]] std::vector<expr_id_t> calls_of(const query_t& query, const symbol_id_t function) {
    std::vector<expr_id_t> found;
    for (std::size_t i = 0; i < query.ast().expr_count(); ++i) {
        const auto id = static_cast<expr_id_t>(i);
        const auto* call = std::get_if<call_expr_t>(&query.ast().get(id).node);
        if (call != nullptr && span_of(query, id).file == query.file() &&
            info(query, call->callee).symbol == function &&
            std::holds_alternative<name_expr_t>(query.ast().get(call->callee).node)) {
            found.push_back(id);
        }
    }

    return found;
}

std::vector<refactoring_t> inline_function(const query_t& query, const named_t& named) {
    if (!inlinable(query, named.symbol)) {
        return {};
    }

    const symbol_t& declared = symbol_of(query, named.symbol);
    const std::vector<expr_id_t> calls = calls_of(query, named.symbol);
    std::vector<refactoring_t> found;
    // This call.
    for (const expr_id_t call : calls) {
        if (within(named.span,
                   span_of(query, std::get<call_expr_t>(query.ast().get(call).node).callee))) {
            if (const std::optional<std::string> text = inlined_call(query, named.symbol, call)) {
                found.push_back({
                    .title = std::format("Inline this call of '{}'", declared.name),
                    .kind = "refactor.inline",
                    .edits = { { .span = span_of(query, call), .replacement = *text } },
                });
            }
        }
    }

    // At its declaration, every call, and the declaration, for a Private one that nothing
    // else names.
    if (!named.declaration || declared.modifiers.visibility != visibility_t::PRIVATE) {
        return found;
    }

    const auto uses =
        static_cast<std::size_t>(std::ranges::count_if(query.names(), [&](const named_t& other) {
            return other.symbol == named.symbol && !other.declaration &&
                   other.span != declared.span;
        }));
    if (uses != calls.size()) {
        return found;
    }

    std::vector<fix_t> edits{
        {
            .span = declaration_lines(query, declared.declaration.value_or(stmt_id_t{})),
            .replacement = "",
        },
    };
    for (const expr_id_t call : calls) {
        const std::optional<std::string> text = inlined_call(query, named.symbol, call);
        if (!text) {
            return found;
        }

        edits.push_back({ .span = span_of(query, call), .replacement = *text });
    }

    found.push_back({
        .title = std::format("Inline '{}' everywhere and remove it", declared.name),
        .kind = "refactor.inline",
        .edits = std::move(edits),
    });
    return found;
}

// Change a signature

/// The pieces of a parameter list, at its commas outside brackets and strings.
[[nodiscard]] std::vector<std::string> pieces(const std::string_view written) {
    std::vector<std::string> found;
    std::string current;
    int depth = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < written.size(); ++i) {
        const char c = written[i];
        // A rename's "<-" isn't a type argument's bracket.
        const bool arrow = c == '<' && written.substr(i).starts_with("<-");
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && !arrow && (c == '(' || c == '[' || c == '<' || c == '{')) {
            ++depth;
        } else if (!quoted && (c == ')' || c == ']' || c == '>' || c == '}')) {
            --depth;
        } else if (!quoted && depth == 0 && c == ',') {
            found.push_back(current);
            current.clear();
            continue;
        }

        current += c;
    }

    found.push_back(current);
    for (std::string& piece : found) {
        const std::size_t begin = piece.find_first_not_of(" \t\r\n");
        const std::size_t end = piece.find_last_not_of(" \t\r\n");
        piece = begin == std::string::npos ? std::string() : piece.substr(begin, end - begin + 1);
    }

    std::erase_if(found, [](const std::string& piece) { return piece.empty(); });
    return found;
}

/// The name a parameter's text starts with.
[[nodiscard]] std::string leading_name(const std::string_view text) {
    std::size_t end = 0;
    while (end < text.size() && identifier_char(text[end])) {
        ++end;
    }

    return std::string(text.substr(0, end));
}

/// A parameter of the new list: the old one it was, by its place, its declaration's text, and
/// for a new one, what each call passes.
struct changed_t {
    std::optional<std::size_t> from;
    std::string name;
    std::string declaration;
    std::string value;
};

/// The span of a declaration's parameter's text, from its name to its default's end or its
/// type's.
[[nodiscard]] span_t parameter_span(const query_t& query, const parameter_t& parameter) {
    std::uint32_t end = query.ast().get(parameter.type).span.end;
    if (parameter.default_value) {
        end = span_of(query, *parameter.default_value).end;
    }

    std::uint32_t begin = parameter.name.span.begin;
    const std::string_view text = query.compilation().sources.get(parameter.name.span.file).text();
    if (parameter.by_ref && begin >= 6 && text.substr(0, begin).ends_with("ByRef ")) {
        begin -= 6;
    }

    return { .file = parameter.name.span.file, .begin = begin, .end = end };
}

/// Whether a function's parameters are its own to change.
[[nodiscard]] std::optional<std::string> fixed_signature(const query_t& query,
                                                         const symbol_id_t function) {
    const symbol_t& declared = symbol_of(query, function);
    if ((declared.kind != symbol_kind_t::FUNCTION && declared.kind != symbol_kind_t::METHOD) ||
        !declared.declaration) {
        return "only a Function's or a Method's signature changes";
    }

    const auto* node = std::get_if<function_stmt_t>(&query.ast().get(*declared.declaration).node);
    if (node == nullptr || node->kind != function_kind_t::FUNCTION || node->required ||
        node->external || node->prebuilt || node->modifiers.is_abstract || declared.overrides ||
        declared.default_method) {
        return std::format("'{}' takes its parameters from what it overrides or implements",
                           declared.name);
    }

    if (std::ranges::any_of(node->parameters,
                            [](const parameter_t& parameter) { return parameter.variadic; })) {
        return std::format("'{}' takes the rest of a call's arguments", declared.name);
    }

    const analysis_t& analysis = query.analysis();
    for (const symbol_t& each : analysis.symbols) {
        if (each.overrides == function) {
            return std::format("'{}' is overridden, which takes its parameters", declared.name);
        }
    }

    for (const conformance_t& conformance : analysis.conformances) {
        if (std::ranges::contains(conformance.members, function)) {
            return std::format("'{}' implements an Interface's Method", declared.name);
        }
    }

    for (const closure_t& closure : analysis.closures) {
        if (!closure.lambda && !closure.body && closure.function == function) {
            return std::format("'{}' is used as a value, whose type has its parameters",
                               declared.name);
        }
    }

    return std::nullopt;
}

} // namespace

std::vector<refactoring_t> refactorings(const query_t& query, span_t selection) {
    const std::string_view text = query.source().text();
    // Without the spaces around it.
    while (selection.begin < selection.end &&
           std::isspace(static_cast<unsigned char>(text[selection.begin])) != 0) {
        ++selection.begin;
    }

    while (selection.end > selection.begin &&
           std::isspace(static_cast<unsigned char>(text[selection.end - 1])) != 0) {
        --selection.end;
    }

    std::vector<refactoring_t> found;
    if (!selection.empty()) {
        if (std::optional<refactoring_t> local = extract_local(query, selection)) {
            found.push_back(std::move(*local));
        }

        std::ranges::move(extract_function(query, selection), std::back_inserter(found));
    }

    if (const std::optional<named_t> named = query.name_at(selection.begin)) {
        if (std::optional<refactoring_t> local = inline_local(query, *named)) {
            found.push_back(std::move(*local));
        }

        std::ranges::move(inline_function(query, *named), std::back_inserter(found));
    }

    return found;
}

std::vector<fix_t> organized_imports(const query_t& query) {
    const std::optional<module_id_t> module = query.module();
    if (!module) {
        return {};
    }

    const std::string_view text = query.source().text();
    std::vector<span_t> statements;
    for (const module_import_t& imported : query.ast().modules().get(*module).imports) {
        const span_t span = query.ast().get(imported.statement).span;
        if (span.file == query.file()) {
            statements.push_back(whole_lines(query, span));
        }
    }

    if (statements.empty()) {
        return {};
    }

    std::ranges::sort(statements, {}, &span_t::begin);
    // The lines from the first to the last, which are Imports or blank, and nothing else.
    const span_t block{
        .file = query.file(),
        .begin = statements.front().begin,
        .end = statements.back().end,
    };
    for (std::uint32_t at = block.begin; at < block.end; at = next_line(text, at)) {
        const bool imports = std::ranges::any_of(
            statements, [&](const span_t lines) { return lines.begin <= at && at < lines.end; });
        const std::string_view line = text.substr(at, next_line(text, at) - at);
        if (!imports && line.find_first_not_of(" \t\r\n") != std::string_view::npos) {
            return {};
        }
    }

    // Each Import's text, without what's unused in it.
    const std::string br(line_break(text));
    std::vector<std::string> kept;
    for (const span_t lines : statements) {
        std::vector<fix_t> fixes;
        bool gone = false;
        for (const diagnostic_t& diagnostic : query.compilation().diagnostics.diagnostics()) {
            if (diagnostic.lint != lint_t::UNUSED_IMPORT || !within(diagnostic.span, lines)) {
                continue;
            }

            for (const suggestion_t& suggestion : diagnostic.suggestions) {
                gone = gone || suggestion.title == "Remove the Import" ||
                       suggestion.title == "Remove the From line";
                if (suggestion.title.starts_with("Remove '")) {
                    std::ranges::copy(suggestion.fixes, std::back_inserter(fixes));
                }
            }
        }

        if (gone) {
            continue;
        }

        std::ranges::sort(fixes, std::greater{}, [](const fix_t& fix) { return fix.span.begin; });
        std::string line(text.substr(lines.begin, lines.length()));
        for (const fix_t& fix : fixes) {
            line.replace(fix.span.begin - lines.begin, fix.span.length(), fix.replacement);
        }

        while (line.ends_with('\n') || line.ends_with('\r')) {
            line.pop_back();
        }

        kept.push_back(std::move(line));
    }

    // std's first, then packages', then quoted paths', each by what it imports.
    const auto key = [](const std::string& line) {
        const std::string_view rest = std::string_view(line).substr(line.find(' ') + 1);
        int group = 1;
        if (rest.starts_with("std.") || rest == "std" || rest.starts_with("std ")) {
            group = 0;
        } else if (rest.starts_with('"')) {
            group = 2;
        }

        std::string lower(rest);
        std::ranges::transform(lower, lower.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return std::tuple(group, lower, line.starts_with("From"));
    };
    std::ranges::stable_sort(kept, {}, key);
    std::string organized;
    for (const std::string& line : kept) {
        organized += line + br;
    }

    if (organized == text.substr(block.begin, block.length())) {
        return {};
    }

    return { { .span = block, .replacement = organized } };
}

std::expected<std::string, std::string> signature_parameters(const query_t& query,
                                                             const symbol_id_t function) {
    if (const std::optional<std::string> why = fixed_signature(query, function)) {
        return std::unexpected(*why);
    }

    const symbol_t& declared = symbol_of(query, function);
    const auto& node =
        std::get<function_stmt_t>(query.ast().get(declared.declaration.value_or(stmt_id_t{})).node);
    std::string written;
    for (const parameter_t& parameter : node.parameters) {
        written += std::format("{}{}", written.empty() ? "" : ", ",
                               text_of(query, parameter_span(query, parameter)));
    }

    return written;
}

std::expected<std::vector<fix_t>, std::string>
change_signature(const query_t& query, const symbol_key_t& key, const std::string_view written) {
    // The declaration, and its generic copies.
    const std::vector<symbol_id_t> symbols = query.symbols_of(key);
    const auto declaring = std::ranges::find_if(symbols, [&](const symbol_id_t id) {
        return symbol_of(query, id).declaration.has_value();
    });
    if (declaring == symbols.end()) {
        return std::vector<fix_t>{};
    }

    const symbol_id_t function = *declaring;
    if (const std::optional<std::string> why = fixed_signature(query, function)) {
        return std::unexpected(*why);
    }

    const symbol_t& declared = symbol_of(query, function);
    const auto& node = std::get<function_stmt_t>(query.ast().get(*declared.declaration).node);
    std::vector<changed_t> changes;
    for (const std::string& piece : pieces(written)) {
        changed_t change;
        std::string declaration = piece;
        std::string old_name;
        if (const std::size_t arrow = piece.find("<-"); arrow != std::string::npos) {
            declaration = std::string(pieces(piece.substr(0, arrow)).front());
            old_name = pieces(piece.substr(arrow + 2)).front();
        } else {
            old_name = leading_name(piece);
        }

        const auto old = std::ranges::find_if(node.parameters, [&](const parameter_t& parameter) {
            return parameter.name.text == old_name;
        });
        if (old != node.parameters.end()) {
            change.from = static_cast<std::size_t>(old - node.parameters.begin());
            change.declaration = declaration;
        } else {
            // A new one: its declaration, and after its type's top-level '=', the value each
            // call passes.
            const std::size_t equals = piece.find('=');
            if (equals == std::string::npos) {
                return std::unexpected(std::format(
                    "'{}' is a new parameter, which needs '= value', the argument each call "
                    "passes",
                    piece));
            }

            change.declaration = pieces(piece.substr(0, equals)).front();
            change.value = pieces(piece.substr(equals + 1)).front();
        }

        change.name = leading_name(change.declaration);
        if (change.name.empty() || std::ranges::any_of(changes, [&](const changed_t& other) {
                return other.name == change.name || (change.from && other.from == change.from);
            })) {
            return std::unexpected(std::format("'{}' is in the list twice", piece));
        }

        changes.push_back(std::move(change));
    }

    // What goes can't be what the code still uses, and a new name can't be what it names
    // already, which the parameter would hide, or what it declares.
    const span_t body = query.ast().get(*declared.declaration).span;
    const std::vector<named_t> named_in = query.names(declared.span.file);
    for (std::size_t old = 0; old < declared.parameters.size(); ++old) {
        const bool kept = std::ranges::any_of(
            changes, [&](const changed_t& change) { return change.from == old; });
        const bool used = std::ranges::any_of(named_in, [&](const named_t& named) {
            return named.symbol == declared.parameters[old] && within(named.span, body) &&
                   named.span != node.parameters[old].name.span;
        });
        if (!kept && used) {
            return std::unexpected(std::format("'{}' goes, but the code still uses it",
                                               node.parameters[old].name.text));
        }
    }

    for (const changed_t& change : changes) {
        const std::optional<symbol_id_t> own =
            change.from ? std::optional(declared.parameters[*change.from]) : std::nullopt;
        const bool taken = std::ranges::any_of(named_in, [&](const named_t& named) {
            const symbol_t& other = symbol_of(query, named.symbol);
            const bool parameter = std::ranges::contains(declared.parameters, named.symbol);
            return within(named.span, body) && other.name == change.name && named.symbol != own &&
                   !parameter;
        });
        if (taken) {
            return std::unexpected(std::format(
                "the code names '{}' already, which a parameter of that name would hide",
                change.name));
        }
    }

    std::vector<fix_t> edits;
    // The declaration's parameters, from the first one's start to the last one's end, or between
    // its parentheses, which have none.
    const std::string_view text = query.compilation().sources.get(declared.span.file).text();
    const auto [open, close] = [&] {
        if (!node.parameters.empty()) {
            return std::pair(parameter_span(query, node.parameters.front()).begin,
                             parameter_span(query, node.parameters.back()).end);
        }

        std::uint32_t from = declared.span.end;
        while (from < node.header_end && text[from] != '(') {
            ++from;
        }

        std::uint32_t to = from + 1;
        while (to < node.header_end && text[to] != ')') {
            ++to;
        }

        return std::pair(from + 1, to);
    }();

    std::string list;
    for (const changed_t& change : changes) {
        list += std::format("{}{}", list.empty() ? "" : ", ", change.declaration);
    }

    edits.push_back({
        .span = { .file = declared.span.file, .begin = open, .end = close },
        .replacement = list,
    });

    // A renamed parameter's uses in the function's code.
    for (const changed_t& change : changes) {
        if (!change.from || node.parameters[*change.from].name.text == change.name) {
            continue;
        }

        const symbol_id_t parameter = declared.parameters[*change.from];
        for (const named_t& named : named_in) {
            if (named.symbol == parameter && within(named.span, body) &&
                named.span != node.parameters[*change.from].name.span) {
                edits.push_back({ .span = named.span, .replacement = change.name });
            }
        }
    }

    // Each call's arguments.
    for (std::size_t i = 0; i < query.ast().expr_count(); ++i) {
        const auto id = static_cast<expr_id_t>(i);
        const auto* call = std::get_if<call_expr_t>(&query.ast().get(id).node);
        const expr_info_t& called = info(query, id);
        if (call == nullptr || !called.symbol || !std::ranges::contains(symbols, *called.symbol)) {
            continue;
        }

        if (called.arguments.size() != node.parameters.size()) {
            return std::unexpected("a call's arguments couldn't be matched to the parameters");
        }

        // Where each argument was written, and whether by name.
        std::map<expr_id_t, std::pair<std::size_t, bool>> written_at;
        for (std::size_t j = 0; j < call->arguments.size(); ++j) {
            written_at[call->arguments[j].value] = { j, call->arguments[j].name.has_value() };
        }

        // An argument that goes, or one that acts moving past another that acts, would change
        // what the call does.
        std::vector<std::size_t> acting;
        for (std::size_t old = 0; old < called.arguments.size(); ++old) {
            const std::optional<expr_id_t> given = called.arguments[old];
            if (!given) {
                continue;
            }

            const bool kept = std::ranges::any_of(
                changes, [&](const changed_t& change) { return change.from == old; });
            const bool acts_here = acts_within(query, *given);
            if (!kept && acts_here) {
                return std::unexpected(std::format(
                    "the argument '{}' does something, which leaving it out would change",
                    text_of(query, span_of(query, *given))));
            }

            if (kept && acts_here) {
                acting.push_back(old);
            }
        }

        // With one that acts, every argument kept stays in its order, as one may change what
        // another reads.
        std::vector<std::size_t> order;
        for (const changed_t& change : changes) {
            if (!acting.empty() && change.from && called.arguments[*change.from]) {
                order.push_back(written_at[*called.arguments[*change.from]].first);
            }
        }

        if (!std::ranges::is_sorted(order)) {
            return std::unexpected(
                "moving arguments that do something past each other would change their order");
        }

        std::string arguments;
        bool named = false;
        for (const changed_t& change : changes) {
            std::string argument;
            bool by_name = false;
            if (change.from) {
                const std::optional<expr_id_t> given = called.arguments[*change.from];
                if (!given) {
                    named = true;
                    continue;
                }

                const auto [place, was_named] = written_at[*given];
                by_name = was_named;
                argument = std::format("{}{}", call->arguments[place].by_ref ? "ByRef " : "",
                                       text_of(query, span_of(query, *given)));
            } else {
                argument = change.value;
            }

            named = named || by_name;
            arguments += std::format("{}{}{}", arguments.empty() ? "" : ", ",
                                     named ? change.name + ": " : "", argument);
        }

        // Its arguments, from the first one's start, its name's or ByRef's, to the last one's
        // end, or between the parentheses the call ends with, which have none.
        const span_t whole = span_of(query, id);
        const std::string_view source = query.compilation().sources.get(whole.file).text();
        span_t listed{ .file = whole.file, .begin = whole.end - 1, .end = whole.end - 1 };
        if (call->arguments.empty()) {
            while (listed.begin > span_of(query, call->callee).end &&
                   source[listed.begin - 1] != '(') {
                --listed.begin;
            }
        } else {
            const argument_t& first = call->arguments.front();
            listed.begin = span_of(query, first.value).begin;
            if (first.name) {
                listed.begin = first.name->span.begin;
            } else if (first.by_ref) {
                const std::size_t keyword = source.rfind("ByRef", listed.begin);
                listed.begin = keyword == std::string_view::npos
                                   ? listed.begin
                                   : static_cast<std::uint32_t>(keyword);
            }

            listed.end = span_of(query, call->arguments.back().value).end;
        }

        edits.push_back({ .span = listed, .replacement = arguments });
    }

    return edits;
}

} // namespace fb::lsp
