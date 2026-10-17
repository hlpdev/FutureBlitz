#include "fb/lower.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
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

/// "Add(Int32, Int32)": overloads get different names. A Struct's members start with its name:
/// "Point.Move(Int32)".
/// An Operator's name in MIR, like "Operator +", or any other function's name as it is.
std::string operator_name(const std::string_view name) {
    const bool letter =
        !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) != 0 || name[0] == '_');
    return letter || name.empty() ? std::string(name) : std::format("Operator {}", name);
}

std::string function_name(const analysis_t& analysis, const symbol_t& function) {
    std::string name;
    if (function.owner != type_id_t::INVALID) {
        name = std::format("{}.", analysis.types.name(function.owner));
    }

    name += operator_name(function.name) + "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        const symbol_t& parameter = analysis.symbol(function.parameters[i]);
        name += i == 0 ? "" : ", ";
        name += parameter.by_ref ? "ByRef " : "";
        name += analysis.types.name(parameter.type);
    }

    return name + ")";
}

/// What leaving a scope does: releases a variable, or runs a Defer's statement or a Finally.
struct cleanup_t {
    std::variant<local_id_t, stmt_id_t, const block_t*> action;

    // Implicit, so a variable, a deferred statement, or a Finally can be put in a scope directly.
    cleanup_t(const local_id_t local) : action(local) {}
    cleanup_t(const stmt_id_t deferred) : action(deferred) {}
    cleanup_t(const block_t* finally) : action(finally) {}
};

/// A loop being lowered. Break and Continue end their blocks without a target, and the loop
/// fills it in once it has made its exit and the place where its next pass starts.
struct loop_t {
    /// The loop statement, which the analysis gives each Break and Continue.
    stmt_id_t stmt;
    std::size_t scope_depth;        // the scopes inside the loop start here
    std::vector<block_id_t> breaks; // blocks that jump past the loop
    std::vector<block_id_t> continues;
};

/// Where a raised error goes: to the Catches of the innermost Try block, or past the innermost
/// Try?, which gives Null instead.
struct handler_t {
    /// A Try?. Its result is set to Null, unless it gives nothing, and the temporaries made inside
    /// it, from `temporaries` on, are released.
    bool optional = false;
    std::optional<local_id_t> result;
    std::size_t temporaries = 0;
    /// A Try block: the scopes inside it start at `scope_depth`, and `error` holds the error for
    /// its Catches.
    std::size_t scope_depth = 0;
    local_id_t error{};
    /// The blocks that jump to the Catches, or past the Try?.
    std::vector<block_id_t> exits;
};

/// Turns the analyzed AST into MIR: one function per Function, Method, and New, plus "main" for
/// the top-level code. Each variable holding a String, or a Struct with Strings in it, owns a
/// reference to each one, so lowering adds the retains and releases. Parameters borrow the
/// caller's.
class lowerer_t {
public:
    lowerer_t(const ast_t& ast, const analysis_t& analysis) : m_ast(ast), m_analysis(analysis) {
        m_module.types = std::make_shared<type_table_t>(analysis.types);
        for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
            if (analysis.symbols[i].kind == symbol_kind_t::TYPE) {
                m_declared_types[analysis.symbols[i].type] = static_cast<symbol_id_t>(i);
            }
        }
    }

    mir_module_t run() {
        declare_globals();
        declare_functions();
        // Every Class in the main file is in the module, whether or not the program makes one. A
        // generic one has one for each instance.
        for (const symbol_t& symbol : m_analysis.symbols) {
            if (symbol.kind == symbol_kind_t::TYPE && !from_library(symbol) &&
                types().get(symbol.type).parameters.empty()) {
                use_class(symbol.type);
            }
        }

        for (const stmt_id_t id : m_ast.items()) {
            // A library file's code is lowered once the program uses it, in lower_library().
            if (m_ast.from_library(id)) {
                continue;
            }

            // Generic code is lowered for each set of type arguments it's used with.
            const stmt_node_t& node = m_ast.get(id).node;
            if (const auto* function = std::get_if<function_stmt_t>(&node);
                function != nullptr && function->type_parameters.empty()) {
                lower_function(id, *function);
            } else if (const auto* declared = std::get_if<type_stmt_t>(&node);
                       declared != nullptr && declared->type_parameters.empty()) {
                for (const stmt_id_t member : declared->members) {
                    lower_member(member);
                }
            }
        }

        lower_top_level();
        lower_library();
        for (const auto& [type, function] : m_texts) {
            lower_text(type, function);
        }

        // The Classes come in the order the program used them, and the module should come out
        // the same however the code is laid out.
        std::ranges::sort(m_module.classes, {}, &mir_class_t::type);
        return std::move(m_module);
    }

private:
    const ast_t& m_ast;
    const analysis_t& m_analysis;
    mir_module_t m_module;
    std::unordered_map<symbol_id_t, function_id_t> m_functions;
    std::unordered_map<symbol_id_t, global_id_t> m_globals;
    /// Each Class's Method table, by its type, made when it's first needed.
    std::unordered_map<type_id_t, std::vector<symbol_id_t>> m_tables;
    /// Each Class Method's slot in the Method tables.
    std::unordered_map<symbol_id_t, std::uint32_t> m_slots;
    /// The Classes the module has, in the order the program used them.
    std::vector<type_id_t> m_used_classes;
    /// The library functions the program uses, in the order it used them. Their bodies are
    /// lowered after the main file's code.
    std::vector<symbol_id_t> m_library_functions;
    /// A closure whose code waits to be lowered: the code that made it may be an instance of
    /// generic code, whose type arguments the closure's code has too.
    struct pending_closure_t {
        closure_t closure;
        type_id_t type;
        std::vector<type_id_t> parameters;
        std::vector<type_id_t> arguments;
    };

    /// The function values the program makes, whose code is lowered after the function that
    /// makes them, and that code's function, by the closure's Class.
    std::vector<pending_closure_t> m_closures;
    std::unordered_map<type_id_t, function_id_t> m_closure_code;
    /// An instance of a generic function: the function, and a type argument for each type
    /// parameter it has, its Struct's or Class's first.
    using instance_t = std::pair<symbol_id_t, std::vector<type_id_t>>;
    /// The instances the program uses, each lowered once, in the order they were used.
    std::map<instance_t, function_id_t> m_instances;
    std::vector<instance_t> m_pending_instances;
    /// While lowering an instance of generic code: its type parameters, and the type argument
    /// that each one stands for.
    std::vector<type_id_t> m_parameters;
    std::vector<type_id_t> m_arguments;
    /// The Enums whose values strings show, with the function that gives each value's text.
    /// Their bodies are lowered last.
    std::vector<std::pair<type_id_t, function_id_t>> m_texts;
    /// Each Struct's symbol, by its type.
    std::unordered_map<type_id_t, symbol_id_t> m_declared_types;

    // The function being lowered
    function_id_t m_function{};
    std::unordered_map<symbol_id_t, local_id_t> m_locals;
    std::optional<block_id_t> m_block;            // nullopt after a jump: nothing reaches here
    std::vector<std::vector<cleanup_t>> m_scopes; // what leaving each does, innermost last
    std::vector<local_id_t> m_temporaries;        // owned values to release after the statement
    std::vector<loop_t> m_loops;                  // innermost last
    /// A Method's or New's Self.
    std::optional<local_id_t> m_self;
    /// Lowering a New, whose Returns give back Self.
    bool m_returns_self = false;
    /// What takes the errors raised here, innermost last. Without one, they leave the function.
    std::vector<handler_t> m_handlers;
    /// In a function that throws: the caller's variable for its error, its last parameter.
    std::optional<local_id_t> m_error_slot;

    // Declarations

    /// A Method or New, or a Property's Get and Set, which are Methods.
    void lower_member(const stmt_id_t id) {
        const stmt_node_t& node = m_ast.get(id).node;
        if (const auto* method = std::get_if<function_stmt_t>(&node)) {
            lower_function(id, *method);
        } else if (const auto* property = std::get_if<property_stmt_t>(&node)) {
            for (const std::optional<stmt_id_t> part : { property->getter, property->setter }) {
                if (part) {
                    lower_member(*part);
                }
            }
        }
    }

    /// Globals and top-level Consts. One whose value is known at compile time starts with it.
    /// The rest start at zero, and main sets the ones with an initializer first.
    void declare_globals() {
        for (const symbol_id_t id : m_analysis.globals) {
            const symbol_t& symbol = m_analysis.symbol(id);
            const auto& node =
                std::get<variable_stmt_t>(m_ast.get(expect(symbol.declaration)).node);
            // Only a built-in type's zero is a constant, and a T?'s, which is Null. A Struct or
            // Class starts all zero.
            std::optional<const_value_t> initial;
            if (types().get(symbol.type).kind == type_kind_t::BUILTIN) {
                initial = zero(symbol.type);
            } else if (is_nullable(symbol.type)) {
                initial = null_of(symbol.type);
            } else if (is_enum(symbol.type)) {
                initial = enum_constant(symbol.type, types().enum_zero(symbol.type));
            }

            if (node.value) {
                if (const std::optional<const_value_t>& value =
                        m_analysis.expr(*node.value).value) {
                    initial = *value;
                }
            }

            // A Static Field is named after its type.
            std::string name(symbol.name);
            if (symbol.owner != type_id_t::INVALID) {
                name = std::format("{}.{}", types().name(symbol.owner), symbol.name);
            }

            m_globals[id] = static_cast<global_id_t>(m_module.globals.size());
            m_module.globals.push_back(
                { .name = std::move(name), .type = symbol.type, .initial = std::move(initial) });
        }
    }

    /// The main file's functions get their ids before any body is lowered, so a call can reach
    /// a function declared further down. "main" comes after them, then the library functions and
    /// the instances of generic ones, as the program uses them.
    void declare_functions() {
        for (const symbol_id_t id : m_analysis.functions) {
            if (!from_library(m_analysis.symbol(id)) &&
                type_parameters_of(m_analysis.symbol(id)).empty()) {
                declare_function(id);
            }
        }

        m_module.entry = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({ .name = "main" });
    }

    function_id_t declare_function(const symbol_id_t id) {
        const symbol_t& symbol = m_analysis.symbol(id);
        // A Class's New sets up an object that its caller made, and returns nothing.
        const bool sets_up = symbol.kind == symbol_kind_t::CONSTRUCTOR && is_class(symbol.type);
        const auto function = static_cast<function_id_t>(m_module.functions.size());
        m_functions[id] = function;
        m_module.functions.push_back({
            .name = function_name(m_analysis, symbol),
            .return_type = sets_up ? type_id_t::NONE : symbol.type,
        });
        return function;
    }

    /// Whether a symbol is declared in a library file.
    [[nodiscard]] bool from_library(const symbol_t& symbol) const {
        return symbol.declaration && m_ast.from_library(*symbol.declaration);
    }

    /// A function's id. A library function gets one when the program first uses it, so the
    /// program only gets the library code it uses. A generic one has one for each set of type
    /// arguments it's used with.
    function_id_t function_of(const symbol_id_t id, const std::vector<type_id_t>& arguments = {}) {
        if (!arguments.empty()) {
            return instance_of(id, arguments);
        }

        if (const auto found = m_functions.find(id); found != m_functions.end()) {
            return found->second;
        }

        m_library_functions.push_back(id);
        return declare_function(id);
    }

    /// An instance of a generic function, declared the first time it's used. Its body is lowered
    /// with the others, after the main file's code.
    function_id_t instance_of(const symbol_id_t id, const std::vector<type_id_t>& arguments) {
        instance_t key{ id, arguments };
        if (const auto found = m_instances.find(key); found != m_instances.end()) {
            return found->second;
        }

        const symbol_t& symbol = m_analysis.symbol(id);
        const std::vector<type_id_t> parameters = type_parameters_of(symbol);
        const bool sets_up = symbol.kind == symbol_kind_t::CONSTRUCTOR && is_class(symbol.type);
        const auto function = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({
            .name = instance_name(symbol, parameters, arguments),
            .return_type =
                sets_up ? type_id_t::NONE : types().substitute(symbol.type, parameters, arguments),
        });
        m_instances.emplace(key, function);
        m_pending_instances.push_back(std::move(key));
        return function;
    }

    /// Like "Box<Int32>.Map<String>(Func(Int32): String)": the function's name, with the type
    /// arguments of its Struct or Class and its own, and its parameter types.
    std::string instance_name(const symbol_t& function, const std::vector<type_id_t>& parameters,
                              const std::vector<type_id_t>& arguments) {
        std::string name;
        std::size_t own = 0;
        if (function.owner != type_id_t::INVALID) {
            own = types().get(function.owner).parameters.size();
            const type_id_t owner = types().substitute(function.owner, parameters, arguments);
            name = std::format("{}.", types().name(owner));
        }

        name += operator_name(function.name);
        for (std::size_t i = own; i < arguments.size(); ++i) {
            name += std::format("{}{}", i == own ? "<" : ", ", types().name(arguments[i]));
        }

        name += arguments.size() > own ? ">(" : "(";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            const type_id_t type = types().substitute(parameter.type, parameters, arguments);
            name += std::format("{}{}{}", i == 0 ? "" : ", ", parameter.by_ref ? "ByRef " : "",
                                types().name(type));
        }

        return name + ")";
    }

    /// The type parameters a function's types can use: its Struct's or Class's, then its own.
    [[nodiscard]] std::vector<type_id_t> type_parameters_of(const symbol_t& function) const {
        std::vector<type_id_t> parameters;
        if (function.owner != type_id_t::INVALID && !is_array(function.owner) &&
            !is_enum(function.owner)) {
            parameters = types().get(function.owner).parameters;
        }

        parameters.insert(parameters.end(), function.type_parameters.begin(),
                          function.type_parameters.end());
        return parameters;
    }

    /// The module's types, which instances of generic code add to.
    [[nodiscard]] type_table_t& types() const {
        return *m_module.types;
    }

    /// A type from the analysis, with the type arguments of the instance being lowered in it.
    type_id_t concrete(const type_id_t type) {
        return types().substitute(type, m_parameters, m_arguments);
    }

    /// A constant from the analysis, like Null, whose type is concrete too.
    const_value_t concrete(const_value_t value) {
        value.type = concrete(value.type);
        return value;
    }

    /// Types from the analysis, each made concrete.
    std::vector<type_id_t> concrete(const std::vector<type_id_t>& types) {
        std::vector<type_id_t> result;
        result.reserve(types.size());
        for (const type_id_t type : types) {
            result.push_back(concrete(type));
        }

        return result;
    }

    type_id_t expr_type(const expr_id_t id) {
        return concrete(m_analysis.expr(id).type);
    }

    type_id_t converted_type(const expr_id_t id) {
        return concrete(m_analysis.expr(id).converted);
    }

    /// The generic Struct or Class an instance comes from, or any other type as it is.
    [[nodiscard]] type_id_t definition_of(const type_id_t type) const {
        const type_id_t generic = types().get(type).generic;
        return generic != type_id_t::INVALID ? generic : type;
    }

    /// The type arguments a call passes: the instance's for a Method of a generic Struct or
    /// Class, from `object`, the type of the object or of the one a New makes, then the call's
    /// own.
    std::vector<type_id_t> call_arguments(const symbol_t& function, const expr_info_t& info,
                                          const type_id_t object) {
        std::vector<type_id_t> arguments;
        if (!type_parameters_of(function).empty() && function.owner != type_id_t::INVALID &&
            !types().get(function.owner).parameters.empty()) {
            arguments = types().get(non_null(object)).arguments;
        }

        for (const type_id_t argument : info.type_arguments) {
            arguments.push_back(concrete(argument));
        }

        return arguments;
    }

    /// The T of a T?, and any other type as it is.
    [[nodiscard]] type_id_t non_null(const type_id_t type) const {
        return is_nullable(type) ? types().get(type).inner : type;
    }

    /// Puts a Class in the module, with the Classes it extends. The program needs it where it
    /// makes an object of it or tests for it, and the main file's Classes are always there.
    void use_class(type_id_t type) {
        type = non_null(type);
        if (!is_class(type) || std::ranges::contains(m_used_classes, type)) {
            return;
        }

        m_used_classes.push_back(type);
        if (const type_id_t base = types().get(type).base; base != type_id_t::INVALID) {
            use_class(base);
        }
    }

    /// What the program uses from the library files: each Class it used, then the bodies of the
    /// functions it called, which can use more. The code of the function values it makes comes
    /// here too.
    void lower_library() {
        std::size_t classes = 0;
        std::size_t functions = 0;
        std::size_t closures = 0;
        std::size_t instances = 0;
        while (classes < m_used_classes.size() || functions < m_library_functions.size() ||
               closures < m_closures.size() || instances < m_pending_instances.size()) {
            if (classes < m_used_classes.size()) {
                add_class(m_used_classes[classes++]);
                continue;
            }

            // Copies, since lowering them can make more.
            if (closures < m_closures.size()) {
                const pending_closure_t closure = m_closures[closures++];
                lower_closure(closure);
                continue;
            }

            if (instances < m_pending_instances.size()) {
                const instance_t instance = m_pending_instances[instances++];
                const symbol_t& symbol = m_analysis.symbol(instance.first);
                const stmt_id_t id = expect(symbol.declaration);
                m_parameters = type_parameters_of(symbol);
                m_arguments = instance.second;
                lower_function(id, std::get<function_stmt_t>(m_ast.get(id).node),
                               m_instances.at(instance));
                continue;
            }

            m_parameters.clear();
            m_arguments.clear();
            const stmt_id_t id =
                expect(m_analysis.symbol(m_library_functions[functions++]).declaration);
            lower_function(id, std::get<function_stmt_t>(m_ast.get(id).node));
        }

        m_parameters.clear();
        m_arguments.clear();
    }

    /// A Class's entry in the module, with its Destroy() if it has one, and its Method table.
    void add_class(const type_id_t type) {
        // An instance of a generic Class runs the instances of its Methods.
        const type_id_t definition = definition_of(type);
        const std::vector<type_id_t> arguments = types().get(type).arguments;
        mir_class_t info{ .type = type };
        for (const symbol_id_t member :
             m_analysis.symbol(m_declared_types.at(definition)).members) {
            const symbol_t& method = m_analysis.symbol(member);
            if (method.kind == symbol_kind_t::METHOD && method.name == "Destroy") {
                info.destroy = function_of(member, arguments);
            }
        }

        for (const symbol_id_t method : method_table(definition)) {
            const bool own = m_analysis.symbol(method).owner == definition;
            info.methods.push_back(function_of(method, own ? arguments : std::vector<type_id_t>{}));
        }

        m_module.classes.push_back(std::move(info));
    }

    /// Whether a function gets an error variable: it throws, or it overrides a Method that gets
    /// one, since every Method in a slot is called the same way.
    [[nodiscard]] bool takes_error(const symbol_t& function) const {
        return !function.throws.empty() ||
               (function.overrides && takes_error(m_analysis.symbol(*function.overrides)));
    }

    /// A Class's Method table: the slots of the Class it extends, with its overrides in them,
    /// then a slot for each of its other Methods. Destroy has none, since each Class's runs.
    const std::vector<symbol_id_t>& method_table(const type_id_t type) {
        if (const auto found = m_tables.find(type); found != m_tables.end()) {
            return found->second;
        }

        std::vector<symbol_id_t> table;
        if (const type_id_t base = types().get(type).base; base != type_id_t::INVALID) {
            table = method_table(base);
        }

        for (const symbol_id_t member : m_analysis.symbol(m_declared_types.at(type)).members) {
            for (const symbol_id_t method : slotted(member)) {
                if (const std::optional<symbol_id_t> overridden =
                        m_analysis.symbol(method).overrides) {
                    const std::uint32_t slot = m_slots.at(*overridden);
                    m_slots[method] = slot;
                    table[slot] = method;
                } else {
                    m_slots[method] = static_cast<std::uint32_t>(table.size());
                    table.push_back(method);
                }
            }
        }

        return m_tables[type] = std::move(table);
    }

    /// A Class Method's slot in the Method tables.
    std::uint32_t slot_of(const symbol_id_t method) {
        method_table(m_analysis.symbol(method).owner);
        return m_slots.at(method);
    }

    /// The Methods a member puts in its Class's Method table: a Method, or a Property's Get and
    /// Set. A Static Method isn't called through an object.
    [[nodiscard]] std::vector<symbol_id_t> slotted(const symbol_id_t member) const {
        // A generic Method has an instance for each set of type arguments, so it has no slot.
        const symbol_t& declared = m_analysis.symbol(member);
        if (declared.kind == symbol_kind_t::METHOD && declared.name != "Destroy" &&
            !declared.modifiers.is_static && declared.type_parameters.empty()) {
            return { member };
        }

        std::vector<symbol_id_t> methods;
        if (declared.kind == symbol_kind_t::PROPERTY) {
            for (const std::optional<symbol_id_t> part : { declared.getter, declared.setter }) {
                if (part) {
                    methods.push_back(*part);
                }
            }
        }

        return methods;
    }

    /// Whether a call to a Class's Method goes through the object's Method table: unless the
    /// Method or its Class is Final, a Class extending it could override it.
    [[nodiscard]] bool is_virtual(const symbol_t& method) const {
        return method.kind == symbol_kind_t::METHOD && is_class(method.owner) &&
               !method.modifiers.is_final && !method.modifiers.is_static &&
               method.type_parameters.empty() &&
               !m_analysis.symbol(m_declared_types.at(method.owner)).modifiers.is_final;
    }

    void begin_function(const function_id_t id) {
        m_function = id;
        m_locals.clear();
        m_scopes.clear();
        m_temporaries.clear();
        m_loops.clear();
        m_self.reset();
        m_returns_self = false;
        m_handlers.clear();
        m_error_slot.reset();
        m_block = new_block();
        push_scope();
    }

    /// Ends the block that runs off the end of the function, if code reaches there.
    void finish_function() {
        if (m_returns_self) {
            return_self();
        } else if (current().return_type == type_id_t::NONE) {
            release_scopes(0);
            terminate(return_t{});
        } else {
            terminate(unreachable_t{}); // analysis checked that every path returns
        }
    }

    void lower_function(const stmt_id_t id, const function_stmt_t& node,
                        const std::optional<function_id_t> instance = std::nullopt) {
        const symbol_id_t symbol_id = expect(m_analysis.stmt(id).symbol);
        const symbol_t& function = m_analysis.symbol(symbol_id);
        begin_function(instance ? *instance : m_functions.at(symbol_id));
        const bool sets_up = function.kind == symbol_kind_t::CONSTRUCTOR && is_class(function.type);
        m_returns_self = function.kind == symbol_kind_t::CONSTRUCTOR && !sets_up;
        // A Method's Self is its first parameter, then come the others, in order, and so is the
        // object a Class's New sets up. Parameters aren't in any scope: they borrow the caller's
        // references, so the function never releases them.
        if (function.self && (function.kind == symbol_kind_t::METHOD || sets_up)) {
            const symbol_t& self = m_analysis.symbol(*function.self);
            m_self = add_local(concrete(self.type), "Self", self.by_ref);
        }

        for (const symbol_id_t parameter_id : function.parameters) {
            const symbol_t& parameter = m_analysis.symbol(parameter_id);
            m_locals[parameter_id] =
                add_local(concrete(parameter.type), parameter.name, parameter.by_ref);
        }

        // A function that throws puts its error in the caller's variable, which comes last.
        if (takes_error(function)) {
            m_error_slot = add_local(m_analysis.nullable_error, "error", true);
        }

        current().parameter_count = current().locals.size();

        // A Struct's New starts Self with the fields' defaults. It isn't in a scope either: New
        // hands it to the caller.
        if (m_returns_self) {
            const type_id_t type = concrete(function.type);
            const local_id_t self = add_local(type, "Self");
            m_self = self;
            emit(assign_t{ .target = self, .value = defaults_of(type) });
        }

        // A Class's New sets up what Self inherits first, unless it starts with Super.New.
        if (function.base_new) {
            set_up(*function.base_new, expect(m_self), {}, node.name.span);
        }

        // An Abstract Method has no body. Only an override of it can run.
        if (function.modifiers.is_abstract) {
            terminate(
                panic_t{ .message = "an Abstract Method was called", .span = node.name.span });
        }

        lower_statements(node.body);
        finish_function();
    }

    /// Runs a Class's New on an object that's already made, with its fields at their defaults:
    /// a New the source declares, or else one its Class got, which passes its arguments on to the
    /// New of the Class it extends, if there's one. Missing arguments take their defaults. When a
    /// New throws, `discard`, the object's new temporary, is discarded.
    void set_up(const symbol_id_t constructor_id, const place_t& object,
                std::vector<call_argument_t> arguments, const span_t span,
                const std::optional<local_id_t> discard = std::nullopt) {
        const symbol_t& constructor = m_analysis.symbol(constructor_id);
        if (!constructor.declaration) {
            if (constructor.base_new) {
                set_up(*constructor.base_new, object, std::move(arguments), span, discard);
            }

            return;
        }

        // A generic Class's New has the type arguments of the object's instance.
        const std::vector<type_id_t> parameters = type_parameters_of(constructor);
        const std::vector<type_id_t> type_arguments =
            parameters.empty() ? std::vector<type_id_t>{} : types().get(type_of(object)).arguments;
        for (std::size_t i = arguments.size(); i < constructor.parameters.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(constructor.parameters[i]);
            const type_id_t type = types().substitute(parameter.type, parameters, type_arguments);
            arguments.push_back({ .value = default_of(parameter, type) });
        }

        arguments.insert(arguments.begin(), { .value = object });
        call_t call{
            .function = function_of(constructor_id, type_arguments),
            .arguments = std::move(arguments),
        };
        std::optional<local_id_t> error;
        if (takes_error(constructor)) {
            error = error_variable();
            call.arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        emit(evaluate_t{ .value = std::move(call) });
        if (error) {
            check_error(*error, span, discard);
        }
    }

    /// A call's variable for its error, Null until it throws.
    local_id_t error_variable() {
        const local_id_t error = add_local(m_analysis.nullable_error);
        emit(assign_t{
            .target = error,
            .value = use_t{ .value = null_of(m_analysis.nullable_error) },
        });
        return error;
    }

    /// New's Return: Self goes back to the caller, which takes over its references.
    void return_self() {
        finish_statement();
        release_scopes(0);
        terminate(return_t{ .value = place_t{ expect(m_self) } });
    }

    /// Globals whose values aren't known at compile time are set first, in source order. Then
    /// the rest of the top-level code runs.
    void lower_top_level() {
        begin_function(m_module.entry);
        for (const stmt_id_t id : m_ast.items()) {
            if (const auto* declared = std::get_if<type_stmt_t>(&m_ast.get(id).node)) {
                for (const stmt_id_t member : declared->members) {
                    initialize_global(member);
                }
            } else {
                initialize_global(id);
            }
        }

        for (const stmt_id_t id : m_ast.items()) {
            const stmt_node_t& node = m_ast.get(id).node;
            const auto* variable = std::get_if<variable_stmt_t>(&node);
            const bool global = variable != nullptr && variable->kind != variable_kind_t::LOCAL;
            if (!global && !std::holds_alternative<function_stmt_t>(node) &&
                !std::holds_alternative<type_stmt_t>(node) &&
                !std::holds_alternative<enum_stmt_t>(node)) {
                lower_stmt(id);
            }
        }

        finish_function();
    }

    /// A Global's, top-level Const's, or Static Field's initializer, when it isn't known at
    /// compile time.
    void initialize_global(const stmt_id_t id) {
        const auto* node = std::get_if<variable_stmt_t>(&m_ast.get(id).node);
        if (node == nullptr || node->kind == variable_kind_t::LOCAL || !node->value ||
            m_analysis.expr(*node->value).value) {
            return;
        }

        const symbol_id_t symbol = expect(m_analysis.stmt(id).symbol);
        if (const auto found = m_globals.find(symbol); found != m_globals.end()) {
            store(found->second, lower_value(*node->value));
            finish_statement();
        }
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
        type_id_t type = type_id_t::INVALID;
        if (const auto* local = std::get_if<local_id_t>(&target.root)) {
            type = current().locals[index(*local)].type;
        } else {
            type = m_module.globals[index(std::get<global_id_t>(target.root))].type;
        }

        for (const step_t& step : target.steps) {
            const type_t& info = types().get(type);
            type = step.indices.empty() ? info.fields[step.field].type : info.inner;
        }

        return type;
    }

    [[nodiscard]] bool is_struct(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::STRUCT;
    }

    [[nodiscard]] bool is_class(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::CLASS;
    }

    [[nodiscard]] bool is_tuple(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::TUPLE;
    }

    [[nodiscard]] bool is_func(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::FUNCTION;
    }

    /// Structs and tuples are values made of fields, which == compares one by one.
    [[nodiscard]] bool has_fields(const type_id_t type) const {
        return is_struct(type) || is_tuple(type);
    }

    [[nodiscard]] bool is_nullable(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::NULLABLE;
    }

    [[nodiscard]] bool is_array(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::ARRAY;
    }

    [[nodiscard]] bool is_enum(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::ENUM;
    }

    /// One of an Enum's values, as a constant of the Enum, or of its integer type.
    [[nodiscard]] const_value_t enum_constant(const type_id_t type, const std::uint64_t bits,
                                              const bool as_integer = false) const {
        const type_id_t integer = types().get(type).inner;
        const type_id_t result = as_integer ? integer : type;
        if (is_signed(integer)) {
            return { .type = result, .data = static_cast<std::int64_t>(bits) };
        }

        return { .type = result, .data = bits };
    }

    [[nodiscard]] bool is_fixed_array(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::FIXED_ARRAY;
    }

    [[nodiscard]] bool is_weak(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::WEAK;
    }

    /// Null, as a constant of a T?.
    static const_value_t null_of(const type_id_t type) {
        return { .type = type, .data = nullptr };
    }

    [[nodiscard]] static bool is_null(const operand_t& value) {
        const auto* constant = std::get_if<const_value_t>(&value);
        return constant != nullptr && std::holds_alternative<std::nullptr_t>(constant->data);
    }

    /// A Bool: whether a T? holds a value.
    operand_t has_value(const operand_t& value, const span_t span) {
        if (const auto* constant = std::get_if<const_value_t>(&value)) {
            return make_bool(!std::holds_alternative<std::nullptr_t>(constant->data));
        }

        return compare(binary_op_t::NOT_EQUAL, value, null_of(type_of(value)), span);
    }

    /// The T in a T? that holds a value: a step into the place, or the constant as a T.
    operand_t unwrap(const operand_t& value) {
        if (const auto* constant = std::get_if<const_value_t>(&value)) {
            return const_value_t{
                .type = types().get(constant->type).inner,
                .data = constant->data,
            };
        }

        place_t place = std::get<place_t>(value);
        place.steps.push_back({ .field = 0 });
        return place;
    }

    /// `value` as `type`, which it converts to without loss. A temporary that owned references
    /// hands them to the converted one.
    operand_t convert(const operand_t& value, const type_id_t type, const span_t span) {
        const type_id_t from = type_of(value);
        if (from == type) {
            return value;
        }

        // Reading a Weak<T> gives a T? with a reference of its own, or Null once the object is
        // gone.
        if (is_weak(from)) {
            const type_id_t read = types().find_nullable(types().get(from).inner);
            const operand_t strong =
                runtime_call(runtime_function_t::WEAK_LOAD, { value }, read, true);
            return convert(strong, type, span);
        }

        // A Weak<T> borrows the object it's made from, and holding it counts a weak reference.
        if (is_weak(type)) {
            return temporary(cast_t{ .value = value, .type = type, .span = span }, type);
        }

        return retype(value, cast_t{ .value = value, .type = type, .span = span }, type);
    }

    /// Whether a type's zero has bits that aren't 0, unlike the fields of a new object: a
    /// String's is the static "", and a Struct, tuple, or T? may hold one. A String? is Null,
    /// which is 0.
    [[nodiscard]] bool needs_zeroing(const type_id_t type) const {
        if (type == type_id_t::STRING) {
            return true;
        }

        const type_t& info = types().get(type);
        if (info.kind == type_kind_t::NULLABLE) {
            return info.inner != type_id_t::STRING && needs_zeroing(info.inner);
        }

        if (info.kind == type_kind_t::FIXED_ARRAY) {
            return needs_zeroing(info.inner);
        }

        if (info.kind == type_kind_t::ENUM) {
            return types().enum_zero(type) != 0;
        }

        return (info.kind == type_kind_t::STRUCT || info.kind == type_kind_t::TUPLE) &&
               std::ranges::any_of(
                   info.fields, [this](const field_t& field) { return needs_zeroing(field.type); });
    }

    /// A new object of a Class, with its fields at their defaults.
    operand_t new_object(const type_id_t type) {
        use_class(type);
        const operand_t object = temporary(new_t{ .type = type }, type, true);
        initialize_fields(std::get<place_t>(object), type);
        return object;
    }

    /// Sets a new object's fields, inherited ones included. The runtime zeroes them, so only the
    /// ones with a default other than Null, or whose zero isn't all zero bits, need setting.
    void initialize_fields(const place_t& object, const type_id_t type) {
        const type_id_t base = types().get(type).base;
        if (base != type_id_t::INVALID) {
            initialize_fields(object, base);
        }

        const symbol_id_t declared = m_declared_types.at(definition_of(type));
        for (const symbol_id_t member : m_analysis.symbol(declared).members) {
            const symbol_t& field = m_analysis.symbol(member);
            if (field.kind != symbol_kind_t::FIELD) {
                continue;
            }

            // An instance of a generic Class has its fields' types with its type arguments.
            const type_id_t field_type = types().get(type).fields[field.field].type;
            const bool zero =
                !field.empty_default &&
                (!field.value || std::holds_alternative<std::nullptr_t>(field.value->data));
            if (zero && !needs_zeroing(field_type)) {
                continue;
            }

            place_t target = object;
            target.steps.push_back({ .field = field.field });
            const operand_t value = default_of(field, field_type);
            take(value);
            emit(assign_t{ .target = target, .value = use_t{ .value = value } });
        }
    }

    /// Whether a value of `type` holds references, which its holder retains and releases.
    [[nodiscard]] bool has_references(const type_id_t type) const {
        return types().has_references(type);
    }

    /// A parameter's or field's default: its value known at compile time, a new empty array for
    /// "[]", or else its type's zero.
    operand_t default_of(const symbol_t& symbol, const type_id_t type) {
        if (symbol.value) {
            const_value_t value = *symbol.value;
            value.type = type;
            return value;
        }

        if (symbol.empty_default) {
            return temporary(array_t{ .type = type }, type, true);
        }

        return zero_value(type);
    }

    /// What a variable holds before it's assigned. The zero of a Struct, tuple, Class, or array
    /// needs a temporary, and a T?'s is Null.
    operand_t zero_value(const type_id_t type) {
        if (has_fields(type) || is_class(type) || is_array(type) || is_fixed_array(type)) {
            return temporary(zero_t{ .type = type }, type, has_references(type));
        }

        // A Weak<T>'s refers to nothing.
        if (is_nullable(type) || is_weak(type)) {
            return null_of(type);
        }

        if (is_enum(type)) {
            return enum_constant(type, types().enum_zero(type));
        }

        return zero(type);
    }

    /// A Struct value with each field at its default.
    aggregate_t defaults_of(const type_id_t type) {
        aggregate_t value{ .type = type };
        const symbol_id_t declared = m_declared_types.at(definition_of(type));
        for (const symbol_id_t member : m_analysis.symbol(declared).members) {
            const symbol_t& field = m_analysis.symbol(member);
            if (field.kind == symbol_kind_t::FIELD) {
                value.fields.push_back(
                    default_of(field, types().get(type).fields[field.field].type));
            }
        }

        // Defaults are constants, or values made just for this, so they need no retains.
        for (const operand_t& field : value.fields) {
            take(field);
        }

        return value;
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

    /// Gives a value's new holder its own references: a whole temporary hands its references
    /// over, and anything else is retained. Constants are static and need neither, and so do
    /// values without references.
    void take(const operand_t& value) {
        if (std::holds_alternative<const_value_t>(value) || !has_references(type_of(value))) {
            return;
        }

        const auto& place = std::get<place_t>(value);
        if (const auto* local = std::get_if<local_id_t>(&place.root);
            local != nullptr && place.steps.empty()) {
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
        take(value);
        emit(assign_t{ .target = local, .value = use_t{ .value = value } });
    }

    /// A new value for a variable that already has one; its old references are released.
    void store(const place_t& target, const operand_t& value) {
        if (has_references(type_of(value))) {
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
        release_scope(m_scopes.size() - 1);
        m_scopes.pop_back();
    }

    /// Leaving a scope releases its variables and runs its Defers' statements and its Finally,
    /// the last first. Each is lowered again at each way out, after the ones further in. Code
    /// that nothing reaches does none of it.
    void release_scope(const std::size_t depth) {
        if (!m_block) {
            return;
        }

        // A copy, since a deferred statement's own blocks add scopes.
        const std::vector<cleanup_t> scope = m_scopes[depth];
        for (const cleanup_t& cleanup : std::views::reverse(scope)) {
            if (const auto* local = std::get_if<local_id_t>(&cleanup.action)) {
                emit(release_t{ .target = *local });
            } else if (const auto* deferred = std::get_if<stmt_id_t>(&cleanup.action)) {
                lower_stmt(*deferred);
            } else {
                lower_block(*std::get<const block_t*>(cleanup.action));
            }
        }
    }

    /// Leaves every scope from `depth` inward, for a jump out of them.
    void release_scopes(const std::size_t depth) {
        for (std::size_t i = m_scopes.size(); i-- > depth;) {
            release_scope(i);
        }
    }

    /// Whether leaving the scopes from `depth` inward runs a deferred statement or a Finally.
    [[nodiscard]] bool defers_from(const std::size_t depth) const {
        return std::ranges::any_of(m_scopes | std::views::drop(depth), [](const auto& scope) {
            return std::ranges::any_of(scope, [](const cleanup_t& cleanup) {
                return !std::holds_alternative<local_id_t>(cleanup.action);
            });
        });
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
        // Only a call can be a statement on its own, with Try or Try? before it or not.
        lower_effect(node.expression);
        finish_statement();
    }

    void lower_stmt(const stmt_id_t id, const span_t span, const assign_stmt_t& node) {
        if (const auto* targets = std::get_if<tuple_expr_t>(&m_ast.get(node.target).node)) {
            assign_elements(*targets, node.value);
            return;
        }

        // "x op= y" on a Struct or Class that defines the operator calls its Operator.
        const stmt_info_t& info = m_analysis.stmt(id);
        const std::vector<type_id_t> arguments = concrete(info.type_arguments);

        const auto apply = [&](const operand_t& current, const operand_t& value) {
            if (info.symbol) {
                return call_operator(*info.symbol, arguments, { current, value }, span);
            }

            return combine(*node.op, current, value, span);
        };

        if (const std::optional<symbol_id_t> property = property_of(node.target)) {
            set_property(node.target, *property, [&](const call_argument_t& receiver) {
                if (!node.op) {
                    return lower_value(node.value);
                }

                const operand_t current =
                    get_property(m_analysis.symbol(*property), receiver.value);
                return apply(current, lower_value(node.value));
            });
            return;
        }

        const place_t target = lower_place(node.target);
        const type_id_t type = type_of(target);
        const operand_t value = lower_value(node.value);
        if (!node.op) {
            store(target, value);
        } else if (info.symbol) {
            store(target, convert(apply(target, value), type, span));
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

    /// "a, b = value": the whole value first, then each target in turn, from the left, which
    /// takes its element. The tuple is held in a variable meanwhile, since setting a Property
    /// finishes a statement of its own. A variable's value is copied, so assigning the targets
    /// can't change it.
    void assign_elements(const tuple_expr_t& targets, const expr_id_t value_id) {
        const operand_t value = lower_value(value_id);
        const type_id_t type = type_of(value);
        local_id_t tuple{};
        if (take_temporary(value)) {
            tuple = std::get<local_id_t>(std::get<place_t>(value).root);
        } else {
            tuple = add_local(type);
            initialize(tuple, value);
        }
        for (std::uint32_t i = 0; i < targets.elements.size(); ++i) {
            const expr_id_t target = targets.elements[i];
            const auto* name = std::get_if<name_expr_t>(&m_ast.get(target).node);
            if (name != nullptr && name->name == "_") {
                continue;
            }

            const place_t element = element_of(place_t{ tuple }, i);
            if (const std::optional<symbol_id_t> property = property_of(target)) {
                set_property(
                    target, *property,
                    [&](const call_argument_t& /*receiver*/) -> operand_t { return element; });
                continue;
            }

            const place_t place = lower_place(target);
            store(place, convert(element, type_of(place), span_of(target)));
        }

        if (has_references(type)) {
            emit(release_t{ .target = tuple });
        }

        finish_statement();
    }

    /// "Local lo, hi = value": each new Local takes its own copy of an element.
    void lower_stmt(const stmt_id_t id, span_t /*span*/, const destructure_stmt_t& node) {
        const place_t tuple = as_place(lower_value(node.value));
        const std::vector<symbol_id_t>& variables = m_analysis.stmt(id).matches;
        std::size_t next = 0;
        for (std::uint32_t i = 0; i < node.names.size(); ++i) {
            if (node.names[i].text.empty() || node.names[i].text == "_") {
                continue;
            }

            const place_t variable = new_variable(variables[next++]);
            const place_t element = element_of(tuple, i);
            take(element);
            emit(assign_t{ .target = variable, .value = use_t{ .value = element } });
        }

        finish_statement();
    }

    void lower_stmt(stmt_id_t /*id*/, const span_t span, const increment_stmt_t& node) {
        const binary_op_t change = node.decrement ? binary_op_t::SUBTRACT : binary_op_t::ADD;
        if (const std::optional<symbol_id_t> property = property_of(node.target)) {
            const type_id_t type = m_analysis.symbol(*property).type;
            set_property(node.target, *property, [&](const call_argument_t& receiver) {
                const operand_t current =
                    get_property(m_analysis.symbol(*property), receiver.value);
                return combine(change, current, one(type), span);
            });
            return;
        }

        const place_t target = lower_place(node.target);
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

        const operand_t value =
            node.value ? lower_value(*node.value) : zero_value(concrete(symbol.type));
        const place_t variable = new_variable(symbol_id);
        take(value);
        emit(assign_t{ .target = variable, .value = use_t{ .value = value } });
        finish_statement();
    }

    /// The variable for a Local, which its scope releases if it holds references. A Local that a
    /// lambda captures lives in a cell, which the lambda shares: the variable holds the cell, and
    /// the value is in its field. Gives the place for the Local's first value.
    place_t new_variable(const symbol_id_t id) {
        const symbol_t& symbol = m_analysis.symbol(id);
        if (symbol.cell == type_id_t::INVALID) {
            const type_id_t type = concrete(symbol.type);
            const local_id_t local = add_local(type, symbol.name);
            m_locals[id] = local;
            if (has_references(type)) {
                m_scopes.back().emplace_back(local);
            }

            return place_t{ local };
        }

        const type_id_t type = concrete(symbol.cell);
        const local_id_t cell = add_local(type, symbol.name);
        add_runtime_class(type);
        emit(assign_t{ .target = cell, .value = new_t{ .type = type } });
        m_locals[id] = cell;
        m_scopes.back().emplace_back(cell);
        return element_of(place_t{ cell }, 0);
    }

    /// A Class the compiler makes, for a cell or a closure, which has no Destroy.
    void add_runtime_class(const type_id_t type, std::vector<function_id_t> methods = {}) {
        if (!std::ranges::contains(m_module.classes, type, &mir_class_t::type)) {
            m_module.classes.push_back({ .type = type, .methods = std::move(methods) });
        }
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

    /// "For x In items": the array is evaluated once and held for the loop, and a counter goes
    /// through its elements, checking a T[]'s Count before each pass. Each pass's x is a Local of
    /// its own, with its own references to the element's.
    void lower_stmt(const stmt_id_t id, const span_t span, const for_in_stmt_t& node) {
        const stmt_info_t& info = m_analysis.stmt(id);
        const type_id_t type = converted_type(node.sequence);
        push_scope();
        const operand_t value = lower_value(node.sequence);
        place_t sequence = as_place(value);
        if (is_array(type)) {
            const local_id_t held = add_local(type);
            initialize(held, value);
            m_scopes.back().emplace_back(held);
            sequence = held;
        } else if (take_temporary(value)) {
            m_scopes.back().emplace_back(std::get<local_id_t>(sequence.root));
        }

        finish_statement();
        const local_id_t counter =
            add_local(type_id_t::INT32, node.index ? node.index->text : std::string_view{});
        if (info.index) {
            m_locals[*info.index] = counter;
        }

        emit(assign_t{ .target = counter, .value = use_t{ .value = zero(type_id_t::INT32) } });
        const block_id_t head = new_block();
        jump(head);
        start(head);
        const auto fixed = static_cast<std::int64_t>(types().element_count(type));
        const operand_t count = is_array(type)
                                    ? count_of(sequence)
                                    : const_value_t{ .type = type_id_t::INT32, .data = fixed };
        const operand_t in_range = compare(binary_op_t::LESS, counter, count, span);
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        m_loops.push_back({ .stmt = id, .scope_depth = m_scopes.size() });
        push_scope();
        const symbol_id_t variable = expect(info.symbol);
        const type_id_t variable_type = concrete(m_analysis.symbol(variable).type);
        const local_id_t local = add_local(variable_type, node.value.text);
        m_locals[variable] = local;
        place_t element = sequence;
        element.steps.push_back({ .indices = { counter }, .span = span });
        initialize(local, element);
        if (has_references(variable_type)) {
            m_scopes.back().emplace_back(local);
        }

        lower_statements(node.body);
        pop_scope();
        loop_t loop = std::move(m_loops.back());
        m_loops.pop_back();
        leave(loop.continues); // the end of the body goes on to the next pass, like Continue
        join(loop.continues);
        count_up(counter, span);
        jump(head);

        const block_id_t exit = join(loop.breaks);
        block_at(test).terminator =
            branch_t{ .condition = in_range, .if_true = body, .if_false = exit };
        pop_scope();
    }

    /// Whether `value` is a temporary that owns its references. If it is, it stops being one, and
    /// its new holder releases it.
    bool take_temporary(const operand_t& value) {
        const auto* place = std::get_if<place_t>(&value);
        const auto* local = place != nullptr ? std::get_if<local_id_t>(&place->root) : nullptr;
        if (local == nullptr || !place->steps.empty()) {
            return false;
        }

        const auto found = std::ranges::find(m_temporaries, *local);
        if (found == m_temporaries.end()) {
            return false;
        }

        m_temporaries.erase(found);
        return true;
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

    void lower_stmt(const stmt_id_t id, const span_t span, const select_stmt_t& node) {
        // The subject is evaluated once, into a local of the Select's own scope.
        push_scope();
        const type_id_t type = converted_type(node.subject);
        const operand_t value = lower_value(node.subject);
        const local_id_t subject = add_local(type);
        initialize(subject, value);
        if (has_references(type)) {
            m_scopes.back().emplace_back(subject);
        }

        finish_statement();

        std::vector<block_id_t> exits;
        std::size_t matches = 0;
        for (const case_t& branch : node.cases) {
            /// One Case value's test, in `block`: a match goes to the body, anything else to
            /// `next`.
            struct test_t {
                block_id_t block;
                operand_t matched;
                block_id_t next;
            };

            // A type match has one test, and values each have their own.
            std::vector<test_t> tests;
            std::optional<symbol_id_t> variable;
            if (branch.match) {
                variable = m_analysis.stmt(id).matches[matches++];
                const type_id_t match = m_analysis.symbol(*variable).type;
                use_class(match);
                const operand_t matched =
                    temporary(is_t{ .value = subject, .type = match }, type_id_t::BOOL);
                const block_id_t block = here();
                m_block.reset();
                const block_id_t next = new_block();
                start(next);
                tests.push_back({ .block = block, .matched = matched, .next = next });
            }

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

            // A type match's variable holds the subject as its Class, in a scope around the body.
            start(body);
            if (variable) {
                push_scope();
                const type_id_t matched = concrete(m_analysis.symbol(*variable).type);
                const operand_t object = temporary(
                    downcast_t{ .value = subject, .type = matched, .span = span }, matched);
                const place_t place = new_variable(*variable);
                take(object);
                emit(assign_t{ .target = place, .value = use_t{ .value = object } });
                lower_block(branch.body);
                pop_scope();
            } else {
                lower_block(branch.body);
            }

            leave(exits);
            start(no_match);
        }

        if (node.default_body) {
            lower_block(*node.default_body);
        }

        // Analysis checked that the Cases cover every value an Enum can have.
        if (m_analysis.stmt(id).exhaustive) {
            terminate(panic_t{ .message = "no Case matched", .span = span });
        }

        leave(exits);
        join(exits);
        pop_scope();
    }

    /// A Bool temporary: whether `subject` equals one Case value, or falls in its range.
    operand_t case_test(const local_id_t subject, const case_value_t& value, const span_t span) {
        const const_value_t low = concrete(expect(m_analysis.expr(value.value).value));
        if (low.type == type_id_t::STRING) {
            return runtime_call(runtime_function_t::EQUAL, { subject, low }, type_id_t::BOOL);
        }

        if (!value.range_end) {
            return compare(binary_op_t::EQUAL, subject, low, span);
        }

        const const_value_t high = concrete(expect(m_analysis.expr(*value.range_end).value));
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
        if (m_returns_self) {
            return_self();
            return;
        }

        // The value is computed before the Defers run, so a copy keeps a variable's from their
        // changes.
        std::optional<operand_t> result;
        if (node.value) {
            result = lower_value(*node.value);
            take(*result); // the caller owns the references it gets back
            if (std::holds_alternative<place_t>(*result) && defers_from(0)) {
                result = temporary(use_t{ .value = *result }, type_of(*result));
            }
        }

        finish_statement();
        release_scopes(0);
        terminate(return_t{ .value = result });
    }

    /// The Property an assignment's target is, if it's one.
    [[nodiscard]] std::optional<symbol_id_t> property_of(const expr_id_t target) const {
        const std::optional<symbol_id_t> found = m_analysis.expr(target).symbol;
        if (found && m_analysis.symbol(*found).kind == symbol_kind_t::PROPERTY) {
            return found;
        }

        return std::nullopt;
    }

    /// Reading a Property calls its Get on `receiver`, through the Method table when a Class
    /// extending it could override it.
    operand_t get_property(const symbol_t& property, const operand_t& receiver) {
        const std::vector<type_id_t> arguments = property_arguments(property, receiver);
        const type_id_t type =
            types().substitute(property.type, type_parameters_of(property), arguments);
        return expect(
            call_accessor(expect(property.getter), { { .value = receiver } }, type, arguments));
    }

    /// The type arguments of a Property's object, when its Struct or Class is generic.
    std::vector<type_id_t> property_arguments(const symbol_t& property, const operand_t& receiver) {
        if (type_parameters_of(property).empty()) {
            return {};
        }

        return types().get(non_null(type_of(receiver))).arguments;
    }

    /// Calls a Property's Get or Set, which returns `type`, or nothing.
    std::optional<operand_t> call_accessor(const symbol_id_t accessor,
                                           std::vector<call_argument_t> arguments,
                                           const type_id_t type,
                                           const std::vector<type_id_t>& type_arguments) {
        call_t call{
            .function = function_of(accessor, type_arguments),
            .arguments = std::move(arguments),
        };
        if (is_virtual(m_analysis.symbol(accessor))) {
            call.slot = slot_of(accessor);
        }

        if (type == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
            return std::nullopt;
        }

        return temporary(std::move(call), type, has_references(type));
    }

    /// The Set that assigning a Property calls: its own, or else the one it overrides.
    [[nodiscard]] symbol_id_t setter_of(const symbol_t& property) const {
        if (property.setter || !property.overrides) {
            return expect(property.setter);
        }

        return setter_of(m_analysis.symbol(*property.overrides));
    }

    /// "x.P = value", "x.P += value", or "x.P++": the receiver first, then `value` with it, which
    /// may read the Property, then the Set. A Struct's Set takes the variable itself, like a ByRef
    /// Method, and a Property used by name is Self's.
    template <typename Value>
    void set_property(const expr_id_t target, const symbol_id_t property_id, const Value& value) {
        const symbol_t& property = m_analysis.symbol(property_id);
        const bool changes_struct = !is_class(property.owner);
        const auto* member = std::get_if<member_expr_t>(&m_ast.get(target).node);
        call_argument_t receiver{ .value = make_bool(false) };
        if (member == nullptr) {
            receiver = { .value = place_t{ expect(m_self) }, .by_ref = changes_struct };
        } else if (changes_struct) {
            receiver = { .value = lower_place(member->object), .by_ref = true };
        } else {
            receiver = { .value = lower_value(member->object) };
        }

        const std::vector<type_id_t> arguments = property_arguments(property, receiver.value);
        const type_id_t type =
            types().substitute(property.type, type_parameters_of(property), arguments);
        const operand_t assigned = convert(value(receiver), type, span_of(target));
        call_accessor(setter_of(property), { receiver, { .value = assigned } }, type_id_t::NONE,
                      arguments);
        finish_statement();
    }

    /// `left op right`, as "x op= y" computes it: Strings add through the runtime.
    operand_t combine(const binary_op_t op, const operand_t& left, const operand_t& right,
                      const span_t span) {
        if (type_of(left) == type_id_t::STRING) {
            return runtime_call(runtime_function_t::CONCAT, { left, right }, type_id_t::STRING,
                                true);
        }

        return temporary(binary_t{ .op = op, .left = left, .right = right, .span = span },
                         type_of(left));
    }

    /// A Defer puts its statement in its scope, which runs it on the way out.
    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const defer_stmt_t& node) {
        m_scopes.back().emplace_back(node.statement);
    }

    /// Throw raises its error, which gets a reference of its own.
    void lower_stmt(stmt_id_t /*id*/, const span_t span, const throw_stmt_t& node) {
        const operand_t value = lower_value(node.error);
        take(value);
        const local_id_t error = add_local(m_analysis.nullable_error);
        emit(assign_t{
            .target = error,
            .value = cast_t{ .value = value, .type = m_analysis.nullable_error, .span = span },
        });
        raise(error, span);
        finish_statement(); // nothing reaches past it, so this only drops the temporaries
    }

    /// The body's errors go to the Catches, which test them in order. The Finally is in a scope
    /// around it all, like a Defer, so it runs however the Try ends.
    void lower_stmt(const stmt_id_t id, const span_t span, const try_stmt_t& node) {
        push_scope();
        if (node.finally_body) {
            m_scopes.back().emplace_back(&*node.finally_body);
        }

        std::vector<block_id_t> exits;
        if (node.catches.empty()) {
            lower_block(node.body);
        } else {
            const local_id_t error = add_local(m_analysis.nullable_error);
            m_handlers.push_back({ .scope_depth = m_scopes.size(), .error = error });
            lower_block(node.body);
            const std::vector<block_id_t> raised = std::move(m_handlers.back().exits);
            m_handlers.pop_back();
            leave(exits);
            if (!raised.empty()) {
                join(raised);
                lower_catches(id, node, error, span, exits);
            }
        }

        // When no path gets past the Try, its Finally only runs on the ways out.
        leave(exits);
        if (!exits.empty()) {
            join(exits);
        }

        pop_scope();
    }

    /// Each Catch tests the error's Class in turn, and its variable takes the error over. "Catch
    /// e" takes any. When none does, the error goes on to the next handler out.
    void lower_catches(const stmt_id_t id, const try_stmt_t& node, const local_id_t error,
                       const span_t span, std::vector<block_id_t>& exits) {
        const std::vector<symbol_id_t>& variables = m_analysis.stmt(id).matches;
        for (std::size_t i = 0; i < node.catches.size(); ++i) {
            const symbol_t& variable = m_analysis.symbol(variables[i]);
            const type_id_t caught_type = concrete(variable.type);
            const bool any = caught_type == m_analysis.error_class;
            std::optional<block_id_t> next;
            if (!any) {
                use_class(caught_type);
                const operand_t matched =
                    temporary(is_t{ .value = error, .type = caught_type }, type_id_t::BOOL);
                const block_id_t test = here();
                m_block.reset();
                const block_id_t body = new_block();
                next = new_block();
                block_at(test).terminator =
                    branch_t{ .condition = matched, .if_true = body, .if_false = *next };
                start(body);
            }

            push_scope();
            rvalue_t caught = use_t{ .value = unwrap(error) };
            if (!any) {
                caught = downcast_t{ .value = error, .type = caught_type, .span = span };
            }

            emit(assign_t{ .target = new_variable(variables[i]), .value = std::move(caught) });
            lower_block(node.catches[i].body);
            pop_scope();
            leave(exits);
            if (!next) {
                return;
            }

            start(*next);
        }

        raise(error, span);
    }

    /// Goes on where a call didn't throw, and raises its error where it did. A New that threw
    /// leaves the object it was setting up to be discarded first.
    void check_error(const local_id_t error, const span_t span,
                     const std::optional<local_id_t> discard = std::nullopt) {
        const operand_t failed = has_value(place_t{ error }, span);
        const block_id_t test = here();
        m_block.reset();
        const block_id_t raised = new_block();
        const block_id_t ok = new_block();
        block_at(test).terminator =
            branch_t{ .condition = failed, .if_true = raised, .if_false = ok };
        start(raised);
        if (discard) {
            runtime_call_t dropped{
                .function = runtime_function_t::OBJECT_DISCARD,
                .arguments = { place_t{ *discard } },
            };
            emit(evaluate_t{ .value = std::move(dropped) });
        }

        raise(error, span);
        start(ok);
    }

    /// An error that `error` holds a reference to leaves the code here. The innermost Try?
    /// around it drops the error and gives Null. Otherwise the statement ends: its temporaries are
    /// released, then the scopes the error leaves, whose Defers and Finallys run, and the error
    /// goes to the innermost Try block's Catches, or out of the function.
    void raise(const local_id_t error, const span_t span) {
        if (!m_handlers.empty() && m_handlers.back().optional) {
            handler_t& handler = m_handlers.back();
            for (std::size_t i = m_temporaries.size(); i-- > handler.temporaries;) {
                emit(release_t{ .target = m_temporaries[i] });
            }

            emit(release_t{ .target = error });
            if (handler.result) {
                const type_id_t type = current().locals[index(*handler.result)].type;
                emit(assign_t{
                    .target = *handler.result,
                    .value = use_t{ .value = null_of(type) },
                });
            }

            leave(handler.exits);
            return;
        }

        // The statement's other paths still have its temporaries, and the cleanup lowered here
        // starts without them.
        const std::vector<local_id_t> temporaries = std::exchange(m_temporaries, {});
        for (const local_id_t temporary : std::views::reverse(temporaries)) {
            emit(release_t{ .target = temporary });
        }

        if (!m_handlers.empty()) {
            release_scopes(m_handlers.back().scope_depth);
            emit(assign_t{
                .target = m_handlers.back().error,
                .value = use_t{ .value = place_t{ error } },
            });
            leave(m_handlers.back().exits);
        } else if (const std::optional<local_id_t> slot = m_error_slot) {
            release_scopes(0);
            // A Struct's New only hands Self to its caller when it doesn't throw.
            if (m_returns_self && has_references(current().return_type)) {
                emit(release_t{ .target = expect(m_self) });
            }

            emit(assign_t{ .target = *slot, .value = use_t{ .value = place_t{ error } } });
            const type_id_t type = current().return_type;
            if (type == type_id_t::NONE) {
                terminate(return_t{});
            } else {
                terminate(return_t{ .value = placeholder(type) });
            }
        } else {
            // Analysis checked that functions let errors out only through their Throws. One that
            // top-level code lets out ends the program, after its Defers and Finallys.
            if (m_function == m_module.entry) {
                release_scopes(0);
                runtime_call_t uncaught{
                    .function = runtime_function_t::ERROR_UNCAUGHT,
                    .arguments = { place_t{ error } },
                    .span = span,
                };
                emit(evaluate_t{ .value = std::move(uncaught) });
            }

            terminate(unreachable_t{});
        }

        m_temporaries = temporaries;
    }

    /// What a function returns when it throws, which its caller never reads: its type's zero.
    operand_t placeholder(const type_id_t type) {
        if (has_fields(type) || is_class(type) || is_array(type) || is_fixed_array(type) ||
            is_func(type)) {
            return temporary(zero_t{ .type = type }, type);
        }

        return zero_value(type);
    }

    // Expressions

    [[nodiscard]] span_t span_of(const expr_id_t id) const {
        return m_ast.get(id).span;
    }

    /// A variable's place. A field used by name in a Method or New is in Self, and a Local that a
    /// lambda captures is in its cell.
    place_t place_of(const symbol_id_t symbol) {
        if (const auto found = m_locals.find(symbol); found != m_locals.end()) {
            place_t place{ found->second };
            if (m_analysis.symbol(symbol).cell != type_id_t::INVALID) {
                place.steps.push_back({ .field = 0 });
            }

            return place;
        }

        if (const symbol_t& field = m_analysis.symbol(symbol); field.kind == symbol_kind_t::FIELD) {
            place_t place{ expect(m_self) };
            place.steps.push_back({ .field = field.field });
            return place;
        }

        return m_globals.at(symbol);
    }

    /// A variable's place. When analysis narrowed its T? to a T, it's the T inside. A Property
    /// used by name is Self's, read into a temporary.
    place_t variable_place(const expr_id_t id) {
        const symbol_id_t symbol = expect(m_analysis.expr(id).symbol);
        if (m_analysis.symbol(symbol).kind == symbol_kind_t::PROPERTY) {
            return as_place(get_property(m_analysis.symbol(symbol), place_t{ expect(m_self) }));
        }

        place_t place = place_of(symbol);
        if (is_nullable(concrete(m_analysis.symbol(symbol).type)) && !is_nullable(expr_type(id))) {
            place.steps.push_back({ .field = 0 });
        }

        return place;
    }

    /// Where a variable, Self, or a field inside one lives, without copying it. Anything else,
    /// like a call, gives a temporary.
    place_t lower_place(const expr_id_t id) {
        const expr_node_t& node = m_ast.get(id).node;
        if (const auto* member = std::get_if<member_expr_t>(&node);
            member != nullptr && !member->null_safe) {
            // A Static Field is a global, and a Property's value comes from its Get.
            const symbol_t& found = m_analysis.symbol(expect(m_analysis.expr(id).symbol));
            if (found.kind == symbol_kind_t::GLOBAL) {
                return m_globals.at(expect(m_analysis.expr(id).symbol));
            }

            if (found.kind == symbol_kind_t::PROPERTY) {
                return as_place(get_property(found, lower_value(member->object)));
            }

            place_t place = lower_place(member->object);
            place.steps.push_back({ .field = found.field });
            return place;
        }

        if (std::holds_alternative<self_expr_t>(node)) {
            return expect(m_self);
        }

        if (std::holds_alternative<name_expr_t>(node)) {
            return variable_place(id);
        }

        // Each index goes in a local of its own, so the place keeps the value the index had.
        if (const auto* element = std::get_if<index_expr_t>(&node)) {
            place_t place = lower_place(element->object);
            step_t step{ .span = span_of(id) };
            for (const expr_id_t position : element->indices) {
                const operand_t value = lower_value(position);
                const operand_t local = temporary(use_t{ .value = value }, type_of(value));
                step.indices.push_back(std::get<local_id_t>(std::get<place_t>(local).root));
            }

            place.steps.push_back(std::move(step));
            return place;
        }

        return as_place(lower_value(id));
    }

    /// A value's place: its own, or a temporary's for a constant.
    place_t as_place(const operand_t& value) {
        if (const auto* place = std::get_if<place_t>(&value)) {
            return *place;
        }

        return std::get<place_t>(temporary(use_t{ .value = value }, type_of(value)));
    }

    /// The value of an expression, converted to the type its context needs.
    operand_t lower_value(const expr_id_t id) {
        const expr_info_t& info = m_analysis.expr(id);
        if (info.value) {
            return concrete(*info.value);
        }

        const operand_t value =
            std::visit([this, id](const auto& node) { return this->lower_expr(id, node); },
                       m_ast.get(id).node);
        return convert(value, converted_type(id), span_of(id));
    }

    /// Literals always have a value, and expressions past M1 never pass analysis.
    template <typename Node>
    operand_t lower_expr(expr_id_t /*id*/, const Node& /*node*/) {
        assert(false);
        return const_value_t{};
    }

    /// A variable, or a Function's name alone, which is a function value.
    operand_t lower_expr(const expr_id_t id, const name_expr_t& /*node*/) {
        if (const std::optional<std::size_t> closure = m_analysis.expr(id).closure) {
            return make_closure(m_analysis.closures[*closure], span_of(id));
        }

        return variable_place(id);
    }

    /// A field, or an array's Count.
    operand_t lower_expr(const expr_id_t id, const member_expr_t& node) {
        if (!node.null_safe && is_array(converted_type(node.object))) {
            return count_of(lower_value(node.object));
        }

        if (!node.null_safe) {
            return lower_place(id);
        }

        const std::optional<symbol_id_t> field = m_analysis.expr(id).symbol;
        return expect(lower_safe(id, node.object, [&](const operand_t& object) {
            const type_id_t type = type_of(object);
            if (is_array(type)) {
                return std::optional<operand_t>(count_of(object));
            }

            if (is_fixed_array(type)) {
                const auto count = static_cast<std::int64_t>(types().element_count(type));
                return std::optional<operand_t>(
                    const_value_t{ .type = type_id_t::INT32, .data = count });
            }

            if (const symbol_t& found = m_analysis.symbol(expect(field));
                found.kind == symbol_kind_t::PROPERTY) {
                return std::optional<operand_t>(get_property(found, object));
            }

            place_t place = as_place(object);
            place.steps.push_back({ .field = m_analysis.symbol(expect(field)).field });
            return std::optional<operand_t>(place);
        }));
    }

    operand_t lower_expr(const expr_id_t id, const index_expr_t& /*node*/) {
        return lower_place(id);
    }

    /// "[a, b, c]": a new T[] that holds the elements' references, or a fixed array's value.
    operand_t lower_expr(const expr_id_t id, const array_expr_t& node) {
        const type_id_t type = expr_type(id);
        array_t value{ .type = type };
        for (const expr_id_t element : node.elements) {
            value.elements.push_back(lower_value(element));
        }

        for (const operand_t& element : value.elements) {
            take(element);
        }

        return temporary(std::move(value), type, has_references(type));
    }

    /// "(a, b)": a tuple, whose elements take their own references.
    operand_t lower_expr(const expr_id_t id, const tuple_expr_t& node) {
        const type_id_t type = expr_type(id);
        aggregate_t value{ .type = type };
        for (const expr_id_t element : node.elements) {
            value.fields.push_back(lower_value(element));
        }

        for (const operand_t& element : value.fields) {
            take(element);
        }

        return temporary(std::move(value), type, has_references(type));
    }

    /// One of a tuple's elements, in its place.
    static place_t element_of(place_t tuple, const std::uint32_t element) {
        tuple.steps.push_back({ .field = element });
        return tuple;
    }

    /// A T[]'s Count, as an Int.
    operand_t count_of(const operand_t& array) {
        return runtime_call(runtime_function_t::ARRAY_COUNT, { array }, type_id_t::INT32);
    }

    /// A T[]'s Methods, with the array as the first argument. Add and Insert take over the
    /// value's references.
    std::optional<operand_t> array_method(const symbol_t& method,
                                          const std::vector<call_argument_t>& arguments,
                                          const span_t span) {
        const operand_t& array = arguments[0].value;
        const auto run = [&](const runtime_function_t function, std::vector<operand_t> operands) {
            runtime_call_t call{
                .function = function,
                .arguments = std::move(operands),
                .span = span,
            };
            emit(evaluate_t{ .value = std::move(call) });
        };

        if (method.name == "Add") {
            take(arguments[1].value);
            run(runtime_function_t::ARRAY_ADD, { array, arguments[1].value });
        } else if (method.name == "Insert") {
            take(arguments[2].value);
            run(runtime_function_t::ARRAY_INSERT,
                { array, arguments[1].value, arguments[2].value });
        } else if (method.name == "RemoveAt") {
            run(runtime_function_t::ARRAY_REMOVE_AT, { array, arguments[1].value });
        } else if (method.name == "Clear") {
            run(runtime_function_t::ARRAY_CLEAR, { array });
        } else if (method.name == "Contains") {
            return contains(array, arguments[1].value, span);
        } else {
            return runtime_call(runtime_function_t::ARRAY_CLONE, { array }, type_of(array), true);
        }

        return std::nullopt;
    }

    /// Whether a T[] has an element equal to `value`, comparing them in order.
    operand_t contains(const operand_t& array, const operand_t& value, const span_t span) {
        const local_id_t result = add_local(type_id_t::BOOL);
        const local_id_t counter = add_local(type_id_t::INT32);
        emit(assign_t{ .target = result, .value = use_t{ .value = make_bool(false) } });
        emit(assign_t{ .target = counter, .value = use_t{ .value = zero(type_id_t::INT32) } });
        const block_id_t head = new_block();
        jump(head);
        start(head);
        const operand_t in_range = compare(binary_op_t::LESS, counter, count_of(array), span);
        const block_id_t test = here();
        m_block.reset();

        const block_id_t check = new_block();
        start(check);
        place_t element = as_place(array);
        element.steps.push_back({ .indices = { counter }, .span = span });
        const operand_t same = equal(element, value, span);
        const block_id_t matched = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        const block_id_t found = new_block();
        start(found);
        emit(assign_t{ .target = result, .value = use_t{ .value = make_bool(true) } });
        leave(exits);

        const block_id_t next = new_block();
        start(next);
        count_up(counter, span);
        jump(head);

        const block_id_t exit = join(exits);
        block_at(test).terminator =
            branch_t{ .condition = in_range, .if_true = check, .if_false = exit };
        block_at(matched).terminator =
            branch_t{ .condition = same, .if_true = found, .if_false = next };
        return place_t{ result };
    }

    /// Adds 1 to a counter, which never overflows.
    void count_up(const local_id_t counter, const span_t span) {
        const binary_t next{
            .op = binary_op_t::ADD,
            .left = counter,
            .right = one(type_id_t::INT32),
            .span = span,
            .wrapping = true,
        };
        emit(assign_t{ .target = counter, .value = next });
    }

    /// "a?.b" and "a?.M()": `access` reads the field, or makes the call, on the value in a, and
    /// the result holds what it gives. When a is Null, nothing else runs and the result is
    /// Null. nullopt for a call that returns nothing.
    template <typename Access>
    std::optional<operand_t> lower_safe(const expr_id_t id, const expr_id_t object_id,
                                        const Access& access) {
        const type_id_t type = expr_type(id);
        const span_t span = span_of(id);
        const operand_t object = lower_value(object_id);
        if (is_null(object)) {
            return type == type_id_t::NONE ? std::nullopt : std::optional<operand_t>(null_of(type));
        }

        // Analysis warned that it can't be Null.
        if (!is_nullable(type_of(object))) {
            const std::optional<operand_t> value = access(object);
            return value ? std::optional(convert(*value, type, span)) : std::nullopt;
        }

        std::optional<local_id_t> result;
        if (type != type_id_t::NONE) {
            result = add_local(type);
        }

        const operand_t present = has_value(object, span);
        const block_id_t test = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        const block_id_t holds = new_block();
        start(holds);
        const std::size_t mark = m_temporaries.size();
        if (const std::optional<operand_t> value = access(unwrap(object)); value && result) {
            const operand_t converted = convert(*value, type, span);
            take(converted);
            emit(assign_t{ .target = *result, .value = use_t{ .value = converted } });
        }

        // The temporaries made here only exist on this path, so they're released on it.
        release_temporaries_from(mark);
        leave(exits);

        const block_id_t absent = new_block();
        start(absent);
        if (result) {
            emit(assign_t{ .target = *result, .value = use_t{ .value = null_of(type) } });
        }

        leave(exits);
        block_at(test).terminator =
            branch_t{ .condition = present, .if_true = holds, .if_false = absent };
        join(exits);
        if (!result) {
            return std::nullopt;
        }

        // The result owns its references, so it's released after the statement like a
        // temporary.
        if (has_references(type)) {
            m_temporaries.push_back(*result);
        }

        return place_t{ *result };
    }

    /// In a lambda with "[Weak Self]", Self is the Weak<T> that stands for it.
    operand_t lower_expr(const expr_id_t id, const self_expr_t& /*node*/) {
        if (const std::optional<symbol_id_t> self = m_analysis.expr(id).symbol;
            self && m_locals.contains(*self)) {
            return place_t{ m_locals.at(*self) };
        }

        return place_t{ expect(m_self) };
    }

    /// "Function(x) => ...": a new function value.
    operand_t lower_expr(const expr_id_t id, const lambda_expr_t& /*node*/) {
        return make_closure(m_analysis.closures[expect(m_analysis.expr(id).closure)], span_of(id));
    }

    /// A new object of a closure's Class, which is the function value, as its Func type. It holds
    /// what the lambda captures, and its code is in the Class's Method table.
    operand_t make_closure(const closure_t& closure, const span_t span) {
        // In generic code, the closure's Class is an instance too.
        const type_id_t type = concrete(closure.type);
        const type_id_t func = concrete(closure.func);
        code_of(closure, type);
        const operand_t object = temporary(new_t{ .type = type }, type, true);
        const place_t place = std::get<place_t>(object);
        const std::vector<field_t> fields = types().get(type).fields;
        for (std::uint32_t i = 0; i < closure.captures.size(); ++i) {
            const capture_t& capture = closure.captures[i];
            operand_t value = captured_value(capture);
            if (capture.kind == capture_kind_t::WEAK) {
                value = convert(value, fields[i].type, span);
            }

            take(value);
            emit(assign_t{ .target = element_of(place, i), .value = use_t{ .value = value } });
        }

        return retype(object, cast_t{ .value = object, .type = func, .span = span }, func);
    }

    /// What a closure keeps of a variable: a Local's cell, or else the variable's value. Self is
    /// the one that isn't a variable here.
    operand_t captured_value(const capture_t& capture) {
        if (capture.kind == capture_kind_t::CELL) {
            return place_t{ m_locals.at(capture.variable) };
        }

        if (m_locals.contains(capture.variable)) {
            return place_of(capture.variable);
        }

        return place_t{ expect(m_self) };
    }

    /// The function for a closure's code, declared with its Class the first time the program
    /// makes one. Its body is lowered after the function being lowered.
    function_id_t code_of(const closure_t& closure, const type_id_t type) {
        if (const auto found = m_closure_code.find(type); found != m_closure_code.end()) {
            return found->second;
        }

        const auto id = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({
            .name = std::string(types().name(type)),
            .return_type = types().get(concrete(closure.func)).inner,
        });
        m_closure_code[type] = id;
        add_runtime_class(type, { id });
        m_closures.push_back({
            .closure = closure,
            .type = type,
            .parameters = m_parameters,
            .arguments = m_arguments,
        });
        return id;
    }

    /// A closure's code takes its object, then the arguments. It reads what it captured from the
    /// object, which lives as long as the call. A Function used by name just calls it.
    void lower_closure(const pending_closure_t& pending) {
        const closure_t& closure = pending.closure;
        m_parameters = pending.parameters;
        m_arguments = pending.arguments;
        begin_function(m_closure_code.at(pending.type));
        const local_id_t object = add_local(pending.type, "closure");
        const symbol_t& function = m_analysis.symbol(closure.function);

        // A Function used by name has its own type parameters, which the closure's Class has.
        if (!closure.lambda) {
            m_parameters = type_parameters_of(function);
            m_arguments = types().get(pending.type).arguments;
        }

        std::vector<call_argument_t> arguments;
        for (const symbol_id_t parameter_id : function.parameters) {
            const symbol_t& parameter = m_analysis.symbol(parameter_id);
            const local_id_t local = add_local(concrete(parameter.type), parameter.name);
            m_locals[parameter_id] = local;
            arguments.push_back({ .value = place_t{ local } });
        }

        current().parameter_count = current().locals.size();
        const type_id_t result = concrete(function.type);
        if (!closure.lambda) {
            call_t call{
                .function = function_of(closure.function, m_arguments),
                .arguments = std::move(arguments),
            };
            if (result == type_id_t::NONE) {
                emit(evaluate_t{ .value = std::move(call) });
                terminate(return_t{});
            } else {
                terminate(return_t{ .value = temporary(std::move(call), result) });
            }

            return;
        }

        // The type table keeps each type where it is, so the locals can use the fields' names.
        const std::vector<field_t>& fields = types().get(pending.type).fields;
        for (std::uint32_t i = 0; i < closure.captures.size(); ++i) {
            const capture_t& capture = closure.captures[i];
            const local_id_t local = add_local(fields[i].type, fields[i].name);
            emit(assign_t{
                .target = local,
                .value = use_t{ .value = element_of(place_t{ object }, i) },
            });
            const bool self = capture.kind != capture_kind_t::WEAK &&
                              m_analysis.symbol(capture.variable).name == "Self";
            if (self) {
                m_self = local;
            } else {
                m_locals[capture.inside] = local;
            }
        }

        const auto& node = std::get<lambda_expr_t>(m_ast.get(*closure.lambda).node);
        if (!node.value) {
            lower_statements(node.body);
            finish_function();
            return;
        }

        if (result == type_id_t::NONE) {
            lower_effect(*node.value);
            finish_statement();
            release_scopes(0);
            terminate(return_t{});
            return;
        }

        const operand_t value = lower_value(*node.value);
        take(value); // the caller owns the references it gets back
        finish_statement();
        release_scopes(0);
        terminate(return_t{ .value = value });
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
            add_text(parts, lower_value(inner), converted_type(inner));
        }

        return runtime_call(runtime_function_t::JOIN, std::move(parts), type_id_t::STRING, true);
    }

    /// Adds the text of a value of `type` to a string's parts. A tuple's is its elements' texts,
    /// like "(1, a)".
    void add_text(std::vector<operand_t>& parts, const operand_t& value, const type_id_t type) {
        if (is_tuple(type)) {
            const place_t tuple = as_place(value);
            const std::vector<field_t>& elements = types().get(type).fields;
            for (std::uint32_t i = 0; i < elements.size(); ++i) {
                parts.emplace_back(make_string(i == 0 ? "(" : ", "));
                add_text(parts, element_of(tuple, i), elements[i].type);
            }

            parts.emplace_back(make_string(")"));
            return;
        }

        if (is_enum(type)) {
            call_t call{
                .function = text_function(type),
                .arguments = { { .value = value } },
            };
            parts.push_back(temporary(std::move(call), type_id_t::STRING, true));
        } else if (type != type_id_t::STRING) {
            parts.push_back(
                runtime_call(runtime_function_t::TO_TEXT, { value }, type_id_t::STRING, true));
        } else {
            parts.push_back(value);
        }
    }

    operand_t lower_expr(const expr_id_t id, const unary_expr_t& node) {
        if (m_analysis.expr(id).symbol) {
            return lower_operator(id, false);
        }

        const operand_t operand = lower_value(node.operand);
        return temporary(unary_t{ .op = node.op, .operand = operand, .span = span_of(id) },
                         expr_type(id));
    }

    operand_t lower_expr(const expr_id_t id, const binary_expr_t& node) {
        if (node.op == binary_op_t::AND || node.op == binary_op_t::OR) {
            return lower_logical(node);
        }

        if (node.op == binary_op_t::COALESCE) {
            return lower_coalesce(id, node);
        }

        if (m_analysis.expr(id).symbol) {
            return lower_operator(id, node.op == binary_op_t::NOT_EQUAL);
        }

        const operand_t left = lower_value(node.left);
        const operand_t right = lower_value(node.right);
        const type_id_t type = type_of(left);
        // Strings add through the runtime.
        if (type == type_id_t::STRING && node.op == binary_op_t::ADD) {
            return runtime_call(runtime_function_t::CONCAT, { left, right }, type_id_t::STRING,
                                true);
        }

        // Structs, tuples, Strings, and the values in T?s only have == and !=, which take steps
        // of their own.
        if (has_fields(type) || type == type_id_t::STRING || compares_values(left, right)) {
            operand_t same = equal(left, right, span_of(id));
            if (node.op == binary_op_t::EQUAL) {
                return same;
            }

            return temporary(unary_t{ .op = unary_op_t::NOT, .operand = same, .span = span_of(id) },
                             type_id_t::BOOL);
        }

        return temporary(
            binary_t{ .op = node.op, .left = left, .right = right, .span = span_of(id) },
            expr_type(id));
    }

    /// A Bool: whether two values of one type are equal, the way == compares them. A Struct or
    /// tuple compares its fields, a String its text, and a T? is equal when both are Null or both
    /// hold equal values. A Class, or Class?, compares references.
    operand_t equal(const operand_t& left, const operand_t& right, const span_t span) {
        const type_id_t type = type_of(left);
        if (const std::optional<symbol_id_t> defined = equal_operator(type)) {
            return call_operator(*defined, types().get(type).arguments, { left, right }, span);
        }

        if (has_fields(type)) {
            return equal_fields(std::get<place_t>(left), std::get<place_t>(right), span);
        }

        if (type == type_id_t::STRING) {
            return runtime_call(runtime_function_t::EQUAL, { left, right }, type_id_t::BOOL);
        }

        if (compares_values(left, right)) {
            return equal_values(left, right, span);
        }

        return compare(binary_op_t::EQUAL, left, right, span);
    }

    /// An operator that a Struct or Class defines calls its Operator with the operands. "!="
    /// is the opposite of "==".
    operand_t lower_operator(const expr_id_t id, const bool negate) {
        const expr_info_t& info = m_analysis.expr(id);
        std::vector<operand_t> operands;
        operands.reserve(info.arguments.size());
        for (const std::optional<expr_id_t> operand : info.arguments) {
            operands.push_back(lower_value(expect(operand)));
        }

        operand_t result = call_operator(expect(info.symbol), concrete(info.type_arguments),
                                         operands, span_of(id));
        if (!negate) {
            return result;
        }

        return temporary(unary_t{ .op = unary_op_t::NOT, .operand = result, .span = span_of(id) },
                         type_id_t::BOOL);
    }

    /// Calls an Operator, with the type arguments of its Struct or Class.
    operand_t call_operator(const symbol_id_t id, const std::vector<type_id_t>& arguments,
                            const std::vector<operand_t>& operands, const span_t span) {
        const symbol_t& function = m_analysis.symbol(id);
        const std::vector<type_id_t> parameters = type_parameters_of(function);
        call_t call{ .function = function_of(id, arguments) };
        for (std::size_t i = 0; i < operands.size(); ++i) {
            const type_id_t type = types().substitute(
                m_analysis.symbol(function.parameters[i]).type, parameters, arguments);
            call.arguments.push_back({ .value = convert(operands[i], type, span) });
        }

        const type_id_t result = types().substitute(function.type, parameters, arguments);
        return temporary(std::move(call), result, has_references(result));
    }

    /// The Operator == that a Struct or Class defines for two of itself, which every == on it
    /// uses: in a Struct's or tuple's fields, a T[]'s Contains, and a T?'s value too.
    [[nodiscard]] std::optional<symbol_id_t> equal_operator(const type_id_t type) const {
        const type_id_t definition = definition_of(type);
        const auto declared = m_declared_types.find(definition);
        if ((!has_fields(type) && !is_class(type)) || declared == m_declared_types.end()) {
            return std::nullopt;
        }

        for (const symbol_id_t member : m_analysis.symbol(declared->second).members) {
            const symbol_t& candidate = m_analysis.symbol(member);
            const bool pair = candidate.parameters.size() == 2 &&
                              m_analysis.symbol(candidate.parameters[0]).type == definition &&
                              m_analysis.symbol(candidate.parameters[1]).type == definition;
            if (candidate.kind == symbol_kind_t::METHOD && candidate.name == "==" &&
                candidate.modifiers.is_static && pair) {
                return member;
            }
        }

        return std::nullopt;
    }

    /// Whether == on two T? values compares the values inside. Comparing with Null only asks
    /// whether the other holds a value, and a Class? compares references.
    [[nodiscard]] bool compares_values(const operand_t& left, const operand_t& right) {
        const type_id_t type = type_of(left);
        return is_nullable(type) && !is_null(left) && !is_null(right) &&
               !is_class(types().get(type).inner);
    }

    /// Whether two Structs or tuples of the same type have equal fields, each compared the way ==
    /// compares its type. A Struct without fields is always equal.
    operand_t equal_fields(const place_t& left, const place_t& right, const span_t span) {
        const type_id_t type = type_of(left);
        operand_t result = make_bool(true);
        const std::vector<field_t>& fields = types().get(type).fields;
        for (std::uint32_t i = 0; i < fields.size(); ++i) {
            place_t a = left;
            place_t b = right;
            a.steps.push_back({ .field = i });
            b.steps.push_back({ .field = i });
            const operand_t same = equal(a, b, span);
            result = i == 0 ? same : compare(binary_op_t::AND, result, same, span);
        }

        return result;
    }

    /// Two T? values are equal when both are Null, or both hold values that are equal. The values
    /// are only compared when both are there.
    operand_t equal_values(const operand_t& left, const operand_t& right, const span_t span) {
        const local_id_t result = add_local(type_id_t::BOOL);
        const operand_t left_holds = has_value(left, span);
        const operand_t right_holds = has_value(right, span);
        const operand_t both = compare(binary_op_t::AND, left_holds, right_holds, span);
        const block_id_t test = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        const block_id_t values = new_block();
        start(values);
        const std::size_t mark = m_temporaries.size();
        const operand_t same = equal(unwrap(left), unwrap(right), span);
        emit(assign_t{ .target = result, .value = use_t{ .value = same } });
        release_temporaries_from(mark);
        leave(exits);

        // At most one holds a value, so they're equal when neither does.
        const block_id_t nulls = new_block();
        start(nulls);
        const binary_t neither{
            .op = binary_op_t::EQUAL,
            .left = left_holds,
            .right = right_holds,
            .span = span,
        };
        emit(assign_t{ .target = result, .value = neither });
        leave(exits);

        block_at(test).terminator =
            branch_t{ .condition = both, .if_true = values, .if_false = nulls };
        join(exits);
        return place_t{ result };
    }

    /// "a ?? b": the value in a, or else b, which is only evaluated when a is Null.
    operand_t lower_coalesce(const expr_id_t id, const binary_expr_t& node) {
        const type_id_t type = expr_type(id);
        const span_t span = span_of(id);
        const operand_t left = lower_value(node.left);
        // Analysis warned when it can't be Null.
        if (!is_nullable(type_of(left))) {
            return convert(left, type, span);
        }

        if (is_null(left)) {
            return lower_value(node.right);
        }

        const local_id_t result = add_local(type);
        const operand_t present = has_value(left, span);
        const block_id_t test = here();
        m_block.reset();

        // Each side writes the result and releases its own temporaries, since only one runs.
        std::vector<block_id_t> exits;
        std::vector<block_id_t> starts;
        for (const bool holds : { true, false }) {
            starts.push_back(new_block());
            start(starts.back());
            const std::size_t mark = m_temporaries.size();
            const operand_t value =
                holds ? convert(unwrap(left), type, span) : lower_value(node.right);
            take(value);
            emit(assign_t{ .target = result, .value = use_t{ .value = value } });
            release_temporaries_from(mark);
            leave(exits);
        }

        block_at(test).terminator =
            branch_t{ .condition = present, .if_true = starts[0], .if_false = starts[1] };
        join(exits);
        if (has_references(type)) {
            m_temporaries.push_back(result);
        }

        return place_t{ result };
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
        const type_id_t type = expr_type(id);
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
            take(value);
            emit(assign_t{ .target = result, .value = use_t{ .value = value } });
            release_temporaries_from(mark);
            leave(exits);
        }

        block_at(test).terminator =
            branch_t{ .condition = condition, .if_true = starts[0], .if_false = starts[1] };
        join(exits);
        // The result owns its references, so it's released after the statement like a
        // temporary.
        if (has_references(type)) {
            m_temporaries.push_back(result);
        }

        return place_t{ result };
    }

    operand_t lower_expr(const expr_id_t id, const call_expr_t& /*node*/) {
        return expect(lower_call(id));
    }

    operand_t lower_expr(const expr_id_t id, const try_expr_t& /*node*/) {
        return expect(lower_try(id));
    }

    /// A call, or a Try, for its effects: its result, or nullopt when it gives nothing.
    std::optional<operand_t> lower_effect(const expr_id_t id) {
        if (std::holds_alternative<try_expr_t>(m_ast.get(id).node)) {
            return lower_try(id);
        }

        return lower_call(id);
    }

    /// "Try x" is x, whose errors go on to the code around it. "Try? x" is x as a T?, or Null
    /// when something in it throws, which nothing else then sees. nullopt when x gives nothing.
    std::optional<operand_t> lower_try(const expr_id_t id) {
        const auto& node = std::get<try_expr_t>(m_ast.get(id).node);
        const auto operand = [&] -> std::optional<operand_t> {
            const expr_node_t& inner = m_ast.get(node.operand).node;
            if (!std::holds_alternative<call_expr_t>(inner) &&
                !std::holds_alternative<try_expr_t>(inner)) {
                return lower_value(node.operand);
            }

            // A call that gives nothing, like Super.New, is lowered for its effects.
            const std::optional<operand_t> value = lower_effect(node.operand);
            if (!value) {
                return std::nullopt;
            }

            return convert(*value, converted_type(node.operand), span_of(node.operand));
        };
        if (!node.optional) {
            return operand();
        }

        const type_id_t type = expr_type(id);
        std::optional<local_id_t> result;
        if (type != type_id_t::NONE) {
            result = add_local(type);
        }

        m_handlers.push_back(
            { .optional = true, .result = result, .temporaries = m_temporaries.size() });
        const std::optional<operand_t> value = operand();
        handler_t handler = std::move(m_handlers.back());
        m_handlers.pop_back();
        if (value && result) {
            const operand_t converted = convert(*value, type, span_of(id));
            take(converted);
            emit(assign_t{ .target = *result, .value = use_t{ .value = converted } });
        }

        // The temporaries made inside only exist on this path, so they're released on it.
        release_temporaries_from(handler.temporaries);
        leave(handler.exits);
        join(handler.exits);
        if (!result) {
            return std::nullopt;
        }

        // The result owns its references, so it's released after the statement like a
        // temporary.
        if (has_references(type)) {
            m_temporaries.push_back(*result);
        }

        return place_t{ *result };
    }

    /// Panic stops the program with its message. Assert does when its condition is False, with
    /// its message, or one saying the assertion failed. The message is only made then.
    void lower_panic(const expr_id_t id, const symbol_t& function) {
        const expr_info_t& info = m_analysis.expr(id);
        const bool asserts = function.name == "Assert";
        std::optional<block_id_t> ok;
        if (asserts) {
            const operand_t condition = lower_value(expect(info.arguments[0]));
            const block_id_t fail = new_block();
            ok = new_block();
            terminate(branch_t{ .condition = condition, .if_true = *ok, .if_false = fail });
            start(fail);
        }

        panic_t panic{ .message = "assertion failed", .span = span_of(id) };
        const std::size_t mark = m_temporaries.size();
        if (const std::optional<expr_id_t> message = info.arguments[asserts ? 1 : 0]) {
            const operand_t text = lower_value(*message);
            if (const auto* constant = std::get_if<const_value_t>(&text)) {
                panic.message = std::get<std::string>(constant->data);
            } else {
                panic.text = text;
            }
        }

        terminate(std::move(panic));
        release_temporaries_from(mark); // the program stops, so they're only dropped
        if (ok) {
            start(*ok);
        }
    }

    /// A call's result, or nullopt when it returns nothing. A result with references is released
    /// after the statement unless something takes it. "a?.M()" only calls when a holds a value.
    std::optional<operand_t> lower_call(const expr_id_t id) {
        const auto& node = std::get<call_expr_t>(m_ast.get(id).node);
        if (is_func(expr_type(node.callee))) {
            return invoke(node);
        }

        if (const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
            member != nullptr && member->null_safe) {
            return lower_safe(id, member->object,
                              [&](const operand_t& receiver) { return call_on(id, receiver); });
        }

        return call_on(id, std::nullopt);
    }

    /// Calling a function value runs its object's code.
    std::optional<operand_t> invoke(const call_expr_t& node) {
        const operand_t callee = lower_value(node.callee);
        invoke_t call{ .callee = callee };
        for (const argument_t& argument : node.arguments) {
            call.arguments.push_back(lower_value(argument.value));
        }

        const type_id_t result = types().get(type_of(callee)).inner;
        if (result == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
            return std::nullopt;
        }

        return temporary(std::move(call), result, has_references(result));
    }

    /// A call, on `receiver` when it's a Method called after a '?.'.
    std::optional<operand_t> call_on(const expr_id_t id, const std::optional<operand_t>& receiver) {
        const expr_info_t& info = m_analysis.expr(id);
        const symbol_t& function = m_analysis.symbol(expect(info.symbol));
        const auto& node = std::get<call_expr_t>(m_ast.get(id).node);
        if (function.kind == symbol_kind_t::BUILTIN_FUNCTION &&
            (function.name == "Panic" || function.name == "Assert")) {
            lower_panic(id, function);
            return std::nullopt;
        }

        std::vector<call_argument_t> arguments;
        // The type of what a New makes, or of a Method's object, gives a generic Struct's or
        // Class's type arguments.
        type_id_t object =
            function.kind == symbol_kind_t::CONSTRUCTOR ? expr_type(id) : type_id_t::INVALID;
        // A Method's Struct comes first: the variable itself for a ByRef Method, else its value.
        // Called by name, it's Self. A Static Method has none.
        if (function.kind == symbol_kind_t::METHOD && !function.modifiers.is_static) {
            const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
            operand_t value = receiver ? *receiver : operand_t{};
            if (!receiver && member == nullptr) {
                value = place_t{ expect(m_self) };
            } else if (!receiver) {
                value = function.by_ref ? lower_place(member->object) : lower_value(member->object);
            }

            if (function.by_ref) {
                arguments.push_back({ .value = as_place(value), .by_ref = true });
            } else {
                arguments.push_back({ .value = value });
            }

            object = type_of(value);
        }

        const std::vector<type_id_t> type_arguments = call_arguments(function, info, object);
        const std::vector<type_id_t> type_parameters = type_parameters_of(function);
        for (std::size_t i = 0; i < info.arguments.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            const std::optional<expr_id_t> argument = info.arguments[i];
            // A missing argument gets the parameter's default, which is known at compile time,
            // or a field's zero. ByRef passes the variable itself.
            if (!argument) {
                const type_id_t type =
                    types().substitute(parameter.type, type_parameters, type_arguments);
                arguments.push_back({ .value = default_of(parameter, type) });
            } else if (parameter.by_ref) {
                arguments.push_back({ .value = lower_place(*argument), .by_ref = true });
            } else {
                arguments.push_back({ .value = lower_value(*argument) });
            }
        }

        if (function.kind == symbol_kind_t::METHOD && is_array(function.owner)) {
            return array_method(function, arguments, span_of(id));
        }

        // A @Flags Enum's Has: whether every one of the flags is set.
        if (function.kind == symbol_kind_t::METHOD && is_enum(function.owner)) {
            const binary_t flags{
                .op = binary_op_t::BIT_AND,
                .left = arguments[0].value,
                .right = arguments[1].value,
                .span = span_of(id),
            };
            const operand_t set = temporary(flags, function.owner);
            return compare(binary_op_t::EQUAL, set, arguments[1].value, span_of(id));
        }

        // A Class's New sets up a new object with its fields at their defaults, or Self for
        // Super.New.
        const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
        const bool on_super = member != nullptr &&
                              std::holds_alternative<super_expr_t>(m_ast.get(member->object).node);
        if (function.kind == symbol_kind_t::CONSTRUCTOR && is_class(function.type)) {
            if (on_super) {
                set_up(expect(info.symbol), expect(m_self), std::move(arguments), span_of(id));
                return std::nullopt;
            }

            // Until its New is done, a thrown error discards the object instead of releasing
            // it, so its Destroy doesn't run on fields New didn't set.
            operand_t made_object = new_object(object);
            const local_id_t made = std::get<local_id_t>(std::get<place_t>(made_object).root);
            (void)take_temporary(made_object);
            set_up(expect(info.symbol), place_t{ made }, std::move(arguments), span_of(id), made);
            m_temporaries.push_back(made);
            return made_object;
        }

        // The constructor a Struct gets puts its arguments in the fields, which take their own
        // references.
        if (function.kind == symbol_kind_t::CONSTRUCTOR && !function.declaration) {
            aggregate_t value{ .type = object };
            for (const call_argument_t& argument : arguments) {
                take(argument.value);
                value.fields.push_back(argument.value);
            }

            return temporary(std::move(value), object, has_references(object));
        }

        // RefEquals compares the references themselves.
        if (function.kind == symbol_kind_t::BUILTIN_FUNCTION && function.name == "RefEquals") {
            return compare(binary_op_t::EQUAL, arguments[0].value, arguments[1].value, span_of(id));
        }

        // Print is the other built-in function.
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

        // Super calls the Method the base Class has, not an override further down.
        call_t call{
            .function = function_of(expect(info.symbol), type_arguments),
            .arguments = std::move(arguments),
        };
        if (is_virtual(function) && !on_super) {
            call.slot = slot_of(expect(info.symbol));
        }

        // A call that throws gets a variable for its error, which is checked right after. Its
        // result is only the caller's once it didn't throw.
        std::optional<local_id_t> error;
        if (takes_error(function)) {
            error = error_variable();
            call.arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        const type_id_t result_type =
            types().substitute(function.type, type_parameters, type_arguments);
        std::optional<operand_t> result;
        if (result_type == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
        } else {
            result = temporary(std::move(call), result_type);
        }

        if (error) {
            check_error(*error, span_of(id));
        }

        if (result && has_references(result_type)) {
            m_temporaries.push_back(std::get<local_id_t>(std::get<place_t>(*result).root));
        }

        return result;
    }

    /// 'As' between Classes checks the object's Class at run time, unless it converts anyway.
    /// So does 'As' from a T? to a T, for Null.
    operand_t lower_expr(const expr_id_t id, const cast_expr_t& node) {
        const operand_t value = lower_value(node.value);
        const type_id_t type = expr_type(id);
        use_class(type);
        if (node.checked) {
            return retype(
                value,
                downcast_t{ .value = value, .type = type, .span = span_of(id), .or_null = true },
                type);
        }

        const type_t& from = types().get(type_of(value));
        const type_id_t source = from.kind == type_kind_t::NULLABLE ? from.inner : type_of(value);
        const type_id_t target = is_nullable(type) ? types().get(type).inner : type;
        const bool checks = is_class(source) && is_class(target) &&
                            (!types().extends(source, target) ||
                             (from.kind == type_kind_t::NULLABLE && !is_nullable(type)));
        if (checks) {
            return retype(value, downcast_t{ .value = value, .type = type, .span = span_of(id) },
                          type);
        }

        if (is_enum(type) && !is_enum(type_of(value)) && !types().get(type).flags) {
            return to_enum(value, type, span_of(id));
        }

        return convert(value, type, span_of(id));
    }

    /// An integer as one of an Enum's values, which it has to be. Values in a run need only a
    /// check that it's in range.
    operand_t to_enum(const operand_t& value, const type_id_t type, const span_t span) {
        const type_id_t integer = types().get(type).inner;
        const operand_t number = convert(value, integer, span);
        std::vector<std::uint64_t> values;
        for (const enum_value_t& member : types().get(type).values) {
            values.push_back(member.bits);
        }

        const auto by_value = [&](const std::uint64_t a, const std::uint64_t b) {
            return is_signed(integer) ? static_cast<std::int64_t>(a) < static_cast<std::int64_t>(b)
                                      : a < b;
        };
        std::ranges::sort(values, by_value);
        bool run = true;
        for (std::size_t i = 1; i < values.size(); ++i) {
            run = run && values[i] == values[i - 1] + 1;
        }

        operand_t valid = make_bool(false);
        if (run) {
            const operand_t above = compare(binary_op_t::GREATER_EQUAL, number,
                                            enum_constant(type, values.front(), true), span);
            const operand_t below = compare(binary_op_t::LESS_EQUAL, number,
                                            enum_constant(type, values.back(), true), span);
            valid = compare(binary_op_t::AND, above, below, span);
        } else {
            for (const std::uint64_t bits : values) {
                const operand_t same =
                    compare(binary_op_t::EQUAL, number, enum_constant(type, bits, true), span);
                valid = compare(binary_op_t::OR, valid, same, span);
            }
        }

        const block_id_t fail = new_block();
        const block_id_t ok = new_block();
        terminate(branch_t{ .condition = valid, .if_true = ok, .if_false = fail });
        start(fail);
        terminate(panic_t{
            .message = std::format("the value isn't one of {}'s", types().name(type)),
            .span = span,
        });
        start(ok);
        return cast(number, type, span);
    }

    /// The function that gives the text a string shows for an Enum's values, made the first
    /// time one is needed. Its body is lowered last, by lower_text().
    function_id_t text_function(const type_id_t type) {
        if (const auto found =
                std::ranges::find(m_texts, type, &std::pair<type_id_t, function_id_t>::first);
            found != m_texts.end()) {
            return found->second;
        }

        const auto id = static_cast<function_id_t>(m_module.functions.size());
        const std::string_view name = types().name(type);
        m_module.functions.push_back({
            .name = std::format("{}.Text({})", name, name),
            .return_type = type_id_t::STRING,
        });
        m_texts.emplace_back(type, id);
        return id;
    }

    /// An Enum value's text: its member's name. A @Flags Enum's value that isn't one of its
    /// members shows the flags it has, like "World | Player". Anything else shows its integer.
    void lower_text(const type_id_t type, const function_id_t id) {
        begin_function(id);
        const local_id_t value = add_local(type, "value");
        current().parameter_count = 1;
        const type_t& info = types().get(type);
        for (const enum_value_t& member : info.values) {
            const operand_t same =
                compare(binary_op_t::EQUAL, value, enum_constant(type, member.bits), {});
            const block_id_t named = new_block();
            const block_id_t next = new_block();
            terminate(branch_t{ .condition = same, .if_true = named, .if_false = next });
            start(named);
            terminate(return_t{ .value = make_string(member.name) });
            start(next);
        }

        if (info.flags) {
            const local_id_t text = add_local(type_id_t::STRING, "text");
            initialize(text, make_string(""));
            m_scopes.back().emplace_back(text);
            for (const enum_value_t& member : info.values) {
                if (std::has_single_bit(member.bits)) {
                    add_flag_text(value, text, type, member);
                }
            }

            const operand_t empty =
                runtime_call(runtime_function_t::EQUAL, { text, make_string("") }, type_id_t::BOOL);
            const block_id_t named = new_block();
            const block_id_t number = new_block();
            terminate(branch_t{ .condition = empty, .if_true = number, .if_false = named });
            start(named);
            take(place_t{ text });
            release_scopes(0);
            terminate(return_t{ .value = place_t{ text } });
            start(number);
        }

        const operand_t integer = cast(value, info.inner, {});
        const operand_t shown =
            runtime_call(runtime_function_t::TO_TEXT, { integer }, type_id_t::STRING, true);
        take(shown);
        release_scopes(0);
        terminate(return_t{ .value = shown });
    }

    /// Adds a flag's name to `text` when `value` has it, after a " | " if `text` has others.
    void add_flag_text(const local_id_t value, const local_id_t text, const type_id_t type,
                       const enum_value_t& member) {
        const operand_t flag = enum_constant(type, member.bits);
        const span_t nowhere;
        const binary_t flags{
            .op = binary_op_t::BIT_AND,
            .left = value,
            .right = flag,
            .span = nowhere,
        };
        const operand_t set = temporary(flags, type);
        const operand_t has = compare(binary_op_t::EQUAL, set, flag, nowhere);
        const block_id_t adding = new_block();
        const block_id_t done = new_block();
        terminate(branch_t{ .condition = has, .if_true = adding, .if_false = done });
        start(adding);
        const operand_t empty =
            runtime_call(runtime_function_t::EQUAL, { text, make_string("") }, type_id_t::BOOL);
        const block_id_t first = new_block();
        const block_id_t more = new_block();
        terminate(branch_t{ .condition = empty, .if_true = first, .if_false = more });
        start(first);
        store(text, make_string(member.name));
        jump(done);
        start(more);
        const operand_t joined = runtime_call(
            runtime_function_t::JOIN, { text, make_string(" | "), make_string(member.name) },
            type_id_t::STRING, true);
        store(text, joined);
        finish_statement();
        jump(done);
        start(done);
    }

    /// "x Is T".
    operand_t lower_expr(const expr_id_t id, const is_expr_t& node) {
        const operand_t value = lower_value(node.value);
        use_class(concrete(m_analysis.expr(id).tested));
        return temporary(is_t{ .value = value, .type = concrete(m_analysis.expr(id).tested) },
                         type_id_t::BOOL);
    }

    /// Super before a call is Self, as an object of the Class it extends.
    operand_t lower_expr(expr_id_t /*id*/, const super_expr_t& /*node*/) {
        return place_t{ expect(m_self) };
    }

    /// A temporary holding `value`'s object as another type, from `rvalue`. A temporary that owned
    /// it hands it over.
    operand_t retype(const operand_t& value, rvalue_t rvalue, const type_id_t type) {
        const bool owned = take_temporary(value);
        return temporary(std::move(rvalue), type, owned);
    }
};

} // namespace

mir_module_t lower(const ast_t& ast, const analysis_t& analysis) {
    return lowerer_t(ast, analysis).run();
}

} // namespace fb
