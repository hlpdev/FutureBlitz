#include "fb/sema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

std::size_t index(const expr_id_t id) {
    return std::to_underlying(id);
}

std::size_t index(const stmt_id_t id) {
    return std::to_underlying(id);
}

std::size_t index(const symbol_id_t id) {
    return std::to_underlying(id);
}

/// What's known at one point in the code: which Locals are assigned on every path that gets here.
struct flow_t {
    std::vector<bool> assigned; // by symbol id
    bool reachable = true;
};

/// Where two paths meet. A path that can't get here doesn't count.
flow_t join(const flow_t& a, const flow_t& b) {
    if (!a.reachable) {
        return b;
    }

    if (!b.reachable) {
        return a;
    }

    flow_t result;
    result.assigned.resize(std::min(a.assigned.size(), b.assigned.size()));
    for (std::size_t i = 0; i < result.assigned.size(); ++i) {
        result.assigned[i] = a.assigned[i] && b.assigned[i];
    }

    return result;
}

/// The flow after Return, Break, Continue, or a loop that never ends. join() ignores it.
const flow_t unreachable_flow{ .reachable = false };

/// A loop being checked, with the flow at each Break and Continue that targets it.
struct loop_t {
    stmt_id_t stmt;
    std::optional<name_t> label;
    std::vector<flow_t> breaks;
    std::vector<flow_t> continues;
};

/// How well an argument fits a parameter. Higher is better.
enum class match_t : std::uint8_t {
    NONE,
    CONVERTIBLE,
    EXACT,
};

/// For each parameter of a call, the index of its argument, or nullopt when it takes its default.
using binding_t = std::vector<std::optional<std::size_t>>;

/// One Case value, or a 'To' range of them.
struct case_range_t {
    const_value_t low;
    const_value_t high;
    span_t span;
};

/// Arithmetic, bitwise, and shift operators: the ones whose literal operands can take any
/// numeric type.
bool is_numeric_op(const binary_op_t op) {
    return op <= binary_op_t::BIT_XOR;
}

bool is_shift(const binary_op_t op) {
    return op == binary_op_t::SHIFT_LEFT || op == binary_op_t::SHIFT_RIGHT;
}

bool is_comparison(const binary_op_t op) {
    return op >= binary_op_t::EQUAL && op <= binary_op_t::GREATER_EQUAL;
}

bool is_ordering(const binary_op_t op) {
    return op >= binary_op_t::LESS && op <= binary_op_t::GREATER_EQUAL;
}

/// Whether `op` works on two operands of `type`.
bool supports(const binary_op_t op, const type_id_t type) {
    switch (op) {
    case binary_op_t::ADD:
        return is_numeric(type) || type == type_id_t::STRING;
    case binary_op_t::SUBTRACT:
    case binary_op_t::MULTIPLY:
    case binary_op_t::DIVIDE:
    case binary_op_t::REMAINDER:
    case binary_op_t::POWER:
    case binary_op_t::LESS:
    case binary_op_t::LESS_EQUAL:
    case binary_op_t::GREATER:
    case binary_op_t::GREATER_EQUAL:
        return is_numeric(type);
    case binary_op_t::BIT_AND:
    case binary_op_t::BIT_OR:
    case binary_op_t::BIT_XOR:
        return is_integer(type);
    case binary_op_t::EQUAL:
    case binary_op_t::NOT_EQUAL:
        return true;
    default:
        return false;
    }
}

bool supports(const unary_op_t op, const type_id_t type) {
    switch (op) {
    case unary_op_t::NEGATE:
        return is_signed(type);
    case unary_op_t::NOT:
        return type == type_id_t::BOOL;
    case unary_op_t::BIT_NOT:
        return is_integer(type);
    }

    std::unreachable();
}

/// The type both sides convert to losslessly, if there is one.
std::optional<type_id_t> common_type(const type_id_t a, const type_id_t b) {
    if (converts_losslessly(a, b)) {
        return b;
    }

    if (converts_losslessly(b, a)) {
        return a;
    }

    return std::nullopt;
}

bool is_zero(const const_value_t& value) {
    if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
        return *number == 0;
    }

    if (const auto* number = std::get_if<std::uint64_t>(&value.data)) {
        return *number == 0;
    }

    if (const auto* number = std::get_if<double>(&value.data)) {
        return *number == 0.0;
    }

    return false;
}

/// Integer ranges overlap when they share a value. Other Case values have no ranges, so they
/// overlap only when equal.
bool overlaps(const case_range_t& a, const case_range_t& b) {
    if (!is_integer(a.low.type)) {
        return a.low == b.low;
    }

    return compare(a.low, b.high) <= 0 && compare(b.low, a.high) <= 0;
}

/// Built-in types from the spec that come after M1.
bool is_later_type(const std::string_view name) {
    constexpr auto names = std::to_array<std::string_view>(
        { "Vec2", "Vec3", "Vec4", "Quat", "Mat3", "Mat4", "Color", "Void" });
    return std::ranges::contains(names, name);
}

/// Checks one file: resolves names, gives every expression a type, and tracks which Locals are
/// assigned. Expressions whose value is known are folded as they're checked, so Consts, Case
/// values, and default values can use them. Results go in an analysis_t for lowering.
class checker_t {
public:
    checker_t(const source_file_t& file, const ast_t& ast, diagnostic_bag_t& diagnostics)
        : m_file(file), m_ast(ast), m_diagnostics(&diagnostics) {
        m_analysis.exprs.resize(ast.expr_count());
        m_analysis.stmts.resize(ast.stmt_count());
    }

    /// Works in passes. Structs get their types first, so any declaration can use them. Then
    /// Functions, Globals, top-level Consts, and the Structs' members are declared, so they're
    /// visible anywhere in the file. Then Global and Const initializers and field defaults are
    /// checked in source order, then default values, and last the top-level code and each
    /// function body.
    analysis_t run() {
        declare_builtins();
        for (const stmt_id_t id : m_ast.items()) {
            declare_type(id);
        }

        for (const stmt_id_t id : m_ast.items()) {
            declare_item(id);
        }

        check_struct_cycles();
        initialize_globals();
        for (const stmt_id_t id : m_ast.items()) {
            check_defaults(id);
        }

        check_top_level();
        for (const stmt_id_t id : m_ast.items()) {
            check_function(id);
        }

        return std::move(m_analysis);
    }

private:
    const source_file_t& m_file;
    const ast_t& m_ast;
    diagnostic_bag_t* m_diagnostics; // swapped out while trying overloads
    analysis_t m_analysis;
    /// Functions, Globals, and top-level Consts by name. Only overloads share a name.
    std::unordered_map<std::string_view, std::vector<symbol_id_t>> m_file_scope;
    std::vector<std::vector<symbol_id_t>> m_scopes; // inside top-level code or a function
    std::vector<loop_t> m_loops;                    // innermost last
    std::optional<symbol_id_t> m_function;          // the function being checked
    /// Each Struct's symbol, by its type.
    std::unordered_map<type_id_t, symbol_id_t> m_structs;
    /// Fields without a type, until their defaults give them one.
    std::vector<symbol_id_t> m_untyped_fields;
    /// The flow at the code being checked.
    flow_t m_flow;
    bool m_initializing = false; // checking Global and Const initializers

    // Helpers

    template <typename... Args>
    diagnostic_t& error(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        return m_diagnostics->error(span, fmt, std::forward<Args>(args)...);
    }

    symbol_t& symbol(const symbol_id_t id) {
        return m_analysis.symbols[index(id)];
    }

    expr_info_t& info(const expr_id_t id) {
        return m_analysis.exprs[index(id)];
    }

    [[nodiscard]] span_t span_of(const expr_id_t id) const {
        return m_ast.get(id).span;
    }

    [[nodiscard]] std::string_view text(const expr_id_t id) const {
        return m_file.slice(span_of(id));
    }

    symbol_id_t add_symbol(symbol_t symbol) {
        const auto id = static_cast<symbol_id_t>(m_analysis.symbols.size());
        m_analysis.symbols.push_back(std::move(symbol));
        return id;
    }

    /// Runs `function` without reporting anything or changing the flow, to try something out.
    template <typename Function>
    auto quietly(Function&& function) {
        diagnostic_bag_t scratch;
        diagnostic_bag_t* const saved_diagnostics = std::exchange(m_diagnostics, &scratch);
        const flow_t saved_flow = m_flow;
        auto result = std::forward<Function>(function)();
        m_diagnostics = saved_diagnostics;
        m_flow = saved_flow;
        return result;
    }

    // Declarations

    /// The temporary built-in Print(String), until std.core exists. User functions can overload it.
    void declare_builtins() {
        const symbol_id_t value = add_symbol({
            .kind = symbol_kind_t::PARAMETER,
            .name = "value",
            .type = type_id_t::STRING,
        });
        const symbol_id_t print = add_symbol({
            .kind = symbol_kind_t::BUILTIN_FUNCTION,
            .name = "Print",
            .type = type_id_t::NONE,
            .parameters = { value },
        });
        m_file_scope["Print"].push_back(print);
    }

    /// Gives a Struct its type before anything else is declared, so types can use each other in
    /// any order.
    void declare_type(const stmt_id_t id) {
        const auto* node = std::get_if<struct_stmt_t>(&m_ast.get(id).node);
        if (node == nullptr) {
            return;
        }

        if (builtin_type(node->name.text)) {
            error(node->name.span, "'{}' is a built-in type", node->name.text);
        }

        const type_id_t type = m_analysis.types.add_struct(std::string(node->name.text));
        const symbol_id_t declared = add_symbol({
            .kind = symbol_kind_t::STRUCT,
            .name = node->name.text,
            .span = node->name.span,
            .type = type,
            .declaration = id,
        });
        m_analysis.stmts[index(id)].symbol = declared;
        m_structs[type] = declared;
        declare_in_file(declared);
    }

    [[nodiscard]] static bool is_global(const variable_stmt_t& node) {
        return node.kind == variable_kind_t::GLOBAL || node.kind == variable_kind_t::CONSTANT;
    }

    /// Puts a top-level function, Global, or Const in the file scope, and declares a Struct's
    /// members. Top-level Locals wait for check_top_level(), since only top-level code can see
    /// them.
    void declare_item(const stmt_id_t id) {
        const stmt_node_t& node = m_ast.get(id).node;
        if (const auto* function = std::get_if<function_stmt_t>(&node)) {
            if (function->kind == function_kind_t::FUNCTION) {
                declare_function(id, *function);
            } else {
                report_misplaced_member(*function);
            }
        } else if (const auto* variable = std::get_if<variable_stmt_t>(&node);
                   variable != nullptr && is_global(*variable)) {
            declare_global(id, *variable);
        } else if (const auto* declared = std::get_if<struct_stmt_t>(&node)) {
            declare_members(id, *declared);
        }
    }

    void report_misplaced_member(const function_stmt_t& node) {
        if (node.kind == function_kind_t::CONSTRUCTOR) {
            error(node.name.span, "New can only be declared in a Struct");
        } else {
            error(node.name.span, "Methods can only be declared in a Struct");
        }
    }

    /// A Struct's fields, Methods, and News. A Struct without a New gets a constructor whose
    /// parameters are its fields.
    void declare_members(const stmt_id_t id, const struct_stmt_t& node) {
        const std::optional<symbol_id_t> declared_struct = m_analysis.stmts[index(id)].symbol;
        if (!declared_struct) {
            return;
        }

        const symbol_id_t owner_id = *declared_struct;
        const type_id_t owner = symbol(owner_id).type;
        std::vector<symbol_id_t> members;
        std::vector<field_t> fields;
        bool has_new = false;
        for (const stmt_id_t member_id : node.members) {
            const stmt_t& member = m_ast.get(member_id);
            const auto* field = std::get_if<variable_stmt_t>(&member.node);
            const auto* function = std::get_if<function_stmt_t>(&member.node);
            if (field != nullptr && field->kind == variable_kind_t::FIELD) {
                const symbol_id_t declared = declare_field(member_id, *field, owner, fields.size());
                fields.push_back(
                    { .name = std::string(field->name.text), .type = symbol(declared).type });
                add_member(members, declared);
            } else if (function != nullptr && function->kind != function_kind_t::FUNCTION) {
                has_new = has_new || function->kind == function_kind_t::CONSTRUCTOR;
                add_member(members, declare_function(member_id, *function, owner));
            } else if (!std::holds_alternative<error_stmt_t>(member.node)) {
                error(member.span, "a Struct can only hold Fields, Methods, and News");
            }
        }

        m_analysis.types.set_fields(owner, std::move(fields));
        if (!has_new) {
            std::vector<symbol_id_t> parameters;
            for (const symbol_id_t member : members) {
                if (symbol(member).kind == symbol_kind_t::FIELD) {
                    parameters.push_back(member);
                }
            }

            members.push_back(add_symbol({
                .kind = symbol_kind_t::CONSTRUCTOR,
                .name = "New",
                .span = node.name.span,
                .type = owner,
                .parameters = std::move(parameters),
                .owner = owner,
            }));
        }

        symbol(owner_id).members = std::move(members);
    }

    /// A field is also a parameter of the constructor its Struct gets, so it always has a
    /// default: its own, or zero. Without a type, it takes its default's, once that's checked.
    symbol_id_t declare_field(const stmt_id_t id, const variable_stmt_t& node,
                              const type_id_t owner, const std::size_t position) {
        if (!node.type && !node.value) {
            error(node.name.span, "'{}' needs a type or a value", node.name.text);
        }

        const symbol_id_t field = add_symbol({
            .kind = symbol_kind_t::FIELD,
            .name = node.name.text,
            .span = node.name.span,
            .type = node.type ? resolve_type(*node.type) : type_id_t::INVALID,
            .has_default = true,
            .declaration = id,
            .owner = owner,
            .field = static_cast<std::uint32_t>(position),
        });
        m_analysis.stmts[index(id)].symbol = field;
        if (!node.type && node.value) {
            m_untyped_fields.push_back(field);
        }

        return field;
    }

    [[nodiscard]] static bool is_callable_member(const symbol_t& symbol) {
        return symbol.kind == symbol_kind_t::METHOD || symbol.kind == symbol_kind_t::CONSTRUCTOR;
    }

    /// Like declare_in_file(), among one Struct's members: Methods, and News, can share a name
    /// when their parameter types differ.
    void add_member(std::vector<symbol_id_t>& members, const symbol_id_t id) {
        const symbol_t& declared = symbol(id);
        if (declared.name.empty()) {
            return;
        }

        for (const symbol_id_t other_id : members) {
            const symbol_t& other = symbol(other_id);
            if (other.name != declared.name) {
                continue;
            }

            const bool overloads = is_callable_member(declared) && is_callable_member(other);
            if (overloads && !same_parameters(declared, other)) {
                continue;
            }

            error(declared.span, "'{}' is already declared{}", declared.name,
                  overloads ? " with the same parameter types" : "")
                .with_label(other.span, "first declared here");
            return;
        }

        members.push_back(id);
    }

    /// A Struct is stored inline, so it can't contain itself, directly or through other Structs.
    void check_struct_cycles() {
        for (const stmt_id_t id : m_ast.items()) {
            const std::optional<symbol_id_t> owner = m_analysis.stmts[index(id)].symbol;
            if (!std::holds_alternative<struct_stmt_t>(m_ast.get(id).node) || !owner) {
                continue;
            }

            const type_id_t type = symbol(*owner).type;
            for (const symbol_id_t member : symbol(*owner).members) {
                std::vector<type_id_t> seen;
                if (symbol(member).kind == symbol_kind_t::FIELD &&
                    contains(symbol(member).type, type, seen)) {
                    error(symbol(member).span,
                          "'{}' can't contain itself; a Struct is stored inline", name_of(type));
                    set_field_type(member, type_id_t::INVALID);
                }
            }
        }
    }

    /// Whether a value of `type` holds a `target` inside, counting `type` itself.
    bool contains(const type_id_t type, const type_id_t target, std::vector<type_id_t>& seen) {
        if (type == target) {
            return true;
        }

        if (std::ranges::contains(seen, type)) {
            return false;
        }

        seen.push_back(type);
        return std::ranges::any_of(m_analysis.types.get(type).fields, [&](const field_t& field) {
            return contains(field.type, target, seen);
        });
    }

    void set_field_type(const symbol_id_t id, const type_id_t type) {
        symbol(id).type = type;
        const symbol_t& field = symbol(id);
        std::vector<field_t> fields = m_analysis.types.get(field.owner).fields;
        fields[field.field].type = type;
        m_analysis.types.set_fields(field.owner, std::move(fields));
    }

    /// A Function, or a Struct's Method or New when `owner` is set. A Method's Self is a
    /// parameter that isn't among the others, ByRef in a "ByRef Method". New makes Self as a
    /// Local and returns it.
    symbol_id_t declare_function(const stmt_id_t id, const function_stmt_t& node,
                                 const type_id_t owner = type_id_t::INVALID) {
        std::vector<symbol_id_t> parameters;
        for (const parameter_t& parameter : node.parameters) {
            if (parameter.by_ref && parameter.default_value) {
                error(span_of(*parameter.default_value),
                      "a ByRef parameter can't have a default value");
            }

            // A ByRef parameter's default was reported above and is ignored, so calls must
            // still pass a variable.
            parameters.push_back(add_symbol({
                .kind = symbol_kind_t::PARAMETER,
                .name = parameter.name.text,
                .span = parameter.name.span,
                .type = resolve_type(parameter.type),
                .by_ref = parameter.by_ref,
                .has_default = parameter.default_value.has_value() && !parameter.by_ref,
            }));
        }

        const bool constructor = node.kind == function_kind_t::CONSTRUCTOR;
        const bool by_ref = node.kind == function_kind_t::BYREF_METHOD;
        // The parser already reported a return type on New.
        type_id_t return_type =
            node.return_type ? resolve_type(*node.return_type) : type_id_t::NONE;
        if (constructor) {
            return_type = owner;
        }

        std::optional<symbol_id_t> self;
        if (owner != type_id_t::INVALID) {
            self = add_symbol({
                .kind = constructor ? symbol_kind_t::LOCAL : symbol_kind_t::PARAMETER,
                .name = "Self",
                .span = node.name.span,
                .type = owner,
                .by_ref = by_ref,
            });
        }

        symbol_kind_t kind = symbol_kind_t::FUNCTION;
        if (owner != type_id_t::INVALID) {
            kind = constructor ? symbol_kind_t::CONSTRUCTOR : symbol_kind_t::METHOD;
        }

        const symbol_id_t function = add_symbol({
            .kind = kind,
            .name = node.name.text,
            .span = node.name.span,
            .type = return_type,
            .by_ref = by_ref,
            .parameters = std::move(parameters),
            .declaration = id,
            .owner = owner,
            .self = self,
        });
        m_analysis.stmts[index(id)].symbol = function;
        m_analysis.functions.push_back(function);
        if (owner == type_id_t::INVALID) {
            declare_in_file(function);
        }

        return function;
    }

    /// A Global or top-level Const. Without an annotation, its type comes from the initializer
    /// later, in initialize_globals().
    void declare_global(const stmt_id_t id, const variable_stmt_t& node) {
        const bool is_global = node.kind == variable_kind_t::GLOBAL;
        const symbol_id_t global = add_symbol({
            .kind = is_global ? symbol_kind_t::GLOBAL : symbol_kind_t::CONSTANT,
            .name = node.name.text,
            .span = node.name.span,
            .type = node.type ? resolve_type(*node.type) : type_id_t::INVALID,
            .declaration = id,
        });
        m_analysis.stmts[index(id)].symbol = global;
        m_analysis.globals.push_back(global);
        declare_in_file(global);
    }

    [[nodiscard]] static bool is_function(const symbol_t& symbol) {
        return symbol.kind == symbol_kind_t::FUNCTION ||
               symbol.kind == symbol_kind_t::BUILTIN_FUNCTION;
    }

    /// Overloads differ by parameter types only, not by names or ByRef.
    [[nodiscard]] bool same_parameters(const symbol_t& a, const symbol_t& b) {
        const auto type_of = [this](const symbol_id_t id) { return symbol(id).type; };
        return std::ranges::equal(a.parameters, b.parameters, {}, type_of, type_of);
    }

    /// Functions can share a name when their parameter types differ; nothing else can.
    void declare_in_file(const symbol_id_t id) {
        const symbol_t& declared = symbol(id);
        if (declared.name.empty()) {
            return; // the parser already reported it
        }

        std::vector<symbol_id_t>& existing = m_file_scope[declared.name];
        for (const symbol_id_t other_id : existing) {
            const symbol_t& other = symbol(other_id);
            const bool overloads = is_function(declared) && is_function(other);
            if (overloads && !same_parameters(declared, other)) {
                continue;
            }

            diagnostic_t& diagnostic =
                error(declared.span, "'{}' is already declared{}", declared.name,
                      overloads ? " with the same parameter types" : "");
            // A built-in has no declaration to point at.
            if (other.kind != symbol_kind_t::BUILTIN_FUNCTION) {
                diagnostic.with_label(other.span, "first declared here");
            }

            return;
        }

        existing.push_back(id);
    }

    void push_scope() {
        m_scopes.emplace_back();
    }

    void pop_scope() {
        m_scopes.pop_back();
    }

    /// A Local, parameter, or loop variable visible here, searching the innermost scope first.
    [[nodiscard]] std::optional<symbol_id_t> find_local(const std::string_view name) {
        for (const std::vector<symbol_id_t>& scope : std::views::reverse(m_scopes)) {
            for (const symbol_id_t id : scope) {
                if (symbol(id).name == name) {
                    return id;
                }
            }
        }

        return std::nullopt;
    }

    /// Puts a symbol in the innermost scope. Locals can't share a name with any other local
    /// they can see.
    void enter_scope(const symbol_id_t id) {
        const symbol_t& declared = symbol(id);
        if (declared.name.empty()) {
            return;
        }

        for (std::size_t i = m_scopes.size(); i-- > 0;) {
            for (const symbol_id_t other : m_scopes[i]) {
                if (symbol(other).name != declared.name) {
                    continue;
                }

                const bool same_scope = i + 1 == m_scopes.size();
                error(declared.span, "'{}' is already declared in {}", declared.name,
                      same_scope ? "this scope" : "an enclosing block")
                    .with_label(symbol(other).span, "first declared here");
                return;
            }
        }

        m_scopes.back().push_back(id);
    }

    symbol_id_t declare_local(symbol_t declared) {
        const symbol_id_t id = add_symbol(std::move(declared));
        enter_scope(id);
        return id;
    }

    // Types

    /// How errors show a type, like "Int32" or "String[]".
    [[nodiscard]] std::string_view name_of(const type_id_t type) const {
        return m_analysis.types.name(type);
    }

    [[nodiscard]] bool is_struct(const type_id_t type) const {
        return m_analysis.types.get(type).kind == type_kind_t::STRUCT;
    }

    /// The type an annotation names: a built-in or a Struct. Types that don't exist yet are
    /// reported and give INVALID.
    type_id_t resolve_type(const type_expr_id_t id) {
        const type_expr_t& type = m_ast.get(id);
        if (const auto* named = std::get_if<named_type_t>(&type.node)) {
            if (const std::optional<type_id_t> builtin = builtin_type(named->name)) {
                return *builtin;
            }

            if (const auto found = m_file_scope.find(named->name); found != m_file_scope.end()) {
                const symbol_t& declared = symbol(found->second.front());
                if (declared.kind == symbol_kind_t::STRUCT) {
                    return declared.type;
                }

                error(type.span, "'{}' isn't a type", named->name);
            } else if (is_later_type(named->name)) {
                error(type.span, "'{}' isn't supported yet", named->name);
            } else {
                error(type.span, "'{}' isn't a type", named->name);
            }
        } else if (std::holds_alternative<nullable_type_t>(type.node)) {
            error(type.span, "nullable types aren't supported yet");
        } else if (std::holds_alternative<array_type_t>(type.node)) {
            error(type.span, "arrays aren't supported yet");
        }

        return type_id_t::INVALID;
    }

    // Flow

    void mark_assigned(const symbol_id_t id) {
        // Symbols are added while checking, so the list grows to fit.
        if (m_flow.assigned.size() <= index(id)) {
            m_flow.assigned.resize(index(id) + 1);
        }

        m_flow.assigned[index(id)] = true;
    }

    /// In code that can't be reached, everything counts as assigned.
    [[nodiscard]] bool is_assigned(const symbol_id_t id) const {
        return !m_flow.reachable ||
               (index(id) < m_flow.assigned.size() && m_flow.assigned[index(id)]);
    }

    /// Reading a Local needs it assigned on every path. While Globals and Consts initialize, only
    /// the ones above can be read.
    void check_read(const symbol_id_t id, const span_t span) {
        if (is_assigned(id)) {
            return;
        }

        const symbol_t& target = symbol(id);
        if (target.kind == symbol_kind_t::LOCAL) {
            error(span, "'{}' is read before it's assigned", target.name);
        } else if ((target.kind == symbol_kind_t::GLOBAL ||
                    target.kind == symbol_kind_t::CONSTANT) &&
                   m_initializing) {
            error(span, "'{}' isn't initialized yet; Globals and Consts initialize in source order",
                  target.name);
        } else {
            return;
        }

        mark_assigned(id); // one error per variable is enough
    }

    // Top level

    /// Checks Global and top-level Const initializers in source order. Each one is marked
    /// assigned after its initializer, so reading one declared further down is an error.
    void initialize_globals() {
        m_initializing = true;
        for (const stmt_id_t id : m_ast.items()) {
            if (const auto* declared = std::get_if<struct_stmt_t>(&m_ast.get(id).node)) {
                for (const stmt_id_t member : declared->members) {
                    check_field_default(member);
                }

                continue;
            }

            const auto* node = std::get_if<variable_stmt_t>(&m_ast.get(id).node);
            const std::optional<symbol_id_t> global = m_analysis.stmts[index(id)].symbol;
            if (node == nullptr || !global) {
                continue; // top-level Locals get their symbols with the rest of the code
            }

            symbol_t declared = symbol(*global);
            check_initializer(*node, declared);
            symbol(*global) = std::move(declared);
            mark_assigned(*global);
        }

        m_initializing = false;
    }

    /// A field's default is a parameter default of the constructor its Struct gets, so it must be
    /// known at compile time. Field defaults are checked in source order with the Globals, so
    /// they can use Consts declared above the Struct.
    void check_field_default(const stmt_id_t id) {
        const auto* node = std::get_if<variable_stmt_t>(&m_ast.get(id).node);
        const std::optional<symbol_id_t> field = m_analysis.stmts[index(id)].symbol;
        if (node == nullptr || node->kind != variable_kind_t::FIELD || !field || !node->value) {
            return;
        }

        const expr_id_t value = *node->value;
        if (node->type) {
            const type_id_t type = symbol(*field).type;
            check(value, type);
            if (!expect_type(value, type)) {
                return;
            }
        } else {
            set_field_type(*field, check(value, type_id_t::NONE));
            std::erase(m_untyped_fields, *field);
        }

        if (info(value).value) {
            symbol(*field).value = info(value).value;
        } else if (info(value).type != type_id_t::INVALID) {
            error(span_of(value), "a field's default must be known at compile time");
        }
    }

    /// A field without a type gets its default's when its Struct's defaults are checked. Global
    /// and Const initializers above the Struct can't use the field before that.
    bool check_field_ready(const symbol_id_t field, const span_t span) {
        if (!std::ranges::contains(m_untyped_fields, field)) {
            return true;
        }

        error(span, "'{}' takes its type from its default, so it can't be used above its Struct",
              symbol(field).name);
        std::erase(m_untyped_fields, field); // reported once is enough
        return false;
    }

    /// Default values are checked before any code, since every call needs them.
    void check_defaults(const stmt_id_t id) {
        if (const auto* declared = std::get_if<struct_stmt_t>(&m_ast.get(id).node)) {
            for (const stmt_id_t member : declared->members) {
                check_defaults(member);
            }

            return;
        }

        const auto* node = std::get_if<function_stmt_t>(&m_ast.get(id).node);
        const std::optional<symbol_id_t> function = m_analysis.stmts[index(id)].symbol;
        if (node == nullptr || !function) {
            return;
        }

        for (std::size_t i = 0; i < node->parameters.size(); ++i) {
            const symbol_id_t parameter = symbol(*function).parameters[i];
            const std::optional<expr_id_t> default_value = node->parameters[i].default_value;
            if (!default_value || !symbol(parameter).has_default) {
                continue;
            }

            const expr_id_t value = *default_value;
            const type_id_t type = symbol(parameter).type;
            check(value, type);
            if (!expect_type(value, type)) {
                continue;
            }

            if (info(value).value) {
                symbol(parameter).value = info(value).value;
            } else if (info(value).type != type_id_t::INVALID) {
                error(span_of(value), "a default value must be known at compile time");
            }
        }
    }

    /// The top-level statements, in order. Globals, top-level Consts, and functions were handled
    /// by the earlier passes.
    void check_top_level() {
        m_flow = {};
        m_scopes.assign(1, {});
        for (const stmt_id_t id : m_ast.items()) {
            const stmt_node_t& node = m_ast.get(id).node;
            const auto* variable = std::get_if<variable_stmt_t>(&node);
            const bool initialized_already = variable != nullptr && is_global(*variable);
            if (!initialized_already && !std::holds_alternative<function_stmt_t>(node) &&
                !std::holds_alternative<struct_stmt_t>(node)) {
                check_stmt(id);
            }
        }

        m_scopes.clear();
    }

    /// A function body starts with a fresh scope holding only its parameters, so it can't see
    /// top-level Locals. A Method or New also sees its Struct's members, through Self.
    void check_function(const stmt_id_t id) {
        if (const auto* declared = std::get_if<struct_stmt_t>(&m_ast.get(id).node)) {
            for (const stmt_id_t member : declared->members) {
                check_function(member);
            }

            return;
        }

        const auto* node = std::get_if<function_stmt_t>(&m_ast.get(id).node);
        const std::optional<symbol_id_t> function_id = m_analysis.stmts[index(id)].symbol;
        if (node == nullptr || !function_id) {
            return;
        }

        const symbol_t function = symbol(*function_id); // a copy: checking the body adds symbols
        m_function = function_id;
        m_flow = {};
        m_scopes.assign(1, {});
        if (function.self) {
            mark_assigned(*function.self); // New's Self starts with the fields' defaults
        }

        for (const symbol_id_t parameter : function.parameters) {
            enter_scope(parameter);
        }

        check_statements(node->body);
        // Reaching the end of the body is a path without a Return. An INVALID return type was
        // already reported, and New returns Self.
        const bool returns_value = function.kind != symbol_kind_t::CONSTRUCTOR &&
                                   function.type != type_id_t::NONE &&
                                   function.type != type_id_t::INVALID;
        if (returns_value && m_flow.reachable) {
            error(function.span, "'{}' doesn't return a value on every path", function.name);
        }

        m_scopes.clear();
        m_function.reset();
    }

    // Statements

    void check_statements(const block_t& block) {
        for (const stmt_id_t id : block) {
            check_stmt(id);
        }
    }

    void check_block(const block_t& block) {
        push_scope();
        check_statements(block);
        pop_scope();
    }

    void check_stmt(const stmt_id_t id) {
        const stmt_t& stmt = m_ast.get(id);
        std::visit([this, id, &stmt](const auto& node) { this->check_stmt(id, stmt.span, node); },
                   stmt.node);
    }

    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const error_stmt_t& /*node*/) {}

    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const expression_stmt_t& node) {
        check_any(node.expression, type_id_t::NONE);
    }

    void check_stmt(stmt_id_t /*id*/, const span_t span, const assign_stmt_t& node) {
        const std::optional<target_t> target = check_target(node.target);
        if (!target) {
            check_any(node.value, type_id_t::NONE);
            return;
        }

        const type_id_t type = target->type;
        if (!node.op) {
            check(node.value, type);
            expect_type(node.value, type);
            if (target->whole) {
                mark_assigned(*target->whole);
            }

            return;
        }

        // "x op= y" works like "x = x op y", and the result has to fit back into x.
        const type_id_t result =
            is_shift(*node.op) ? check_shift(node.target, node.value, type, span)
                               : check_operation(*node.op, node.target, node.value, type, span);
        if (result != type_id_t::INVALID && type != type_id_t::INVALID && result != type) {
            report_mismatch(node.value, type);
        }
    }

    void check_stmt(stmt_id_t /*id*/, const span_t span, const increment_stmt_t& node) {
        if (!check_target(node.target)) {
            return;
        }

        const type_id_t type = check(node.target, type_id_t::NONE);
        if (type != type_id_t::INVALID && !is_numeric(type)) {
            error(span, "'{}' can't be used on {}", node.decrement ? "--" : "++", name_of(type));
        }
    }

    void check_stmt(const stmt_id_t id, const span_t span, const variable_stmt_t& node) {
        if (node.kind == variable_kind_t::GLOBAL) {
            error(span, "Globals can only be declared at the top level");
        } else if (node.kind == variable_kind_t::FIELD) {
            error(node.name.span, "Fields can only be declared in a Struct");
            return;
        }

        symbol_t declared{
            .kind = node.kind == variable_kind_t::CONSTANT ? symbol_kind_t::CONSTANT
                                                           : symbol_kind_t::LOCAL,
            .name = node.name.text,
            .span = node.name.span,
            .type = node.type ? resolve_type(*node.type) : type_id_t::INVALID,
            .declaration = id,
        };
        // Declared after its initializer is checked, so the initializer can't refer to it.
        check_initializer(node, declared);
        const symbol_id_t local = declare_local(std::move(declared));
        m_analysis.stmts[index(id)].symbol = local;
        if (node.value) {
            mark_assigned(local);
        }
    }

    /// The type comes from the annotation, or else from the initializer. A Const keeps the value
    /// when it's known at compile time.
    void check_initializer(const variable_stmt_t& node, symbol_t& declared) {
        const bool annotated = node.type.has_value();
        if (!node.value) {
            if (!annotated) {
                error(node.name.span, "'{}' needs a type or a value", node.name.text);
            } else if (declared.kind == symbol_kind_t::CONSTANT) {
                error(node.name.span, "'{}' is a Const, so it needs a value", node.name.text);
            }

            return;
        }

        if (annotated) {
            check(*node.value, declared.type);
            expect_type(*node.value, declared.type);
        } else {
            declared.type = check(*node.value, type_id_t::NONE);
        }

        if (declared.kind == symbol_kind_t::CONSTANT) {
            declared.value = info(*node.value).value;
        }
    }

    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const function_stmt_t& node) {
        if (node.kind == function_kind_t::FUNCTION) {
            error(node.name.span, "Functions can only be declared at the top level");
        } else {
            report_misplaced_member(node);
        }
    }

    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const struct_stmt_t& node) {
        error(node.name.span, "Structs can only be declared at the top level");
    }

    /// Each condition and body starts from the flow before the If, since a branch only runs when
    /// the ones before it didn't. After it, a Local counts as assigned only if every way through
    /// assigned it, including skipping every branch when there's no Else.
    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const if_stmt_t& node) {
        const flow_t entry = m_flow;
        flow_t result = unreachable_flow;
        for (const if_branch_t& branch : node.branches) {
            m_flow = entry;
            check_condition(branch.condition);
            check_block(branch.body);
            result = join(result, m_flow);
        }

        m_flow = entry;
        if (node.else_body) {
            check_block(*node.else_body);
        }

        m_flow = join(result, m_flow);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const while_stmt_t& node) {
        check_condition(node.condition);
        const bool forever = is_constant(node.condition, true);
        const flow_t entry = m_flow;
        push_loop(id, node.label);
        check_block(node.body);
        const loop_t loop = pop_loop();
        // The body may not run, so only what was assigned before the loop counts after it. Code
        // after a While True is reached only through Break.
        m_flow = forever ? unreachable_flow : entry;
        leave_loop(loop);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const repeat_stmt_t& node) {
        push_loop(id, node.label);
        check_block(node.body);
        const loop_t loop = pop_loop();
        // The body runs at least once, so its assignments count. Until is reached from the end
        // of the body and from every Continue, and it can't see the body's Locals.
        for (const flow_t& path : loop.continues) {
            m_flow = join(m_flow, path);
        }

        check_condition(node.condition);
        if (is_constant(node.condition, false)) {
            m_flow = unreachable_flow;
        }

        leave_loop(loop);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const loop_stmt_t& node) {
        push_loop(id, node.label);
        check_block(node.body);
        const loop_t loop = pop_loop();
        // Code after a Loop is reached only through Break.
        m_flow = unreachable_flow;
        leave_loop(loop);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const for_stmt_t& node) {
        // The variable takes the start's type, unless the start is a literal and the end isn't.
        type_id_t type = type_id_t::INVALID;
        if (is_flexible(node.start) && !is_flexible(node.end)) {
            type = check(node.start, check(node.end, type_id_t::NONE));
        } else {
            type = check(node.start, type_id_t::NONE);
            check(node.end, type);
        }

        if (type != type_id_t::INVALID && !is_numeric(type)) {
            error(span_of(node.start), "a For loop counts with numbers, not {}", name_of(type));
            type = type_id_t::INVALID;
        }

        expect_type(node.end, type);
        if (node.step) {
            const expr_id_t step = *node.step;
            check(step, type);
            const std::optional<const_value_t>& value = info(step).value;
            if (expect_type(step, type) && value && is_zero(*value)) {
                error(span_of(step), "Step can't be 0");
            }
        }

        // The variable gets a scope of its own around the body's. The body may not run, so the
        // flow after the loop is the flow before it, plus any Breaks.
        const flow_t entry = m_flow;
        push_loop(id, node.label);
        push_scope();
        const symbol_id_t variable = declare_local({
            .kind = symbol_kind_t::LOOP_VARIABLE,
            .name = node.variable.text,
            .span = node.variable.span,
            .type = type,
            .declaration = id,
        });
        m_analysis.stmts[index(id)].symbol = variable;
        check_block(node.body);
        pop_scope();
        const loop_t loop = pop_loop();
        m_flow = entry;
        leave_loop(loop);
    }

    void check_stmt(const stmt_id_t id, const span_t span, const for_in_stmt_t& node) {
        const span_t header = {
            .file = span.file,
            .begin = span.begin,
            .end = span_of(node.sequence).end,
        };
        error(header, "'For ... In' isn't supported yet");

        // The body is still checked, with variables that have no type.
        const flow_t entry = m_flow;
        push_loop(id, node.label);
        push_scope();
        for (const std::optional<name_t>& name : { node.index, std::optional(node.value) }) {
            if (name) {
                declare_local({
                    .kind = symbol_kind_t::LOOP_VARIABLE,
                    .name = name->text,
                    .span = name->span,
                    .declaration = id,
                });
            }
        }

        check_block(node.body);
        pop_scope();
        const loop_t loop = pop_loop();
        m_flow = entry;
        leave_loop(loop);
    }

    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const select_stmt_t& node) {
        type_id_t type = check(node.subject, type_id_t::NONE);
        if (type != type_id_t::INVALID && !is_integer(type) && type != type_id_t::BOOL &&
            type != type_id_t::STRING) {
            error(span_of(node.subject), "Select works on integers, Bool, and String, not {}",
                  name_of(type));
            type = type_id_t::INVALID;
        }

        const flow_t entry = m_flow;
        flow_t result = unreachable_flow;
        std::vector<case_range_t> seen;
        for (const case_t& branch : node.cases) {
            for (const case_value_t& value : branch.values) {
                check_case_value(value, type, seen);
            }

            m_flow = entry;
            check_block(branch.body);
            result = join(result, m_flow);
        }

        m_flow = entry;
        if (node.default_body) {
            check_block(*node.default_body);
        }

        m_flow = join(result, m_flow);
    }

    /// Checks one Case value or 'To' range, and that no earlier Case in `seen` covers it.
    void check_case_value(const case_value_t& value, const type_id_t type,
                          std::vector<case_range_t>& seen) {
        const std::optional<const_value_t> low = case_constant(value.value, type);
        std::optional<const_value_t> high = low;
        span_t span = span_of(value.value);
        if (value.range_end) {
            high = case_constant(*value.range_end, type);
            span = merge(span, span_of(*value.range_end));
            if (type != type_id_t::INVALID && !is_integer(type)) {
                error(span, "'To' ranges only work on integers");
                return;
            }
        }

        if (!low || !high) {
            return;
        }

        if (value.range_end && compare(*low, *high) > 0) {
            error(span, "this range is empty");
            return;
        }

        const case_range_t range{ .low = *low, .high = *high, .span = span };
        for (const case_range_t& earlier : seen) {
            if (overlaps(range, earlier)) {
                error(span, "this Case value is already covered by an earlier Case")
                    .with_label(earlier.span, "covered here");
                return;
            }
        }

        seen.push_back(range);
    }

    /// A Case value's compile-time value, which must have the subject's type. nullopt when it
    /// can't be used.
    std::optional<const_value_t> case_constant(const expr_id_t id, const type_id_t type) {
        check(id, type);
        if (type == type_id_t::INVALID || !expect_type(id, type)) {
            return std::nullopt;
        }

        if (!info(id).value) {
            error(span_of(id), "a Case value must be known at compile time");
        }

        return info(id).value;
    }

    void check_stmt(const stmt_id_t id, const span_t span, const break_stmt_t& node) {
        check_jump(id, span, node.label, true);
    }

    void check_stmt(const stmt_id_t id, const span_t span, const continue_stmt_t& node) {
        check_jump(id, span, node.label, false);
    }

    /// Break and Continue target the innermost loop, or the enclosing one with their label. The
    /// target is recorded for lowering, and the flow here is saved on it: Breaks reach the code
    /// after the loop, and Continues reach a Repeat's Until.
    void check_jump(const stmt_id_t id, const span_t span, const std::optional<name_t>& label,
                    const bool is_break) {
        const std::string_view keyword = is_break ? "Break" : "Continue";
        auto loop = m_loops.rbegin();
        if (label) {
            loop = std::ranges::find_if(m_loops.rbegin(), m_loops.rend(), [&](const loop_t& l) {
                return l.label && l.label->text == label->text;
            });
        }

        if (m_loops.empty()) {
            error(span, "{} must be inside a loop", keyword);
        } else if (label && loop == m_loops.rend()) {
            error(label->span, "no enclosing loop is labeled '{}'", label->text);
        } else {
            m_analysis.stmts[index(id)].loop = loop->stmt;
            (is_break ? loop->breaks : loop->continues).push_back(m_flow);
        }

        m_flow = unreachable_flow;
    }

    /// In a function with a return type, Return needs a value; in one without, it can't have
    /// one. In top-level code, a bare Return ends the program.
    void check_stmt(stmt_id_t /*id*/, const span_t span, const return_stmt_t& node) {
        if (!m_function) {
            if (node.value) {
                error(span_of(*node.value), "top-level code can't return a value");
                check_any(*node.value, type_id_t::NONE);
            }
        } else if (const symbol_t& function = symbol(*m_function);
                   function.kind == symbol_kind_t::CONSTRUCTOR) {
            if (node.value) {
                error(span_of(*node.value), "New can't return a value; it returns Self");
                check_any(*node.value, type_id_t::NONE);
            }
        } else {
            const std::string_view name = function.name;
            const type_id_t type = function.type;
            if (type == type_id_t::NONE && node.value) {
                error(span_of(*node.value), "'{}' has no return type, so it can't return a value",
                      name);
                check_any(*node.value, type_id_t::NONE);
            } else if (node.value) {
                check(*node.value, type);
                expect_type(*node.value, type);
            } else if (type != type_id_t::NONE && type != type_id_t::INVALID) {
                error(span, "'{}' returns {}, so Return needs a value", name, name_of(type));
            }
        }

        m_flow = unreachable_flow;
    }

    bool check_condition(const expr_id_t id) {
        check(id, type_id_t::BOOL);
        return expect_type(id, type_id_t::BOOL);
    }

    /// Whether a condition is always `value`, like the True in "While True".
    [[nodiscard]] bool is_constant(const expr_id_t id, const bool value) {
        return info(id).value == make_bool(value);
    }

    /// A label can't repeat an enclosing loop's, so each labeled Break or Continue has one target.
    void push_loop(const stmt_id_t id, const std::optional<name_t>& label) {
        if (label) {
            for (const loop_t& outer : m_loops) {
                if (outer.label && outer.label->text == label->text) {
                    error(label->span, "'{}' already labels an enclosing loop", label->text)
                        .with_label(outer.label->span, "used here");
                    break;
                }
            }
        }

        m_loops.push_back({ .stmt = id, .label = label });
    }

    loop_t pop_loop() {
        loop_t loop = std::move(m_loops.back());
        m_loops.pop_back();
        return loop;
    }

    /// After a loop, the code is reached by the normal exit (already in m_flow) and every Break.
    void leave_loop(const loop_t& loop) {
        for (const flow_t& path : loop.breaks) {
            m_flow = join(m_flow, path);
        }
    }

    /// What an assignment changes.
    struct target_t {
        type_id_t type;
        /// The variable, when the assignment replaces all of it.
        std::optional<symbol_id_t> whole;
    };

    /// Checks what an assignment changes: a variable, Self, or a field inside one. Plain '='
    /// doesn't read a whole variable, so it may be unassigned; changing a field reads the rest.
    std::optional<target_t> check_target(const expr_id_t id) {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* name = std::get_if<name_expr_t>(&node)) {
            const std::optional<symbol_id_t> found = lookup_value(name->name, span_of(id));
            if (!found) {
                return std::nullopt;
            }

            const type_id_t type = symbol(*found).type;
            info(id) = { .type = type, .converted = type, .symbol = found };
            if (!check_assignable(*found, span_of(id), "changed")) {
                return std::nullopt;
            }

            // A field used by name is part of Self, not a whole variable.
            const bool field = symbol(*found).kind == symbol_kind_t::FIELD;
            return target_t{ .type = type, .whole = field ? std::nullopt : found };
        }

        if (std::holds_alternative<member_expr_t>(node) ||
            std::holds_alternative<self_expr_t>(node)) {
            const type_id_t type = check(id, type_id_t::NONE);
            if (type == type_id_t::INVALID) {
                return std::nullopt;
            }

            const std::optional<symbol_id_t> root = root_of(id);
            if (!root) {
                error(span_of(id), "only a variable or one of its fields can be assigned to");
                return std::nullopt;
            }

            if (!check_assignable(*root, span_of(id), "changed")) {
                return std::nullopt;
            }

            return target_t{ .type = type };
        }

        if (std::holds_alternative<index_expr_t>(node)) {
            check(id, type_id_t::NONE);
        } else if (!std::holds_alternative<error_expr_t>(node)) {
            error(span_of(id), "only a variable can be assigned to");
        }

        return std::nullopt;
    }

    /// The function being checked's Self, if it's a Method or New.
    [[nodiscard]] std::optional<symbol_id_t> current_self() {
        return m_function ? symbol(*m_function).self : std::nullopt;
    }

    /// The variable a place starts from, after it's checked: "p" in "p.pos.x", and Self for a
    /// field used by name. nullopt for anything else, like a call's result.
    std::optional<symbol_id_t> root_of(const expr_id_t id) {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* member = std::get_if<member_expr_t>(&node)) {
            return root_of(member->object);
        }

        if (!std::holds_alternative<name_expr_t>(node) &&
            !std::holds_alternative<self_expr_t>(node)) {
            return std::nullopt;
        }

        const std::optional<symbol_id_t> found = info(id).symbol;
        if (found && symbol(*found).kind == symbol_kind_t::FIELD) {
            return current_self();
        }

        return found;
    }

    /// The fields a place goes through from its root, outermost first. Together with the root,
    /// this tells whether two ByRef arguments overlap.
    std::vector<std::uint32_t> path_of(const expr_id_t id) {
        std::vector<std::uint32_t> path;
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* member = std::get_if<member_expr_t>(&node)) {
            path = path_of(member->object);
        }

        if (const std::optional<symbol_id_t> found = info(id).symbol;
            found && symbol(*found).kind == symbol_kind_t::FIELD) {
            path.push_back(symbol(*found).field);
        }

        return path;
    }

    /// Whether Self can be changed: in a "ByRef Method" and in New.
    [[nodiscard]] bool can_change_self() {
        const std::optional<symbol_id_t> self = current_self();
        return self && (symbol(*self).kind == symbol_kind_t::LOCAL || symbol(*self).by_ref);
    }

    /// Whether a variable can be changed, which assigning and passing ByRef both need. Parameters
    /// are read-only unless they're ByRef. Reports why not, with `action` ending the message.
    bool check_assignable(const symbol_id_t id, const span_t span, const std::string_view action) {
        const symbol_t& target = symbol(id);
        switch (target.kind) {
        case symbol_kind_t::LOCAL:
        case symbol_kind_t::GLOBAL:
            return true;
        case symbol_kind_t::FIELD:
            if (can_change_self()) {
                return true;
            }

            error(span, "'{}' is a field, and a Method can't change Self; make it a ByRef Method",
                  target.name);
            return false;
        case symbol_kind_t::PARAMETER:
            if (target.by_ref) {
                return true;
            }

            if (id == current_self()) {
                error(span, "a Method can't change Self; make it a ByRef Method");
                return false;
            }

            error(span,
                  "'{}' is a parameter and can't be {}; copy it into a Local, or make it ByRef",
                  target.name, action);
            return false;
        case symbol_kind_t::CONSTANT:
            error(span, "'{}' is a Const and can't be {}", target.name, action);
            return false;
        case symbol_kind_t::LOOP_VARIABLE:
            error(span, "'{}' is a loop variable and can't be {}", target.name, action);
            return false;
        // lookup_value() never gives a function or a type, so these aren't reached.
        case symbol_kind_t::FUNCTION:
        case symbol_kind_t::BUILTIN_FUNCTION:
        case symbol_kind_t::STRUCT:
        case symbol_kind_t::METHOD:
        case symbol_kind_t::CONSTRUCTOR:
            return false;
        }

        std::unreachable();
    }

    // Expressions

    /// Literals, and arithmetic made only of literals. They take their type from the context.
    [[nodiscard]] bool is_flexible(const expr_id_t id) const {
        const expr_node_t& node = m_ast.get(id).node;
        if (std::holds_alternative<integer_expr_t>(node) ||
            std::holds_alternative<decimal_expr_t>(node) ||
            std::holds_alternative<character_expr_t>(node)) {
            return true;
        }

        if (const auto* unary = std::get_if<unary_expr_t>(&node)) {
            return unary->op != unary_op_t::NOT && is_flexible(unary->operand);
        }

        if (const auto* binary = std::get_if<binary_expr_t>(&node)) {
            return is_numeric_op(binary->op) && is_flexible(binary->left) &&
                   is_flexible(binary->right);
        }

        if (const auto* ternary = std::get_if<ternary_expr_t>(&node)) {
            return is_flexible(ternary->if_true) && is_flexible(ternary->if_false);
        }

        return false;
    }

    [[nodiscard]] bool has_decimal(const expr_id_t id) const {
        const expr_node_t& node = m_ast.get(id).node;
        if (std::holds_alternative<decimal_expr_t>(node)) {
            return true;
        }

        if (const auto* unary = std::get_if<unary_expr_t>(&node)) {
            return has_decimal(unary->operand);
        }

        if (const auto* binary = std::get_if<binary_expr_t>(&node)) {
            return has_decimal(binary->left) || has_decimal(binary->right);
        }

        if (const auto* ternary = std::get_if<ternary_expr_t>(&node)) {
            return has_decimal(ternary->if_true) || has_decimal(ternary->if_false);
        }

        return false;
    }

    /// The type literals get without context: Float if there's a decimal among them, else Int.
    [[nodiscard]] type_id_t default_type(const expr_id_t id) const {
        return has_decimal(id) ? type_id_t::FLOAT32 : type_id_t::INT32;
    }

    /// Checks an expression whose value is used. `hint` is the type the context expects, or
    /// NONE; literals take it when their value fits.
    type_id_t check(const expr_id_t id, const type_id_t hint) {
        const type_id_t type = check_any(id, hint);
        if (type != type_id_t::NONE) {
            return type;
        }

        // Only a call to a function without a return type has no type.
        const auto& call = std::get<call_expr_t>(m_ast.get(id).node);
        error(span_of(id), "'{}' doesn't return a value", text(call.callee));
        info(id).type = type_id_t::INVALID;
        info(id).converted = type_id_t::INVALID;
        return type_id_t::INVALID;
    }

    /// Like check(), but a call that returns nothing is fine.
    type_id_t check_any(const expr_id_t id, type_id_t hint) {
        // With no numeric type to take, literals use the default type of the whole expression,
        // so in "1 + 0.5" the 1 is a Float too.
        if (is_flexible(id) && !is_numeric(hint)) {
            hint = default_type(id);
        }

        // Trying overloads checks arguments more than once, so each check starts fresh.
        info(id) = {};
        const type_id_t type = std::visit(
            [this, id, hint](const auto& node) { return this->check_expr(id, node, hint); },
            m_ast.get(id).node);
        info(id).type = type;
        info(id).converted = type;
        return type;
    }

    /// Converts a checked expression to `type`, which has to be lossless. An INVALID type on
    /// either side passes, since its error was already reported.
    bool expect_type(const expr_id_t id, const type_id_t type) {
        if (!converts_losslessly(info(id).type, type)) {
            report_mismatch(id, type);
            return false;
        }

        convert(id, type);
        return true;
    }

    /// A literal would have taken the expected numeric type if its value fit, so it says that.
    void report_mismatch(const expr_id_t id, const type_id_t expected) {
        const expr_info_t& found = info(id);
        if (found.value && is_numeric(found.type) && is_numeric(expected) && is_flexible(id)) {
            error(span_of(id), "{} doesn't fit in {}", text(id), name_of(expected));
        } else {
            error(span_of(id), "expected {}, found {}", name_of(expected), name_of(found.type));
        }
    }

    /// Records a lossless conversion, and converts the value if it's known.
    void convert(const expr_id_t id, const type_id_t type) {
        expr_info_t& out = info(id);
        if (out.converted == type || out.type == type_id_t::INVALID || type == type_id_t::INVALID) {
            return;
        }

        out.converted = type;
        // A lossless conversion can't fail.
        if (out.value) {
            out.value = *fold_cast(*out.value, type);
        }
    }

    /// Records a folded value, or reports why it can't be computed.
    void fold(const expr_id_t id, const fold_result_t& result) {
        if (result) {
            info(id).value = *result;
        } else {
            error(span_of(id), "{}", result.error());
        }
    }

    type_id_t unsupported(const expr_id_t id, const std::string_view what) {
        error(span_of(id), "{} isn't supported yet", what);
        return type_id_t::INVALID;
    }

    static type_id_t check_expr(expr_id_t /*id*/, const error_expr_t& /*node*/,
                                type_id_t /*hint*/) {
        return type_id_t::INVALID;
    }

    type_id_t check_expr(const expr_id_t id, const integer_expr_t& node, const type_id_t hint) {
        return check_integer(id, node.value, false, hint);
    }

    type_id_t check_expr(const expr_id_t id, const character_expr_t& node, const type_id_t hint) {
        return check_integer(id, node.value, false, hint);
    }

    /// An integer literal takes the expected type when its value fits, and is Int otherwise.
    type_id_t check_integer(const expr_id_t id, const std::uint64_t magnitude, const bool negative,
                            const type_id_t hint) {
        std::optional<const_value_t> value;
        if (is_numeric(hint)) {
            value = make_number(magnitude, negative, hint);
        }

        if (!value) {
            value = make_number(magnitude, negative, type_id_t::INT32);
        }

        if (!value) {
            error(span_of(id), "{} doesn't fit in {}", text(id),
                  name_of(is_numeric(hint) ? hint : type_id_t::INT32));
            return type_id_t::INVALID;
        }

        info(id).value = value;
        return value->type;
    }

    /// A decimal literal takes Float64 when it's expected, and is Float otherwise.
    type_id_t check_expr(const expr_id_t id, const decimal_expr_t& node, const type_id_t hint) {
        const type_id_t type = hint == type_id_t::FLOAT64 ? hint : type_id_t::FLOAT32;
        info(id).value = make_decimal(node.value, type);
        if (!info(id).value) {
            error(span_of(id), "{} doesn't fit in {}", text(id), name_of(type));
            return type_id_t::INVALID;
        }

        return type;
    }

    /// The string is known at compile time when every interpolated part is.
    type_id_t check_expr(const expr_id_t id, const string_expr_t& node, type_id_t /*hint*/) {
        std::string value;
        bool known = true;
        for (const string_part_t& part : node.parts) {
            if (const auto* piece = std::get_if<std::string>(&part)) {
                value += *piece;
                continue;
            }

            const expr_id_t inner = std::get<expr_id_t>(part);
            if (const type_id_t type = check(inner, type_id_t::NONE); is_struct(type)) {
                error(span_of(inner), "{} can't be shown in a string", name_of(type));
            }

            if (const std::optional<const_value_t>& inner_value = info(inner).value) {
                value += to_text(*inner_value);
            } else {
                known = false;
            }
        }

        if (known) {
            info(id).value = make_string(std::move(value));
        }

        return type_id_t::STRING;
    }

    type_id_t check_expr(const expr_id_t id, const bool_expr_t& node, type_id_t /*hint*/) {
        info(id).value = make_bool(node.value);
        return type_id_t::BOOL;
    }

    type_id_t check_expr(const expr_id_t id, const null_expr_t& /*node*/, type_id_t /*hint*/) {
        return unsupported(id, "'Null'");
    }

    type_id_t check_expr(const expr_id_t id, const name_expr_t& node, type_id_t /*hint*/) {
        const std::optional<symbol_id_t> found = lookup_value(node.name, span_of(id));
        if (!found) {
            return type_id_t::INVALID;
        }

        check_read(*found, span_of(id));
        const symbol_t& target = symbol(*found);
        info(id).symbol = found;
        if (target.kind == symbol_kind_t::CONSTANT) {
            info(id).value = target.value;
        }

        return target.type;
    }

    /// The variable, Const, or field a name refers to. Locals come first, so one can hide a field
    /// or a file-level name. In a Method or New, Self's fields come next. Reports names that don't
    /// refer to one.
    std::optional<symbol_id_t> lookup_value(const std::string_view name, const span_t span) {
        if (const std::optional<symbol_id_t> local = find_local(name)) {
            return local;
        }

        if (const std::vector<symbol_id_t> members = own_members(name); !members.empty()) {
            if (symbol(members.front()).kind == symbol_kind_t::FIELD) {
                return members.front();
            }

            error(span, "'{}' is a method; call it with ()", name);
            return std::nullopt;
        }

        const auto found = m_file_scope.find(name);
        if (found == m_file_scope.end()) {
            if (builtin_type(name)) {
                error(span, "'{}' is a type, not a value", name);
            } else {
                error(span, "'{}' isn't declared", name);
            }

            return std::nullopt;
        }

        // Only overloads share a name, so the first symbol says what the name is.
        const symbol_id_t first = found->second.front();
        if (is_function(symbol(first))) {
            error(span, "'{}' is a function, not a value", name);
            return std::nullopt;
        }

        if (symbol(first).kind == symbol_kind_t::STRUCT) {
            error(span, "'{}' is a type, not a value", name);
            return std::nullopt;
        }

        return first;
    }

    /// Members of the Struct whose Method or New is being checked, which it can use by name.
    std::vector<symbol_id_t> own_members(const std::string_view name) {
        if (!m_function) {
            return {};
        }

        const type_id_t owner = symbol(*m_function).owner;
        if (owner == type_id_t::INVALID) {
            return {};
        }

        return members_of(owner, name);
    }

    /// A Struct's fields or Methods with a name. Constructors aren't reached by name.
    std::vector<symbol_id_t> members_of(const type_id_t type, const std::string_view name) {
        const auto found = m_structs.find(type);
        if (found == m_structs.end()) {
            return {};
        }

        std::vector<symbol_id_t> members;
        for (const symbol_id_t member : symbol(found->second).members) {
            if (symbol(member).name == name && symbol(member).kind != symbol_kind_t::CONSTRUCTOR) {
                members.push_back(member);
            }
        }

        return members;
    }

    /// The members named after a '.': a field, or a Method's overloads. Reports a value that
    /// has no members, or no member with that name.
    std::vector<symbol_id_t> lookup_member(const type_id_t type, const name_t& member) {
        if (type == type_id_t::INVALID || member.text.empty()) {
            return {};
        }

        if (!is_struct(type)) {
            error(member.span, "{} has no members", name_of(type));
            return {};
        }

        std::vector<symbol_id_t> members = members_of(type, member.text);
        if (members.empty()) {
            error(member.span, "{} has no member named '{}'", name_of(type), member.text);
        }

        return members;
    }

    type_id_t check_expr(const expr_id_t id, const unary_expr_t& node, const type_id_t hint) {
        // "-2147483648" is one literal, so the most negative values fit their type.
        if (const auto* literal = std::get_if<integer_expr_t>(&m_ast.get(node.operand).node);
            literal != nullptr && node.op == unary_op_t::NEGATE) {
            const type_id_t type = check_integer(id, literal->value, true, hint);
            info(node.operand) = { .type = type, .converted = type };
            return type;
        }

        const bool is_not = node.op == unary_op_t::NOT;
        const type_id_t type = check(node.operand, is_not ? type_id_t::BOOL : hint);
        if (type == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        if (!supports(node.op, type)) {
            error(span_of(id), "'{}' can't be used on {}", spelling(node.op), name_of(type));
            return type_id_t::INVALID;
        }

        if (const std::optional<const_value_t>& value = info(node.operand).value) {
            fold(id, fold_unary(node.op, *value));
        }

        return type;
    }

    type_id_t check_expr(const expr_id_t id, const binary_expr_t& node, const type_id_t hint) {
        type_id_t type = type_id_t::INVALID;
        switch (node.op) {
        case binary_op_t::COALESCE:
            return unsupported(id, std::format("'{}'", spelling(node.op)));
        case binary_op_t::AND:
        case binary_op_t::OR: {
            const bool left_valid = check_condition(node.left);
            const bool right_valid = check_condition(node.right);
            type = left_valid && right_valid ? type_id_t::BOOL : type_id_t::INVALID;
            break;
        }
        case binary_op_t::SHIFT_LEFT:
        case binary_op_t::SHIFT_RIGHT:
            type = check_shift(node.left, node.right, hint, span_of(id));
            break;
        default: {
            // The hint is for a comparison's Bool result, not its operands.
            const bool compares = is_comparison(node.op);
            type = check_operation(node.op, node.left, node.right,
                                   compares ? type_id_t::NONE : hint, span_of(id));
            if (type != type_id_t::INVALID && compares) {
                type = type_id_t::BOOL;
            }

            break;
        }
        }

        const std::optional<const_value_t>& left = info(node.left).value;
        const std::optional<const_value_t>& right = info(node.right).value;
        if (type != type_id_t::INVALID && left && right) {
            fold(id, fold_binary(node.op, *left, *right));
        }

        return type;
    }

    /// Checks both operands. A literal side takes the other side's type, so `x + 1` works for
    /// any numeric x.
    std::pair<type_id_t, type_id_t> check_operands(const expr_id_t left, const expr_id_t right,
                                                   type_id_t hint) {
        const bool left_flexible = is_flexible(left);
        const bool right_flexible = is_flexible(right);
        if (left_flexible && !right_flexible) {
            const type_id_t right_type = check(right, hint);
            return { check(left, is_numeric(right_type) ? right_type : hint), right_type };
        }

        if (right_flexible && !left_flexible) {
            const type_id_t left_type = check(left, hint);
            return { left_type, check(right, is_numeric(left_type) ? left_type : hint) };
        }

        // Two literal sides with no numeric hint share one default type: "1 < 0.5" compares
        // Floats.
        if (left_flexible && !is_numeric(hint)) {
            hint = has_decimal(left) || has_decimal(right) ? type_id_t::FLOAT32 : type_id_t::INT32;
        }

        const type_id_t left_type = check(left, hint);
        return { left_type, check(right, hint) };
    }

    /// Checks an arithmetic, bitwise, or comparison operator. Both operands are converted to
    /// their common type, which is returned.
    type_id_t check_operation(const binary_op_t op, const expr_id_t left, const expr_id_t right,
                              const type_id_t hint, const span_t span) {
        const auto [left_type, right_type] = check_operands(left, right, hint);
        if (left_type == type_id_t::INVALID || right_type == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        const std::optional<type_id_t> common = common_type(left_type, right_type);
        if (!common) {
            const bool numbers = is_numeric(left_type) && is_numeric(right_type);
            error(span, "'{}' can't combine {} and {}{}", spelling(op), name_of(left_type),
                  name_of(right_type), numbers ? "; convert one side with 'As'" : "");
            return type_id_t::INVALID;
        }

        convert(left, *common);
        convert(right, *common);
        if (supports(op, *common)) {
            return *common;
        }

        if (*common == type_id_t::STRING && is_ordering(op)) {
            error(span, "Strings can only be compared with == and != for now");
        } else {
            error(span, "'{}' can't be used on {}", spelling(op), name_of(*common));
        }

        return type_id_t::INVALID;
    }

    /// The result has the left side's type; the amount can be any integer.
    type_id_t check_shift(const expr_id_t left, const expr_id_t right, const type_id_t hint,
                          const span_t span) {
        const type_id_t type = check(left, hint);
        const type_id_t amount = check(right, type_id_t::NONE);
        if (type == type_id_t::INVALID || amount == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        if (!is_integer(type)) {
            error(span, "shifts only work on integers, not {}", name_of(type));
            return type_id_t::INVALID;
        }

        if (!is_integer(amount)) {
            error(span_of(right), "the shift amount must be an integer, not {}", name_of(amount));
            return type_id_t::INVALID;
        }

        return type;
    }

    type_id_t check_expr(const expr_id_t id, const ternary_expr_t& node, const type_id_t hint) {
        const bool condition_valid = check_condition(node.condition);
        const auto [true_type, false_type] = check_operands(node.if_true, node.if_false, hint);
        if (true_type == type_id_t::INVALID || false_type == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        const std::optional<type_id_t> common = common_type(true_type, false_type);
        if (!common) {
            error(span_of(id), "the results of '?' have different types, {} and {}",
                  name_of(true_type), name_of(false_type));
            return type_id_t::INVALID;
        }

        convert(node.if_true, *common);
        convert(node.if_false, *common);
        // A known condition picks a side, so the other side doesn't need to be known.
        if (const std::optional<const_value_t>& condition = info(node.condition).value;
            condition_valid && condition) {
            const bool chosen = std::get<bool>(condition->data);
            info(id).value = info(chosen ? node.if_true : node.if_false).value;
        }

        return *common;
    }

    type_id_t check_expr(const expr_id_t id, const call_expr_t& node, type_id_t /*hint*/) {
        const std::vector<symbol_id_t> candidates = lookup_callee(node.callee);
        std::optional<symbol_id_t> function;
        // With one candidate there's nothing to rank, and check_arguments() gives more precise
        // errors than "no overload".
        if (candidates.size() == 1) {
            function = candidates.front();
        } else if (candidates.size() > 1) {
            function = choose_overload(node, candidates);
        }

        if (!function) {
            if (candidates.empty()) {
                check_alone(node.arguments);
            }

            return type_id_t::INVALID;
        }

        info(node.callee).symbol = function;
        info(id).symbol = function;
        check_receiver(node, *function);
        check_arguments(id, node, *function);
        // The constructor a Struct gets takes its fields, which may not have types yet.
        if (symbol(*function).kind == symbol_kind_t::CONSTRUCTOR &&
            !symbol(*function).declaration) {
            for (const symbol_id_t field : symbol(*function).parameters) {
                check_field_ready(field, span_of(node.callee));
            }
        }

        return symbol(*function).type;
    }

    /// A "ByRef Method" changes the value it's called on, so that has to be something that can
    /// change. Called by name, it's Self.
    void check_receiver(const call_expr_t& node, const symbol_id_t function_id) {
        const symbol_t& function = symbol(function_id);
        if (function.kind != symbol_kind_t::METHOD || !function.by_ref) {
            return;
        }

        const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
        if (member == nullptr) {
            if (!can_change_self()) {
                error(span_of(node.callee),
                      "'{}' is a ByRef Method, and a Method can't change Self; make this a ByRef "
                      "Method too",
                      function.name);
            }

            return;
        }

        const std::optional<symbol_id_t> root = root_of(member->object);
        if (!root) {
            error(span_of(member->object),
                  "'{}' is a ByRef Method, so it needs a variable to change, not a value",
                  function.name);
            return;
        }

        check_assignable(*root, span_of(member->object),
                         std::format("changed by '{}'", function.name));
    }

    /// When there's no parameter to check them against, arguments are still checked for their
    /// own errors.
    void check_alone(const std::vector<argument_t>& arguments) {
        for (const argument_t& argument : arguments) {
            check(argument.value, type_id_t::NONE);
        }
    }

    /// What a callee names: a function's overloads, a Struct's constructors, or Methods. A Method
    /// is called after a '.' on a Struct value, or by name in another of its Struct's Methods.
    std::vector<symbol_id_t> lookup_callee(const expr_id_t callee) {
        if (const auto* member = std::get_if<member_expr_t>(&m_ast.get(callee).node);
            member != nullptr && !member->null_safe) {
            const type_id_t type = check(member->object, type_id_t::NONE);
            std::vector<symbol_id_t> members = lookup_member(type, member->member);
            if (!members.empty() && symbol(members.front()).kind == symbol_kind_t::FIELD) {
                error(member->member.span, "'{}' is a field, not a method", member->member.text);
                return {};
            }

            return members;
        }

        const auto* name = std::get_if<name_expr_t>(&m_ast.get(callee).node);
        if (name == nullptr) {
            if (check(callee, type_id_t::NONE) != type_id_t::INVALID) {
                error(span_of(callee), "only functions can be called");
            }

            return {};
        }

        const span_t span = span_of(callee);
        // A local hides a function of the same name.
        if (find_local(name->name)) {
            error(span, "'{}' isn't a function", name->name);
            return {};
        }

        if (std::vector<symbol_id_t> members = own_members(name->name); !members.empty()) {
            if (symbol(members.front()).kind == symbol_kind_t::FIELD) {
                error(span, "'{}' is a field, not a method", name->name);
                return {};
            }

            return members;
        }

        const auto found = m_file_scope.find(name->name);
        if (found == m_file_scope.end()) {
            if (builtin_type(name->name)) {
                error(span, "'{}' is a type; convert to it with 'As'", name->name);
            } else {
                error(span, "'{}' isn't declared", name->name);
            }

            return {};
        }

        const symbol_t& first = symbol(found->second.front());
        if (first.kind == symbol_kind_t::STRUCT) {
            std::vector<symbol_id_t> constructors;
            for (const symbol_id_t member : first.members) {
                if (symbol(member).kind == symbol_kind_t::CONSTRUCTOR) {
                    constructors.push_back(member);
                }
            }

            return constructors;
        }

        if (!is_function(first)) {
            error(span, "'{}' isn't a function", name->name);
            return {};
        }

        return found->second;
    }

    /// How errors name something callable: a constructor by its Struct.
    [[nodiscard]] std::string_view callable_name(const symbol_t& function) const {
        return function.kind == symbol_kind_t::CONSTRUCTOR ? name_of(function.owner)
                                                           : function.name;
    }

    /// An overload whose arguments all match exactly beats one that needs conversions. More than
    /// one at the same level is ambiguous. Candidates are tried quietly; only the outcome is
    /// reported.
    std::optional<symbol_id_t> choose_overload(const call_expr_t& node,
                                               const std::vector<symbol_id_t>& candidates) {
        // Each argument's own type, for the error when nothing fits. A literal too big for its
        // default type may still fit a parameter, so only other invalid arguments stop here.
        std::vector<type_id_t> types;
        for (const argument_t& argument : node.arguments) {
            const type_id_t type = quietly([&] { return check(argument.value, type_id_t::NONE); });
            if (type == type_id_t::INVALID && !is_flexible(argument.value)) {
                check_alone(node.arguments); // report the argument's own errors instead
                return std::nullopt;
            }

            types.push_back(type);
        }

        std::vector<symbol_id_t> exact;
        std::vector<symbol_id_t> convertible;
        for (const symbol_id_t candidate : candidates) {
            const match_t match = quietly([&] { return rank(candidate, node); });
            if (match == match_t::EXACT) {
                exact.push_back(candidate);
            } else if (match == match_t::CONVERTIBLE) {
                convertible.push_back(candidate);
            }
        }

        if (exact.size() == 1) {
            return exact.front();
        }

        if (exact.empty() && convertible.size() == 1) {
            return convertible.front();
        }

        const std::string_view name = callable_name(symbol(candidates.front()));
        const span_t span = span_of(node.callee);
        const std::vector<symbol_id_t>& tied = exact.empty() ? convertible : exact;
        if (tied.empty()) {
            std::string list;
            for (const type_id_t type : types) {
                list += list.empty() ? "" : ", ";
                list += name_of(type);
            }

            error(span, "no overload of '{}' takes ({})", name, list);
            return std::nullopt;
        }

        diagnostic_t& diagnostic = error(span, "the call to '{}' is ambiguous", name);
        for (const symbol_id_t candidate : tied) {
            if (symbol(candidate).kind != symbol_kind_t::BUILTIN_FUNCTION) {
                diagnostic.with_label(symbol(candidate).span, "could be this");
            }
        }

        return std::nullopt;
    }

    /// How well a call fits `function`: its worst argument's match. NONE when the arguments
    /// don't bind to the parameters, or one's ByRef doesn't match.
    match_t rank(const symbol_id_t function, const call_expr_t& node) {
        const std::optional<binding_t> bound = bind(function, node, span_of(node.callee));
        if (!bound) {
            return match_t::NONE;
        }

        const binding_t& slots = *bound;
        match_t worst = match_t::EXACT;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const std::optional<std::size_t> slot = slots[i];
            if (!slot) {
                continue;
            }

            const argument_t& argument = node.arguments[*slot];
            const symbol_t& parameter = symbol(symbol(function).parameters[i]);
            if (argument.by_ref != parameter.by_ref) {
                return match_t::NONE;
            }

            worst = std::min(worst, match(argument, parameter.type));
        }

        return worst;
    }

    /// How well one argument fits a parameter of `type`. A literal matches its default type
    /// exactly, and any other type it fits by conversion. A ByRef argument needs the exact type.
    match_t match(const argument_t& argument, const type_id_t type) {
        const type_id_t natural = check(argument.value, type_id_t::NONE);
        if (natural == type || type == type_id_t::INVALID) {
            return match_t::EXACT;
        }

        if (argument.by_ref) {
            return match_t::NONE;
        }

        if (is_flexible(argument.value)) {
            return check(argument.value, type) == type ? match_t::CONVERTIBLE : match_t::NONE;
        }

        return converts_losslessly(natural, type) ? match_t::CONVERTIBLE : match_t::NONE;
    }

    /// Matches arguments to parameters: positional ones first, then named ones, and defaults fill
    /// the rest.
    std::optional<binding_t> bind(const symbol_id_t function_id, const call_expr_t& node,
                                  const span_t call_span) {
        const symbol_t& function = symbol(function_id);
        const std::size_t count = function.parameters.size();
        binding_t bound(count);
        bool valid = true;
        bool named = false;
        std::size_t position = 0;
        for (std::size_t i = 0; i < node.arguments.size(); ++i) {
            const argument_t& argument = node.arguments[i];
            const span_t span = span_of(argument.value);
            std::optional<std::size_t> parameter;
            if (argument.name) {
                named = true;
                const auto found =
                    std::ranges::find(function.parameters, argument.name->text,
                                      [this](const symbol_id_t id) { return symbol(id).name; });
                if (found == function.parameters.end()) {
                    error(argument.name->span, "'{}' has no parameter named '{}'",
                          callable_name(function), argument.name->text);
                    valid = false;
                    continue;
                }

                parameter = static_cast<std::size_t>(found - function.parameters.begin());
            } else if (named) {
                error(span, "positional arguments must come before named ones");
                valid = false;
                continue;
            } else if (position < count) {
                parameter = position++;
            } else {
                error(span, "too many arguments to '{}'", callable_name(function));
                valid = false;
                continue;
            }

            if (bound[*parameter]) {
                error(span, "parameter '{}' already has an argument",
                      symbol(function.parameters[*parameter]).name);
                valid = false;
                continue;
            }

            bound[*parameter] = i;
        }

        if (!valid) {
            return std::nullopt; // a missing argument is probably the misplaced one
        }

        for (std::size_t i = 0; i < count; ++i) {
            const symbol_t& parameter = symbol(function.parameters[i]);
            if (!bound[i] && !parameter.has_default) {
                error(call_span, "missing an argument for parameter '{}'", parameter.name);
                valid = false;
            }
        }

        if (!valid) {
            return std::nullopt;
        }

        return bound;
    }

    /// Checks each argument against its parameter, and records for lowering which argument each
    /// parameter gets: nullopt means its default.
    void check_arguments(const expr_id_t id, const call_expr_t& node, const symbol_id_t function) {
        const std::optional<binding_t> bound = bind(function, node, span_of(id));
        if (!bound) {
            check_alone(node.arguments);
            return;
        }

        const binding_t& slots = *bound;
        std::vector<std::optional<expr_id_t>> arguments;
        std::vector<access_t> by_ref;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const std::optional<std::size_t> slot = slots[i];
            if (slot) {
                const argument_t& argument = node.arguments[*slot];
                check_argument(argument, symbol(function).parameters[i], by_ref);
                arguments.emplace_back(argument.value);
            } else {
                arguments.emplace_back(std::nullopt);
            }
        }

        info(id).arguments = std::move(arguments);
    }

    /// A variable, or a field inside one, that a call passes ByRef.
    struct access_t {
        expr_id_t argument;
        symbol_id_t root;
        std::vector<std::uint32_t> path;
    };

    /// Two ByRef arguments overlap when they're the same variable, or one is a field inside the
    /// other.
    [[nodiscard]] static bool overlap(const access_t& a, const access_t& b) {
        const std::size_t shared = std::min(a.path.size(), b.path.size());
        return a.root == b.root &&
               std::ranges::equal(std::span(a.path).first(shared), std::span(b.path).first(shared));
    }

    /// A ByRef argument must be a variable that can be changed, or a field inside one, of exactly
    /// the parameter's type, and not overlap another ByRef argument in the same call.
    void check_argument(const argument_t& argument, const symbol_id_t parameter_id,
                        std::vector<access_t>& by_ref) {
        const symbol_t& parameter = symbol(parameter_id);
        const expr_id_t value = argument.value;
        // This reads a ByRef variable too, so it must already be assigned.
        const type_id_t type = check(value, parameter.type);
        if (argument.by_ref != parameter.by_ref) {
            if (parameter.by_ref) {
                error(span_of(value), "parameter '{}' is ByRef; write 'ByRef {}'", parameter.name,
                      text(value));
            } else {
                error(span_of(value), "parameter '{}' isn't ByRef; remove 'ByRef'", parameter.name);
            }

            return;
        }

        if (!argument.by_ref) {
            expect_type(value, parameter.type);
            return;
        }

        if (type == type_id_t::INVALID) {
            return;
        }

        const std::optional<symbol_id_t> variable = root_of(value);
        if (!variable) {
            error(span_of(value), "only a variable or one of its fields can be passed ByRef");
            return;
        }

        if (!check_assignable(*variable, span_of(value), "passed ByRef")) {
            return;
        }

        const access_t access{ .argument = value, .root = *variable, .path = path_of(value) };
        const auto other = std::ranges::find_if(
            by_ref, [&](const access_t& earlier) { return overlap(access, earlier); });
        if (type != parameter.type && parameter.type != type_id_t::INVALID) {
            error(span_of(value), "a ByRef argument must be exactly {}, but this is {}",
                  name_of(parameter.type), name_of(type));
        } else if (other != by_ref.end() && other->path == access.path) {
            error(span_of(value), "'{}' is passed ByRef twice in one call", text(value));
        } else if (other != by_ref.end()) {
            error(span_of(value), "'{}' and '{}' overlap, so only one can be passed ByRef",
                  text(other->argument), text(value));
        }

        by_ref.push_back(access);
    }

    /// A field after a '.'. Methods can only be called.
    type_id_t check_expr(const expr_id_t id, const member_expr_t& node, type_id_t /*hint*/) {
        if (node.null_safe) {
            check(node.object, type_id_t::NONE);
            return unsupported(id, "'?.'");
        }

        const type_id_t object = check(node.object, type_id_t::NONE);
        const std::vector<symbol_id_t> members = lookup_member(object, node.member);
        if (members.empty()) {
            return type_id_t::INVALID;
        }

        const symbol_id_t member = members.front();
        if (symbol(member).kind != symbol_kind_t::FIELD) {
            error(node.member.span, "'{}' is a method; call it with ()", node.member.text);
            return type_id_t::INVALID;
        }

        if (!check_field_ready(member, node.member.span)) {
            return type_id_t::INVALID;
        }

        info(id).symbol = member;
        return symbol(member).type;
    }

    type_id_t check_expr(const expr_id_t id, const self_expr_t& /*node*/, type_id_t /*hint*/) {
        const std::optional<symbol_id_t> self = current_self();
        if (!self) {
            error(span_of(id), "'Self' can only be used in a Method or New");
            return type_id_t::INVALID;
        }

        info(id).symbol = self;
        return symbol(*self).type;
    }

    type_id_t check_expr(const expr_id_t id, const index_expr_t& /*node*/, type_id_t /*hint*/) {
        return unsupported(id, "indexing");
    }

    /// A cast gives a literal its type directly: `5_000_000_000 As Long`.
    type_id_t check_expr(const expr_id_t id, const cast_expr_t& node, type_id_t /*hint*/) {
        if (node.checked) {
            return unsupported(id, "'As?'");
        }

        const type_id_t target = resolve_type(node.type);
        const type_id_t source = check(node.value, target);
        if (target == type_id_t::INVALID || source == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        // Numbers convert to each other, and a Bool to an integer as 0 or 1.
        const bool converts = source == target || (is_numeric(source) && is_numeric(target)) ||
                              (source == type_id_t::BOOL && is_integer(target));
        if (!converts) {
            error(span_of(id), "'As' can't convert {} to {}", name_of(source), name_of(target));
            return type_id_t::INVALID;
        }

        if (const std::optional<const_value_t>& value = info(node.value).value) {
            fold(id, fold_cast(*value, target));
        }

        return target;
    }

    type_id_t check_expr(const expr_id_t id, const is_expr_t& /*node*/, type_id_t /*hint*/) {
        return unsupported(id, "'Is'");
    }
};

} // namespace

analysis_t analyze(const source_file_t& file, const ast_t& ast, diagnostic_bag_t& diagnostics) {
    return checker_t(file, ast, diagnostics).run();
}

} // namespace fb
