#include "fb/sema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
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

/// What's known at one point in the code: which Locals are assigned on every path that gets
/// here, and which nullable variables hold a value on every one.
struct flow_t {
    std::vector<bool> assigned; // by symbol id
    std::vector<bool> narrowed; // by symbol id
    bool reachable = true;
};

/// What's true on both paths. A symbol past the end of either list counts as false.
std::vector<bool> both(const std::vector<bool>& a, const std::vector<bool>& b) {
    std::vector<bool> result(std::min(a.size(), b.size()));
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = a[i] && b[i];
    }

    return result;
}

/// Where two paths meet. A path that can't get here doesn't count.
flow_t join(const flow_t& a, const flow_t& b) {
    if (!a.reachable) {
        return b;
    }

    if (!b.reachable) {
        return a;
    }

    return { .assigned = both(a.assigned, b.assigned), .narrowed = both(a.narrowed, b.narrowed) };
}

/// Sets a symbol's bit in one of a flow's lists. Symbols are added while checking, so the list
/// grows to fit.
void set_bit(std::vector<bool>& bits, const symbol_id_t id, const bool value) {
    if (bits.size() <= index(id)) {
        bits.resize(index(id) + 1);
    }

    bits[index(id)] = value;
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

/// The flows after a condition: where it's True, and where it's False. They differ when it
/// checks for Null, which narrows on one side.
struct branches_t {
    expr_id_t condition;
    flow_t if_true;
    flow_t if_false;
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
    /// visible anywhere in the file, and fields are checked against their types. Then Global and
    /// Const initializers and field defaults are checked in source order, then default values,
    /// and last the top-level code and each function body.
    analysis_t run() {
        declare_builtins();
        for (const stmt_id_t id : m_ast.items()) {
            declare_type(id);
        }

        for (const stmt_id_t id : m_ast.items()) {
            declare_item(id);
        }

        link_classes();
        check_struct_cycles();
        check_field_zeros();
        link_constructors();
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
    std::unordered_map<type_id_t, symbol_id_t> m_declared_types;
    /// Fields without a type, until their defaults give them one.
    std::vector<symbol_id_t> m_untyped_fields;
    /// The flow at the code being checked.
    flow_t m_flow;
    /// The branches of the condition checked last, when it narrows something.
    std::optional<branches_t> m_branches;
    /// While checking a New: its type's fields without a zero, which it must assign. Inherited
    /// ones are assigned by the base Class's New.
    std::vector<symbol_id_t> m_required;
    /// While checking a New that starts with Super.New: that call. Until it's checked, the
    /// object isn't set up, so Self can't be used.
    std::optional<expr_id_t> m_super_new;
    bool m_before_super = false;
    /// An assignment's target, or the Self in "Self.x". Neither reads the fields that New hasn't
    /// assigned yet.
    std::optional<expr_id_t> m_partial;
    /// Each T[]'s Methods, made the first time they're looked up.
    std::unordered_map<type_id_t, std::vector<symbol_id_t>> m_array_methods;
    bool m_initializing = false; // checking Global and Const initializers

    // Helpers

    template <typename... Args>
    diagnostic_t& error(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        return m_diagnostics->error(span, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    diagnostic_t& warning(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        return m_diagnostics->warning(span, fmt, std::forward<Args>(args)...);
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

    /// The temporary built-in Print(String), until std.core exists, and RefEquals(a, b), which
    /// tells whether two Class references are the same object. User functions can overload them.
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

        // RefEquals takes any Classes, so its parameters have no type; check_ref_equals() checks
        // the arguments instead.
        const symbol_id_t left = add_symbol({ .kind = symbol_kind_t::PARAMETER, .name = "a" });
        const symbol_id_t right = add_symbol({ .kind = symbol_kind_t::PARAMETER, .name = "b" });
        const symbol_id_t ref_equals = add_symbol({
            .kind = symbol_kind_t::BUILTIN_FUNCTION,
            .name = "RefEquals",
            .type = type_id_t::BOOL,
            .parameters = { left, right },
        });
        m_file_scope["RefEquals"].push_back(ref_equals);
    }

    /// Gives a Struct or Class its type before anything else is declared, so types can use each
    /// other in any order.
    void declare_type(const stmt_id_t id) {
        const auto* node = std::get_if<type_stmt_t>(&m_ast.get(id).node);
        if (node == nullptr) {
            return;
        }

        if (builtin_type(node->name.text)) {
            error(node->name.span, "'{}' is a built-in type", node->name.text);
        }

        const modifiers_t& modifiers = node->modifiers;
        if (!node->is_class && (modifiers.is_abstract || modifiers.is_final)) {
            error(node->name.span, "a Struct can't be Abstract or Final; only a Class can");
        } else if (modifiers.is_abstract && modifiers.is_final) {
            error(node->name.span, "'{}' can't be both Abstract and Final", node->name.text);
        }

        if (modifiers.is_override) {
            error(node->name.span, "Override only goes before a Method");
        }

        std::string name(node->name.text);
        const type_id_t type = node->is_class ? m_analysis.types.add_class(std::move(name))
                                              : m_analysis.types.add_struct(std::move(name));
        const symbol_id_t declared = add_symbol({
            .kind = symbol_kind_t::TYPE,
            .name = node->name.text,
            .span = node->name.span,
            .type = type,
            .declaration = id,
            .modifiers = modifiers,
        });
        m_analysis.stmts[index(id)].symbol = declared;
        m_declared_types[type] = declared;
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
        } else if (const auto* declared = std::get_if<type_stmt_t>(&node)) {
            declare_members(id, *declared);
        }
    }

    void report_misplaced_member(const function_stmt_t& node) {
        if (node.kind == function_kind_t::CONSTRUCTOR) {
            error(node.name.span, "New can only be declared in a Struct or Class");
        } else {
            error(node.name.span, "Methods can only be declared in a Struct or Class");
        }
    }

    /// A type's fields, Methods, and News. Without a New, a Struct gets a constructor whose
    /// parameters are its fields, and a Class gets one without parameters.
    void declare_members(const stmt_id_t id, const type_stmt_t& node) {
        const std::optional<symbol_id_t> declared_struct = m_analysis.stmts[index(id)].symbol;
        if (!declared_struct) {
            return;
        }

        const symbol_id_t owner_id = *declared_struct;
        const type_id_t owner = symbol(owner_id).type;
        if (node.base) {
            declare_base(owner, *node.base);
        }

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
                check_member_function(*function, node.is_class);
                add_member(members, declare_function(member_id, *function, owner));
            } else if (!std::holds_alternative<error_stmt_t>(member.node)) {
                error(member.span, "a {} can only hold Fields, Methods, and News",
                      node.is_class ? "Class" : "Struct");
            }
        }

        m_analysis.types.set_fields(owner, std::move(fields));
        if (!has_new) {
            std::vector<symbol_id_t> parameters;
            for (const symbol_id_t member : members) {
                if (symbol(member).kind == symbol_kind_t::FIELD && !node.is_class) {
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

    /// The Class after Extends. A Struct is a value, so it can't extend anything, and a Class
    /// can't extend a Final one or itself.
    void declare_base(const type_id_t type, const name_t& name) {
        if (!is_class(type)) {
            error(name.span, "only a Class can extend another; a Struct is a value");
            return;
        }

        const auto found = m_file_scope.find(name.text);
        if (found == m_file_scope.end() ||
            symbol(found->second.front()).kind != symbol_kind_t::TYPE ||
            !is_class(symbol(found->second.front()).type)) {
            error(name.span, "'{}' isn't a Class", name.text);
            return;
        }

        const symbol_t& base = symbol(found->second.front());
        if (base.modifiers.is_final) {
            error(name.span, "{} is Final, so it can't be extended", base.name);
        } else if (m_analysis.types.extends(base.type, type)) {
            error(name.span, "{} can't extend itself, directly or through other Classes",
                  name_of(type));
        } else {
            m_analysis.types.set_base(type, base.type);
        }
    }

    /// Abstract, Final, and Override only go on a Class's Methods. An Abstract Method needs an
    /// Abstract Class.
    void check_modifiers(const function_stmt_t& node, const type_id_t owner) {
        const modifiers_t& modifiers = node.modifiers;
        if (!modifiers.is_abstract && !modifiers.is_final && !modifiers.is_override) {
            return;
        }

        if (node.kind == function_kind_t::FUNCTION) {
            error(node.name.span, "Abstract, Final, and Override only go before a Class's Methods");
        } else if (node.kind == function_kind_t::CONSTRUCTOR) {
            error(node.name.span, "New can't be Abstract, Final, or Override");
        } else if (!is_class(owner)) {
            error(node.name.span, "a Struct's Methods can't be Abstract, Final, or Override");
        } else if (node.name.text == "Destroy") {
            error(node.name.span,
                  "Destroy can't be Abstract, Final, or Override; every Class's Destroy runs");
        } else if (modifiers.is_abstract && modifiers.is_final) {
            error(node.name.span, "'{}' can't be both Abstract and Final", node.name.text);
        } else if (modifiers.is_abstract &&
                   !symbol(m_declared_types.at(owner)).modifiers.is_abstract) {
            error(node.name.span, "'{}' is Abstract, so {} must be Abstract too", node.name.text,
                  name_of(owner));
        }
    }

    /// Only a Class's Destroy runs by itself, and only a Struct's Methods can be ByRef: a
    /// Class's Methods can always change its fields.
    void check_member_function(const function_stmt_t& node, const bool in_class) {
        if (node.kind == function_kind_t::BYREF_METHOD && in_class) {
            error(node.name.span,
                  "only a Struct's Methods can be ByRef; a Class's Methods can always change its "
                  "fields");
        }

        if (node.name.text != "Destroy" || node.kind == function_kind_t::CONSTRUCTOR) {
            return;
        }

        if (!in_class) {
            error(node.name.span,
                  "a Struct can't have a Destroy; only a Class's runs when its last reference "
                  "goes away");
        } else if (!node.parameters.empty() || node.return_type) {
            error(node.name.span, "Destroy takes no parameters and returns nothing");
        }
    }

    /// A field is also a parameter of the constructor its Struct gets, so it has a default: its
    /// own, or zero. A type without a zero leaves it without one; see check_field_zeros().
    /// Without a type, it takes its default's, once that's checked.
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

    /// The Class a type extends, or INVALID.
    [[nodiscard]] type_id_t base_of(const type_id_t type) const {
        return m_analysis.types.get(type).base;
    }

    /// Each Class gets the fields of the Class it extends before its own, and its Methods are
    /// checked against the ones it inherits. The Class it extends goes first.
    void link_classes() {
        std::vector<type_id_t> linked;
        for (const stmt_id_t id : m_ast.items()) {
            const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node);
            const std::optional<symbol_id_t> owner = m_analysis.stmts[index(id)].symbol;
            if (declared != nullptr && declared->is_class && owner) {
                link_class(symbol(*owner).type, linked);
            }
        }
    }

    void link_class(const type_id_t type, std::vector<type_id_t>& linked) {
        if (std::ranges::contains(linked, type)) {
            return;
        }

        linked.push_back(type);
        const type_id_t base = base_of(type);
        if (base == type_id_t::INVALID) {
            check_abstract(type);
            return;
        }

        link_class(base, linked);
        std::vector<field_t> fields = m_analysis.types.get(base).fields;
        const auto inherited = static_cast<std::uint32_t>(fields.size());
        for (const symbol_id_t member : symbol(m_declared_types.at(type)).members) {
            symbol_t& declared = symbol(member);
            if (declared.kind == symbol_kind_t::FIELD) {
                declared.field += inherited;
                fields.push_back({ .name = std::string(declared.name), .type = declared.type });
            }

            check_inherited(member, base);
        }

        m_analysis.types.set_fields(type, std::move(fields));
        check_abstract(type);
    }

    /// A member against what a Class inherits: a field's name is taken, and a Method with the
    /// parameters of an inherited one overrides it, which takes Override.
    void check_inherited(const symbol_id_t id, const type_id_t base) {
        symbol_t& member = symbol(id);
        // Every Class's Destroy runs, so none overrides another.
        if (member.kind == symbol_kind_t::CONSTRUCTOR || member.name == "Destroy") {
            return;
        }

        const std::vector<symbol_id_t> inherited = members_of(base, member.name);
        const bool is_method = member.kind == symbol_kind_t::METHOD;
        if (inherited.empty()) {
            if (member.modifiers.is_override) {
                error(member.span, "'{}' has Override, but {} has no Method like it", member.name,
                      name_of(base));
            }

            return;
        }

        if (!is_method || symbol(inherited.front()).kind == symbol_kind_t::FIELD) {
            error(member.span, "'{}' is already declared in {}", member.name,
                  name_of(symbol(inherited.front()).owner));
            return;
        }

        const auto overridden = std::ranges::find_if(inherited, [&](const symbol_id_t other) {
            return same_parameters(member, symbol(other));
        });
        if (overridden == inherited.end()) {
            if (member.modifiers.is_override) {
                error(member.span, "'{}' has Override, but {} has no Method like it", member.name,
                      name_of(base));
            }

            return; // an overload
        }

        const symbol_t& original = symbol(*overridden);
        member.overrides = *overridden;
        if (!member.modifiers.is_override) {
            error(member.span, "'{}' overrides {}'s; write 'Override Method {}'", member.name,
                  name_of(original.owner), member.name);
        } else if (original.modifiers.is_final) {
            error(member.span, "'{}' is Final in {}, so it can't be overridden", member.name,
                  name_of(original.owner));
        } else if (member.type != original.type) {
            error(member.span, "'{}' must return {}, like {}'s", member.name,
                  name_of(original.type), name_of(original.owner));
        } else if (!std::ranges::equal(
                       member.parameters, original.parameters, {},
                       [this](const symbol_id_t parameter) { return symbol(parameter).by_ref; },
                       [this](const symbol_id_t parameter) { return symbol(parameter).by_ref; })) {
            error(member.span, "'{}' must take the same ByRef parameters as {}'s", member.name,
                  name_of(original.owner));
        }
    }

    /// A Class that isn't Abstract overrides every Abstract Method it inherits.
    void check_abstract(const type_id_t type) {
        const symbol_t& declared = symbol(m_declared_types.at(type));
        if (declared.modifiers.is_abstract) {
            return;
        }

        for (const symbol_id_t method : methods_of(type)) {
            if (symbol(method).modifiers.is_abstract && symbol(method).owner != type) {
                error(declared.span, "{} must override {}'s Abstract Method '{}', or be Abstract",
                      declared.name, name_of(symbol(method).owner), symbol(method).name);
            }
        }
    }

    /// Every Method a Class has, its own and the ones it inherits, without the ones it overrides.
    std::vector<symbol_id_t> methods_of(const type_id_t type) {
        std::vector<symbol_id_t> methods;
        for (type_id_t current = type; current != type_id_t::INVALID; current = base_of(current)) {
            for (const symbol_id_t member : symbol(m_declared_types.at(current)).members) {
                const symbol_t& method = symbol(member);
                const bool hidden = std::ranges::any_of(methods, [&](const symbol_id_t other) {
                    return symbol(other).name == method.name &&
                           same_parameters(symbol(other), method);
                });
                if (method.kind == symbol_kind_t::METHOD && !hidden) {
                    methods.push_back(member);
                }
            }
        }

        return methods;
    }

    /// A Class's New runs the New of the Class it extends first: the one that starts it with
    /// Super.New, or else one that takes no arguments.
    void link_constructors() {
        for (const stmt_id_t id : m_ast.items()) {
            const std::optional<symbol_id_t> owner = m_analysis.stmts[index(id)].symbol;
            if (!std::holds_alternative<type_stmt_t>(m_ast.get(id).node) || !owner) {
                continue;
            }

            const type_id_t base = base_of(symbol(*owner).type);
            if (base == type_id_t::INVALID) {
                continue;
            }

            const std::optional<symbol_id_t> plain = plain_constructor(base);
            for (const symbol_id_t member : symbol(*owner).members) {
                symbol_t& constructor = symbol(member);
                if (constructor.kind != symbol_kind_t::CONSTRUCTOR || super_new_of(constructor)) {
                    continue;
                }

                if (plain) {
                    constructor.base_new = plain;
                } else if (constructor.declaration) {
                    error(constructor.span,
                          "New must start with Super.New(...), since {} has no New without "
                          "parameters",
                          name_of(base));
                } else {
                    error(constructor.span,
                          "{} needs a New that starts with Super.New(...), since {} has no New "
                          "without parameters",
                          symbol(*owner).name, name_of(base));
                }
            }
        }
    }

    /// A Class's New that can be called without arguments, if it has one.
    std::optional<symbol_id_t> plain_constructor(const type_id_t type) {
        for (const symbol_id_t member : symbol(m_declared_types.at(type)).members) {
            const symbol_t& constructor = symbol(member);
            const bool plain =
                std::ranges::all_of(constructor.parameters, [this](const symbol_id_t parameter) {
                    return symbol(parameter).has_default;
                });
            if (constructor.kind == symbol_kind_t::CONSTRUCTOR && plain) {
                return member;
            }
        }

        return std::nullopt;
    }

    /// The "Super.New(...)" call that starts a New, if it has one.
    [[nodiscard]] std::optional<expr_id_t> super_new_of(const symbol_t& constructor) const {
        if (!constructor.declaration) {
            return std::nullopt;
        }

        const auto& node = std::get<function_stmt_t>(m_ast.get(*constructor.declaration).node);
        if (node.body.empty()) {
            return std::nullopt;
        }

        const auto* statement = std::get_if<expression_stmt_t>(&m_ast.get(node.body.front()).node);
        if (statement == nullptr || !is_super_call(statement->expression, "New")) {
            return std::nullopt;
        }

        return statement->expression;
    }

    /// Whether an expression is a call to Super's `name`, like "Super.New()".
    [[nodiscard]] bool is_super_call(const expr_id_t id, const std::string_view name) const {
        const auto* call = std::get_if<call_expr_t>(&m_ast.get(id).node);
        if (call == nullptr) {
            return false;
        }

        const auto* member = std::get_if<member_expr_t>(&m_ast.get(call->callee).node);
        return member != nullptr && member->member.text == name &&
               std::holds_alternative<super_expr_t>(m_ast.get(member->object).node);
    }

    /// A Struct is stored inline, so it can't contain itself, directly or through other Structs.
    /// A Class's fields are in its object, so they can refer to it.
    void check_struct_cycles() {
        for (const stmt_id_t id : m_ast.items()) {
            const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node);
            const std::optional<symbol_id_t> owner = m_analysis.stmts[index(id)].symbol;
            if (declared == nullptr || declared->is_class || !owner) {
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

    /// Whether a value of `type` holds a `target` inside, counting `type` itself. A T? holds its
    /// T, but a Class only refers to its object, so it holds nothing inside.
    bool contains(const type_id_t type, const type_id_t target, std::vector<type_id_t>& seen) {
        if (type == target) {
            return true;
        }

        if (is_nullable(type) || is_fixed_array(type)) {
            return contains(element_of(type), target, seen);
        }

        if (!is_struct(type) || std::ranges::contains(seen, type)) {
            return false;
        }

        seen.push_back(type);
        return std::ranges::any_of(m_analysis.types.get(type).fields, [&](const field_t& field) {
            return contains(field.type, target, seen);
        });
    }

    /// A field whose type has no zero needs a value from every constructor. The one a Struct
    /// gets takes it without a default, and a Class without a New would leave it unassigned.
    void check_field_zeros() {
        for (const stmt_id_t id : m_ast.items()) {
            const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node);
            const std::optional<symbol_id_t> owner = m_analysis.stmts[index(id)].symbol;
            if (declared == nullptr || !owner) {
                continue;
            }

            const std::vector<symbol_id_t>& members = symbol(*owner).members;
            const bool has_new = std::ranges::any_of(members, [this](const symbol_id_t member) {
                return symbol(member).kind == symbol_kind_t::CONSTRUCTOR &&
                       symbol(member).declaration.has_value();
            });
            for (const symbol_id_t member : members) {
                symbol_t& field = symbol(member);
                if (field.kind != symbol_kind_t::FIELD || m_analysis.types.has_zero(field.type) ||
                    has_written_default(field)) {
                    continue;
                }

                field.has_default = false;
                if (declared->is_class && !has_new) {
                    error(field.span,
                          "{} needs a New that assigns '{}', since {} has no zero value",
                          name_of(field.owner), field.name, name_of(field.type));
                }
            }
        }
    }

    /// Whether a field's declaration gives it a default.
    [[nodiscard]] bool has_written_default(const symbol_t& field) const {
        return field.declaration &&
               std::get<variable_stmt_t>(m_ast.get(*field.declaration).node).value.has_value();
    }

    /// A field's type, in its type's fields and in those of every Class extending it.
    void set_field_type(const symbol_id_t id, const type_id_t type) {
        symbol(id).type = type;
        const symbol_t& field = symbol(id);
        for (const auto& entry : m_declared_types) {
            const type_id_t declared = entry.first;
            if (!m_analysis.types.extends(declared, field.owner)) {
                continue;
            }

            std::vector<field_t> fields = m_analysis.types.get(declared).fields;
            fields[field.field].type = type;
            m_analysis.types.set_fields(declared, std::move(fields));
        }
    }

    /// A Function, or a type's Method or New when `owner` is set. A Method's Self is a parameter
    /// that isn't among the others, ByRef in a "ByRef Method". A Struct's New makes Self as a
    /// Local and returns it. A Class's Self is always a reference that can't be replaced.
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
                .kind = constructor && !is_class(owner) ? symbol_kind_t::LOCAL
                                                        : symbol_kind_t::PARAMETER,
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

        check_modifiers(node, owner);
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
            .modifiers = node.modifiers,
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

    [[nodiscard]] bool is_class(const type_id_t type) const {
        return m_analysis.types.get(type).kind == type_kind_t::CLASS;
    }

    /// Structs and Classes have fields and Methods.
    [[nodiscard]] bool has_members(const type_id_t type) const {
        return is_struct(type) || is_class(type);
    }

    [[nodiscard]] bool is_nullable(const type_id_t type) const {
        return m_analysis.types.get(type).kind == type_kind_t::NULLABLE;
    }

    /// "T[]", a reference to an array that can grow.
    [[nodiscard]] bool is_array(const type_id_t type) const {
        return m_analysis.types.get(type).kind == type_kind_t::ARRAY;
    }

    /// "T[N]" or "T[W, H]", stored inline.
    [[nodiscard]] bool is_fixed_array(const type_id_t type) const {
        return m_analysis.types.get(type).kind == type_kind_t::FIXED_ARRAY;
    }

    [[nodiscard]] bool is_any_array(const type_id_t type) const {
        return is_array(type) || is_fixed_array(type);
    }

    /// An array's T.
    [[nodiscard]] type_id_t element_of(const type_id_t type) const {
        return m_analysis.types.get(type).inner;
    }

    /// Whether == works on a type: not on arrays, or a Struct or T? holding one.
    [[nodiscard]] bool comparable(const type_id_t type) const {
        if (is_any_array(type)) {
            return false;
        }

        if (is_nullable(type)) {
            return comparable(non_null(type));
        }

        return !is_struct(type) ||
               std::ranges::all_of(m_analysis.types.get(type).fields,
                                   [this](const field_t& field) { return comparable(field.type); });
    }

    /// The T of a T?, and any other type as it is.
    [[nodiscard]] type_id_t non_null(const type_id_t type) const {
        return is_nullable(type) ? m_analysis.types.get(type).inner : type;
    }

    /// The T? of a T, and a T? as it is.
    type_id_t nullable_of(const type_id_t type) {
        return is_nullable(type) ? type : m_analysis.types.nullable(type);
    }

    /// Whether `from` converts to `to` without an 'As': losslessly between numbers, from a Class
    /// to one it extends, and from a T, or a T?, to a U? when the T converts to the U.
    [[nodiscard]] bool converts(const type_id_t from, const type_id_t to) const {
        if (from == to || !is_nullable(to)) {
            return converts_plainly(from, to);
        }

        return converts_plainly(non_null(from), non_null(to));
    }

    [[nodiscard]] bool converts_plainly(const type_id_t from, const type_id_t to) const {
        return converts_losslessly(from, to) ||
               (is_class(from) && is_class(to) && m_analysis.types.extends(from, to));
    }

    /// Whether objects of two Classes can be the same object: one extends the other.
    [[nodiscard]] bool related(const type_id_t a, const type_id_t b) const {
        return m_analysis.types.extends(a, b) || m_analysis.types.extends(b, a);
    }

    /// The type both sides convert to, if there is one. Int32 and Int64? meet at Int64?.
    std::optional<type_id_t> common_type(const type_id_t a, const type_id_t b) {
        if (converts(a, b)) {
            return b;
        }

        if (converts(b, a)) {
            return a;
        }

        // Two Classes meet at the nearest one both extend.
        if (is_class(non_null(a)) && is_class(non_null(b))) {
            for (type_id_t base = non_null(a); base != type_id_t::INVALID; base = base_of(base)) {
                if (m_analysis.types.extends(non_null(b), base)) {
                    return is_nullable(a) || is_nullable(b) ? nullable_of(base) : base;
                }
            }

            return std::nullopt;
        }

        if (!is_nullable(a) && !is_nullable(b)) {
            return std::nullopt;
        }

        const std::optional<type_id_t> inner = common_type(non_null(a), non_null(b));
        if (!inner) {
            return std::nullopt;
        }

        return nullable_of(*inner);
    }

    /// A known value converted without loss. A T? holds what its T would, or Null.
    [[nodiscard]] const_value_t convert_value(const const_value_t& value,
                                              const type_id_t type) const {
        if (!is_nullable(type)) {
            return *fold_cast(value, type);
        }

        if (std::holds_alternative<std::nullptr_t>(value.data)) {
            return { .type = type, .data = nullptr };
        }

        const const_value_t inner{ .type = non_null(value.type), .data = value.data };
        const_value_t converted = *fold_cast(inner, non_null(type));
        converted.type = type;
        return converted;
    }

    /// The type an annotation names: a built-in, a Struct, a Class, or a T?, T[], or fixed
    /// array of one. Types that don't exist yet are reported and give INVALID.
    type_id_t resolve_type(const type_expr_id_t id) {
        const type_expr_t& type = m_ast.get(id);
        if (const auto* array = std::get_if<array_type_t>(&type.node)) {
            const type_id_t element = resolve_type(array->element);
            if (array->sizes.empty()) {
                return m_analysis.types.array(element);
            }

            return resolve_fixed_array(element, *array, type.span);
        }

        if (const auto* nullable = std::get_if<nullable_type_t>(&type.node)) {
            const type_id_t inner = resolve_type(nullable->inner);
            if (is_nullable(inner)) {
                error(type.span, "{} can already be Null", name_of(inner));
                return inner;
            }

            return m_analysis.types.nullable(inner);
        }

        if (const auto* named = std::get_if<named_type_t>(&type.node)) {
            if (const std::optional<type_id_t> builtin = builtin_type(named->name)) {
                return *builtin;
            }

            if (const auto found = m_file_scope.find(named->name); found != m_file_scope.end()) {
                const symbol_t& declared = symbol(found->second.front());
                if (declared.kind == symbol_kind_t::TYPE) {
                    return declared.type;
                }

                error(type.span, "'{}' isn't a type", named->name);
            } else if (is_later_type(named->name)) {
                error(type.span, "'{}' isn't supported yet", named->name);
            } else {
                error(type.span, "'{}' isn't a type", named->name);
            }
        }

        return type_id_t::INVALID;
    }

    /// "T[N]" or "T[W, H]". Each size is a positive integer known at compile time, and the
    /// elements have to fit an Int's count.
    type_id_t resolve_fixed_array(const type_id_t element, const array_type_t& node,
                                  const span_t span) {
        std::vector<std::uint32_t> sizes;
        std::uint64_t count = 1;
        for (const expr_id_t expression : node.sizes) {
            if (check(expression, type_id_t::INT64) == type_id_t::INVALID) {
                return type_id_t::INVALID;
            }

            const std::optional<std::int64_t> size = integer_value(expression);
            if (!size) {
                error(span_of(expression),
                      "a fixed array's size must be an integer known at compile time, like 64 or "
                      "8 * 8");
                return type_id_t::INVALID;
            }

            if (*size < 1) {
                error(span_of(expression), "a fixed array's size must be at least 1");
                return type_id_t::INVALID;
            }

            count *= static_cast<std::uint64_t>(*size);
            if (count > max_count) {
                error(span, "a fixed array can hold at most {} elements", max_count);
                return type_id_t::INVALID;
            }

            sizes.push_back(static_cast<std::uint32_t>(*size));
        }

        return m_analysis.types.fixed_array(element, sizes);
    }

    /// The most elements an array can hold: as many as an Int counts.
    static constexpr std::uint64_t max_count = 2'147'483'647;

    /// A checked integer's value, when it's known and fits an Int64.
    [[nodiscard]] std::optional<std::int64_t> integer_value(const expr_id_t id) {
        const std::optional<const_value_t>& value = info(id).value;
        if (!value) {
            return std::nullopt;
        }

        if (const auto* number = std::get_if<std::int64_t>(&value->data)) {
            return *number;
        }

        const auto* number = std::get_if<std::uint64_t>(&value->data);
        if (number == nullptr ||
            *number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }

        return static_cast<std::int64_t>(*number);
    }

    // Flow

    void mark_assigned(const symbol_id_t id) {
        set_bit(m_flow.assigned, id, true);
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
        } else if (target.kind == symbol_kind_t::FIELD && std::ranges::contains(m_required, id)) {
            error(span, "'{}' is read before New assigns it", target.name);
        } else {
            return;
        }

        mark_assigned(id); // one error per variable is enough
    }

    /// Whether a nullable variable holds a value here, so it can be used as its T. In code that
    /// can't be reached, none does.
    [[nodiscard]] bool is_narrowed(const symbol_id_t id) const {
        return index(id) < m_flow.narrowed.size() && m_flow.narrowed[index(id)];
    }

    /// Whether a null check on a variable holds until it's assigned again: for a Local, a Const,
    /// or a parameter that isn't ByRef. A Global or a field can change during any call, and a
    /// ByRef parameter is the caller's variable.
    [[nodiscard]] bool is_narrowable(const symbol_id_t id) {
        const symbol_t& variable = symbol(id);
        return variable.kind == symbol_kind_t::LOCAL || variable.kind == symbol_kind_t::CONSTANT ||
               (variable.kind == symbol_kind_t::PARAMETER && !variable.by_ref);
    }

    /// The variable an expression names, when it's nullable and a null check can narrow it.
    std::optional<symbol_id_t> narrowable_variable(const expr_id_t id) {
        const std::optional<symbol_id_t> found = info(id).symbol;
        if (!std::holds_alternative<name_expr_t>(m_ast.get(id).node) || !found ||
            !is_nullable(symbol(*found).type) || !is_narrowable(*found)) {
            return std::nullopt;
        }

        return found;
    }

    /// A nullable variable that was just assigned holds a value when the value's type can't be
    /// Null.
    void narrow_after_assigning(const symbol_id_t id, const expr_id_t value) {
        if (is_nullable(symbol(id).type) && is_narrowable(id)) {
            set_bit(m_flow.narrowed, id, !is_nullable(info(value).type));
        }
    }

    /// A null check from before a loop only holds inside it if the loop never changes the
    /// variable, since the next pass would see the new value. So the variables a loop assigns,
    /// or passes ByRef, lose their narrowing where it starts.
    void forget_changed(const block_t& body, const std::optional<expr_id_t> condition = {}) {
        if (std::ranges::none_of(m_flow.narrowed, std::identity{})) {
            return;
        }

        std::vector<std::string_view> names;
        changed_in(body, names);
        if (condition) {
            changed_in(*condition, names);
        }

        for (std::size_t i = 0; i < m_flow.narrowed.size(); ++i) {
            if (std::ranges::contains(names, m_analysis.symbols[i].name)) {
                m_flow.narrowed[i] = false;
            }
        }
    }

    /// The names of the variables some code assigns or passes ByRef. A Local inside a loop can't
    /// share a name with one it can see outside, so the names say which outside variables change.
    void changed_in(const block_t& block, std::vector<std::string_view>& names) const {
        for (const stmt_id_t id : block) {
            changed_in(id, names);
        }
    }

    void changed_in(const stmt_id_t id, std::vector<std::string_view>& names) const {
        const stmt_node_t& node = m_ast.get(id).node;
        if (const auto* assign = std::get_if<assign_stmt_t>(&node)) {
            if (const auto* name = std::get_if<name_expr_t>(&m_ast.get(assign->target).node)) {
                names.push_back(name->name);
            }

            changed_in(assign->target, names);
            changed_in(assign->value, names);
        } else if (const auto* variable = std::get_if<variable_stmt_t>(&node)) {
            if (variable->value) {
                changed_in(*variable->value, names);
            }
        } else if (const auto* expression = std::get_if<expression_stmt_t>(&node)) {
            changed_in(expression->expression, names);
        } else if (const auto* returned = std::get_if<return_stmt_t>(&node)) {
            if (returned->value) {
                changed_in(*returned->value, names);
            }
        } else if (const auto* if_node = std::get_if<if_stmt_t>(&node)) {
            for (const if_branch_t& branch : if_node->branches) {
                changed_in(branch.condition, names);
                changed_in(branch.body, names);
            }

            if (if_node->else_body) {
                changed_in(*if_node->else_body, names);
            }
        } else if (const auto* select = std::get_if<select_stmt_t>(&node)) {
            changed_in(select->subject, names);
            for (const case_t& branch : select->cases) {
                changed_in(branch.body, names);
            }

            if (select->default_body) {
                changed_in(*select->default_body, names);
            }
        } else {
            changed_in_loop(node, names);
        }
    }

    void changed_in_loop(const stmt_node_t& node, std::vector<std::string_view>& names) const {
        if (const auto* loop = std::get_if<while_stmt_t>(&node)) {
            changed_in(loop->condition, names);
            changed_in(loop->body, names);
        } else if (const auto* repeat = std::get_if<repeat_stmt_t>(&node)) {
            changed_in(repeat->body, names);
            changed_in(repeat->condition, names);
        } else if (const auto* forever = std::get_if<loop_stmt_t>(&node)) {
            changed_in(forever->body, names);
        } else if (const auto* counted = std::get_if<for_stmt_t>(&node)) {
            changed_in(counted->start, names);
            changed_in(counted->end, names);
            if (counted->step) {
                changed_in(*counted->step, names);
            }

            changed_in(counted->body, names);
        } else if (const auto* each = std::get_if<for_in_stmt_t>(&node)) {
            changed_in(each->sequence, names);
            changed_in(each->body, names);
        }
    }

    /// In an expression, only a call can change a variable: by taking it ByRef.
    void changed_in(const expr_id_t id, std::vector<std::string_view>& names) const {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* call = std::get_if<call_expr_t>(&node)) {
            changed_in(call->callee, names);
            for (const argument_t& argument : call->arguments) {
                const auto* name = std::get_if<name_expr_t>(&m_ast.get(argument.value).node);
                if (argument.by_ref && name != nullptr) {
                    names.push_back(name->name);
                }

                changed_in(argument.value, names);
            }
        } else if (const auto* string = std::get_if<string_expr_t>(&node)) {
            for (const string_part_t& part : string->parts) {
                if (const auto* inner = std::get_if<expr_id_t>(&part)) {
                    changed_in(*inner, names);
                }
            }
        } else if (const auto* unary = std::get_if<unary_expr_t>(&node)) {
            changed_in(unary->operand, names);
        } else if (const auto* binary = std::get_if<binary_expr_t>(&node)) {
            changed_in(binary->left, names);
            changed_in(binary->right, names);
        } else if (const auto* ternary = std::get_if<ternary_expr_t>(&node)) {
            changed_in(ternary->condition, names);
            changed_in(ternary->if_true, names);
            changed_in(ternary->if_false, names);
        } else if (const auto* member = std::get_if<member_expr_t>(&node)) {
            changed_in(member->object, names);
        } else if (const auto* cast = std::get_if<cast_expr_t>(&node)) {
            changed_in(cast->value, names);
        }
    }

    /// New must assign the fields that have no zero, on every path, before it returns.
    void check_required(const span_t span, const std::string_view where) {
        for (const symbol_id_t field : m_required) {
            if (!is_assigned(field)) {
                error(span, "New must assign '{}' {}, since {} has no zero value",
                      symbol(field).name, where, name_of(symbol(field).type));
            }
        }
    }

    /// In a New, Self can only be used whole once Super.New has set up what it inherits, and
    /// the fields without a zero are assigned, since a Method could read them.
    void check_self_ready(const span_t span) {
        if (m_before_super) {
            error(span, "Self can't be used before Super.New");
            return;
        }

        for (const symbol_id_t field : m_required) {
            if (!is_assigned(field)) {
                error(span, "Self can't be used before New assigns '{}'", symbol(field).name);
                return;
            }
        }
    }

    // Top level

    /// Checks Global and top-level Const initializers in source order. Each one is marked
    /// assigned after its initializer, so reading one declared further down is an error.
    void initialize_globals() {
        m_initializing = true;
        for (const stmt_id_t id : m_ast.items()) {
            if (const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node)) {
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

    /// "[]" of a T[]: a default that isn't known at compile time, but gives each use a new empty
    /// array.
    [[nodiscard]] bool is_empty_array(const expr_id_t id) {
        const auto* array = std::get_if<array_expr_t>(&m_ast.get(id).node);
        return array != nullptr && array->elements.empty() && is_array(info(id).converted);
    }

    /// A field's default is a parameter default of the constructor its Struct gets, so it must be
    /// known at compile time, or be "[]". Field defaults are checked in source order with the
    /// Globals, so they can use Consts declared above the Struct.
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
        } else if (is_empty_array(value)) {
            symbol(*field).empty_default = true;
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
        if (const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node)) {
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
            } else if (is_empty_array(value)) {
                symbol(parameter).empty_default = true;
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
                !std::holds_alternative<type_stmt_t>(node)) {
                check_stmt(id);
            }
        }

        m_scopes.clear();
    }

    /// A function body starts with a fresh scope holding only its parameters, so it can't see
    /// top-level Locals. A Method or New also sees its Struct's members, through Self.
    void check_function(const stmt_id_t id) {
        if (const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node)) {
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

        // New must assign the fields that have no zero. The base Class's New assigns the ones
        // Self inherits, first thing.
        m_required.clear();
        const bool extends = function.kind == symbol_kind_t::CONSTRUCTOR &&
                             is_class(function.owner) &&
                             base_of(function.owner) != type_id_t::INVALID;
        m_super_new = extends ? super_new_of(function) : std::nullopt;
        m_before_super = m_super_new.has_value();
        if (function.kind == symbol_kind_t::CONSTRUCTOR) {
            for (type_id_t type = function.owner; type != type_id_t::INVALID;
                 type = base_of(type)) {
                for (const symbol_id_t member : symbol(m_declared_types.at(type)).members) {
                    // "[]" gives a T[] field its value.
                    if (symbol(member).kind == symbol_kind_t::FIELD &&
                        !m_analysis.types.has_zero(symbol(member).type) &&
                        !symbol(member).empty_default) {
                        m_required.push_back(member);
                    }

                    if (type != function.owner && !m_super_new) {
                        mark_assigned(member);
                    }
                }
            }
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
        // An Abstract Method has no body to return from.
        if (returns_value && m_flow.reachable && !function.modifiers.is_abstract) {
            error(function.span, "'{}' doesn't return a value on every path", function.name);
        }

        check_required(function.span, "on every path");
        m_required.clear();
        m_super_new.reset();
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
                narrow_after_assigning(*target->whole, node.value);
            }

            if (target->field) {
                mark_assigned(*target->field);
            }

            // A new Self in a Struct's New has all its fields.
            if (target->whole && target->whole == current_self()) {
                for (const symbol_id_t field : m_required) {
                    mark_assigned(field);
                }
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
            narrow_after_assigning(local, *node.value);
        } else if (is_fixed_array(symbol(local).type) && node.kind == variable_kind_t::LOCAL) {
            // A fixed array starts with every element at zero, ready to be filled in.
            if (m_analysis.types.has_zero(symbol(local).type)) {
                mark_assigned(local);
            } else {
                error(node.name.span, "'{}' needs a value, since {} has no zero value",
                      node.name.text, name_of(symbol(local).type));
            }
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
            } else if (declared.kind == symbol_kind_t::GLOBAL &&
                       !m_analysis.types.has_zero(declared.type)) {
                // A Local can wait to be assigned; a Global can't, and a Class has no zero.
                error(node.name.span, "'{}' needs a value, since {} has no zero value",
                      node.name.text, name_of(declared.type));
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

    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const type_stmt_t& node) {
        error(node.name.span, "Structs can only be declared at the top level");
    }

    /// Each condition starts from the flow where the ones before it were False, since a branch
    /// only runs when they didn't, and its body from where it's True. After the If, a Local
    /// counts as assigned only if every way through assigned it, including skipping every branch
    /// when there's no Else.
    void check_stmt(stmt_id_t /*id*/, span_t /*span*/, const if_stmt_t& node) {
        flow_t next = m_flow;
        flow_t result = unreachable_flow;
        for (const if_branch_t& branch : node.branches) {
            m_flow = std::move(next);
            auto [if_true, if_false] = check_branches(branch.condition);
            m_flow = std::move(if_true);
            check_block(branch.body);
            result = join(result, m_flow);
            next = std::move(if_false);
        }

        m_flow = std::move(next);
        if (node.else_body) {
            check_block(*node.else_body);
        }

        m_flow = join(result, m_flow);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const while_stmt_t& node) {
        forget_changed(node.body, node.condition);
        auto [if_true, if_false] = check_branches(node.condition);
        const bool forever = is_constant(node.condition, true);
        m_flow = std::move(if_true);
        push_loop(id, node.label);
        check_block(node.body);
        const loop_t loop = pop_loop();
        // The body may not run, so only what was assigned before the loop counts after it, where
        // the condition is False. Code after a While True is reached only through Break.
        if (forever) {
            m_flow = unreachable_flow;
        } else {
            m_flow = std::move(if_false);
        }

        leave_loop(loop);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const repeat_stmt_t& node) {
        forget_changed(node.body, node.condition);
        push_loop(id, node.label);
        check_block(node.body);
        const loop_t loop = pop_loop();
        // The body runs at least once, so its assignments count. Until is reached from the end
        // of the body and from every Continue, and it can't see the body's Locals.
        for (const flow_t& path : loop.continues) {
            m_flow = join(m_flow, path);
        }

        // The loop ends where the condition is True.
        const flow_t if_true = check_branches(node.condition).first;
        m_flow = is_constant(node.condition, false) ? unreachable_flow : if_true;
        leave_loop(loop);
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const loop_stmt_t& node) {
        forget_changed(node.body);
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
        forget_changed(node.body);
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

    /// "For x In items" goes over an array's elements in order, and "For i, x In items" counts
    /// them with an Int. A T[] is read once, and its Count before each pass.
    void check_stmt(const stmt_id_t id, span_t /*span*/, const for_in_stmt_t& node) {
        const type_id_t sequence = check(node.sequence, type_id_t::NONE);
        type_id_t element = type_id_t::INVALID;
        if (is_any_array(sequence)) {
            element = element_of(sequence);
        } else if (is_nullable(sequence) && is_any_array(non_null(sequence))) {
            report_maybe_null(node.sequence);
        } else if (sequence != type_id_t::INVALID) {
            error(span_of(node.sequence), "'For ... In' goes over arrays, not {}",
                  name_of(sequence));
        }

        // The body may not run, so the flow after the loop is the flow before it, plus any
        // Breaks.
        forget_changed(node.body);
        const flow_t entry = m_flow;
        push_loop(id, node.label);
        push_scope();
        if (node.index) {
            m_analysis.stmts[index(id)].index = declare_local({
                .kind = symbol_kind_t::LOOP_VARIABLE,
                .name = node.index->text,
                .span = node.index->span,
                .type = type_id_t::INT32,
                .declaration = id,
            });
        }

        m_analysis.stmts[index(id)].symbol = declare_local({
            .kind = symbol_kind_t::LOOP_VARIABLE,
            .name = node.value.text,
            .span = node.value.span,
            .type = element,
            .declaration = id,
        });
        check_block(node.body);
        pop_scope();
        const loop_t loop = pop_loop();
        m_flow = entry;
        leave_loop(loop);
    }

    /// "Case e As Boss": each Case matches an object of a Class related to the subject's, and
    /// names it as one. A Case after one for a Class it extends never matches.
    void check_type_select(const stmt_id_t id, const select_stmt_t& node) {
        const type_id_t subject = check(node.subject, type_id_t::NONE);
        const type_id_t object = non_null(subject);
        const bool valid = subject != type_id_t::INVALID && is_class(object);
        if (subject != type_id_t::INVALID && !valid) {
            error(span_of(node.subject), "'Case ... As' matches objects of Classes, not {}",
                  name_of(subject));
        }

        const flow_t entry = m_flow;
        flow_t result = unreachable_flow;
        std::vector<std::pair<type_id_t, span_t>> seen;
        std::vector<symbol_id_t> matches;
        for (const case_t& branch : node.cases) {
            m_flow = entry;
            if (!branch.match) {
                error(span_of(branch.values.front().value),
                      "a Select's Cases are all values, or all type matches like 'Case e As Boss'");
                check_block(branch.body);
                result = join(result, m_flow);
                continue;
            }

            const type_id_t type =
                check_case_type(*branch.match, valid ? object : type_id_t::INVALID, seen);
            push_scope();
            const symbol_id_t variable = declare_local({
                .kind = symbol_kind_t::LOCAL,
                .name = branch.match->name.text,
                .span = branch.match->name.span,
                .type = type,
                .declaration = id,
            });
            mark_assigned(variable);
            matches.push_back(variable);
            check_block(branch.body);
            pop_scope();
            result = join(result, m_flow);
        }

        m_flow = entry;
        if (node.default_body) {
            check_block(*node.default_body);
        }

        m_flow = join(result, m_flow);
        m_analysis.stmts[index(id)].matches = std::move(matches);
    }

    /// One type match's Class, which an earlier Case in `seen` mustn't cover.
    type_id_t check_case_type(const type_case_t& match, const type_id_t object,
                              std::vector<std::pair<type_id_t, span_t>>& seen) {
        const type_id_t type = resolve_type(match.type);
        const span_t span = m_ast.get(match.type).span;
        if (type == type_id_t::INVALID) {
            return type;
        }

        if (!is_class(type)) {
            error(span, "'Case ... As' matches a Class, not {}", name_of(type));
            return type_id_t::INVALID;
        }

        if (object != type_id_t::INVALID && !related(object, type)) {
            error(span, "{} and {} aren't related, so this Case never matches", name_of(object),
                  name_of(type));
            return type;
        }

        for (const auto& [earlier, earlier_span] : seen) {
            if (m_analysis.types.extends(type, earlier)) {
                error(span, "this Case is already covered by an earlier Case")
                    .with_label(earlier_span, "covered here");
                return type;
            }
        }

        seen.emplace_back(type, span);
        return type;
    }

    void check_stmt(const stmt_id_t id, span_t /*span*/, const select_stmt_t& node) {
        if (std::ranges::any_of(node.cases,
                                [](const case_t& branch) { return branch.match.has_value(); })) {
            check_type_select(id, node);
            return;
        }

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

            check_required(span, "before this Return");
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

    /// Checks a condition, and gives the flows where it's True and where it's False.
    std::pair<flow_t, flow_t> check_branches(const expr_id_t id) {
        check_condition(id);
        return branches_of(id);
    }

    /// The flows where a condition that was just checked is True and where it's False. They
    /// differ when it checks for Null: "e != Null" narrows e where it's True.
    std::pair<flow_t, flow_t> branches_of(const expr_id_t id) {
        if (!m_branches || m_branches->condition != id) {
            return { m_flow, m_flow };
        }

        branches_t found = std::move(*m_branches);
        m_branches.reset();
        return { std::move(found.if_true), std::move(found.if_false) };
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
        /// A field of Self that's replaced whole, by name or as "Self.x", which a New may need
        /// to assign.
        std::optional<symbol_id_t> field;
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
            return target_t{
                .type = type,
                .whole = field ? std::nullopt : found,
                .field = field ? found : std::nullopt,
            };
        }

        if (const auto* member = std::get_if<member_expr_t>(&node);
            member != nullptr && member->null_safe) {
            check(id, type_id_t::NONE);
            error(span_of(id), "a '?.' can't be assigned to; check that it isn't Null first");
            return std::nullopt;
        }

        if (std::holds_alternative<member_expr_t>(node) ||
            std::holds_alternative<self_expr_t>(node) ||
            std::holds_alternative<index_expr_t>(node)) {
            // Assigning doesn't read the target, so New can assign the fields it hasn't yet.
            m_partial = id;
            const type_id_t type = check(id, type_id_t::NONE);
            m_partial.reset();
            if (type == type_id_t::INVALID) {
                return std::nullopt;
            }

            if (const auto* member = std::get_if<member_expr_t>(&node);
                member != nullptr && is_any_array(non_null(info(member->object).type))) {
                error(span_of(id),
                      "an array's Count can't be changed; use Add, Insert, RemoveAt, or Clear");
                return std::nullopt;
            }

            // An object's fields can change through any reference to it.
            if (in_object(id)) {
                return target_t{ .type = type, .field = self_field(id) };
            }

            const std::optional<symbol_id_t> root = root_of(id);
            if (!root) {
                error(span_of(id), "only a variable or one of its fields can be assigned to");
                return std::nullopt;
            }

            if (!check_assignable(*root, span_of(id), "changed")) {
                return std::nullopt;
            }

            // Replacing Self, in a Struct's New, replaces all of it.
            const bool is_self = std::holds_alternative<self_expr_t>(node);
            return target_t{
                .type = type,
                .whole = is_self ? root : std::nullopt,
                .field = self_field(id),
            };
        }

        if (!std::holds_alternative<error_expr_t>(node)) {
            error(span_of(id), "only a variable can be assigned to");
        }

        return std::nullopt;
    }

    /// The function being checked's Self, if it's a Method or New.
    [[nodiscard]] std::optional<symbol_id_t> current_self() {
        return m_function ? symbol(*m_function).self : std::nullopt;
    }

    /// The field in "Self.x", after it's checked.
    std::optional<symbol_id_t> self_field(const expr_id_t id) {
        const auto* member = std::get_if<member_expr_t>(&m_ast.get(id).node);
        if (member == nullptr ||
            !std::holds_alternative<self_expr_t>(m_ast.get(member->object).node)) {
            return std::nullopt;
        }

        return info(id).symbol;
    }

    /// Whether a place, after it's checked, is in an object: "e.hp" with `e` a Class, a field
    /// used by name in a Class's Method, or an element of a T[]. A reference lets its holder use
    /// the object, so those can change even through a read-only variable.
    [[nodiscard]] bool in_object(const expr_id_t id) {
        if (const auto* member = std::get_if<member_expr_t>(&m_ast.get(id).node)) {
            return is_class(info(member->object).type) || in_object(member->object);
        }

        // A T[]'s elements are in its object too.
        if (const auto* element = std::get_if<index_expr_t>(&m_ast.get(id).node)) {
            return is_array(info(element->object).type) || in_object(element->object);
        }

        const std::optional<symbol_id_t> found = info(id).symbol;
        return found && symbol(*found).kind == symbol_kind_t::FIELD &&
               is_class(symbol(*found).owner);
    }

    /// The variable a place starts from, after it's checked: "p" in "p.pos.x", and Self for a
    /// field used by name. nullopt for anything else, like a call's result, or a field after a
    /// '?.', which may not be there.
    std::optional<symbol_id_t> root_of(const expr_id_t id) {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* member = std::get_if<member_expr_t>(&node)) {
            return member->null_safe ? std::nullopt : root_of(member->object);
        }

        if (const auto* element = std::get_if<index_expr_t>(&node)) {
            return root_of(element->object);
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

    /// Whether a place is stored in a T[]'s elements, which move when the array grows: "a[i]" or
    /// "a[i].pos", but not a field of an object the element refers to.
    [[nodiscard]] bool in_array_storage(const expr_id_t id) {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* element = std::get_if<index_expr_t>(&node)) {
            return is_array(info(element->object).type) || in_array_storage(element->object);
        }

        const auto* member = std::get_if<member_expr_t>(&node);
        return member != nullptr && !is_class(info(member->object).type) &&
               in_array_storage(member->object);
    }

    /// Whether a place goes through an array's element.
    [[nodiscard]] bool through_element(const expr_id_t id) {
        const expr_node_t& node = m_ast.get(id).node;
        if (std::holds_alternative<index_expr_t>(node)) {
            return true;
        }

        const auto* member = std::get_if<member_expr_t>(&node);
        return member != nullptr && through_element(member->object);
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
            if (is_class(target.owner) || can_change_self()) {
                return true;
            }

            error(span, "'{}' is a field, and a Method can't change Self; make it a ByRef Method",
                  target.name);
            return false;
        case symbol_kind_t::PARAMETER:
            if (target.by_ref) {
                return true;
            }

            if (id == current_self() && is_class(target.type)) {
                error(span, "Self is the object itself and can't be replaced");
                return false;
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
        case symbol_kind_t::TYPE:
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
        // A literal expected as a T? takes the T, then converts.
        if (is_flexible(id)) {
            hint = non_null(hint);
        }

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
        if (!converts(info(id).type, type)) {
            report_mismatch(id, type);
            return false;
        }

        convert(id, type);
        return true;
    }

    /// A literal would have taken the expected numeric type if its value fit, so it says that.
    /// A T? where a T is expected needs a null check first.
    void report_mismatch(const expr_id_t id, const type_id_t expected) {
        const expr_info_t& found = info(id);
        if (found.value && is_numeric(found.type) && is_numeric(non_null(expected)) &&
            is_flexible(id)) {
            error(span_of(id), "{} doesn't fit in {}", text(id), name_of(expected));
        } else if (is_nullable(found.type) && converts(non_null(found.type), expected)) {
            error(span_of(id),
                  "expected {}, found {}, which may be Null; check it first, or give a value for "
                  "Null with '{}'",
                  name_of(expected), name_of(found.type), spelling(binary_op_t::COALESCE));
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
        if (out.value) {
            out.value = convert_value(*out.value, type);
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
            if (const type_id_t type = check(inner, type_id_t::NONE);
                has_members(type) || is_any_array(type)) {
                error(span_of(inner), "{} can't be shown in a string", name_of(type));
            } else if (is_nullable(type)) {
                error(span_of(inner),
                      "{} can't be shown in a string, since it may be Null; give a value for Null "
                      "with '{}'",
                      name_of(type), spelling(binary_op_t::COALESCE));
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

    /// Null takes the nullable type its context expects.
    type_id_t check_expr(const expr_id_t id, const null_expr_t& /*node*/, const type_id_t hint) {
        if (hint == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        if (hint == type_id_t::NONE) {
            error(span_of(id),
                  "Null needs a type from where it's used, like 'Local e: Enemy? = Null'");
            return type_id_t::INVALID;
        }

        if (!is_nullable(hint)) {
            error(span_of(id), "{} can't be Null; {}? can", name_of(hint), name_of(hint));
            return type_id_t::INVALID;
        }

        info(id).value = const_value_t{ .type = hint, .data = nullptr };
        return hint;
    }

    [[nodiscard]] bool is_null(const expr_id_t id) const {
        return std::holds_alternative<null_expr_t>(m_ast.get(id).node);
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

        // After a null check, a nullable variable is its T until it's assigned again.
        if (!is_nullable(target.type) || !is_narrowed(*found)) {
            return target.type;
        }

        const type_id_t type = non_null(target.type);
        if (std::optional<const_value_t>& value = info(id).value) {
            if (std::holds_alternative<std::nullptr_t>(value->data)) {
                value.reset(); // only in a branch that can't run
            } else {
                value->type = type;
            }
        }

        return type;
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

        if (symbol(first).kind == symbol_kind_t::TYPE) {
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

    /// A Struct's or Class's fields or Methods with a name, its own first, then the ones it
    /// inherits, without a Method that one further down overrides. Constructors aren't reached
    /// by name.
    std::vector<symbol_id_t> members_of(const type_id_t type, const std::string_view name) {
        std::vector<symbol_id_t> members;
        for (type_id_t current = type; current != type_id_t::INVALID; current = base_of(current)) {
            const auto found = m_declared_types.find(current);
            if (found == m_declared_types.end()) {
                break;
            }

            for (const symbol_id_t id : symbol(found->second).members) {
                const symbol_t& member = symbol(id);
                const bool overridden = std::ranges::any_of(members, [&](const symbol_id_t other) {
                    return symbol(other).kind == symbol_kind_t::METHOD &&
                           same_parameters(symbol(other), member);
                });
                if (member.name == name && member.kind != symbol_kind_t::CONSTRUCTOR &&
                    (member.kind != symbol_kind_t::METHOD || !overridden)) {
                    members.push_back(id);
                }
            }
        }

        return members;
    }

    /// Using a T? as its T needs a null check, which only narrows some variables.
    void report_maybe_null(const expr_id_t id) {
        if (narrowable_variable(id)) {
            error(span_of(id), "'{}' may be Null; check that it isn't, or use '?.'", text(id));
        } else {
            error(span_of(id), "'{}' may be Null; check a Local copy of it, or use '?.'", text(id));
        }
    }

    /// A T[]'s Methods: Add, Insert, RemoveAt, Clear, Contains, and Clone. Their owner is the
    /// array type, and they have no declaration.
    const std::vector<symbol_id_t>& array_methods(const type_id_t type) {
        if (const auto found = m_array_methods.find(type); found != m_array_methods.end()) {
            return found->second;
        }

        const type_id_t element = element_of(type);
        const auto parameter = [this](const std::string_view name, const type_id_t parameter_type) {
            return add_symbol(
                { .kind = symbol_kind_t::PARAMETER, .name = name, .type = parameter_type });
        };

        std::vector<symbol_id_t> methods;
        const auto method = [&](const std::string_view name, const type_id_t result,
                                std::vector<symbol_id_t> parameters) {
            methods.push_back(add_symbol({
                .kind = symbol_kind_t::METHOD,
                .name = name,
                .type = result,
                .parameters = std::move(parameters),
                .owner = type,
            }));
        };

        method("Add", type_id_t::NONE, { parameter("value", element) });
        method("Insert", type_id_t::NONE,
               { parameter("index", type_id_t::INT32), parameter("value", element) });
        method("RemoveAt", type_id_t::NONE, { parameter("index", type_id_t::INT32) });
        method("Clear", type_id_t::NONE, {});
        method("Contains", type_id_t::BOOL, { parameter("value", element) });
        method("Clone", type, {});
        return m_array_methods[type] = std::move(methods);
    }

    /// The Methods after a '.' on an array, by name. A fixed array has only its Count.
    std::vector<symbol_id_t> lookup_array_method(const type_id_t type, const name_t& member) {
        if (member.text == "Count") {
            error(member.span, "Count isn't a Method; leave out the ()");
            return {};
        }

        std::vector<symbol_id_t> methods;
        if (is_array(type)) {
            for (const symbol_id_t method : array_methods(type)) {
                if (symbol(method).name == member.text) {
                    methods.push_back(method);
                }
            }
        }

        if (methods.empty()) {
            error(member.span, "{} has no member named '{}'", name_of(type), member.text);
        }

        return methods;
    }

    /// "a.Count": how many elements an array has, as an Int. A fixed array's is known at compile
    /// time.
    type_id_t check_count(const expr_id_t id, const member_expr_t& node, const type_id_t object) {
        if (node.member.text != "Count") {
            const bool method =
                is_array(object) &&
                std::ranges::any_of(array_methods(object), [&](const symbol_id_t found) {
                    return symbol(found).name == node.member.text;
                });
            if (method) {
                error(node.member.span, "'{}' is a method; call it with ()", node.member.text);
            } else {
                error(node.member.span, "{} has no member named '{}'", name_of(object),
                      node.member.text);
            }

            return type_id_t::INVALID;
        }

        if (node.null_safe) {
            return nullable_of(type_id_t::INT32);
        }

        if (is_fixed_array(object)) {
            info(id).value =
                make_number(m_analysis.types.element_count(object), false, type_id_t::INT32);
        }

        return type_id_t::INT32;
    }

    /// The members named after a '.' on `object`: a field, or a Method's overloads. Reports a
    /// value that has no members, or no member with that name, or may be Null.
    std::vector<symbol_id_t> lookup_member(const expr_id_t object, const type_id_t type,
                                           const name_t& member) {
        if (type == type_id_t::INVALID || member.text.empty()) {
            return {};
        }

        if (is_nullable(type) && has_members(non_null(type))) {
            report_maybe_null(object);
            return {};
        }

        if (!has_members(type)) {
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

        // "!c" swaps c's branches.
        if (is_not && m_branches && m_branches->condition == node.operand) {
            auto [if_true, if_false] = branches_of(node.operand);
            m_branches = branches_t{
                .condition = id,
                .if_true = std::move(if_false),
                .if_false = std::move(if_true),
            };
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
            return check_coalesce(id, node, hint);
        case binary_op_t::AND:
        case binary_op_t::OR:
            type = check_logical(id, node);
            break;
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
                check_null_comparison(id, node);
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

    /// && and ||: the right side only runs when the left doesn't decide, so it sees the left's
    /// narrowing. In "e != Null && e.hp > 0", e holds a value on the right.
    type_id_t check_logical(const expr_id_t id, const binary_expr_t& node) {
        const bool is_and = node.op == binary_op_t::AND;
        const bool left_valid = check_condition(node.left);
        auto [left_true, left_false] = branches_of(node.left);
        m_flow = is_and ? left_true : left_false;
        const bool right_valid = check_condition(node.right);
        auto [right_true, right_false] = branches_of(node.right);
        branches_t result{
            .condition = id,
            .if_true = is_and ? std::move(right_true) : join(left_true, right_true),
            .if_false = is_and ? join(left_false, right_false) : std::move(right_false),
        };
        m_flow = join(result.if_true, result.if_false);
        m_branches = std::move(result);
        return left_valid && right_valid ? type_id_t::BOOL : type_id_t::INVALID;
    }

    /// "e != Null" narrows e where it's True, and "e == Null" where it's False. Comparing
    /// something that can't be Null with Null always gives the same answer.
    void check_null_comparison(const expr_id_t id, const binary_expr_t& node) {
        if (is_null(node.left) == is_null(node.right)) {
            return;
        }

        const expr_id_t other = is_null(node.left) ? node.right : node.left;
        const bool is_equal = node.op == binary_op_t::EQUAL;
        if (!is_nullable(info(other).type)) {
            warning(span_of(id), "'{}' can't be Null here, so this is always {}", text(other),
                    is_equal ? "False" : "True");
            return;
        }

        const std::optional<symbol_id_t> variable = narrowable_variable(other);
        if (!variable) {
            return;
        }

        flow_t holds = m_flow;
        set_bit(holds.narrowed, *variable, true);
        m_branches = branches_t{
            .condition = id,
            .if_true = is_equal ? m_flow : holds,
            .if_false = is_equal ? holds : m_flow,
        };
    }

    /// "a ?? b": the value in a, or else b, which only runs when a is Null. b takes the T of a's
    /// T?, and the result is the type both meet at.
    type_id_t check_coalesce(const expr_id_t id, const binary_expr_t& node, const type_id_t hint) {
        const bool typed = hint != type_id_t::NONE && hint != type_id_t::INVALID;
        const type_id_t left = check(node.left, typed ? nullable_of(hint) : hint);
        if (left == type_id_t::INVALID) {
            check(node.right, type_id_t::NONE);
            return type_id_t::INVALID;
        }

        if (!is_nullable(left)) {
            warning(span_of(node.left), "'{}' can't be Null here, so '{}' always gives it",
                    text(node.left), spelling(node.op));
        }

        const type_id_t inner = non_null(left);
        const type_id_t right = check(node.right, is_null(node.right) ? left : inner);
        if (right == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        const std::optional<type_id_t> common = common_type(inner, right);
        if (!common) {
            error(span_of(id), "the sides of '{}' have different types, {} and {}",
                  spelling(node.op), name_of(inner), name_of(right));
            return type_id_t::INVALID;
        }

        convert(node.right, *common);
        // A known value decides it: b is only needed for Null.
        if (const std::optional<const_value_t>& value = info(node.left).value) {
            if (std::holds_alternative<std::nullptr_t>(value->data)) {
                info(id).value = info(node.right).value;
            } else {
                info(id).value = convert_value({ .type = inner, .data = value->data }, *common);
            }
        }

        return *common;
    }

    /// Checks both operands. A literal side takes the other side's type, so `x + 1` works for
    /// any numeric x, and Null takes the T? of the other side's T. With `flows`, each side is
    /// checked in its own flow, which is updated: a ternary's results each see their side of
    /// the condition.
    std::pair<type_id_t, type_id_t> check_operands(const expr_id_t left, const expr_id_t right,
                                                   type_id_t hint,
                                                   std::array<flow_t, 2>* flows = nullptr) {
        const auto check_side = [&](const std::size_t side, const type_id_t side_hint) {
            if (flows != nullptr) {
                m_flow = (*flows)[side];
            }

            const type_id_t type = check(side == 0 ? left : right, side_hint);
            if (flows != nullptr) {
                (*flows)[side] = m_flow;
            }

            return type;
        };

        if (is_null(left) != is_null(right)) {
            const std::size_t other = is_null(left) ? 1 : 0;
            const type_id_t other_type = check_side(other, hint);
            const type_id_t null_type = check_side(1 - other, nullable_of(other_type));
            return other == 0 ? std::pair{ other_type, null_type }
                              : std::pair{ null_type, other_type };
        }

        const bool left_flexible = is_flexible(left);
        const bool right_flexible = is_flexible(right);
        if (left_flexible && !right_flexible) {
            const type_id_t right_type = check_side(1, hint);
            return { check_side(0, is_numeric(right_type) ? right_type : hint), right_type };
        }

        if (right_flexible && !left_flexible) {
            const type_id_t left_type = check_side(0, hint);
            return { left_type, check_side(1, is_numeric(left_type) ? left_type : hint) };
        }

        // Two literal sides with no numeric hint share one default type: "1 < 0.5" compares
        // Floats.
        if (left_flexible && !is_numeric(hint)) {
            hint = has_decimal(left) || has_decimal(right) ? type_id_t::FLOAT32 : type_id_t::INT32;
        }

        const type_id_t left_type = check_side(0, hint);
        return { left_type, check_side(1, hint) };
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
        if ((op == binary_op_t::EQUAL || op == binary_op_t::NOT_EQUAL) && !comparable(*common)) {
            error(span, "'{}' can't be used on {}{}", spelling(op), name_of(*common),
                  is_array(non_null(*common)) ? "; RefEquals tells whether two are the same array"
                                              : "");
            return type_id_t::INVALID;
        }

        if (supports(op, *common)) {
            return *common;
        }

        if (*common == type_id_t::STRING && is_ordering(op)) {
            error(span, "Strings can only be compared with == and != for now");
        } else if (is_nullable(*common)) {
            error(span, "'{}' can't be used on {}, which may be Null", spelling(op),
                  name_of(*common));
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

    /// Each result is checked where its side of the condition holds.
    type_id_t check_expr(const expr_id_t id, const ternary_expr_t& node, const type_id_t hint) {
        const bool condition_valid = check_condition(node.condition);
        auto [if_true, if_false] = branches_of(node.condition);
        std::array<flow_t, 2> flows{ std::move(if_true), std::move(if_false) };
        const auto [true_type, false_type] =
            check_operands(node.if_true, node.if_false, hint, &flows);
        m_flow = join(flows[0], flows[1]);
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
        check_special_call(node, symbol(*function));
        // The constructor a Struct gets takes its fields, which may not have types yet.
        if (symbol(*function).kind == symbol_kind_t::CONSTRUCTOR &&
            !symbol(*function).declaration) {
            for (const symbol_id_t field : symbol(*function).parameters) {
                check_field_ready(field, span_of(node.callee));
            }
        }

        const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
        const bool on_super = member != nullptr &&
                              std::holds_alternative<super_expr_t>(m_ast.get(member->object).node);
        // A Method called by name, or on Super, gets Self.
        if (symbol(*function).kind == symbol_kind_t::METHOD && (member == nullptr || on_super)) {
            check_self_ready(span_of(node.callee));
        }

        check_class_call(id, symbol(*function), on_super);

        // "a?.M()" gives Null when a is Null.
        const type_id_t type = symbol(*function).type;
        if (member != nullptr && member->null_safe && type != type_id_t::NONE) {
            return nullable_of(type);
        }

        return type;
    }

    /// An Abstract Class can't be made, and Super can't call an Abstract Method. Super.New only
    /// starts a New, and sets up what Self inherits.
    void check_class_call(const expr_id_t id, const symbol_t& function, const bool on_super) {
        const bool is_new = function.kind == symbol_kind_t::CONSTRUCTOR;
        if (is_new && !on_super && is_class(function.owner) &&
            symbol(m_declared_types.at(function.owner)).modifiers.is_abstract) {
            error(span_of(id), "{} is Abstract, so it can't be made; make a Class that extends it",
                  name_of(function.owner));
        }

        if (!on_super) {
            return;
        }

        if (function.modifiers.is_abstract) {
            error(span_of(id), "'{}' is Abstract in {}, so Super can't call it", function.name,
                  name_of(function.owner));
        }

        if (is_new && m_super_new != id) {
            error(span_of(id), "Super.New can only start a New");
        } else if (is_new && m_function) {
            m_before_super = false;
            const type_id_t owner = symbol(*m_function).owner;
            for (const symbol_id_t field : m_required) {
                if (symbol(field).owner != owner) {
                    mark_assigned(field);
                }
            }
        }
    }

    /// RefEquals only compares Class references, and Destroy runs by itself.
    void check_special_call(const call_expr_t& node, const symbol_t& function) {
        if (function.kind == symbol_kind_t::BUILTIN_FUNCTION && function.name == "RefEquals") {
            for (const argument_t& argument : node.arguments) {
                const type_id_t type = info(argument.value).type;
                if (is_null(argument.value)) {
                    error(span_of(argument.value),
                          "RefEquals compares two references; check for Null with '== Null'");
                } else if (type != type_id_t::INVALID && !is_class(non_null(type)) &&
                           !is_array(non_null(type))) {
                    error(span_of(argument.value), "RefEquals compares Class references, not {}",
                          name_of(type));
                }
            }
        }

        if (function.kind == symbol_kind_t::METHOD && function.name == "Contains" &&
            is_array(function.owner) && !comparable(element_of(function.owner))) {
            error(span_of(node.callee), "Contains compares with ==, which {} doesn't have",
                  name_of(element_of(function.owner)));
        }

        if (function.kind == symbol_kind_t::METHOD && function.name == "Destroy" &&
            is_class(function.owner)) {
            error(span_of(node.callee),
                  "Destroy runs by itself when an object's last reference goes away; it can't "
                  "be called");
        }
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

        // An element of a T[] could move while the Method changes it, if the array grows.
        if (in_array_storage(member->object)) {
            error(span_of(member->object),
                  "'{}' is a ByRef Method, and an element of a T[] can move when the array grows; "
                  "change a Local copy, then store it back",
                  function.name);
            return;
        }

        // An object's fields, and a T[]'s elements, change through any reference.
        if (in_object(member->object)) {
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

    void check_alone(const std::vector<expr_id_t>& expressions) {
        for (const expr_id_t expression : expressions) {
            check(expression, type_id_t::NONE);
        }
    }

    /// What a callee names: a function's overloads, a Struct's constructors, or Methods. A Method
    /// is called after a '.' or '?.' on a Struct value, or by name in another of its Struct's
    /// Methods.
    std::vector<symbol_id_t> lookup_callee(const expr_id_t callee) {
        if (const auto* member = std::get_if<member_expr_t>(&m_ast.get(callee).node);
            member != nullptr &&
            std::holds_alternative<super_expr_t>(m_ast.get(member->object).node)) {
            return lookup_super(*member);
        }

        if (const auto* member = std::get_if<member_expr_t>(&m_ast.get(callee).node)) {
            type_id_t type = check(member->object, type_id_t::NONE);
            if (member->null_safe) {
                type = check_safe_object(member->object, type);
            }

            if (is_any_array(type)) {
                return lookup_array_method(type, member->member);
            }

            if (is_nullable(type) && is_any_array(non_null(type))) {
                report_maybe_null(member->object);
                return {};
            }

            std::vector<symbol_id_t> members = lookup_member(member->object, type, member->member);
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
        if (first.kind == symbol_kind_t::TYPE) {
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

    /// "Super.M" names the Methods of the Class that Self's Class extends, and "Super.New" its
    /// News. They're called on Self, without the overrides further down.
    std::vector<symbol_id_t> lookup_super(const member_expr_t& member) {
        const span_t span = span_of(member.object);
        const type_id_t owner = m_function ? symbol(*m_function).owner : type_id_t::INVALID;
        if (!is_class(owner) || base_of(owner) == type_id_t::INVALID) {
            error(span, "'Super' needs a Class that extends another, in one of its Methods");
            return {};
        }

        const type_id_t base = base_of(owner);
        info(member.object) = { .type = base, .converted = base, .symbol = current_self() };
        if (member.member.text == "New") {
            std::vector<symbol_id_t> constructors;
            for (const symbol_id_t id : symbol(m_declared_types.at(base)).members) {
                if (symbol(id).kind == symbol_kind_t::CONSTRUCTOR) {
                    constructors.push_back(id);
                }
            }

            return constructors;
        }

        std::vector<symbol_id_t> members = lookup_member(member.object, base, member.member);
        if (!members.empty() && symbol(members.front()).kind == symbol_kind_t::FIELD) {
            error(member.member.span, "'{}' is a field; use it without Super", member.member.text);
            return {};
        }

        return members;
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
            if (type == type_id_t::INVALID && !is_flexible(argument.value) &&
                !is_null(argument.value)) {
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
            for (std::size_t i = 0; i < types.size(); ++i) {
                list += i == 0 ? "" : ", ";
                list += is_null(node.arguments[i].value) ? "Null" : name_of(types[i]);
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
    /// exactly, and any other type it fits by conversion. Null fits any T? by conversion. A
    /// ByRef argument needs the exact type.
    match_t match(const argument_t& argument, const type_id_t type) {
        if (is_null(argument.value)) {
            return is_nullable(type) || type == type_id_t::INVALID ? match_t::CONVERTIBLE
                                                                   : match_t::NONE;
        }

        const type_id_t natural = check(argument.value, type_id_t::NONE);
        if (natural == type || type == type_id_t::INVALID) {
            return match_t::EXACT;
        }

        if (argument.by_ref) {
            return match_t::NONE;
        }

        if (is_flexible(argument.value)) {
            return check(argument.value, type) == non_null(type) ? match_t::CONVERTIBLE
                                                                 : match_t::NONE;
        }

        return converts(natural, type) ? match_t::CONVERTIBLE : match_t::NONE;
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
        const type_id_t checked = check(value, parameter.type);
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

        const type_id_t type = by_ref_type(value, checked);
        if (type == type_id_t::INVALID) {
            return;
        }

        if (through_element(value)) {
            error(span_of(value),
                  "an array's element can't be passed ByRef yet; copy it into a Local");
            return;
        }

        // An object's fields can be passed through any reference to it. Two references might be
        // the same object, so only the same variable counts as overlapping.
        const std::optional<symbol_id_t> variable = root_of(value);
        if (!in_object(value)) {
            if (!variable) {
                error(span_of(value), "only a variable or one of its fields can be passed ByRef");
                return;
            }

            if (!check_assignable(*variable, span_of(value), "passed ByRef")) {
                return;
            }
        }

        if (!variable) {
            if (type != parameter.type && parameter.type != type_id_t::INVALID) {
                error(span_of(value), "a ByRef argument must be exactly {}, but this is {}",
                      name_of(parameter.type), name_of(type));
            }

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

    /// A ByRef argument is the variable itself: one that was narrowed is its T? again, and may
    /// be Null after the call.
    type_id_t by_ref_type(const expr_id_t value, const type_id_t type) {
        const std::optional<symbol_id_t> variable = narrowable_variable(value);
        if (!variable) {
            return type;
        }

        const type_id_t declared = symbol(*variable).type;
        info(value).type = declared;
        info(value).converted = declared;
        set_bit(m_flow.narrowed, *variable, false);
        return declared;
    }

    /// The object before a '?.', as its T. One that can't be Null works too, though '.' would do.
    type_id_t check_safe_object(const expr_id_t object, const type_id_t type) {
        if (type != type_id_t::INVALID && !is_nullable(type)) {
            warning(span_of(object), "'{}' can't be Null here, so '?.' isn't needed", text(object));
        }

        return non_null(type);
    }

    /// A field after a '.', or after a '?.', which gives Null when the object is Null. Methods
    /// can only be called. "Self.x" only uses one field, so a New can use it before the others
    /// are assigned.
    type_id_t check_expr(const expr_id_t id, const member_expr_t& node, type_id_t /*hint*/) {
        const bool target = m_partial == id;
        const bool on_self = std::holds_alternative<self_expr_t>(m_ast.get(node.object).node);
        m_partial = on_self ? std::optional(node.object) : std::nullopt;
        type_id_t object = check(node.object, type_id_t::NONE);
        m_partial.reset();
        if (node.null_safe) {
            object = check_safe_object(node.object, object);
        }

        if (is_any_array(object)) {
            return check_count(id, node, object);
        }

        if (is_nullable(object) && is_any_array(non_null(object))) {
            report_maybe_null(node.object);
            return type_id_t::INVALID;
        }

        const std::vector<symbol_id_t> members = lookup_member(node.object, object, node.member);
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
        if (on_self && !target) {
            check_read(member, span_of(id));
        }

        if (!node.null_safe) {
            return symbol(member).type;
        }

        const type_id_t type = nullable_of(symbol(member).type);
        if (const std::optional<const_value_t>& value = info(node.object).value;
            value && std::holds_alternative<std::nullptr_t>(value->data)) {
            info(id).value = const_value_t{ .type = type, .data = nullptr };
        }

        return type;
    }

    type_id_t check_expr(const expr_id_t id, const self_expr_t& /*node*/, type_id_t /*hint*/) {
        const std::optional<symbol_id_t> self = current_self();
        if (!self) {
            error(span_of(id), "'Self' can only be used in a Method or New");
            return type_id_t::INVALID;
        }

        if (m_partial != id) {
            check_self_ready(span_of(id));
        }

        info(id).symbol = self;
        return symbol(*self).type;
    }

    /// Super only comes before a call, which lookup_super() checks.
    type_id_t check_expr(const expr_id_t id, const super_expr_t& /*node*/, type_id_t /*hint*/) {
        error(span_of(id), "'Super' only comes before a call, like 'Super.Hurt(5)'");
        return type_id_t::INVALID;
    }

    /// "a[i]", or "grid[x, y]" with an index for each dimension. Indices are integers of any
    /// type. A T[]'s are checked when the program runs, and a fixed array's known ones here too.
    type_id_t check_expr(const expr_id_t id, const index_expr_t& node, type_id_t /*hint*/) {
        const type_id_t object = check(node.object, type_id_t::NONE);
        if (object == type_id_t::INVALID) {
            check_alone(node.indices);
            return type_id_t::INVALID;
        }

        if (is_nullable(object) && is_any_array(non_null(object))) {
            report_maybe_null(node.object);
            return type_id_t::INVALID;
        }

        if (!is_any_array(object)) {
            error(span_of(node.object), "{} can't be indexed", name_of(object));
            check_alone(node.indices);
            return type_id_t::INVALID;
        }

        const std::vector<std::uint32_t>& sizes = m_analysis.types.get(object).sizes;
        const std::size_t dimensions = is_array(object) ? 1 : sizes.size();
        if (node.indices.size() != dimensions) {
            error(span_of(id), "{} takes {} {}", name_of(object), dimensions,
                  dimensions == 1 ? "index" : "indices");
            check_alone(node.indices);
            return type_id_t::INVALID;
        }

        bool valid = true;
        for (std::size_t i = 0; i < dimensions; ++i) {
            valid = check_index(node.indices[i], is_array(object) ? 0 : sizes[i], object) && valid;
        }

        return valid ? element_of(object) : type_id_t::INVALID;
    }

    /// One index: an integer, which can't be negative, or past `size` when it's known (0 for a
    /// T[], whose size isn't).
    bool check_index(const expr_id_t id, const std::uint32_t size, const type_id_t array) {
        const type_id_t type = check(id, type_id_t::INT32);
        if (type == type_id_t::INVALID) {
            return false;
        }

        if (!is_integer(type)) {
            error(span_of(id), "an index must be an integer, not {}", name_of(type));
            return false;
        }

        const std::optional<std::int64_t> value = integer_value(id);
        if (value && *value < 0) {
            error(span_of(id), "an index can't be negative");
            return false;
        }

        if (value && size != 0 && std::cmp_greater_equal(*value, size)) {
            error(span_of(id), "index {} is out of range for {}", *value, name_of(array));
            return false;
        }

        return true;
    }

    /// "[a, b, c]": a new T[], or a T[N] when one is expected. The elements take the expected
    /// T, or else meet at a common one, with literals taking it.
    type_id_t check_expr(const expr_id_t id, const array_expr_t& node, const type_id_t hint) {
        const type_id_t expected = non_null(hint);
        if (is_fixed_array(expected) && m_analysis.types.get(expected).sizes.size() > 1) {
            error(span_of(id),
                  "{} can't come from an array literal; it starts with every element at zero",
                  name_of(expected));
            check_alone(node.elements);
            return type_id_t::INVALID;
        }

        if (is_any_array(expected)) {
            return check_elements(id, node, expected);
        }

        if (node.elements.empty()) {
            error(span_of(id), "an empty array needs a type from where it's used, like 'Local a: "
                               "Int[] = []'");
            return type_id_t::INVALID;
        }

        // Typed elements first, then Null and literals, which take the type they meet at. NONE
        // until the first one.
        type_id_t common = type_id_t::NONE;
        for (const bool flexible : { false, true }) {
            for (const expr_id_t element : node.elements) {
                if ((is_flexible(element) || is_null(element)) != flexible) {
                    continue;
                }

                type_id_t element_hint = type_id_t::NONE;
                if (is_null(element) && common != type_id_t::NONE) {
                    element_hint = nullable_of(common);
                } else if (is_numeric(common)) {
                    element_hint = common;
                } else if (is_flexible(element)) {
                    element_hint =
                        has_decimal_among(node.elements) ? type_id_t::FLOAT32 : type_id_t::INT32;
                }

                const type_id_t type = check(element, element_hint);
                if (type == type_id_t::INVALID) {
                    return type_id_t::INVALID;
                }

                const std::optional<type_id_t> meet =
                    common == type_id_t::NONE ? type : common_type(common, type);
                if (!meet) {
                    error(span_of(element),
                          "the elements of an array have different types, {} and {}",
                          name_of(common), name_of(type));
                    return type_id_t::INVALID;
                }

                common = *meet;
            }
        }

        for (const expr_id_t element : node.elements) {
            convert(element, common);
        }

        return m_analysis.types.array(common);
    }

    /// Whether any of the literal elements has a decimal, which makes them all Floats.
    [[nodiscard]] bool has_decimal_among(const std::vector<expr_id_t>& elements) const {
        return std::ranges::any_of(elements, [this](const expr_id_t element) {
            return is_flexible(element) && has_decimal(element);
        });
    }

    /// The elements of an array literal of the type `array`, which a fixed array has a number
    /// of.
    type_id_t check_elements(const expr_id_t id, const array_expr_t& node, const type_id_t array) {
        const type_id_t element = element_of(array);
        bool valid = true;
        for (const expr_id_t value : node.elements) {
            check(value, element);
            valid = expect_type(value, element) && valid;
        }

        const std::uint64_t count = m_analysis.types.element_count(array);
        if (is_fixed_array(array) && node.elements.size() != count) {
            error(span_of(id), "{} needs {} elements, not {}", name_of(array), count,
                  node.elements.size());
            return type_id_t::INVALID;
        }

        return valid ? array : type_id_t::INVALID;
    }

    /// A cast gives a literal its type directly: `5_000_000_000 As Long`.
    type_id_t check_expr(const expr_id_t id, const cast_expr_t& node, type_id_t /*hint*/) {
        const type_id_t target = resolve_type(node.type);
        const type_id_t source = check(node.value, node.checked ? type_id_t::NONE : target);
        if (target == type_id_t::INVALID || source == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        if (node.checked) {
            return check_checked_cast(id, node, source, target);
        }

        // Numbers convert to each other, a Bool to an integer as 0 or 1, and anything to what
        // it converts to without 'As', like a T to a T?. An object converts to a related Class,
        // which is checked at run time.
        const bool classes = is_class(non_null(source)) && is_class(non_null(target)) &&
                             related(non_null(source), non_null(target));
        const bool allowed = converts(source, target) || classes ||
                             (is_numeric(source) && is_numeric(target)) ||
                             (source == type_id_t::BOOL && is_integer(target));
        if (!allowed) {
            error(span_of(id), "'As' can't convert {} to {}", name_of(source), name_of(target));
            return type_id_t::INVALID;
        }

        // The only known object is Null, which a T? keeps, and a T can't be.
        if (const std::optional<const_value_t>& value = info(node.value).value) {
            if (is_nullable(target)) {
                info(id).value = convert_value(*value, target);
            } else if (!classes) {
                fold(id, fold_cast(*value, target));
            }
        }

        return target;
    }

    /// "x As? T": x as a T?, which is Null when x isn't a T, or is Null.
    type_id_t check_checked_cast(const expr_id_t id, const cast_expr_t& node,
                                 const type_id_t source, const type_id_t target) {
        const span_t type_span = m_ast.get(node.type).span;
        if (is_nullable(target)) {
            error(type_span, "'As?' gives a {} already; write 'As? {}'", name_of(target),
                  name_of(non_null(target)));
            return type_id_t::INVALID;
        }

        const type_id_t object = non_null(source);
        if (!is_class(object) || !is_class(target)) {
            error(span_of(id), "'As?' converts objects between Classes, not {} to {}",
                  name_of(source), name_of(target));
            return type_id_t::INVALID;
        }

        if (!related(object, target)) {
            error(span_of(id), "{} and {} aren't related, so this is always Null", name_of(object),
                  name_of(target));
            return type_id_t::INVALID;
        }

        const type_id_t type = nullable_of(target);
        if (const std::optional<const_value_t>& value = info(node.value).value) {
            info(id).value = convert_value(*value, type);
        }

        return type;
    }

    /// "x Is T": whether x is an object of the Class T, or of one that extends it. False for
    /// Null.
    type_id_t check_expr(const expr_id_t id, const is_expr_t& node, type_id_t /*hint*/) {
        const type_id_t target = resolve_type(node.type);
        const type_id_t source = check(node.value, type_id_t::NONE);
        if (target == type_id_t::INVALID || source == type_id_t::INVALID) {
            return type_id_t::INVALID;
        }

        const type_id_t object = non_null(source);
        if (!is_class(object)) {
            error(span_of(node.value), "'Is' tests objects of Classes, not {}", name_of(source));
            return type_id_t::INVALID;
        }

        if (!is_class(target)) {
            error(m_ast.get(node.type).span, "'Is' tests for a Class, not {}", name_of(target));
            return type_id_t::INVALID;
        }

        if (!related(object, target)) {
            error(span_of(id), "{} and {} aren't related, so this is always False", name_of(object),
                  name_of(target));
            return type_id_t::INVALID;
        }

        if (!is_nullable(source) && m_analysis.types.extends(object, target)) {
            warning(span_of(id), "this is always True: '{}' is {} already", text(node.value),
                    name_of(object));
        }

        info(id).tested = target;
        return type_id_t::BOOL;
    }
};

} // namespace

analysis_t analyze(const source_file_t& file, const ast_t& ast, diagnostic_bag_t& diagnostics) {
    return checker_t(file, ast, diagnostics).run();
}

} // namespace fb
