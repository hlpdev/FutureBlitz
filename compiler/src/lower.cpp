#include "fb/lower.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

template <typename Id>
std::size_t index(const Id id) {
    return std::to_underlying(id);
}

/// Lowering only sees code that analyzed without errors, so these are always set.
template <typename T>
T expect(const std::optional<T>& value) {
    assert(value.has_value());
    return value.value_or(T{});
}

/// What a variable holds before it's assigned: zero, False, or "".
const_value_t zero(const type_id_t type) {
    if (type == type_id_t::BOOL) {
        return make_bool(false);
    }

    if (type == type_id_t::STRING) {
        return make_string("");
    }

    if (is_float(type)) {
        return { .type = type, .data = 0.0 };
    }

    if (is_signed(type)) {
        return { .type = type, .data = std::int64_t{ 0 } };
    }

    return { .type = type, .data = std::uint64_t{ 0 } };
}

/// What ++ and -- add, and the Step of a For without one.
const_value_t one(const type_id_t type) {
    if (is_float(type)) {
        return { .type = type, .data = 1.0 };
    }

    if (is_signed(type)) {
        return { .type = type, .data = std::int64_t{ 1 } };
    }

    return { .type = type, .data = std::uint64_t{ 1 } };
}

bool is_negative(const const_value_t& value) {
    if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
        return *number < 0;
    }

    if (const auto* number = std::get_if<double>(&value.data)) {
        return *number < 0;
    }

    return false;
}

/// The unsigned type of the same width.
type_id_t unsigned_of(const type_id_t type) {
    switch (type) {
    case type_id_t::INT8:
        return type_id_t::UINT8;
    case type_id_t::INT16:
        return type_id_t::UINT16;
    case type_id_t::INT32:
        return type_id_t::UINT32;
    case type_id_t::INT64:
        return type_id_t::UINT64;
    default:
        return type;
    }
}

/// "Add(Int32, Int32)": overloads get different names.
std::string function_name(const analysis_t& analysis, const symbol_t& function) {
    std::string name = std::string(function.name) + "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        const symbol_t& parameter = analysis.symbol(function.parameters[i]);
        name += i == 0 ? "" : ", ";
        name += parameter.by_ref ? "ByRef " : "";
        name += type_name(parameter.type);
    }

    return name + ")";
}

/// A loop being lowered. Break and Continue end their blocks without a target, and the loop
/// fills it in once it has made its exit and the place where its next pass starts.
struct loop_t {
    /// The loop statement, which the analysis gives each Break and Continue.
    stmt_id_t stmt;
    std::size_t scope_depth;        // the scopes inside the loop start here
    std::vector<block_id_t> breaks; // blocks that jump past the loop
    std::vector<block_id_t> continues;
};

/// Turns the analyzed AST into MIR: one function per Function, plus "main" for the top-level
/// code. Each String variable owns a reference to its value, so lowering adds the retains and
/// releases. Parameters borrow the caller's.
class lowerer_t {
public:
    lowerer_t(const ast_t& ast, const analysis_t& analysis) : m_ast(ast), m_analysis(analysis) {}

    mir_module_t run() {
        declare_globals();
        declare_functions();
        for (const stmt_id_t id : m_ast.items()) {
            if (const auto* node = std::get_if<function_stmt_t>(&m_ast.get(id).node)) {
                lower_function(id, *node);
            }
        }

        lower_top_level();
        return std::move(m_module);
    }

private:
    const ast_t& m_ast;
    const analysis_t& m_analysis;
    mir_module_t m_module;
    std::unordered_map<symbol_id_t, function_id_t> m_functions;
    std::unordered_map<symbol_id_t, global_id_t> m_globals;

    // The function being lowered
    function_id_t m_function{};
    std::unordered_map<symbol_id_t, local_id_t> m_locals;
    std::optional<block_id_t> m_block;             // nullopt after a jump: nothing reaches here
    std::vector<std::vector<local_id_t>> m_scopes; // String locals to release, innermost last
    std::vector<local_id_t> m_temporaries;         // owned Strings to release after the statement
    std::vector<loop_t> m_loops;                   // innermost last

    // Declarations

    /// Globals and top-level Consts. One whose value is known at compile time starts with it.
    /// The rest start at zero, and main sets the ones with an initializer first.
    void declare_globals() {
        for (const symbol_id_t id : m_analysis.globals) {
            const symbol_t& symbol = m_analysis.symbol(id);
            const auto& node =
                std::get<variable_stmt_t>(m_ast.get(expect(symbol.declaration)).node);
            const_value_t initial = zero(symbol.type);
            if (node.value) {
                if (const std::optional<const_value_t>& value =
                        m_analysis.expr(*node.value).value) {
                    initial = *value;
                }
            }

            m_globals[id] = static_cast<global_id_t>(m_module.globals.size());
            m_module.globals.push_back(
                { .name = symbol.name, .type = symbol.type, .initial = std::move(initial) });
        }
    }

    /// Every function gets its id before any body is lowered, so a call can reach a function
    /// declared further down. "main" comes last.
    void declare_functions() {
        for (const symbol_id_t id : m_analysis.functions) {
            const symbol_t& symbol = m_analysis.symbol(id);
            m_functions[id] = static_cast<function_id_t>(m_module.functions.size());
            m_module.functions.push_back(
                { .name = function_name(m_analysis, symbol), .return_type = symbol.type });
        }

        m_module.entry = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({ .name = "main" });
    }

    void begin_function(const function_id_t id) {
        m_function = id;
        m_locals.clear();
        m_scopes.clear();
        m_temporaries.clear();
        m_loops.clear();
        m_block = new_block();
        push_scope();
    }

    /// Ends the block that runs off the end of the function, if code reaches there.
    void finish_function() {
        if (current().return_type == type_id_t::NONE) {
            release_scopes(0);
            terminate(return_t{});
        } else {
            terminate(unreachable_t{}); // analysis checked that every path returns
        }
    }

    void lower_function(const stmt_id_t id, const function_stmt_t& node) {
        const symbol_id_t symbol_id = expect(m_analysis.stmt(id).symbol);
        const symbol_t& function = m_analysis.symbol(symbol_id);
        begin_function(m_functions.at(symbol_id));
        // Parameters are the first locals, in order. They aren't in any scope: a String parameter
        // borrows the caller's reference, so the function never releases it.
        for (const symbol_id_t parameter_id : function.parameters) {
            const symbol_t& parameter = m_analysis.symbol(parameter_id);
            m_locals[parameter_id] = add_local(parameter.type, parameter.name, parameter.by_ref);
        }

        current().parameter_count = function.parameters.size();
        lower_statements(node.body);
        finish_function();
    }

    /// Globals whose values aren't known at compile time are set first, in source order. Then
    /// the rest of the top-level code runs.
    void lower_top_level() {
        begin_function(m_module.entry);
        for (const stmt_id_t id : m_ast.items()) {
            const auto* node = std::get_if<variable_stmt_t>(&m_ast.get(id).node);
            if (node == nullptr || node->kind == variable_kind_t::LOCAL || !node->value ||
                m_analysis.expr(*node->value).value) {
                continue;
            }

            const global_id_t global = m_globals.at(expect(m_analysis.stmt(id).symbol));
            store(global, lower_value(*node->value));
            finish_statement();
        }

        for (const stmt_id_t id : m_ast.items()) {
            const stmt_node_t& node = m_ast.get(id).node;
            const auto* variable = std::get_if<variable_stmt_t>(&node);
            const bool global = variable != nullptr && variable->kind != variable_kind_t::LOCAL;
            if (!global && !std::holds_alternative<function_stmt_t>(node)) {
                lower_stmt(id);
            }
        }

        finish_function();
    }

    // Building blocks

    mir_function_t& current() {
        return m_module.functions[index(m_function)];
    }

    basic_block_t& block_at(const block_id_t id) {
        return current().blocks[index(id)];
    }

    local_id_t add_local(const type_id_t type, const std::string_view name = {},
                         const bool by_ref = false) {
        const auto id = static_cast<local_id_t>(current().locals.size());
        current().locals.push_back({ .type = type, .name = name, .by_ref = by_ref });
        return id;
    }

    [[nodiscard]] type_id_t type_of(const operand_t& value) {
        if (const auto* constant = std::get_if<const_value_t>(&value)) {
            return constant->type;
        }

        const auto& target = std::get<place_t>(value);
        if (const auto* local = std::get_if<local_id_t>(&target)) {
            return current().locals[index(*local)].type;
        }

        return m_module.globals[index(std::get<global_id_t>(target))].type;
    }

    block_id_t new_block() {
        const auto id = static_cast<block_id_t>(current().blocks.size());
        current().blocks.emplace_back();
        return id;
    }

    /// The block code goes into. Code that nothing reaches still gets a block of its own.
    block_id_t here() {
        if (!m_block) {
            m_block = new_block();
        }

        return *m_block;
    }

    void start(const block_id_t id) {
        m_block = id;
    }

    void emit(statement_t statement) {
        block_at(here()).statements.push_back(std::move(statement));
    }

    /// Ends the current block. Does nothing when there's none, as right after a Return.
    void terminate(terminator_t terminator) {
        if (m_block) {
            block_at(*m_block).terminator = std::move(terminator);
            m_block.reset();
        }
    }

    void jump(const block_id_t target) {
        terminate(goto_t{ .target = target });
    }

    /// Ends the current block with a jump whose target comes later, from join() or patch().
    void leave(std::vector<block_id_t>& exits) {
        if (m_block) {
            exits.push_back(*m_block);
            m_block.reset();
        }
    }

    void patch(const std::vector<block_id_t>& exits, const block_id_t target) {
        for (const block_id_t exit : exits) {
            block_at(exit).terminator = goto_t{ .target = target };
        }
    }

    /// Starts a new block that all of `exits` jump to.
    block_id_t join(const std::vector<block_id_t>& exits) {
        const block_id_t target = new_block();
        patch(exits, target);
        start(target);
        return target;
    }

    // Strings

    /// A new temporary holding `value`. An owned String in it is released after the statement.
    operand_t temporary(rvalue_t value, const type_id_t type, const bool owned = false) {
        const local_id_t local = add_local(type);
        emit(assign_t{ .target = local, .value = std::move(value) });
        if (owned) {
            m_temporaries.push_back(local);
        }

        return place_t{ local };
    }

    /// Gives a String's new holder its own reference: a temporary hands its reference over, and
    /// anything else is retained. Constants are static and need neither.
    void take(const operand_t& value) {
        if (std::holds_alternative<const_value_t>(value)) {
            return;
        }

        if (const auto* local = std::get_if<local_id_t>(&std::get<place_t>(value))) {
            const auto found = std::ranges::find(m_temporaries, *local);
            if (found != m_temporaries.end()) {
                m_temporaries.erase(found);
                return;
            }
        }

        emit(retain_t{ .value = value });
    }

    /// The first value of a new variable.
    void initialize(const local_id_t local, const operand_t& value) {
        if (type_of(value) == type_id_t::STRING) {
            take(value);
        }

        emit(assign_t{ .target = local, .value = use_t{ .value = value } });
    }

    /// A new value for a variable that already has one; a String's old value is released.
    void store(const place_t& target, const operand_t& value) {
        if (type_of(value) == type_id_t::STRING) {
            take(value);
            emit(release_t{ .target = target });
        }

        emit(assign_t{ .target = target, .value = use_t{ .value = value } });
    }

    /// Releases the temporaries made since `mark`, newest first. Code that nothing reaches just
    /// drops them.
    void release_temporaries_from(const std::size_t mark) {
        while (m_temporaries.size() > mark) {
            if (m_block) {
                emit(release_t{ .target = m_temporaries.back() });
            }

            m_temporaries.pop_back();
        }
    }

    /// Releases the Strings made for the statement that just finished.
    void finish_statement() {
        release_temporaries_from(0);
    }

    void push_scope() {
        m_scopes.emplace_back();
    }

    void pop_scope() {
        release_scope(m_scopes.back());
        m_scopes.pop_back();
    }

    void release_scope(const std::vector<local_id_t>& scope) {
        if (!m_block) {
            return;
        }

        for (const local_id_t local : std::views::reverse(scope)) {
            emit(release_t{ .target = local });
        }
    }

    /// Releases the Strings of every scope from `depth` inward, for a jump out of them.
    void release_scopes(const std::size_t depth) {
        for (std::size_t i = m_scopes.size(); i-- > depth;) {
            release_scope(m_scopes[i]);
        }
    }

    // Statements

    void lower_statements(const block_t& block) {
        for (const stmt_id_t id : block) {
            lower_stmt(id);
        }
    }

    /// A block with a scope of its own, whose Strings are released where it ends.
    void lower_block(const block_t& block) {
        push_scope();
        lower_statements(block);
        pop_scope();
    }

    void lower_stmt(const stmt_id_t id) {
        const stmt_t& stmt = m_ast.get(id);
        std::visit([this, id, &stmt](const auto& node) { this->lower_stmt(id, stmt.span, node); },
                   stmt.node);
    }

    /// Statements past M1 never pass analysis, and functions are only at the top level.
    template <typename Node>
    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const Node& /*node*/) {
        assert(false);
    }

    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const expression_stmt_t& node) {
        // Only a call can be a statement on its own.
        lower_call(node.expression);
        finish_statement();
    }

    void lower_stmt(stmt_id_t /*id*/, const span_t span, const assign_stmt_t& node) {
        const place_t target = place_of(expect(m_analysis.expr(node.target).symbol));
        const type_id_t type = type_of(target);
        const operand_t value = lower_value(node.value);
        if (!node.op) {
            store(target, value);
        } else if (type == type_id_t::STRING) {
            // += is the only compound assignment a String has.
            const runtime_call_t concat{
                .function = runtime_function_t::CONCAT,
                .arguments = { target, value },
            };
            store(target, temporary(concat, type_id_t::STRING, true));
        } else {
            emit(assign_t{
                .target = target,
                .value = binary_t{ .op = *node.op, .left = target, .right = value, .span = span },
            });
        }

        finish_statement();
    }

    void lower_stmt(stmt_id_t /*id*/, const span_t span, const increment_stmt_t& node) {
        const place_t target = place_of(expect(m_analysis.expr(node.target).symbol));
        const binary_op_t op = node.decrement ? binary_op_t::SUBTRACT : binary_op_t::ADD;
        emit(assign_t{
            .target = target,
            .value =
                binary_t{ .op = op, .left = target, .right = one(type_of(target)), .span = span },
        });
    }

    void lower_stmt(const stmt_id_t id, span_t /*span*/, const variable_stmt_t& node) {
        const symbol_id_t symbol_id = expect(m_analysis.stmt(id).symbol);
        const symbol_t& symbol = m_analysis.symbol(symbol_id);
        if (symbol.kind == symbol_kind_t::CONSTANT && symbol.value) {
            return; // every use reads the value directly
        }

        const operand_t value = node.value ? lower_value(*node.value) : zero(symbol.type);
        const local_id_t local = add_local(symbol.type, symbol.name);
        m_locals[symbol_id] = local;
        initialize(local, value);
        if (symbol.type == type_id_t::STRING) {
            m_scopes.back().push_back(local);
        }

        finish_statement();
    }

    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const if_stmt_t& node) {
        // A false condition goes on to the next one, and then to the Else. Each body jumps past
        // the rest when it ends.
        std::vector<block_id_t> exits;
        for (const if_branch_t& branch : node.branches) {
            const operand_t condition = lower_value(branch.condition);
            finish_statement();
            const block_id_t test = here();
            m_block.reset();
            const block_id_t body = new_block();
            start(body);
            lower_block(branch.body);
            leave(exits);
            const block_id_t next = new_block();
            block_at(test).terminator =
                branch_t{ .condition = condition, .if_true = body, .if_false = next };
            start(next);
        }

        if (node.else_body) {
            lower_block(*node.else_body);
        }

        leave(exits);
        join(exits);
    }

    /// Break and Continue in the body are collected in the returned loop, for the caller to
    /// point at the right blocks.
    loop_t lower_loop_body(const stmt_id_t id, const block_t& body) {
        m_loops.push_back({ .stmt = id, .scope_depth = m_scopes.size() });
        lower_block(body);
        loop_t loop = std::move(m_loops.back());
        m_loops.pop_back();
        return loop;
    }

    void lower_stmt(const stmt_id_t id, span_t /*span*/, const while_stmt_t& node) {
        const block_id_t head = new_block();
        jump(head);
        start(head);
        const operand_t condition = lower_value(node.condition);
        finish_statement();
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        const loop_t loop = lower_loop_body(id, node.body);
        jump(head);
        patch(loop.continues, head);
        const block_id_t exit = join(loop.breaks);
        block_at(test).terminator =
            branch_t{ .condition = condition, .if_true = body, .if_false = exit };
    }

    void lower_stmt(const stmt_id_t id, span_t /*span*/, const repeat_stmt_t& node) {
        const block_id_t body = new_block();
        jump(body);
        start(body);
        loop_t loop = lower_loop_body(id, node.body);
        leave(loop.continues); // the end of the body goes to the condition, like Continue
        join(loop.continues);
        const operand_t condition = lower_value(node.condition);
        finish_statement();
        const block_id_t test = here();
        m_block.reset();

        // Until: a True condition ends the loop.
        const block_id_t exit = join(loop.breaks);
        block_at(test).terminator =
            branch_t{ .condition = condition, .if_true = exit, .if_false = body };
    }

    void lower_stmt(const stmt_id_t id, span_t /*span*/, const loop_stmt_t& node) {
        const block_id_t body = new_block();
        jump(body);
        start(body);
        const loop_t loop = lower_loop_body(id, node.body);
        jump(body);
        patch(loop.continues, body);
        join(loop.breaks);
    }

    void lower_stmt(const stmt_id_t id, const span_t span, const for_stmt_t& node) {
        const symbol_id_t symbol_id = expect(m_analysis.stmt(id).symbol);
        const type_id_t type = m_analysis.symbol(symbol_id).type;
        const local_id_t variable = add_local(type, node.variable.text);
        m_locals[symbol_id] = variable;

        // The start, end, and step are evaluated once, in that order.
        initialize(variable, lower_value(node.start));
        const operand_t end = stable(lower_value(node.end));
        const operand_t step = node.step ? stable(lower_value(*node.step)) : one(type);
        finish_statement();

        // The step's sign picks the direction. A Step only known at run time is checked there:
        // 0 panics, and only a signed Step can count down.
        std::optional<bool> up;
        operand_t up_at_run_time;
        if (const auto* constant = std::get_if<const_value_t>(&step)) {
            up = !is_negative(*constant);
        } else {
            const span_t step_span = m_ast.get(expect(node.step)).span;
            const operand_t is_zero = compare(binary_op_t::EQUAL, step, zero(type), span);
            const block_id_t fail = new_block();
            const block_id_t ok = new_block();
            terminate(branch_t{ .condition = is_zero, .if_true = fail, .if_false = ok });
            start(fail);
            terminate(panic_t{ .message = "Step can't be 0", .span = step_span });
            start(ok);
            if (is_signed(type)) {
                up_at_run_time = compare(binary_op_t::GREATER, step, zero(type), span);
            } else {
                up = true;
            }
        }

        // `make`'s test for the direction known at compile time, or both tests with the result
        // picked at run time.
        const auto by_direction = [&](const auto& make) {
            if (up) {
                return make(*up);
            }

            const operand_t if_up = make(true);
            const operand_t if_down = make(false);
            return choose(up_at_run_time, if_up, if_down, span);
        };

        // Before each pass: is the variable still in range?
        const block_id_t check = new_block();
        jump(check);
        start(check);
        const operand_t in_range = by_direction([&](const bool going_up) {
            binary_op_t op = node.inclusive ? binary_op_t::LESS_EQUAL : binary_op_t::LESS;
            if (!going_up) {
                op = node.inclusive ? binary_op_t::GREATER_EQUAL : binary_op_t::GREATER;
            }

            return compare(op, variable, end, span);
        });
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        loop_t loop = lower_loop_body(id, node.body);
        leave(loop.continues);
        join(loop.continues);

        // After each pass: would another step pass the end? Integers check the distance left
        // first, so the variable never overflows.
        operand_t next = place_t{ variable };
        operand_t done;
        if (is_float(type)) {
            next = temporary(
                binary_t{ .op = binary_op_t::ADD, .left = variable, .right = step, .span = span },
                type);
            done = by_direction([&](const bool going_up) {
                binary_op_t op = node.inclusive ? binary_op_t::GREATER : binary_op_t::GREATER_EQUAL;
                if (!going_up) {
                    op = node.inclusive ? binary_op_t::LESS : binary_op_t::LESS_EQUAL;
                }

                return compare(op, next, end, span);
            });
        } else {
            done = by_direction([&](const bool going_up) {
                const type_id_t distance_type = unsigned_of(type);
                const operand_t remaining =
                    going_up ? wrapping(binary_op_t::SUBTRACT, end, variable, span)
                             : wrapping(binary_op_t::SUBTRACT, variable, end, span);
                const operand_t size =
                    going_up ? step : wrapping(binary_op_t::SUBTRACT, zero(type), step, span);
                const operand_t remaining_distance = cast(remaining, distance_type, span);
                const operand_t step_distance = cast(size, distance_type, span);
                return compare(node.inclusive ? binary_op_t::LESS : binary_op_t::LESS_EQUAL,
                               remaining_distance, step_distance, span);
            });
            next = wrapping(binary_op_t::ADD, variable, step, span);
        }

        const block_id_t latch = here();
        m_block.reset();
        const block_id_t advance = new_block();
        start(advance);
        emit(assign_t{ .target = variable, .value = use_t{ .value = next } });
        jump(body);

        const block_id_t exit = join(loop.breaks);
        block_at(test).terminator =
            branch_t{ .condition = in_range, .if_true = body, .if_false = exit };
        block_at(latch).terminator =
            branch_t{ .condition = done, .if_true = exit, .if_false = advance };
    }

    /// A copy of a variable, so later changes to it don't matter.
    operand_t stable(const operand_t& value) {
        if (std::holds_alternative<const_value_t>(value)) {
            return value;
        }

        return temporary(use_t{ .value = value }, type_of(value));
    }

    /// A Bool temporary for `left op right`. In MIR, && and || don't short-circuit: both sides
    /// are already values.
    operand_t compare(const binary_op_t op, const operand_t& left, const operand_t& right,
                      const span_t span) {
        return temporary(binary_t{ .op = op, .left = left, .right = right, .span = span },
                         type_id_t::BOOL);
    }

    /// A temporary for `left op right` that wraps on overflow, even in debug builds.
    operand_t wrapping(const binary_op_t op, const operand_t& left, const operand_t& right,
                       const span_t span) {
        return temporary(
            binary_t{ .op = op, .left = left, .right = right, .span = span, .wrapping = true },
            type_of(left));
    }

    operand_t cast(const operand_t& value, const type_id_t type, const span_t span) {
        return temporary(cast_t{ .value = value, .type = type, .span = span }, type);
    }

    /// `if_up` when `up` is True, else `if_down`, without branching.
    operand_t choose(const operand_t& up, const operand_t& if_up, const operand_t& if_down,
                     const span_t span) {
        const operand_t down = temporary(
            unary_t{ .op = unary_op_t::NOT, .operand = up, .span = span }, type_id_t::BOOL);
        const operand_t a = compare(binary_op_t::AND, up, if_up, span);
        const operand_t b = compare(binary_op_t::AND, down, if_down, span);
        return compare(binary_op_t::OR, a, b, span);
    }

    /// A temporary for a runtime call's result. `owned` when it returns a new String.
    operand_t runtime_call(const runtime_function_t function, std::vector<operand_t> arguments,
                           const type_id_t type, const bool owned = false) {
        return temporary(runtime_call_t{ .function = function, .arguments = std::move(arguments) },
                         type, owned);
    }

    void lower_stmt(stmt_id_t /*id*/, const span_t span, const select_stmt_t& node) {
        // The subject is evaluated once, into a local of the Select's own scope.
        push_scope();
        const type_id_t type = m_analysis.expr(node.subject).converted;
        const operand_t value = lower_value(node.subject);
        const local_id_t subject = add_local(type);
        initialize(subject, value);
        if (type == type_id_t::STRING) {
            m_scopes.back().push_back(subject);
        }

        finish_statement();

        std::vector<block_id_t> exits;
        for (const case_t& branch : node.cases) {
            /// One Case value's test, in `block`: a match goes to the body, anything else to
            /// `next`.
            struct test_t {
                block_id_t block;
                operand_t matched;
                block_id_t next;
            };

            std::vector<test_t> tests;
            for (const case_value_t& case_value : branch.values) {
                const operand_t matched = case_test(subject, case_value, span);
                const block_id_t block = here();
                m_block.reset();
                const block_id_t next = new_block();
                start(next);
                tests.push_back({ .block = block, .matched = matched, .next = next });
            }

            // The last value's failed test leads here: the next Case is tested, or the Default
            // runs.
            const block_id_t no_match = here();
            m_block.reset();
            const block_id_t body = new_block();
            for (const test_t& test : tests) {
                block_at(test.block).terminator =
                    branch_t{ .condition = test.matched, .if_true = body, .if_false = test.next };
            }

            start(body);
            lower_block(branch.body);
            leave(exits);
            start(no_match);
        }

        if (node.default_body) {
            lower_block(*node.default_body);
        }

        leave(exits);
        join(exits);
        pop_scope();
    }

    /// A Bool temporary: whether `subject` equals one Case value, or falls in its range.
    operand_t case_test(const local_id_t subject, const case_value_t& value, const span_t span) {
        const const_value_t low = expect(m_analysis.expr(value.value).value);
        if (low.type == type_id_t::STRING) {
            return runtime_call(runtime_function_t::EQUAL, { subject, low }, type_id_t::BOOL);
        }

        if (!value.range_end) {
            return compare(binary_op_t::EQUAL, subject, low, span);
        }

        const const_value_t high = expect(m_analysis.expr(*value.range_end).value);
        const operand_t above = compare(binary_op_t::GREATER_EQUAL, subject, low, span);
        const operand_t below = compare(binary_op_t::LESS_EQUAL, subject, high, span);
        return compare(binary_op_t::AND, above, below, span);
    }

    void lower_stmt(const stmt_id_t id, span_t /*span*/, const break_stmt_t& /*node*/) {
        lower_jump(id, true);
    }

    void lower_stmt(const stmt_id_t id, span_t /*span*/, const continue_stmt_t& /*node*/) {
        lower_jump(id, false);
    }

    /// Break or Continue: leaves the scopes inside the loop, releasing their Strings, and jumps
    /// to a target the loop fills in later.
    void lower_jump(const stmt_id_t id, const bool is_break) {
        const stmt_id_t target = expect(m_analysis.stmt(id).loop);
        loop_t& loop =
            *std::ranges::find_if(m_loops, [target](const loop_t& l) { return l.stmt == target; });
        release_scopes(loop.scope_depth);
        leave(is_break ? loop.breaks : loop.continues);
    }

    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const return_stmt_t& node) {
        std::optional<operand_t> result;
        if (node.value) {
            result = lower_value(*node.value);
            if (current().return_type == type_id_t::STRING) {
                take(*result); // the caller owns the String it gets back
            }
        }

        finish_statement();
        release_scopes(0);
        terminate(return_t{ .value = result });
    }

    // Expressions

    [[nodiscard]] span_t span_of(const expr_id_t id) const {
        return m_ast.get(id).span;
    }

    place_t place_of(const symbol_id_t symbol) {
        if (const auto found = m_locals.find(symbol); found != m_locals.end()) {
            return found->second;
        }

        return m_globals.at(symbol);
    }

    /// The value of an expression, converted to the type its context needs.
    operand_t lower_value(const expr_id_t id) {
        const expr_info_t& info = m_analysis.expr(id);
        if (info.value) {
            return *info.value;
        }

        operand_t value =
            std::visit([this, id](const auto& node) { return this->lower_expr(id, node); },
                       m_ast.get(id).node);
        if (info.converted != info.type) {
            value = cast(value, info.converted, span_of(id));
        }

        return value;
    }

    /// Literals always have a value, and expressions past M1 never pass analysis.
    template <typename Node>
    operand_t lower_expr(expr_id_t /*id*/, const Node& /*node*/) {
        assert(false);
        return const_value_t{};
    }

    operand_t lower_expr(const expr_id_t id, const name_expr_t& /*node*/) {
        return place_of(expect(m_analysis.expr(id).symbol));
    }

    /// Interpolation: every part becomes a String, then they're joined.
    operand_t lower_expr(expr_id_t /*id*/, const string_expr_t& node) {
        std::vector<operand_t> parts;
        for (const string_part_t& part : node.parts) {
            if (const auto* text = std::get_if<std::string>(&part)) {
                parts.emplace_back(make_string(*text));
                continue;
            }

            const expr_id_t inner = std::get<expr_id_t>(part);
            operand_t value = lower_value(inner);
            if (m_analysis.expr(inner).converted != type_id_t::STRING) {
                value =
                    runtime_call(runtime_function_t::TO_TEXT, { value }, type_id_t::STRING, true);
            }

            parts.push_back(std::move(value));
        }

        return runtime_call(runtime_function_t::JOIN, std::move(parts), type_id_t::STRING, true);
    }

    operand_t lower_expr(const expr_id_t id, const unary_expr_t& node) {
        const operand_t operand = lower_value(node.operand);
        return temporary(unary_t{ .op = node.op, .operand = operand, .span = span_of(id) },
                         m_analysis.expr(id).type);
    }

    operand_t lower_expr(const expr_id_t id, const binary_expr_t& node) {
        if (node.op == binary_op_t::AND || node.op == binary_op_t::OR) {
            return lower_logical(node);
        }

        const operand_t left = lower_value(node.left);
        const operand_t right = lower_value(node.right);
        // Strings only have +, ==, and !=, which go through the runtime.
        if (type_of(left) == type_id_t::STRING) {
            if (node.op == binary_op_t::ADD) {
                return runtime_call(runtime_function_t::CONCAT, { left, right }, type_id_t::STRING,
                                    true);
            }

            operand_t equal =
                runtime_call(runtime_function_t::EQUAL, { left, right }, type_id_t::BOOL);
            if (node.op == binary_op_t::EQUAL) {
                return equal;
            }

            return temporary(
                unary_t{ .op = unary_op_t::NOT, .operand = equal, .span = span_of(id) },
                type_id_t::BOOL);
        }

        return temporary(
            binary_t{ .op = node.op, .left = left, .right = right, .span = span_of(id) },
            m_analysis.expr(id).type);
    }

    /// && and || only evaluate the right side when it decides the result.
    operand_t lower_logical(const binary_expr_t& node) {
        const bool is_and = node.op == binary_op_t::AND;
        const local_id_t result = add_local(type_id_t::BOOL);
        const operand_t left = lower_value(node.left);
        const block_id_t test = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        const block_id_t decided = new_block();
        start(decided);
        emit(assign_t{ .target = result, .value = use_t{ .value = make_bool(!is_and) } });
        leave(exits);

        const block_id_t right_side = new_block();
        start(right_side);
        const std::size_t mark = m_temporaries.size();
        const operand_t right = lower_value(node.right);
        emit(assign_t{ .target = result, .value = use_t{ .value = right } });
        // The right side's temporaries only exist on this path, so they're released on it.
        release_temporaries_from(mark);
        leave(exits);

        block_at(test).terminator = branch_t{
            .condition = left,
            .if_true = is_and ? right_side : decided,
            .if_false = is_and ? decided : right_side,
        };
        join(exits);
        return place_t{ result };
    }

    /// Each branch writes the result and releases its own temporaries, since only one runs.
    operand_t lower_expr(const expr_id_t id, const ternary_expr_t& node) {
        const type_id_t type = m_analysis.expr(id).type;
        const local_id_t result = add_local(type);
        const operand_t condition = lower_value(node.condition);
        const block_id_t test = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        std::vector<block_id_t> starts;
        for (const expr_id_t branch : { node.if_true, node.if_false }) {
            starts.push_back(new_block());
            start(starts.back());
            const std::size_t mark = m_temporaries.size();
            const operand_t value = lower_value(branch);
            if (type == type_id_t::STRING) {
                take(value);
            }

            emit(assign_t{ .target = result, .value = use_t{ .value = value } });
            release_temporaries_from(mark);
            leave(exits);
        }

        block_at(test).terminator =
            branch_t{ .condition = condition, .if_true = starts[0], .if_false = starts[1] };
        join(exits);
        // The result owns its String, so it's released after the statement like a temporary.
        if (type == type_id_t::STRING) {
            m_temporaries.push_back(result);
        }

        return place_t{ result };
    }

    operand_t lower_expr(const expr_id_t id, const call_expr_t& /*node*/) {
        return expect(lower_call(id));
    }

    /// A call's result, or nullopt when it returns nothing. A String result is released after
    /// the statement unless something takes it.
    std::optional<operand_t> lower_call(const expr_id_t id) {
        const expr_info_t& info = m_analysis.expr(id);
        const symbol_t& function = m_analysis.symbol(expect(info.symbol));
        std::vector<call_argument_t> arguments;
        for (std::size_t i = 0; i < info.arguments.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            const std::optional<expr_id_t> argument = info.arguments[i];
            // A missing argument gets the parameter's default, which is known at compile time.
            // ByRef passes the variable itself.
            if (!argument) {
                arguments.push_back({ .value = expect(parameter.value) });
            } else if (parameter.by_ref) {
                const place_t variable = place_of(expect(m_analysis.expr(*argument).symbol));
                arguments.push_back({ .value = variable, .by_ref = true });
            } else {
                arguments.push_back({ .value = lower_value(*argument) });
            }
        }

        // Print is the only built-in function for now.
        if (function.kind == symbol_kind_t::BUILTIN_FUNCTION) {
            emit(evaluate_t{
                .value =
                    runtime_call_t{
                        .function = runtime_function_t::PRINT,
                        .arguments = { arguments[0].value },
                    },
            });
            return std::nullopt;
        }

        call_t call{
            .function = m_functions.at(expect(info.symbol)),
            .arguments = std::move(arguments),
        };
        if (function.type == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
            return std::nullopt;
        }

        return temporary(std::move(call), function.type, function.type == type_id_t::STRING);
    }

    operand_t lower_expr(const expr_id_t id, const cast_expr_t& node) {
        const operand_t value = lower_value(node.value);
        return cast(value, m_analysis.expr(id).type, span_of(id));
    }
};

} // namespace

mir_module_t lower(const ast_t& ast, const analysis_t& analysis) {
    return lowerer_t(ast, analysis).run();
}

} // namespace fb
