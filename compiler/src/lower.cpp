#include "fb/lower.hpp"

#include "fb/c_abi.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

/// What a variable holds before it's assigned: zero, False, "", or a null CString.
const_value_t zero(const type_id_t type) {
    if (type == type_id_t::BOOL) {
        return make_bool(false);
    }

    if (type == type_id_t::STRING) {
        return make_string("");
    }

    if (type == type_id_t::CSTRING) {
        return { .type = type, .data = nullptr };
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

/// The unsigned type of the same width, which holds the distance between two values: a CLong's
/// is a CULong, as wide on every target, and a CWChar's a UInt32, which holds what its wrapping
/// differences are as a UInt16, an Int32, or a UInt32.
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
    case type_id_t::CLONG:
        return type_id_t::CULONG;
    case type_id_t::CWCHAR:
        return type_id_t::UINT32;
    default:
        return type;
    }
}

/// An Operator's name in MIR, like "Operator +", or any other function's name as it is.
std::string operator_name(const std::string_view name) {
    const bool letter =
        !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) != 0 || name[0] == '_');
    return letter || name.empty() ? std::string(name) : std::format("Operator {}", name);
}

/// The Mutex that a Lock block holds, which leaving the block lets go of.
struct unlock_t {
    place_t mutex;
};

/// What leaving a scope does: releases a variable, runs a Defer's statement or a Finally, or
/// unlocks a Lock's Mutex.
struct cleanup_t {
    std::variant<local_id_t, stmt_id_t, const block_t*, unlock_t> action;

    // Implicit, so a variable, a deferred statement, a Finally, or a Lock's Mutex can be put in
    // a scope directly.
    cleanup_t(const local_id_t local) : action(local) {}
    cleanup_t(const stmt_id_t deferred) : action(deferred) {}
    cleanup_t(const block_t* finally) : action(finally) {}
    cleanup_t(unlock_t unlock) : action(std::move(unlock)) {}
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

/// A way out of scopes that is running a deferred statement or a Finally: its scope, its place
/// there, and how many scopes there were. The way out has already let go of what comes after it
/// in its scope, and of the scopes after that one.
struct leaving_t {
    std::size_t scope = 0;
    std::size_t position = 0;
    std::size_t scopes = 0;
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

/// Sets a variable while it's in scope, and puts back what it held before.
template <typename T>
class scoped_value_t {
public:
    scoped_value_t(T& variable, T value)
        : m_variable(variable), m_outer(std::exchange(variable, std::move(value))) {}
    scoped_value_t(const scoped_value_t&) = delete;
    scoped_value_t(scoped_value_t&&) = delete;
    scoped_value_t& operator=(const scoped_value_t&) = delete;
    scoped_value_t& operator=(scoped_value_t&&) = delete;
    ~scoped_value_t() {
        m_variable = std::move(m_outer);
    }

private:
    T& m_variable;
    T m_outer;
};

/// A member that a copy of a loop over a type's members is for.
struct reflected_t {
    std::string name;
    std::uint32_t index = 0;
    /// The member's symbol. A tuple's elements have none.
    std::optional<symbol_id_t> symbol;
    /// The type it's a member of.
    type_id_t owner = type_id_t::INVALID;
};

/// Turns the analyzed AST into MIR: one function per Function, Method, and New, plus "main" for
/// the top-level code. Each variable holding a reference, to a String, an object, an array, or a
/// Weak's object, or a Struct or tuple with references in it, owns each one, so lowering adds the
/// retains and releases. Parameters borrow the caller's.
class lowerer_t {
public:
    lowerer_t(const ast_t& ast, const analysis_t& analysis) : m_ast(ast), m_analysis(analysis) {
        m_module.types = std::make_shared<type_table_t>(analysis.types);
        if (const std::optional<module_id_t> main = ast.modules().main()) {
            m_module.main_module = ast.modules().get(*main).name;
        }

        for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
            if (analysis.symbols[i].kind == symbol_kind_t::TYPE) {
                m_declared_types[analysis.symbols[i].type] = static_cast<symbol_id_t>(i);
            }
        }

        // What implements an Interface's Method that throws is called through its table the
        // same way, with an error variable, whether or not it throws itself.
        for (const conformance_t& conformance : analysis.conformances) {
            const symbol_t& interface =
                analysis.symbol(m_declared_types.at(definition_of(conformance.interface)));
            for (std::size_t i = 0; i < interface.members.size() && i < conformance.members.size();
                 ++i) {
                if (!analysis.symbol(interface.members[i]).throws.empty()) {
                    m_takes_error.insert(conformance.members[i]);
                }
            }
        }

        // So is every Method in a Class's slot, so the one an override overrides gets one too.
        for (const symbol_id_t id : std::vector(m_takes_error.begin(), m_takes_error.end())) {
            for (std::optional<symbol_id_t> above = analysis.symbol(id).overrides; above;
                 above = analysis.symbol(*above).overrides) {
                m_takes_error.insert(*above);
            }
        }
    }

    mir_module_t run() {
        declare_globals();
        declare_functions();
        // Every Class in the main module is in the MIR module, whether or not the program makes
        // one. A generic one has one for each instance.
        for (const symbol_t& symbol : m_analysis.symbols) {
            if (symbol.kind == symbol_kind_t::TYPE && !outside_main(symbol) &&
                types().get(symbol.type).parameters.empty()) {
                use_class(symbol.type);
            }
        }

        for (const stmt_id_t id : m_ast.items()) {
            // Other modules' code is lowered once the program uses it, in lower_library().
            if (m_ast.outside_main(id)) {
                continue;
            }

            // Generic code is lowered for each set of type arguments it's used with.
            const stmt_node_t& node = m_ast.get(id).node;
            if (const auto* function = std::get_if<function_stmt_t>(&node);
                function != nullptr && function->type_parameters.empty()) {
                lower_function(id, *function);
            } else if (const auto* declared = std::get_if<type_stmt_t>(&node);
                       declared != nullptr && declared->type_parameters.empty() &&
                       !declared->is_interface) {
                // An Interface's Methods run with the type for Self, as each one needs them.
                for (const stmt_id_t member : declared->members) {
                    lower_member(member);
                }
            } else if (const auto* extension = std::get_if<extend_stmt_t>(&node)) {
                // What an Extend adds to a generic type, or to T[], has its type parameters.
                for (const stmt_id_t member : extension->members) {
                    const std::optional<symbol_id_t> added = m_analysis.stmt(member).symbol;
                    if (added && type_parameters_of(m_analysis.symbol(*added)).empty()) {
                        lower_member(member);
                    }
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
        number_alike_names();
        return std::move(m_module);
    }

    /// Two functions or Globals can come out with one name, since a module's name and a type's
    /// are both dotted: module x's Class y's Static Method Z and module x.y's Function Z are both
    /// "x.y.Z()", as are a Static of the main module's Class lib and a Global of module lib.
    /// Each after the first gets "#2", "#3", and so on, so a name means one thing.
    void number_alike_names() {
        std::unordered_map<std::string, int> seen;
        const auto number = [&](std::string& name) {
            if (const int count = ++seen[name]; count > 1) {
                name += std::format("#{}", count);
            }
        };
        for (mir_function_t& function : m_module.functions) {
            number(function.name);
        }

        for (auto& global : m_module.globals) {
            number(global.name);
        }
    }

    /// One function, and the ones it calls, after it.
    mir_module_t run_one(const symbol_id_t id, const std::vector<type_id_t>& type_arguments) {
        (void)function_of(id, type_arguments);
        lower_library();
        for (const auto& [type, function] : m_texts) {
            lower_text(type, function);
        }

        return std::move(m_module);
    }

private:
    const ast_t& m_ast;
    const analysis_t& m_analysis;
    mir_module_t m_module;
    std::unordered_map<symbol_id_t, function_id_t> m_functions;
    std::unordered_map<symbol_id_t, global_id_t> m_globals;
    /// Each ThreadLocal Global whose initializer runs, by its declaration.
    std::unordered_map<global_id_t, stmt_id_t> m_thread_local_declarations;
    /// The ThreadLocal Globals whose initializers' functions code has needed, in that order,
    /// which lower_library() lowers.
    std::vector<global_id_t> m_thread_initializers;
    /// The functions besides main whose uncaught errors end the program: the ThreadLocals'
    /// initializers.
    std::unordered_set<function_id_t> m_ending_functions;
    /// Each instance's Static Fields of a generic type, by the field and the type arguments.
    std::map<std::pair<symbol_id_t, std::vector<type_id_t>>, global_id_t> m_instance_globals;
    /// Each Class's Method table, by its type, made when it's first needed.
    std::unordered_map<type_id_t, std::vector<symbol_id_t>> m_tables;
    /// Each Class Method's slot in the Method tables.
    std::unordered_map<symbol_id_t, std::uint32_t> m_slots;
    /// The Classes the module has, in the order the program used them.
    std::vector<type_id_t> m_used_classes;
    /// The functions from outside the main module that the program uses, in the order it used
    /// them. Their bodies are lowered after the main module's code.
    std::vector<symbol_id_t> m_library_functions;
    /// A closure whose code waits to be lowered: the code that made it may be an instance of
    /// generic code, whose type arguments the closure's code has too.
    struct pending_closure_t {
        closure_t closure;
        type_id_t type;
        std::vector<type_id_t> parameters;
        std::vector<type_id_t> arguments;
        /// Inside a copy of a loop over a type's members: the member each loop's variable is.
        std::unordered_map<symbol_id_t, reflected_t> reflected;
        /// An Async lambda's coroutine, rather than the code its calls run, which starts it.
        bool coroutine = false;
        /// Where the code that made it came from, which its own code continues.
        std::optional<std::size_t> origin;
    };

    /// The function values the program makes, whose code is lowered after the function that
    /// makes them, and that code's function, by the closure's Class.
    std::vector<pending_closure_t> m_closures;
    std::unordered_map<type_id_t, function_id_t> m_closure_code;
    /// Where the code being lowered is: the innermost expression or statement, whose place a
    /// call of a @PanicsAtCaller function gives it. nullopt in code the compiler makes itself,
    /// like a function value's.
    std::optional<span_t> m_span;
    /// Expressions lowered already that a statement uses again, by what they gave: an index's
    /// object and indices, which "grid[x] += v" reads and sets the element with.
    std::unordered_map<expr_id_t, operand_t> m_reused;
    /// Each Async lambda's coroutine, by the closure's Class.
    std::unordered_map<type_id_t, function_id_t> m_coroutine_code;
    /// An instance of a generic function: the function, and a type argument for each type
    /// parameter it has, its Struct's or Class's first.
    using instance_t = std::pair<symbol_id_t, std::vector<type_id_t>>;
    /// The instances the program uses, each lowered once, in the order they were used.
    std::map<instance_t, function_id_t> m_instances;
    std::vector<instance_t> m_pending_instances;
    /// Where an instance of generic code, or a Class's instance, came from: what it's an instance
    /// of, and what was being lowered when the program first needed it, so code that makes a
    /// larger instance each time makes a chain of them.
    struct origin_t {
        std::variant<symbol_id_t, type_id_t> of;
        std::optional<std::size_t> parent;
    };

    std::vector<origin_t> m_origins;
    std::unordered_map<function_id_t, std::size_t> m_instance_origins;
    std::unordered_map<type_id_t, std::size_t> m_class_origins;
    /// The origin of the instance, or Class's table, being lowered.
    std::optional<std::size_t> m_origin;
    /// The types whose Const values check_values() has checked.
    std::unordered_set<type_id_t> m_checked_values;
    /// While lowering an instance of generic code: its type parameters, and the type argument
    /// that each one stands for.
    std::vector<type_id_t> m_parameters;
    std::vector<type_id_t> m_arguments;
    /// The Enums whose values strings show, with the function that gives each value's text.
    /// Their bodies are lowered last.
    std::vector<std::pair<type_id_t, function_id_t>> m_texts;

    /// What a function that lowering makes for an array type does: compare two arrays element
    /// by element, hash one, or give its text.
    enum class generated_kind_t : std::uint8_t {
        EQUAL,
        HASH,
        TEXT,
        /// A Task's or Thread's: runs its closure and keeps the result.
        RUN,
    };

    struct generated_t {
        generated_kind_t kind;
        type_id_t type;
        function_id_t function;
    };

    /// The array functions the program uses, and each Task's and Thread's work, whose bodies are
    /// lowered with the code used from outside the main module.
    std::vector<generated_t> m_generated;

    /// What an Interface's table runs for a Method that isn't called the way the Interface's
    /// is: one that gives a Struct where the Interface's gives an Iterator<T>, as a collection's
    /// Iterate() does for Iterable<T>'s, or one that gets an error variable where the
    /// Interface's doesn't. A function that calls it, then puts the Struct in a box.
    struct table_thunk_t {
        struct parameter_t {
            type_id_t type;
            bool by_ref = false;
        };

        function_id_t function;
        function_id_t inner;
        /// The Method's parameters, Self first.
        std::vector<parameter_t> parameters;
        /// The Method gets an error variable, as what implements an Interface's Method that
        /// throws does.
        bool takes_error = false;
        /// The Interface's Method throws, so the function puts the Method's error in its caller's
        /// variable. Otherwise nothing sets it: what implements one that doesn't can't throw.
        bool passes_error = false;
        type_id_t given = type_id_t::INVALID;
        type_id_t wanted = type_id_t::INVALID;
    };

    std::vector<table_thunk_t> m_table_thunks;
    /// Each declared type's symbol, a Struct's, Class's, Interface's, or Enum's, by its type.
    std::unordered_map<type_id_t, symbol_id_t> m_declared_types;
    /// The Methods that implement an Interface's Method that throws.
    std::unordered_set<symbol_id_t> m_takes_error;
    /// While lowering a copy of a For Field's or For Method's body: the member each loop's
    /// variable is, by the variable.
    std::unordered_map<symbol_id_t, reflected_t> m_reflected;
    /// Each copy of a loop's body, inside the loops around it, has its own closure Classes, by
    /// the Class and the copies.
    std::vector<std::uint32_t> m_copies;
    std::uint32_t m_next_copy = 0;
    std::map<std::pair<type_id_t, std::vector<std::uint32_t>>, type_id_t> m_closure_copies;

    // The function being lowered
    function_id_t m_function{};
    std::unordered_map<symbol_id_t, local_id_t> m_locals;
    /// In an Async Function: its Task, its first parameter, which gets the result.
    std::optional<local_id_t> m_async_task;
    /// In an Async Function: the parameters it keeps references to while it runs.
    std::vector<local_id_t> m_async_kept;
    /// A Lock block's variable: the value in its Mutex, where it is.
    std::unordered_map<symbol_id_t, place_t> m_aliases;
    /// In a closure's code: the locals holding copies it captured, which never change. A
    /// Parallel For's threads share them.
    std::vector<local_id_t> m_unchanging;
    std::optional<block_id_t> m_block;            // nullopt after a jump: nothing reaches here
    std::vector<std::vector<cleanup_t>> m_scopes; // what leaving each does, innermost last
    std::vector<local_id_t> m_temporaries;        // owned values to release after the statement
    std::vector<loop_t> m_loops;                  // innermost last
    std::vector<leaving_t> m_leaving;             // innermost last
    /// Errors on their way out while the Defers and Finallys they pass run, innermost last.
    std::vector<local_id_t> m_raising;
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
        // A generic Method is lowered for each set of type arguments it's called with.
        if (const auto* method = std::get_if<function_stmt_t>(&node);
            method != nullptr && method->type_parameters.empty()) {
            lower_function(id, *method);
        } else if (const auto* property = std::get_if<property_stmt_t>(&node)) {
            for (const std::optional<stmt_id_t> part : { property->getter, property->setter }) {
                if (part) {
                    lower_member(*part);
                }
            }
        }
    }

    /// Globals, Static Fields, and top-level Consts. A generic type's Static Fields are each
    /// instance's, made as it's used, and a Const known at compile time has none until code needs
    /// its place, since its uses read its value.
    void declare_globals() {
        for (const symbol_id_t id : m_analysis.globals) {
            const symbol_t& symbol = m_analysis.symbol(id);
            const bool generic =
                symbol.owner != type_id_t::INVALID && !types().get(symbol.owner).parameters.empty();
            const bool known = symbol.kind == symbol_kind_t::CONSTANT && symbol.value.has_value();
            if (!generic && !known) {
                declare_global(id);
            }
        }
    }

    /// A Global's, Static Field's, or Const's global. One whose value is known at compile time
    /// starts with it. The rest start at zero, and main sets the ones with an initializer first.
    global_id_t declare_global(const symbol_id_t id) {
        const symbol_t& symbol = m_analysis.symbol(id);
        const auto& node = std::get<variable_stmt_t>(m_ast.get(expect(symbol.declaration)).node);
        // An instance in its type has sizes it worked out, which are checked where it's declared.
        {
            const scoped_value_t<std::optional<span_t>> at(m_span, symbol.span);
            check_values(symbol.type);
        }

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
            if (const std::optional<const_value_t>& value = m_analysis.expr(*node.value).value) {
                initial = *value;
            }
        }

        // A Static Field is named after its type, and one outside the main module after its
        // module.
        std::string name(symbol.name);
        if (symbol.owner != type_id_t::INVALID) {
            name = std::format("{}.{}", types().qualified_name(symbol.owner), symbol.name);
        } else if (const std::string_view module = qualifier_of(symbol); !module.empty()) {
            name = std::format("{}.{}", module, symbol.name);
        }

        const auto global = static_cast<global_id_t>(m_module.globals.size());
        m_globals[id] = global;
        const bool thread_local_global = symbol.modifiers.is_thread_local;
        const bool runs = node.value && !m_analysis.expr(*node.value).value;
        m_module.globals.push_back({
            .name = std::move(name),
            .type = symbol.type,
            .initial = std::move(initial),
            .set_in_main = runs && !thread_local_global,
            .is_thread_local = thread_local_global,
        });
        if (runs && thread_local_global) {
            m_thread_local_declarations[global] = expect(symbol.declaration);
        }

        return global;
    }

    /// A ThreadLocal Global's initializer, as a function of its own that sets the calling
    /// thread's value, made the first time code uses the Global and lowered with the library's
    /// code, so one the program never uses costs nothing.
    void use_thread_local(const global_id_t global) {
        if (!m_thread_local_declarations.contains(global) ||
            m_module.globals[index(global)].initializer) {
            return;
        }

        const auto function = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back(
            { .name = std::format("{}.Initializer", m_module.globals[index(global)].name) });
        m_module.globals[index(global)].initializer = function;
        m_thread_initializers.push_back(global);
        m_ending_functions.insert(function);
    }

    /// The calling thread's value of a ThreadLocal Global, from its initializer. An error it lets
    /// out ends the program, as top-level code's does.
    void lower_thread_initializer(const global_id_t global) {
        const stmt_id_t declaration = m_thread_local_declarations.at(global);
        const auto& node = std::get<variable_stmt_t>(m_ast.get(declaration).node);
        begin_function(expect(m_module.globals[index(global)].initializer));
        store(global, lower_value(expect(node.value)));
        finish_statement();
        finish_function();
    }

    /// A generic type's Static Field for one instance, which starts with its value known at
    /// compile time, or its zero.
    global_id_t instance_global(const symbol_id_t field, const std::vector<type_id_t>& arguments) {
        const auto key = std::pair(field, arguments);
        if (const auto found = m_instance_globals.find(key); found != m_instance_globals.end()) {
            return found->second;
        }

        const symbol_t& symbol = m_analysis.symbol(field);
        const type_id_t owner = types().instance(symbol.owner, arguments);
        const std::vector<type_id_t>& parameters = types().get(symbol.owner).parameters;
        const type_id_t type = types().substitute(symbol.type, parameters, arguments);
        check_values(type);
        std::optional<const_value_t> initial;
        const auto& node = std::get<variable_stmt_t>(m_ast.get(expect(symbol.declaration)).node);
        if (node.value && m_analysis.expr(*node.value).value) {
            // A T?'s Null is the instance's.
            initial = *m_analysis.expr(*node.value).value;
            initial->type = types().substitute(initial->type, parameters, arguments);
        } else if (types().get(type).kind == type_kind_t::BUILTIN) {
            initial = zero(type);
        } else if (is_nullable(type)) {
            initial = null_of(type);
        } else if (is_enum(type)) {
            initial = enum_constant(type, types().enum_zero(type));
        }

        const auto global = static_cast<global_id_t>(m_module.globals.size());
        m_module.globals.push_back({
            .name = std::format("{}.{}", types().qualified_name(owner), symbol.name),
            .type = type,
            .initial = std::move(initial),
        });
        m_instance_globals[key] = global;
        return global;
    }

    /// A Static Field's global, through an expression that uses it: for a generic type's, the
    /// instance's, from the type arguments analysis recorded.
    global_id_t global_of(const symbol_id_t field, const expr_id_t use) {
        const symbol_t& symbol = m_analysis.symbol(field);
        if (symbol.owner == type_id_t::INVALID || types().get(symbol.owner).parameters.empty()) {
            return ready(m_globals.at(field), use);
        }

        // Used by name, it's in its type's own code, whose instance is being lowered.
        std::vector<type_id_t> arguments = m_analysis.expr(use).type_arguments;
        if (arguments.empty()) {
            arguments = types().get(symbol.owner).parameters;
        }

        for (type_id_t& argument : arguments) {
            argument = concrete(argument);
        }

        return instance_global(field, arguments);
    }

    /// The main module's functions get their ids before any body is lowered, so a call can reach
    /// a function declared further down. "main" comes after them, then the other modules'
    /// functions and the instances of generic ones, as the program uses them.
    void declare_functions() {
        for (const symbol_id_t id : m_analysis.functions) {
            if (!outside_main(m_analysis.symbol(id)) &&
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
        // An Async Function's code gives its result through its Task.
        mir_function_t declared{
            .name = function_name(symbol),
            .return_type = sets_up || symbol.modifiers.is_async ? type_id_t::NONE : symbol.type,
            .is_inline = symbol.modifiers.is_inline,
            .panics_at_caller = symbol.modifiers.panics_at_caller,
            .is_async = symbol.modifiers.is_async,
        };
        // An Extern Function is only its parameters, under its C symbol. C gives a String's text
        // as a CString, which the call copies.
        if (const external_t* external = external_of(symbol)) {
            declared.external = {
                .symbol = external->symbol.value_or(std::string(symbol.name)),
                .c_abi = external->convention != convention_t::RUNTIME,
            };
            if (declared.external->c_abi && is_text(symbol.type)) {
                declared.return_type = type_id_t::CSTRING;
            }

            // The runtime takes the Task it gives first, and finishes it later.
            if (symbol.gives_task) {
                declared.return_type = type_id_t::NONE;
                declared.locals.push_back({ .type = symbol.type, .name = "task" });
            }

            for (const symbol_id_t parameter : symbol.parameters) {
                const symbol_t& found = m_analysis.symbol(parameter);
                declared.locals.push_back(
                    { .type = found.type, .name = found.name, .by_ref = found.by_ref });
            }

            declared.parameter_count = declared.locals.size();
        }

        m_module.functions.push_back(std::move(declared));
        return function;
    }

    /// A String or String?, which C gives as a CString.
    [[nodiscard]] bool is_text(const type_id_t type) const {
        return type == type_id_t::STRING ||
               (is_nullable(type) && types().get(type).inner == type_id_t::STRING);
    }

    /// What defines an Extern Function, or null for any other function.
    [[nodiscard]] const external_t* external_of(const symbol_t& function) const {
        if (!function.declaration) {
            return nullptr;
        }

        const auto* node = std::get_if<function_stmt_t>(&m_ast.get(*function.declaration).node);
        return node != nullptr && node->external ? &*node->external : nullptr;
    }

    /// Whether a symbol is declared outside the main module.
    [[nodiscard]] bool outside_main(const symbol_t& symbol) const {
        return symbol.declaration && m_ast.outside_main(*symbol.declaration);
    }

    /// The module that a symbol's MIR name starts with: its declaration's, or none in the main
    /// module.
    [[nodiscard]] std::string_view qualifier_of(const symbol_t& symbol) const {
        if (!symbol.declaration || !m_ast.outside_main(*symbol.declaration)) {
            return {};
        }

        const file_id_t file = m_ast.get(*symbol.declaration).span.file;
        return m_ast.modules().get(m_ast.modules().of(file).value_or(module_id_t{})).name;
    }

    /// What a function's MIR name starts with: its Struct's or Class's qualified name, if it has
    /// one, and before that, outside the main module, its module, unless the type's name starts
    /// with it already. What an Extend adds to another module's type, or to a built-in one,
    /// starts with the Extend's module.
    [[nodiscard]] std::string name_start(const symbol_t& function, const type_id_t owner) const {
        std::string start =
            owner != type_id_t::INVALID ? std::format("{}.", types().qualified_name(owner)) : "";
        const std::string_view module = qualifier_of(function);
        if (owner == type_id_t::INVALID) {
            return module.empty() ? start : std::format("{}.", module);
        }

        // A type's own member, or what an Extend in its module adds.
        if (!function.extended || types().get(owner).module == module) {
            return start;
        }

        return std::format("{}.{}", module.empty() ? m_module.main_module : module, start);
    }

    /// "Add(Int32, Int32)": overloads get different names. A Struct's members start with its
    /// name, "Point.Move(Int32)", and outside the main module, functions start with the module,
    /// "std.core.error.Error.New(String)".
    [[nodiscard]] std::string function_name(const symbol_t& function) const {
        std::string name = name_start(function, function.owner);
        name += operator_name(function.name) + "(";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            name += i == 0 ? "" : ", ";
            name += parameter.by_ref ? "ByRef " : "";
            name += types().qualified_name(parameter.type);
        }

        return name + ")";
    }

    /// A function's id. One outside the main module gets one when the program first uses it, so
    /// the program only gets the library code it uses. A generic one has one for each set of
    /// type arguments it's used with.
    function_id_t function_of(const symbol_id_t id, const std::vector<type_id_t>& arguments = {}) {
        // A Method a type got from an Interface runs the default's code with the type for Self,
        // like an instance of generic code.
        if (!arguments.empty() || m_analysis.symbol(id).default_method) {
            return instance_of(id, arguments);
        }

        if (const auto found = m_functions.find(id); found != m_functions.end()) {
            return found->second;
        }

        // An Extern Function has no body to lower.
        if (external_of(m_analysis.symbol(id)) == nullptr) {
            m_library_functions.push_back(id);
        }

        return declare_function(id);
    }

    /// An instance of a generic function, declared the first time it's used. Its body is lowered
    /// with the others, after the main module's code.
    function_id_t instance_of(const symbol_id_t id, const std::vector<type_id_t>& arguments) {
        instance_t key{ id, arguments };
        if (const auto found = m_instances.find(key); found != m_instances.end()) {
            return found->second;
        }

        const symbol_t& symbol = m_analysis.symbol(id);
        const std::size_t origin = add_origin(id);
        // A type that's too large on a chain of the code that makes it grew there.
        const std::size_t chain = chain_length(origin);
        const bool large = std::ranges::any_of(
            arguments, [&](const type_id_t argument) { return too_large(argument); });
        const bool grown = chain > max_chain || large;
        if (grown) {
            stop_growing(std::format("'{}'", symbol.name), symbol.span, chain == 1);
        }

        check_arguments(type_parameters_of(symbol), arguments);

        const std::vector<type_id_t> parameters = type_parameters_of(symbol);
        const bool sets_up = symbol.kind == symbol_kind_t::CONSTRUCTOR && is_class(symbol.type);
        const auto function = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({
            .name = instance_name(symbol, parameters, arguments),
            .return_type = sets_up || symbol.modifiers.is_async
                               ? type_id_t::NONE
                               : types().substitute(symbol.type, parameters, arguments),
            .is_inline = symbol.modifiers.is_inline,
            .panics_at_caller = symbol.modifiers.panics_at_caller,
            .is_async = symbol.modifiers.is_async,
        });
        m_instances.emplace(key, function);
        m_instance_origins.emplace(function, origin);
        if (!grown) {
            m_pending_instances.push_back(std::move(key));
        }

        return function;
    }

    /// A new origin, of an instance of `of` that the code being lowered needs.
    std::size_t add_origin(const std::variant<symbol_id_t, type_id_t> of) {
        m_origins.push_back({ .of = of, .parent = m_origin });
        return m_origins.size() - 1;
    }

    /// How many instances of what `origin` is an instance of are on its chain, itself included.
    [[nodiscard]] std::size_t chain_length(const std::size_t origin) const {
        std::size_t length = 0;
        for (std::optional<std::size_t> at = origin; at && length <= max_chain;
             at = m_origins[*at].parent) {
            if (m_origins[*at].of == m_origins[origin].of) {
                ++length;
            }
        }

        return length;
    }

    /// Whether an integer type holds `number` as it is, rather than its low bits.
    [[nodiscard]] static bool fits(const std::int64_t number, const type_id_t type) {
        const const_value_t value{ .type = type_id_t::INT64, .data = number };
        const fold_result_t cast = fold_cast(value, type);
        const fold_result_t back = cast ? fold_cast(*cast, type_id_t::INT64) : cast;
        return back && back->data == value.data;
    }

    /// Reports, once, a Const type argument that an instance worked out outside what its type
    /// parameter takes, or in a type argument, outside what one can be.
    void check_arguments(const std::vector<type_id_t>& parameters,
                         const std::vector<type_id_t>& arguments) {
        for (std::size_t i = 0; i < arguments.size() && !m_module.error; ++i) {
            check_values(arguments[i]);
            const type_t& argument = types().get(arguments[i]);
            const type_id_t takes =
                i < parameters.size() && types().get(parameters[i]).kind == type_kind_t::PARAMETER
                    ? types().get(parameters[i]).inner
                    : type_id_t::INVALID;
            if (!m_module.error && takes != type_id_t::INVALID &&
                argument.kind == type_kind_t::VALUE && argument.operation == value_op_t::NONE &&
                !fits(argument.number, takes)) {
                m_module.error = mir_error_t{
                    .message = std::format(
                        "a Const type argument comes out as {} here, which doesn't fit in {}",
                        argument.number, types().name(takes)),
                    .span = m_span.value_or(span_t{}),
                };
            }
        }
    }

    /// Reports, once, a Const type argument or fixed array size that an instance worked out
    /// outside what one can be, a size or a count from 1 to max_count that fits its type, as
    /// N * 2 can be.
    void check_values(const type_id_t type) {
        if (m_module.error || !m_checked_values.insert(type).second) {
            return;
        }

        const type_t& info = types().get(type);
        if (info.kind == type_kind_t::VALUE && info.operation == value_op_t::NONE) {
            std::string why;
            if (info.number < 1 || info.number > max_count) {
                why = std::format("but it's a size or a count, from 1 to {}", max_count);
            } else if (!fits(info.number, info.inner)) {
                why = std::format("which doesn't fit in {}", types().name(info.inner));
            }

            if (!why.empty()) {
                m_module.error = mir_error_t{
                    .message = std::format("a Const type argument comes out as {} here, {}",
                                           info.number, why),
                    .span = m_span.value_or(span_t{}),
                };
            }

            return;
        }

        std::ranges::for_each(info.arguments, [this](const type_id_t part) { check_values(part); });
        std::ranges::for_each(info.parameters, [this](const type_id_t part) {
            if (part != type_id_t::INVALID) {
                check_values(part);
            }
        });
        if (info.inner != type_id_t::INVALID) {
            check_values(info.inner);
        }

        // An instance's Fields and base were worked out from its type arguments too, and a Struct
        // or Class that isn't generic can hold an instance in a Field, or extend one.
        if (info.kind == type_kind_t::TUPLE || info.kind == type_kind_t::STRUCT ||
            info.kind == type_kind_t::CLASS) {
            for (const field_t& field : info.fields) {
                check_values(field.type);
            }
        }

        if (info.base != type_id_t::INVALID) {
            check_values(info.base);
        }

        // Sizes that each fit can still make more elements than a fixed array holds, as a
        // written one's can.
        if (info.kind == type_kind_t::FIXED_ARRAY && !m_module.error) {
            std::int64_t count = 1;
            for (const std::uint32_t size : info.sizes) {
                if (size != 0 && count > max_count / size) {
                    m_module.error = mir_error_t{
                        .message = std::format("{} comes out here with more elements than a "
                                               "fixed array can hold, at most {}",
                                               types().name(type), max_count),
                        .span = m_span.value_or(span_t{}),
                    };
                    return;
                }

                count *= size != 0 ? size : 1;
            }
        }
    }

    /// How many instances of one generic function or Class a chain of code that needs them, one
    /// from the other, can make before it's one that makes a new one each time, like Step<N + 1>.
    static constexpr std::size_t max_chain = 50;

    /// How deep a type can nest, as deep as the parser lets code write one.
    static constexpr std::uint32_t max_depth = 1000;

    /// Whether a type nests deeper, or is made of more types, than fb makes: one that grows by
    /// doubling, like Pair<Self, Self>, gets there before a chain of instances is long.
    [[nodiscard]] bool too_large(const type_id_t type) const {
        const auto [depth, size] = types().measure(type);
        return depth > max_depth || size > type_table_t::max_size;
    }

    /// Records, once, that code needs a larger instance each time, where it's lowered.
    void stop_growing(const std::string_view what, const span_t fallback, const bool large) {
        if (m_module.error) {
            return;
        }

        m_module.error = mir_error_t{
            .message = large
                           ? std::format("{} needs an instance of a type nested more than {} deep, "
                                         "or made of more than {} types, here",
                                         what, max_depth, type_table_t::max_size)
                           : std::format("{} needs a larger instance each time it runs here, "
                                         "without end",
                                         what),
            .span = m_span.value_or(fallback),
        };
    }

    /// Like "Box<Int32>.Map<String>(Func(Int32): String)": the function's name, with the type
    /// arguments of its Struct or Class and its own, and its parameter types.
    std::string instance_name(const symbol_t& function, const std::vector<type_id_t>& parameters,
                              const std::vector<type_id_t>& arguments) {
        std::size_t own = 0;
        type_id_t owner = type_id_t::INVALID;
        if (function.owner != type_id_t::INVALID) {
            // What an Extend adds to T[] has T as a type parameter of its own, which the array
            // shows.
            own = function.extended && is_array(function.owner)
                      ? 1
                      : owner_parameters(function.owner).size();
            owner = types().substitute(function.owner, parameters, arguments);
        }

        std::string name = name_start(function, owner) + operator_name(function.name);
        for (std::size_t i = own; i < arguments.size(); ++i) {
            name +=
                std::format("{}{}", i == own ? "<" : ", ", types().qualified_name(arguments[i]));
        }

        name += arguments.size() > own ? ">(" : "(";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            const type_id_t type = types().substitute(parameter.type, parameters, arguments);
            name += std::format("{}{}{}", i == 0 ? "" : ", ", parameter.by_ref ? "ByRef " : "",
                                types().qualified_name(type));
        }

        return name + ")";
    }

    /// The type parameters a type's members can use: a generic Struct's or Class's, or an
    /// Interface's Self.
    [[nodiscard]] std::vector<type_id_t> owner_parameters(const type_id_t owner) const {
        const type_t& info = types().get(owner);
        if (info.kind == type_kind_t::INTERFACE) {
            std::vector<type_id_t> parameters{ info.inner };
            parameters.insert(parameters.end(), info.parameters.begin(), info.parameters.end());
            return parameters;
        }

        if (info.kind == type_kind_t::STRUCT || info.kind == type_kind_t::CLASS) {
            return info.parameters;
        }

        return {};
    }

    /// The type parameters a function's types can use: its Struct's or Class's, or its
    /// Interface's Self and type parameters, then its own.
    [[nodiscard]] std::vector<type_id_t> type_parameters_of(const symbol_t& function) const {
        std::vector<type_id_t> parameters;
        if (function.owner != type_id_t::INVALID) {
            parameters = owner_parameters(function.owner);
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
    /// Code that isn't generic can use an instance whose Fields work out a size, too.
    type_id_t concrete(const type_id_t type) {
        const type_id_t made = types().substitute(type, m_parameters, m_arguments);
        check_values(made);
        return made;
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
            arguments = owner_arguments(non_null(object), function.owner);
        }

        for (const type_id_t argument : info.type_arguments) {
            arguments.push_back(concrete(argument));
        }

        return arguments;
    }

    /// The type arguments of the instance of `owner`, a generic Struct or Class, that `type` is
    /// or extends: Base<Int32>'s for a Derived<Int32> that extends Base<T> with its T. None when
    /// it isn't generic.
    [[nodiscard]] std::vector<type_id_t> owner_arguments(const type_id_t type,
                                                         const type_id_t owner) const {
        for (type_id_t current = type; current != type_id_t::INVALID;
             current = types().get(current).base) {
            if (definition_of(current) == owner) {
                return types().get(current).arguments;
            }
        }

        return {};
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

        const std::size_t origin = add_origin(definition_of(type));
        m_class_origins.emplace(type, origin);
        const std::size_t chain = definition_of(type) != type ? chain_length(origin) : 1;
        if (chain > max_chain || too_large(type)) {
            stop_growing(types().name(definition_of(type)), span_t(), chain == 1);
            return;
        }

        check_arguments(types().get(definition_of(type)).parameters, types().get(type).arguments);

        m_used_classes.push_back(type);
        if (const type_id_t base = types().get(type).base; base != type_id_t::INVALID) {
            use_class(base);
        }
    }

    /// What the program uses from outside the main module: each Class it used, then the bodies
    /// of the functions it called, which can use more. The code of the function values it makes,
    /// and the instances of generic code, come here too.
    void lower_library() {
        std::size_t classes = 0;
        std::size_t functions = 0;
        std::size_t closures = 0;
        std::size_t instances = 0;
        std::size_t generated = 0;
        std::size_t thunks = 0;
        std::size_t initializers = 0;
        while (!m_module.error &&
               (classes < m_used_classes.size() || functions < m_library_functions.size() ||
                closures < m_closures.size() || instances < m_pending_instances.size() ||
                generated < m_generated.size() || thunks < m_table_thunks.size() ||
                initializers < m_thread_initializers.size())) {
            if (initializers < m_thread_initializers.size()) {
                const global_id_t global = m_thread_initializers[initializers++];
                m_parameters.clear();
                m_arguments.clear();
                const scoped_value_t<std::optional<std::size_t>> from(m_origin, std::nullopt);
                lower_thread_initializer(global);
                continue;
            }

            if (classes < m_used_classes.size()) {
                const type_id_t type = m_used_classes[classes++];
                const scoped_value_t<std::optional<std::size_t>> from(
                    m_origin, m_class_origins.contains(type)
                                  ? std::optional(m_class_origins.at(type))
                                  : std::nullopt);
                add_class(type);
                continue;
            }

            if (generated < m_generated.size()) {
                const generated_t function = m_generated[generated++];
                m_parameters.clear();
                m_arguments.clear();
                lower_generated(function);
                continue;
            }

            if (thunks < m_table_thunks.size()) {
                const table_thunk_t thunk = m_table_thunks[thunks++];
                m_parameters.clear();
                m_arguments.clear();
                lower_table_thunk(thunk);
                continue;
            }

            // Copies, since lowering them can make more.
            if (closures < m_closures.size()) {
                const pending_closure_t closure = m_closures[closures++];
                const scoped_value_t<std::optional<std::size_t>> from(m_origin, closure.origin);
                lower_closure(closure);
                continue;
            }

            if (instances < m_pending_instances.size()) {
                const instance_t instance = m_pending_instances[instances++];
                const scoped_value_t<std::optional<std::size_t>> from(
                    m_origin, m_instance_origins.at(m_instances.at(instance)));
                lower_instance(instance);
                continue;
            }

            m_parameters.clear();
            m_arguments.clear();
            const scoped_value_t<std::optional<std::size_t>> from(m_origin, std::nullopt);
            lower_body(m_library_functions[functions++], std::nullopt);
        }

        m_parameters.clear();
        m_arguments.clear();
    }

    /// An instance of generic code, with its type arguments for its type parameters. A Method a
    /// type got from an Interface runs the default's code, with the type for Self.
    void lower_instance(const instance_t& instance) {
        const symbol_t& symbol = m_analysis.symbol(instance.first);
        const function_id_t function = m_instances.at(instance);
        if (symbol.default_method) {
            // With the type for Self, then a generic Interface's type arguments, then a generic
            // Method's own, which the copy's last type parameters are.
            const symbol_t& method = m_analysis.symbol(*symbol.default_method);
            const std::vector<type_id_t> parameters = type_parameters_of(symbol);
            const type_id_t interface =
                types().substitute(symbol.default_interface, parameters, instance.second);
            m_parameters = type_parameters_of(method);
            m_arguments = { types().substitute(symbol.owner, parameters, instance.second) };
            std::ranges::copy(types().get(interface).arguments, std::back_inserter(m_arguments));
            const std::size_t own = method.type_parameters.size();
            assert(instance.second.size() >= own);
            std::ranges::copy(instance.second | std::views::drop(instance.second.size() - own),
                              std::back_inserter(m_arguments));
            lower_body(*symbol.default_method, function, instance.first);
            return;
        }

        m_parameters = type_parameters_of(symbol);
        m_arguments = instance.second;
        lower_body(instance.first, function);
    }

    /// A function's code, from its declaration. A Get or Set without one, an Abstract
    /// Property's or an Interface's, only panics: what overrides or implements it runs instead.
    /// `copy` is a type's copy of an Interface's default whose code this is.
    void lower_body(const symbol_id_t id, const std::optional<function_id_t> instance,
                    const std::optional<symbol_id_t> copy = std::nullopt) {
        const symbol_t& function = m_analysis.symbol(id);
        if (!function.declaration) {
            lower_abstract(id, instance);
            return;
        }

        const stmt_id_t declaration = *function.declaration;
        lower_function(declaration, std::get<function_stmt_t>(m_ast.get(declaration).node),
                       instance, copy);
    }

    void lower_abstract(const symbol_id_t id, const std::optional<function_id_t> instance) {
        const symbol_t& function = m_analysis.symbol(id);
        begin_function(instance ? *instance : m_functions.at(id));
        if (function.self) {
            const symbol_t& self = m_analysis.symbol(*function.self);
            const type_id_t type = concrete(self.type);
            m_self = add_local(type, "Self", self.by_ref && !is_object(type));
        }

        for (const symbol_id_t parameter_id : function.parameters) {
            const symbol_t& parameter = m_analysis.symbol(parameter_id);
            m_locals[parameter_id] =
                add_local(concrete(parameter.type), parameter.name, parameter.by_ref);
        }

        current().parameter_count = current().locals.size();
        terminate(panic_t{ .message = "an Abstract Method was called", .span = function.span });
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

        // An inherited Method has the type arguments of the instance of its Class.
        for (const symbol_id_t method : method_table(definition)) {
            const type_id_t owner = m_analysis.symbol(method).owner;
            const std::vector<type_id_t> owners = owner_arguments(type, owner);
            if (owners.empty() && !types().get(owner).parameters.empty()) {
                stop_growing(types().name(definition), span_t(), false);
                return;
            }

            info.methods.push_back(function_of(method, owners));
        }

        // A table for each Interface its objects implement.
        for (const type_id_t interface : implemented(type)) {
            mir_implementation_t table{ .interface = interface };
            for (const symbol_id_t required : interface_slots(interface)) {
                table.methods.push_back(table_entry(info, required, type, interface));
            }

            info.interfaces.push_back(std::move(table));
        }

        m_module.classes.push_back(std::move(info));
    }

    /// An array type's tables for the Interfaces its objects implement, made the first time one
    /// goes where an Interface is expected.
    void use_array_tables(const type_id_t type) {
        if (std::ranges::contains(m_module.array_classes, type, &mir_class_t::type)) {
            return;
        }

        mir_class_t info{ .type = type };
        for (const type_id_t interface : array_interfaces(type)) {
            mir_implementation_t table{ .interface = interface };
            for (const symbol_id_t required : interface_slots(interface)) {
                table.methods.push_back(array_table_entry(info, required, type, interface));
            }

            info.interfaces.push_back(std::move(table));
        }

        m_module.array_classes.push_back(std::move(info));
    }

    /// The Interfaces that can be types which an array type implements: Printable, when its
    /// elements are, the ones an Extend of T[] or of the one array type gives, and the ones those
    /// extend. The other built-in ones can only be constraints.
    [[nodiscard]] std::vector<type_id_t> array_interfaces(const type_id_t type) const {
        std::vector<type_id_t> pending{ m_analysis.interfaces.printable };
        const type_id_t element = types().get(type).inner;
        for (const extension_interface_t& extension : m_analysis.extension_interfaces) {
            if (is_array(extension.target) &&
                types().get(types().get(extension.target).inner).kind == type_kind_t::PARAMETER) {
                pending.push_back(
                    types().substitute(extension.interface, extension.parameters, { element }));
            } else if (extension.target == type) {
                pending.push_back(extension.interface);
            }
        }

        std::vector<type_id_t> found;
        while (!pending.empty()) {
            const type_id_t interface = pending.front();
            pending.erase(pending.begin());
            if (interface == type_id_t::INVALID || std::ranges::contains(found, interface) ||
                !fb::implements(m_analysis, types(), type, interface)) {
                continue;
            }

            found.push_back(interface);
            std::ranges::copy(types().get(interface).interfaces, std::back_inserter(pending));
        }

        std::erase_if(found, [this](const type_id_t interface) {
            return m_analysis.symbol(m_declared_types.at(definition_of(interface))).constraint_only;
        });
        return found;
    }

    /// What an array type's table runs for an Interface's member: what an Extend of T[] gives,
    /// or the array's own text, for Printable's ToString.
    function_id_t array_table_entry(const mir_class_t& info, const symbol_id_t required,
                                    const type_id_t type, const type_id_t interface) {
        if (implementation(type, required)) {
            return table_entry(info, required, type, interface);
        }

        // The built-in meaning: Printable's ToString, the only one an array's table holds.
        assert(required == builtin_member(m_analysis.interfaces.printable, "ToString"));
        return generated(generated_kind_t::TEXT, type);
    }

    /// The Interfaces that can be types which a Class's objects implement: the ones it lists, and
    /// that the Classes it extends list, or Extends give them, and the ones those extend.
    [[nodiscard]] std::vector<type_id_t> implemented(const type_id_t type) const {
        // An instance's Interfaces have its type arguments in them.
        std::vector<type_id_t> found;
        for (type_id_t current = type; current != type_id_t::INVALID;
             current = types().get(current).base) {
            std::vector<type_id_t> pending = types().get(current).interfaces;
            while (!pending.empty()) {
                const type_id_t interface = pending.front();
                pending.erase(pending.begin());
                if (std::ranges::contains(found, interface)) {
                    continue;
                }

                found.push_back(interface);
                std::ranges::copy(types().get(interface).interfaces, std::back_inserter(pending));
            }
        }

        // And the ones an Extend gives the instances whose type arguments meet its constraints,
        // this one or one a Class it extends is, with the ones those extend.
        for (type_id_t current = type; current != type_id_t::INVALID;
             current = types().get(current).base) {
            for (const extension_interface_t& extension : m_analysis.extension_interfaces) {
                if (extension.bounds.empty() || extension.target != definition_of(current)) {
                    continue;
                }

                // A copy, since making types can move the entries.
                const std::vector<type_id_t> arguments = types().get(current).arguments;
                const type_id_t interface =
                    types().substitute(extension.interface, extension.parameters, arguments);
                if (std::ranges::contains(found, interface) ||
                    !fb::implements(m_analysis, types(), current, interface)) {
                    continue;
                }

                std::vector<type_id_t> pending{ interface };
                while (!pending.empty()) {
                    const type_id_t next = pending.front();
                    pending.erase(pending.begin());
                    if (!std::ranges::contains(found, next)) {
                        found.push_back(next);
                        std::ranges::copy(types().get(next).interfaces,
                                          std::back_inserter(pending));
                    }
                }
            }
        }

        std::erase_if(found, [this](const type_id_t interface) {
            return m_analysis.symbol(m_declared_types.at(definition_of(interface))).constraint_only;
        });
        return found;
    }

    /// What an Interface's table holds, in order: its Methods, and each Get and Set of its
    /// Properties. Its Statics and Operators aren't called through objects.
    [[nodiscard]] std::vector<symbol_id_t> interface_slots(const type_id_t interface) const {
        std::vector<symbol_id_t> slots;
        for (const symbol_id_t member :
             m_analysis.symbol(m_declared_types.at(definition_of(interface))).members) {
            const symbol_t& declared = m_analysis.symbol(member);
            if (declared.modifiers.is_static) {
                continue;
            }

            // A generic, conditional, or Final default runs as written, with the value for
            // Self, so it isn't in the table, and neither is a generic Method, which has an
            // instance for each set of type arguments.
            if (declared.kind == symbol_kind_t::PROPERTY) {
                slots.push_back(expect(declared.getter));
                if (declared.setter) {
                    slots.push_back(*declared.setter);
                }
            } else if (declared.kind == symbol_kind_t::METHOD && declared.type_parameters.empty() &&
                       !outside_table(declared)) {
                slots.push_back(member);
            }
        }

        return slots;
    }

    /// Whether an Interface's Method is a default outside its table, as analysis has it: a
    /// generic one, a conditional one, whose Where lines constrain the Interface's type
    /// parameters, or a Final one, which a call through the Interface's value runs as written.
    [[nodiscard]] bool outside_table(const symbol_t& member) const {
        if (member.kind != symbol_kind_t::METHOD || !is_interface(member.owner) ||
            member.modifiers.is_abstract || member.modifiers.is_static) {
            return false;
        }

        if (!member.type_parameters.empty() || member.modifiers.is_final) {
            return true;
        }

        const std::vector<type_id_t>& own = types().get(definition_of(member.owner)).parameters;
        return std::ranges::any_of(member.constraints, [&](const bound_t& bound) {
            return bound.field == type_id_t::INVALID && std::ranges::contains(own, bound.parameter);
        });
    }

    /// Whether a Method is a type's copy of a default outside its Interface's table.
    [[nodiscard]] bool copies_outside_table(const symbol_t& method) const {
        return method.default_method && outside_table(m_analysis.symbol(*method.default_method));
    }

    /// An Interface's Method's, Get's, or Set's place in its table.
    [[nodiscard]] std::uint32_t interface_slot(const symbol_id_t member) const {
        const std::vector<symbol_id_t> slots = interface_slots(m_analysis.symbol(member).owner);
        return static_cast<std::uint32_t>(std::ranges::find(slots, member) - slots.begin());
    }

    /// What objects of a Class run for one of an Interface's Methods, Gets, or Sets: what
    /// implements it, from the Class's own Method table when a Class could override it.
    function_id_t table_entry(const mir_class_t& info, const symbol_id_t required,
                              const type_id_t type, const type_id_t interface) {
        const symbol_id_t found = expect(implementation(type, required));
        const std::vector<type_id_t> arguments = implementation_arguments(type, found);
        const function_id_t entry = is_virtual(m_analysis.symbol(found))
                                        ? info.methods[slot_of(found)]
                                        : function_of(found, arguments);
        // A Struct that a Method gives where the Interface's gives an Iterator<T> goes in a box,
        // and a Method that gets an error variable where the Interface's doesn't is given one.
        const symbol_t& member = m_analysis.symbol(required);
        const symbol_t& method = m_analysis.symbol(found);
        std::vector<type_id_t> abstract{ interface };
        std::ranges::copy(types().get(interface).arguments, std::back_inserter(abstract));
        const type_id_t wanted =
            types().substitute(member.type, type_parameters_of(member), abstract);
        const type_id_t given =
            types().substitute(method.type, type_parameters_of(method), arguments);
        const bool boxes = wanted != given && is_struct(given);
        if (!boxes && (takes_error(required) || !takes_error(found))) {
            return entry;
        }

        return table_thunk(entry, found, required, type, arguments, boxes ? given : wanted, wanted);
    }

    /// The function a table runs for a Method that isn't called the way the Interface's is,
    /// declared the first time it's needed, like "Bag.Iterate() as Iterator<Int32>", or
    /// "Steady.Value() without an error variable".
    function_id_t table_thunk(const function_id_t inner, const symbol_id_t method_id,
                              const symbol_id_t required, const type_id_t type,
                              const std::vector<type_id_t>& arguments, const type_id_t given,
                              const type_id_t wanted) {
        const bool passes_error = takes_error(required);
        const auto found = std::ranges::find_if(m_table_thunks, [&](const table_thunk_t& thunk) {
            return thunk.inner == inner && thunk.wanted == wanted &&
                   thunk.passes_error == passes_error;
        });
        if (found != m_table_thunks.end()) {
            return found->function;
        }

        const symbol_t& method = m_analysis.symbol(method_id);
        std::vector<table_thunk_t::parameter_t> parameters{ { .type = type } };
        for (const symbol_id_t parameter : method.parameters) {
            const symbol_t& symbol = m_analysis.symbol(parameter);
            parameters.push_back({
                .type = types().substitute(symbol.type, type_parameters_of(method), arguments),
                .by_ref = symbol.by_ref,
            });
        }

        const std::string& name = m_module.functions[index(inner)].name;
        const auto id = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({
            .name = given == wanted ? std::format("{} without an error variable", name)
                                    : std::format("{} as {}", name, types().qualified_name(wanted)),
            .return_type = wanted,
        });
        m_table_thunks.push_back({
            .function = id,
            .inner = inner,
            .parameters = std::move(parameters),
            .takes_error = takes_error(method_id),
            .passes_error = passes_error,
            .given = given,
            .wanted = wanted,
        });
        return id;
    }

    /// A table thunk's body: it calls the Method, and gives what it gives, a Struct in a box.
    void lower_table_thunk(const table_thunk_t& thunk) {
        begin_function(thunk.function);
        std::vector<call_argument_t> arguments;
        for (std::size_t i = 0; i < thunk.parameters.size(); ++i) {
            const table_thunk_t::parameter_t& parameter = thunk.parameters[i];
            const local_id_t local =
                add_local(parameter.type, i == 0 ? "Self" : std::string_view{}, parameter.by_ref);
            arguments.push_back({ .value = place_t{ local }, .by_ref = parameter.by_ref });
        }

        if (thunk.passes_error) {
            m_error_slot = add_local(m_analysis.nullable_error, "error", true);
        }

        current().parameter_count = current().locals.size();
        std::optional<local_id_t> error;
        if (thunk.takes_error) {
            error = error_variable();
            arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        // The Method's error goes on to the thunk's caller where the Interface's Method throws.
        const auto pass_error = [&] {
            if (thunk.passes_error && error) {
                check_error(*error, {});
            }
        };
        call_t call{ .function = thunk.inner, .arguments = std::move(arguments) };
        if (thunk.given == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
            pass_error();
            finish_statement();
            terminate(return_t{});
            return;
        }

        const operand_t value = temporary(std::move(call), thunk.given);
        pass_error();

        if (has_references(thunk.given)) {
            m_temporaries.push_back(std::get<local_id_t>(std::get<place_t>(value).root));
        }

        const operand_t box = boxed(value, thunk.wanted, {});
        take(box);
        finish_statement();
        terminate(return_t{ .value = box });
    }

    /// The Iterator<T> a Struct that implements it is, in std's Boxed<S, T>, whose object holds
    /// it. A Class's object is one already.
    operand_t boxed(const operand_t& value, const type_id_t wanted, const span_t span) {
        const type_id_t given = type_of(value);
        if (!is_struct(given) || definition_of(wanted) != m_analysis.interfaces.iterator) {
            return convert(value, wanted, span);
        }

        const type_id_t box = types().instance(m_analysis.boxed_iterator,
                                               { given, types().get(wanted).arguments.front() });
        const operand_t object = new_object(box);
        place_t source = std::get<place_t>(object);
        source.steps.push_back({ .field = 0 });
        take(value);
        emit(assign_t{ .target = source, .value = use_t{ .value = value } });
        return convert(object, wanted, span);
    }

    /// The member of a Struct or Class that implements one of an Interface's: from how the type,
    /// or a Class it extends, implements the Interface. A Get or Set is the one of the Property
    /// that implements its Property. nullopt for the built-in meaning, which a built-in type
    /// has, and a Struct or Class without its own == or Hash.
    std::optional<symbol_id_t> implementation(const type_id_t type, const symbol_id_t required) {
        const symbol_t& member = m_analysis.symbol(required);
        const std::vector<symbol_id_t>& members =
            m_analysis.symbol(m_declared_types.at(member.owner)).members;
        if (!is_struct(type) && !is_class(type) && std::ranges::contains(members, required)) {
            return extension_implementation(type, required);
        }

        if (!std::ranges::contains(members, required)) {
            for (const symbol_id_t property : members) {
                const symbol_t& declared = m_analysis.symbol(property);
                if (declared.kind == symbol_kind_t::PROPERTY &&
                    (declared.getter == required || declared.setter == required)) {
                    const symbol_t& found =
                        m_analysis.symbol(expect(implementation(type, property)));
                    return declared.getter == required ? expect(found.getter) : setter_of(found);
                }
            }
        }

        const auto place =
            static_cast<std::size_t>(std::ranges::find(members, required) - members.begin());
        // How the type, or the nearest Class it extends that does, implements it: an Extend of
        // that one instance, like Box<String>, first, then the generic type. A type implements
        // one instance of a generic Interface.
        for (type_id_t current = type; current != type_id_t::INVALID;
             current = types().get(current).base) {
            for (const type_id_t listed : { current, definition_of(current) }) {
                for (const conformance_t& conformance : m_analysis.conformances) {
                    if (conformance.type == listed &&
                        definition_of(conformance.interface) == member.owner) {
                        const symbol_id_t found = conformance.members[place];
                        return found != required ? std::optional(found) : std::nullopt;
                    }
                }
            }
        }

        assert(false); // analysis checked that the type implements the Interface
        return std::nullopt;
    }

    /// What an Extend gives a built-in type, an Enum, or arrays to implement an Interface's
    /// member, or nullopt for the built-in meaning, without one.
    [[nodiscard]] std::optional<symbol_id_t>
    extension_implementation(const type_id_t type, const symbol_id_t required) const {
        const symbol_t& member = m_analysis.symbol(required);
        const std::vector<symbol_id_t>& members =
            m_analysis.symbol(m_declared_types.at(member.owner)).members;
        const auto place =
            static_cast<std::size_t>(std::ranges::find(members, required) - members.begin());
        for (const conformance_t& conformance : m_analysis.conformances) {
            // One array type's, or every T[]'s.
            const bool every =
                is_array(conformance.type) &&
                types().get(types().get(conformance.type).inner).kind == type_kind_t::PARAMETER;
            const bool same = conformance.type == type || (every && is_array(type));
            if (same && definition_of(conformance.interface) == member.owner &&
                place < members.size()) {
                const symbol_id_t found = conformance.members[place];
                return found != required ? std::optional(found) : std::nullopt;
            }
        }

        return std::nullopt;
    }

    /// One of a built-in Interface's members, by name.
    [[nodiscard]] symbol_id_t builtin_member(const type_id_t interface,
                                             const std::string_view name) const {
        const std::vector<symbol_id_t>& members =
            m_analysis.symbol(m_declared_types.at(interface)).members;
        return *std::ranges::find(
            members, name, [this](const symbol_id_t id) { return m_analysis.symbol(id).name; });
    }

    /// The type arguments of what implements an Interface's member on `type`: the instance's,
    /// for a member of a generic Struct or Class itself.
    [[nodiscard]] std::vector<type_id_t> implementation_arguments(const type_id_t type,
                                                                  const symbol_id_t found) const {
        // What an Extend of T[] adds takes the array's element for T.
        const symbol_t& member = m_analysis.symbol(found);
        if (member.extended && is_array(member.owner) &&
            types().get(types().get(member.owner).inner).kind == type_kind_t::PARAMETER) {
            return { types().get(type).inner };
        }

        return owner_arguments(type, member.owner);
    }

    /// Whether every object of `source` is a `target` already: one of a Class it extends, or an
    /// Interface it implements.
    [[nodiscard]] bool upcasts(const type_id_t source, const type_id_t target) const {
        if (!is_interface(target)) {
            return is_class(source) && types().extends(source, target);
        }

        if (is_interface(source)) {
            return types().extends(source, target);
        }

        return std::ranges::contains(implemented(source), target);
    }

    /// Whether a function gets an error variable: it throws, or it overrides a Method that gets
    /// one, or implements an Interface's that does, since every Method in a slot is called the
    /// same way.
    [[nodiscard]] bool takes_error(const symbol_id_t id) const {
        const symbol_t& function = m_analysis.symbol(id);
        // An Async Function's error goes in its Task.
        if (function.modifiers.is_async) {
            return false;
        }

        return !function.throws.empty() || m_takes_error.contains(id) ||
               (function.overrides && takes_error(*function.overrides));
    }

    /// A Class's Method table: the slots of the Class it extends, with its overrides in them,
    /// then a slot for each of its other Methods. Destroy has none, since each Class's runs.
    const std::vector<symbol_id_t>& method_table(const type_id_t type) {
        if (const auto found = m_tables.find(type); found != m_tables.end()) {
            return found->second;
        }

        std::vector<symbol_id_t> table;
        if (const type_id_t base = types().get(type).base; base != type_id_t::INVALID) {
            table = method_table(definition_of(base));
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
        // A generic Method has an instance for each set of type arguments, so it has no slot,
        // and neither does a conditional one, which only some instances have.
        // What an Extend adds isn't in the table either.
        const symbol_t& declared = m_analysis.symbol(member);
        if (declared.extended) {
            return {};
        }

        // A copy of a default outside an Interface's table runs only where it's called, so its
        // body isn't made for each instance, which could wrap Self in instances without end.
        if (declared.kind == symbol_kind_t::METHOD && declared.name != "Destroy" &&
            !declared.modifiers.is_static && declared.type_parameters.empty() &&
            declared.constraints.empty() && !copies_outside_table(declared)) {
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
               method.type_parameters.empty() && method.constraints.empty() && !method.extended &&
               !copies_outside_table(method) &&
               !m_analysis.symbol(m_declared_types.at(method.owner)).modifiers.is_final;
    }

    void begin_function(const function_id_t id) {
        m_function = id;
        m_span.reset();
        m_locals.clear();
        m_aliases.clear();
        m_unchanging.clear();
        m_async_task.reset();
        m_async_kept.clear();
        m_scopes.clear();
        m_temporaries.clear();
        m_loops.clear();
        m_leaving.clear();
        m_raising.clear();
        m_self.reset();
        m_returns_self = false;
        m_handlers.clear();
        m_error_slot.reset();
        m_block = new_block();
        push_scope();
    }

    /// An Async Function is done: what waits for its Task goes on, and it lets go of what it
    /// kept.
    void finish_async(const local_id_t task) {
        runtime_call_t finish{
            .function = runtime_function_t::ASYNC_FINISH,
            .arguments = { place_t{ task } },
        };
        emit(evaluate_t{ std::move(finish) });
        for (const local_id_t kept : std::views::reverse(m_async_kept)) {
            emit(release_t{ .target = kept });
        }
    }

    /// Ends the block that runs off the end of the function, if code reaches there.
    void finish_function() {
        if (m_returns_self) {
            return_self();
        } else if (current().return_type == type_id_t::NONE) {
            release_scopes(0);
            if (const std::optional<local_id_t> task = m_async_task) {
                finish_async(*task);
            }

            terminate(return_t{});
        } else {
            terminate(unreachable_t{}); // analysis checked that every path returns
        }
    }

    void lower_function(const stmt_id_t id, const function_stmt_t& node,
                        const std::optional<function_id_t> instance = std::nullopt,
                        const std::optional<symbol_id_t> copy = std::nullopt) {
        const symbol_id_t symbol_id = expect(m_analysis.stmt(id).symbol);
        const symbol_t& function = m_analysis.symbol(symbol_id);
        begin_function(instance ? *instance : m_functions.at(symbol_id));
        const bool sets_up = function.kind == symbol_kind_t::CONSTRUCTOR && is_class(function.type);
        m_returns_self = function.kind == symbol_kind_t::CONSTRUCTOR && !sets_up;
        // A Method's Self is its first parameter, then come the others, in order, and so is the
        // object a Class's New sets up. Parameters aren't in any scope: they borrow the caller's
        // references, so the function never releases them.
        // An object's Self is never ByRef, even when an Interface's Method runs on one.
        if (function.self && (function.kind == symbol_kind_t::METHOD || sets_up)) {
            const symbol_t& self = m_analysis.symbol(*function.self);
            const type_id_t type = concrete(self.type);
            m_self = add_local(type, "Self", self.by_ref && !is_object(type));
        }

        // An Async Function's Task comes first.
        if (function.modifiers.is_async) {
            const type_id_t task =
                types().task(concrete(function.type), function.throws, m_analysis.nullable_error);
            m_async_task = add_local(task, "task");
        }

        // A parameter's type is checked where it's written, as an instance's sizes are.
        for (const symbol_id_t parameter_id : function.parameters) {
            const symbol_t& parameter = m_analysis.symbol(parameter_id);
            const scoped_value_t<std::optional<span_t>> at(m_span, parameter.span);
            m_locals[parameter_id] =
                add_local(concrete(parameter.type), parameter.name, parameter.by_ref);
        }

        // A function that throws puts its error in the caller's variable, which comes last. A
        // type's copy of a default takes one as the copy does, as what implements another
        // Interface's Method that throws, or what an override of that overrides.
        if (takes_error(copy.value_or(symbol_id))) {
            m_error_slot = add_local(m_analysis.nullable_error, "error", true);
        }

        current().parameter_count = current().locals.size();

        // An Async Function goes on after its caller returns, so it keeps its own references to
        // its Task and parameters until it's done.
        if (m_async_task) {
            for (std::size_t i = 0; i < current().parameter_count; ++i) {
                const auto local = static_cast<local_id_t>(i);
                if (has_references(current().locals[i].type)) {
                    emit(retain_t{ .value = place_t{ local } });
                    m_async_kept.push_back(local);
                }
            }
        }

        // A Struct's New starts Self with the fields' defaults. It isn't in a scope either: New
        // hands it to the caller.
        if (m_returns_self) {
            const type_id_t type = concrete(function.type);
            const local_id_t self = add_local(type, "Self");
            m_self = self;
            emit(assign_t{ .target = self, .value = defaults_of(type) });
        }

        // A Class's New sets up what Self inherits first, unless it calls Super.New.
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
            parameters.empty() ? std::vector<type_id_t>{}
                               : owner_arguments(type_of(object), constructor.owner);
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
        if (takes_error(constructor_id)) {
            error = error_variable();
            call.arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        emit(evaluate_t{ .value = std::move(call) });
        if (error) {
            check_error(*error, span, discard);
        }
    }

    /// Where the Task of an Async Function that throws keeps its error: its last field.
    place_t error_field(const operand_t& task) {
        const type_id_t type = concrete(type_of(task));
        return element_of(as_place(task),
                          static_cast<std::uint32_t>(types().get(type).fields.size() - 1));
    }

    /// After a Task of an Async Function that throws is done, its error, if it has one, is raised
    /// here. Each Wait and Await re-throws it.
    void rethrow(const operand_t& task, const span_t span) {
        if (types().get(concrete(type_of(task))).throws.empty()) {
            return;
        }

        const local_id_t error = error_variable();
        emit(assign_t{ .target = error, .value = use_t{ .value = error_field(task) } });
        emit(retain_t{ .value = place_t{ error } });
        check_error(error, span);
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
                !std::holds_alternative<enum_stmt_t>(node) &&
                !std::holds_alternative<extend_stmt_t>(node) &&
                !std::holds_alternative<import_stmt_t>(node) &&
                !std::holds_alternative<extern_stmt_t>(node)) {
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
        // A ThreadLocal's initializer is a function of its own, which each thread runs.
        if (const auto found = m_globals.find(symbol);
            found != m_globals.end() && !m_module.globals[index(found->second)].is_thread_local) {
            store(found->second, lower_value(*node->value));
            finish_statement();
            emit(ready_t{ .global = found->second, .mark = true });
        }
    }

    /// Before a function's use of a Global that main sets, at `use`: a check that main has, since
    /// an earlier Global's initializer could call the function first. Main's own code uses only
    /// the Globals above, which analysis checks.
    global_id_t ready(const global_id_t global, const expr_id_t use) {
        if (m_module.globals[index(global)].is_thread_local) {
            // One with no initializer to run or references to release is each thread's from the
            // start, so its uses check nothing.
            use_thread_local(global);
            if (m_thread_local_declarations.contains(global) ||
                has_references(m_module.globals[index(global)].type)) {
                emit(ready_t{ .global = global, .span = m_ast.get(use).span });
            }
        } else if (m_function != m_module.entry && m_module.globals[index(global)].set_in_main) {
            emit(ready_t{ .global = global, .span = m_ast.get(use).span });
        }

        return global;
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

    [[nodiscard]] bool is_interface(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::INTERFACE;
    }

    /// A Class or an Interface: a reference to an object.
    [[nodiscard]] bool is_object(const type_id_t type) const {
        return is_class(type) || is_interface(type) || is_task(type) || is_channel(type) ||
               is_scheduler(type);
    }

    [[nodiscard]] bool is_scheduler(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::SCHEDULER;
    }

    [[nodiscard]] bool is_channel(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::CHANNEL;
    }

    /// A Task or Thread, a reference to its object.
    [[nodiscard]] bool is_task(const type_id_t type) const {
        const type_kind_t kind = types().get(type).kind;
        return kind == type_kind_t::TASK || kind == type_kind_t::THREAD;
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

    [[nodiscard]] bool is_any_array(const type_id_t type) const {
        return is_array(type) || is_fixed_array(type);
    }

    [[nodiscard]] bool is_weak(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::WEAK;
    }

    [[nodiscard]] bool is_pointer(const type_id_t type) const {
        return types().get(type).kind == type_kind_t::PTR;
    }

    /// Null, as a constant of a T?.
    static const_value_t null_of(const type_id_t type) {
        return { .type = type, .data = nullptr };
    }

    [[nodiscard]] static bool is_null(const operand_t& value) {
        const auto* constant = std::get_if<const_value_t>(&value);
        return constant != nullptr && std::holds_alternative<std::nullptr_t>(constant->data);
    }

    /// The type inside every T? around it: Int32 for an Int32??.
    [[nodiscard]] type_id_t without_nullables(type_id_t type) const {
        while (types().get(type).kind == type_kind_t::NULLABLE) {
            type = types().get(type).inner;
        }

        return type;
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

        // An array where an Interface is expected, in any T?s, needs its type's tables.
        if (const type_id_t source = without_nullables(from);
            is_array(source) && is_interface(without_nullables(type))) {
            use_array_tables(source);
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
    /// String's is the static "", and a Struct, tuple, T?, or Mutex<T> may hold one. A String? is
    /// Null, which is 0.
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

        return (info.kind == type_kind_t::STRUCT || info.kind == type_kind_t::TUPLE ||
                info.kind == type_kind_t::MUTEX) &&
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

    /// What a variable holds before it's assigned. The zero of a Struct, tuple, Mutex, Class, or
    /// array needs a temporary, and a T?'s is Null.
    operand_t zero_value(const type_id_t type) {
        if (has_fields(type) || is_object(type) || is_array(type) || is_fixed_array(type) ||
            types().get(type).kind == type_kind_t::MUTEX) {
            return temporary(zero_t{ .type = type }, type, has_references(type));
        }

        // A Weak<T>'s refers to nothing, and a Ptr's is Null.
        if (is_nullable(type) || is_weak(type) || is_pointer(type)) {
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

    /// Adds a statement where the current block is. What a call in it borrows is pinned before
    /// it and let go right after it.
    void emit(statement_t statement) {
        std::vector<local_id_t> pins;
        if (auto* assign = std::get_if<assign_t>(&statement)) {
            pins = pin_arguments(assign->value);
            give_place(assign->value);
        } else if (auto* evaluate = std::get_if<evaluate_t>(&statement)) {
            pins = pin_arguments(evaluate->value);
            give_place(evaluate->value);
        }

        add_statement(std::move(statement));
        for (const local_id_t pin : pins) {
            add_statement(release_t{ .target = place_t{ pin } });
        }
    }

    void add_statement(statement_t statement) {
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

    /// Whether evaluating an expression could change a variable, field, or element: anything
    /// but reading literals, variables, fields, and elements, and the built-in operators on them.
    /// A call, an Await, a Property's Get, a type's own Operator, or a conversion could.
    [[nodiscard]] bool may_change(const expr_id_t id) const {
        const expr_node_t& node = m_ast.get(id).node;
        const auto any = [this](const std::vector<expr_id_t>& parts) {
            return std::ranges::any_of(parts,
                                       [this](const expr_id_t part) { return may_change(part); });
        };
        if (const std::optional<symbol_id_t> used = m_analysis.expr(id).symbol) {
            const symbol_t& called = m_analysis.symbol(*used);
            // A Struct's own constructor, which the compiler makes, only puts its arguments
            // together.
            const auto* call = std::get_if<call_expr_t>(&node);
            if (call != nullptr && called.kind == symbol_kind_t::CONSTRUCTOR &&
                !called.declaration && is_struct(called.owner)) {
                return std::ranges::any_of(call->arguments, [this](const argument_t& argument) {
                    return argument.by_ref || may_change(argument.value);
                });
            }

            if (called.kind == symbol_kind_t::PROPERTY || called.kind == symbol_kind_t::METHOD ||
                called.kind == symbol_kind_t::FUNCTION ||
                called.kind == symbol_kind_t::CONSTRUCTOR ||
                called.kind == symbol_kind_t::BUILTIN_FUNCTION) {
                return true;
            }
        }

        if (std::holds_alternative<integer_expr_t>(node) ||
            std::holds_alternative<decimal_expr_t>(node) ||
            std::holds_alternative<character_expr_t>(node) ||
            std::holds_alternative<bool_expr_t>(node) ||
            std::holds_alternative<null_expr_t>(node) ||
            std::holds_alternative<name_expr_t>(node) ||
            std::holds_alternative<self_expr_t>(node) ||
            std::holds_alternative<lambda_expr_t>(node)) {
            return false;
        }

        // A value's text, and ==, can run a type's own ToString or Operator ==.
        if (const auto* text = std::get_if<string_expr_t>(&node)) {
            return std::ranges::any_of(text->parts, [this](const string_part_t& part) {
                const auto* inner = std::get_if<expr_id_t>(&part);
                return inner != nullptr &&
                       (may_change(*inner) || runs_code(m_analysis.expr(*inner).type));
            });
        }

        if (const auto* unary = std::get_if<unary_expr_t>(&node)) {
            return may_change(unary->operand);
        }

        if (const auto* binary = std::get_if<binary_expr_t>(&node)) {
            const bool equality =
                binary->op == binary_op_t::EQUAL || binary->op == binary_op_t::NOT_EQUAL;
            return may_change(binary->left) || may_change(binary->right) ||
                   (equality && (runs_code(m_analysis.expr(binary->left).type) ||
                                 runs_code(m_analysis.expr(binary->right).type)));
        }

        if (const auto* choice = std::get_if<ternary_expr_t>(&node)) {
            return any({ choice->condition, choice->if_true, choice->if_false });
        }

        if (const auto* member = std::get_if<member_expr_t>(&node)) {
            return may_change(member->object);
        }

        if (const auto* index = std::get_if<index_expr_t>(&node)) {
            return may_change(index->object) || any(index->indices);
        }

        if (const auto* test = std::get_if<is_expr_t>(&node)) {
            return may_change(test->value);
        }

        if (const auto* tuple = std::get_if<tuple_expr_t>(&node)) {
            return any(tuple->elements);
        }

        if (const auto* array = std::get_if<array_expr_t>(&node)) {
            return any(array->elements);
        }

        return true;
    }

    /// Whether making a value's text, or comparing two with ==, could run a type's own ToString
    /// or Operator ==: a Struct's, Class's, or Interface's, or one of what a tuple, an array, or
    /// a T? holds. A type parameter could be any of them.
    [[nodiscard]] bool runs_code(const type_id_t type) const {
        const type_t& found = types().get(type);
        switch (found.kind) {
        case type_kind_t::BUILTIN:
        case type_kind_t::ENUM:
            return false;
        case type_kind_t::NULLABLE:
        case type_kind_t::ARRAY:
        case type_kind_t::FIXED_ARRAY:
            return runs_code(found.inner);
        case type_kind_t::TUPLE:
            return std::ranges::any_of(
                found.fields, [this](const field_t& field) { return runs_code(field.type); });
        default:
            return true;
        }
    }

    /// Operands go left to right: one read from a variable, field, or element that a later
    /// operand could change is copied first, with its own references, which go after the
    /// statement.
    operand_t settled(const operand_t& value, const bool changes_later) {
        if (!changes_later || !std::holds_alternative<place_t>(value)) {
            return value;
        }

        // Self's reference to an object never changes, and nor does a copy a closure captured.
        const auto& place = std::get<place_t>(value);
        const auto* local = std::get_if<local_id_t>(&place.root);
        const type_id_t type = type_of(value);
        if (local != nullptr && place.steps.empty() &&
            (std::ranges::contains(m_unchanging, *local) ||
             (*local == m_self && is_object(type)))) {
            return value;
        }

        // A whole temporary is nothing a later operand can reach. One that borrows what a
        // variable refers to, like a cast of it, takes references of its own, so the object
        // outlives a change to the variable.
        if (local != nullptr && place.steps.empty() && !holds_variable(*local)) {
            if (has_references(type) && !std::ranges::contains(m_temporaries, *local)) {
                emit(retain_t{ .value = value });
                m_temporaries.push_back(*local);
            }

            return value;
        }

        const local_id_t copy = add_local(type);
        emit(assign_t{ .target = copy, .value = use_t{ .value = value } });
        if (has_references(type)) {
            emit(retain_t{ .value = place_t{ copy } });
            m_temporaries.push_back(copy);
        }

        return place_t{ copy };
    }

    /// A place whose last reference on the way in, to an object, a T[], or what a Ptr points at,
    /// is copied first, so a later operand that changes a variable or field on the way can't
    /// change which object, array, or memory the place is in. Without one, the place is a
    /// variable's own, which stays where it is. The place itself counts as on the way when it's
    /// what an element is taken from, but not when it's an assignment's target.
    place_t settled_place(const place_t& place, const bool itself) {
        for (std::size_t count = place.steps.size() + (itself ? 1 : 0); count-- > 0;) {
            place_t prefix = place;
            prefix.steps.erase(prefix.steps.begin() + static_cast<std::ptrdiff_t>(count),
                               prefix.steps.end());
            const type_id_t type = type_of(prefix);
            if (is_object(type) || is_array(type) || is_pointer(type)) {
                place_t settled_prefix = std::get<place_t>(settled(prefix, true));
                settled_prefix.steps.insert(
                    settled_prefix.steps.end(),
                    place.steps.begin() + static_cast<std::ptrdiff_t>(count), place.steps.end());
                return settled_prefix;
            }
        }

        return place;
    }

    /// Whether a local is a variable's, Self, or another name for one, which code can change.
    [[nodiscard]] bool holds_variable(const local_id_t local) const {
        if (std::ranges::contains(m_unchanging, local)) {
            return false;
        }

        return m_self == local ||
               std::ranges::any_of(m_locals,
                                   [&](const auto& entry) { return entry.second == local; }) ||
               std::ranges::any_of(m_aliases, [&](const auto& entry) {
                   const auto* root = std::get_if<local_id_t>(&entry.second.root);
                   return root != nullptr && *root == local;
               });
    }

    /// For each of a list of operands, whether one after it could change what it reads.
    [[nodiscard]] std::vector<bool> changes_after(const std::vector<expr_id_t>& operands) const {
        std::vector<bool> later(operands.size(), false);
        bool seen = false;
        for (std::size_t i = operands.size(); i-- > 0;) {
            later[i] = seen;
            seen = seen || may_change(operands[i]);
        }

        return later;
    }

    /// A new temporary holding `value`. An owned one, which holds references of its own, is
    /// released after the statement.
    operand_t temporary(rvalue_t value, const type_id_t type, const bool owned = false) {
        const local_id_t local = add_local(type);
        emit(assign_t{ .target = local, .value = std::move(value) });
        if (owned) {
            m_temporaries.push_back(local);
        }

        return place_t{ local };
    }

    /// Whether a place is reached through a reference: it's a Global's, or a ByRef parameter's or
    /// Self's, which is where the caller keeps it, or a step goes into an object, an array, what
    /// a Ptr points at, or a captured variable's cell. Code that a call runs could change such a
    /// place, which a variable of the caller's own can't be.
    [[nodiscard]] bool through_reference(const place_t& place) {
        const auto* local = std::get_if<local_id_t>(&place.root);
        if (local == nullptr || current().locals[index(*local)].by_ref) {
            return true;
        }

        for (std::size_t count = 0; count < place.steps.size(); ++count) {
            place_t prefix = place;
            prefix.steps.erase(prefix.steps.begin() + static_cast<std::ptrdiff_t>(count),
                               prefix.steps.end());
            const type_id_t type = type_of(prefix);
            if (is_object(type) || is_array(type) || is_pointer(type)) {
                return true;
            }
        }

        return false;
    }

    /// A call's argument as the callee borrows it: what it reads through a reference, or from a
    /// variable the call takes ByRef too, is copied with references of its own, which go right
    /// after the call, so the callee can't free what it uses by changing where it came from, as
    /// `g.Replace()` would by assigning the Global g. Each copy goes in `pins`.
    operand_t pinned(const operand_t& value, const std::vector<local_id_t>& by_ref_roots,
                     std::vector<local_id_t>& pins) {
        const auto* place = std::get_if<place_t>(&value);
        if (place == nullptr || !has_references(type_of(value))) {
            return value;
        }

        const auto* local = std::get_if<local_id_t>(&place->root);
        const bool aliased = local != nullptr && std::ranges::contains(by_ref_roots, *local);
        if (!aliased && !through_reference(*place)) {
            return value;
        }

        const local_id_t copy = add_local(type_of(value));
        add_statement(assign_t{ .target = copy, .value = use_t{ .value = value } });
        add_statement(retain_t{ .value = place_t{ copy } });
        pins.push_back(copy);
        return place_t{ copy };
    }

    /// A place a call takes ByRef, with its last reference on the way in, to an object, a T[], or
    /// what a Ptr points at, pinned, so the callee can't free what holds the variable it's
    /// changing.
    place_t pinned_place(const place_t& place, std::vector<local_id_t>& pins) {
        for (std::size_t count = place.steps.size(); count-- > 0;) {
            place_t prefix = place;
            prefix.steps.erase(prefix.steps.begin() + static_cast<std::ptrdiff_t>(count),
                               prefix.steps.end());
            const type_id_t type = type_of(prefix);
            if (is_object(type) || is_array(type) || is_pointer(type)) {
                place_t pinned_prefix = std::get<place_t>(pinned(prefix, {}, pins));
                pinned_prefix.steps.insert(pinned_prefix.steps.end(),
                                           place.steps.begin() + static_cast<std::ptrdiff_t>(count),
                                           place.steps.end());
                return pinned_prefix;
            }
        }

        return place;
    }

    /// A call is where the code being lowered is, and a call of a @PanicsAtCaller function gives
    /// it that place. So is a runtime call, unless it says where it panics already.
    void give_place(rvalue_t& value) {
        if (auto* runtime = std::get_if<runtime_call_t>(&value); runtime != nullptr) {
            if (!runtime->span) {
                runtime->span = m_span;
            }

            return;
        }

        auto* call = std::get_if<call_t>(&value);
        if (call == nullptr) {
            return;
        }

        if (!call->span) {
            call->span = m_span;
        }

        if (!call->place && m_module.functions[index(call->function)].panics_at_caller) {
            call->place = m_span;
        }
    }

    /// Pins what a call borrows: its object and arguments, the function value it invokes, and
    /// what the runtime calls that run the program's code, like a Scheduler's Tick, are given.
    /// Gives the pins, which go after the call.
    std::vector<local_id_t> pin_arguments(rvalue_t& value) {
        std::vector<local_id_t> pins;
        // The runtime's own functions run none of the program's code.
        auto* call = std::get_if<call_t>(&value);
        const std::optional<mir_extern_t>& external =
            call != nullptr ? m_module.functions[index(call->function)].external : std::nullopt;
        if (call != nullptr && (!external || external->c_abi)) {
            std::vector<local_id_t> roots;
            for (const call_argument_t& argument : call->arguments) {
                const auto* place = std::get_if<place_t>(&argument.value);
                const auto* local =
                    place != nullptr ? std::get_if<local_id_t>(&place->root) : nullptr;
                if (argument.by_ref && local != nullptr) {
                    roots.push_back(*local);
                }
            }

            for (call_argument_t& argument : call->arguments) {
                if (!argument.by_ref) {
                    argument.value = pinned(argument.value, roots, pins);
                } else if (const auto* place = std::get_if<place_t>(&argument.value);
                           place != nullptr && through_reference(*place)) {
                    argument.value = pinned_place(*place, pins);
                }
            }
        } else if (auto* invoke = std::get_if<invoke_t>(&value)) {
            invoke->callee = pinned(invoke->callee, {}, pins);
            for (operand_t& argument : invoke->arguments) {
                argument = pinned(argument, {}, pins);
            }
        } else if (auto* runtime = std::get_if<runtime_call_t>(&value)) {
            static constexpr std::array runs_code{
                runtime_function_t::TASK_WAIT,
                runtime_function_t::PARALLEL_FOR,
                runtime_function_t::SCHEDULER_RUN,
                runtime_function_t::SCHEDULER_TICK,
            };
            if (std::ranges::contains(runs_code, runtime->function)) {
                for (operand_t& argument : runtime->arguments) {
                    argument = pinned(argument, {}, pins);
                }
            }
        }

        return pins;
    }

    /// A value that reads a part of something, like Make()[1]'s element or a Local object's
    /// field, copied into a local of its own, so a Return can still give it once the statement's
    /// temporaries and the scopes' variables, and the objects they alone held, are gone.
    /// Anything else, or anything when nothing goes, as it is.
    operand_t outliving(const operand_t& value) {
        const auto* place = std::get_if<place_t>(&value);
        const bool releases =
            !m_temporaries.empty() ||
            std::ranges::any_of(m_scopes, [](const auto& scope) { return !scope.empty(); });
        if (place == nullptr || place->steps.empty() || !releases) {
            return value;
        }

        return temporary(use_t{ .value = value }, type_of(value));
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

    /// A new value for a variable that already has one; its old references are released. A value
    /// read from a part of something, a Global, or a ByRef parameter, is copied out first, since
    /// releasing the old value can let go of what it was read through, as in
    /// `Root = Root.Children[0]`, or run a Destroy() that changes it.
    void store(const place_t& target, const operand_t& value) {
        if (!has_references(type_of(value))) {
            emit(assign_t{ .target = target, .value = use_t{ .value = value } });
            return;
        }

        operand_t kept = value;
        if (const auto* place = std::get_if<place_t>(&value);
            place != nullptr && (!place->steps.empty() || through_reference(*place))) {
            kept = temporary(use_t{ .value = value }, type_of(value));
        }

        take(kept);
        emit(release_t{ .target = target });
        emit(assign_t{ .target = target, .value = use_t{ .value = kept } });
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

    /// Releases the temporaries made for the statement that just finished.
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
        for (std::size_t i = scope.size(); i-- > 0;) {
            const cleanup_t& cleanup = scope[i];
            if (const auto* local = std::get_if<local_id_t>(&cleanup.action)) {
                emit(release_t{ .target = *local });
                continue;
            }

            if (const auto* unlock = std::get_if<unlock_t>(&cleanup.action)) {
                emit(evaluate_t{ lock_t{ .target = unlock->mutex, .unlock = true } });
                continue;
            }

            // An Await in it that never comes back lets go of only what's still held.
            m_leaving.push_back({ .scope = depth, .position = i, .scopes = m_scopes.size() });
            if (const auto* deferred = std::get_if<stmt_id_t>(&cleanup.action)) {
                lower_stmt(*deferred);
            } else {
                lower_block(*std::get<const block_t*>(cleanup.action));
            }

            m_leaving.pop_back();
        }
    }

    /// Whether a way out that's running a deferred statement or a Finally has let go of what's at
    /// `position` in the scope at `depth`.
    [[nodiscard]] bool left(const std::size_t depth, const std::size_t position) const {
        return std::ranges::any_of(m_leaving, [&](const leaving_t& leaving) {
            return (depth > leaving.scope && depth < leaving.scopes) ||
                   (depth == leaving.scope && position >= leaving.position);
        });
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

    /// A block with a scope of its own, whose variables are released where it ends.
    void lower_block(const block_t& block) {
        push_scope();
        lower_statements(block);
        pop_scope();
    }

    void lower_stmt(const stmt_id_t id) {
        const stmt_t& stmt = m_ast.get(id);
        const scoped_value_t<std::optional<span_t>> at(m_span, stmt.span);
        std::visit([this, id, &stmt](const auto& node) { this->lower_stmt(id, stmt.span, node); },
                   stmt.node);
    }

    /// Declarations, like a Function's or a type's, are lowered on their own, and never come here
    /// as statements.
    template <typename Node>
    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const Node& /*node*/) {
        assert(false);
    }

    /// "Unsafe ... End Unsafe" is a block like any other here.
    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const unsafe_stmt_t& node) {
        lower_block(node.body);
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

        if (node.index_set && m_analysis.expr(*node.index_set).symbol) {
            lower_index_set(node.target, *node.index_set, node.op.has_value());
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
            // For op=, the Get runs between the receiver and the Set, and can change it too.
            const bool changes = node.op.has_value() || may_change(node.value);
            set_property(node.target, *property, changes, [&](const auto& read) {
                if (!node.op) {
                    return lower_value(node.value);
                }

                const operand_t current = read();
                return apply(current, lower_value(node.value));
            });
            return;
        }

        // The target goes first: the object or T[] it's in, and for op=, its value.
        const bool value_changes = may_change(node.value);
        place_t target = lower_place(node.target);
        if (value_changes) {
            target = settled_place(target, false);
        }

        const type_id_t type = type_of(target);
        const operand_t current = node.op ? settled(target, value_changes) : operand_t{ target };
        const operand_t value = lower_value(node.value);
        if (!node.op) {
            store(target, value);
        } else if (info.symbol) {
            store(target, convert(apply(current, value), type, span));
        } else if (is_pointer(type)) {
            check_steps(type, span);
            emit(assign_t{
                .target = target,
                .value = pointer_t{ .op = *node.op, .left = current, .right = value },
            });
        } else if (type == type_id_t::STRING) {
            // += is the only compound assignment a String has.
            const runtime_call_t concat{
                .function = runtime_function_t::CONCAT,
                .arguments = { current, value },
            };
            store(target, temporary(concat, type_id_t::STRING, true));
        } else {
            emit(assign_t{
                .target = target,
                .value = binary_t{ .op = *node.op, .left = current, .right = value, .span = span },
            });
        }

        finish_statement();
    }

    /// "grid[x, y] = value" through Operator []=: its call, the object first, then the indices,
    /// then the value. For "op=" and "++", the value is "grid[x, y] op value", which reads the
    /// element through Operator [] with the same object and indices, lowered once: the object as
    /// the Set takes it, a Struct's variable itself.
    void lower_index_set(const expr_id_t target, const expr_id_t set, const bool reads) {
        const auto& element = std::get<index_expr_t>(m_ast.get(target).node);
        std::vector<expr_id_t> shared;
        if (reads) {
            const symbol_t& setter = m_analysis.symbol(expect(m_analysis.expr(set).symbol));
            const bool in_place = setter.by_ref && !is_object(converted_type(element.object));
            // Each is settled before what comes after it runs, which could change it.
            m_reused[element.object] =
                in_place ? operand_t{ settled_place(lower_place(element.object), false) }
                         : settled(lower_value(element.object), true);
            shared.push_back(element.object);
            for (const expr_id_t position : element.indices) {
                m_reused[position] = settled(lower_value(position), true);
                shared.push_back(position);
            }
        }

        lower_effect(set);
        for (const expr_id_t id : shared) {
            m_reused.erase(id);
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
                set_property(target, *property, false,
                             [&](const auto& /*read*/) -> operand_t { return element; });
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
        if (node.index_set && m_analysis.expr(*node.index_set).symbol) {
            lower_index_set(node.target, *node.index_set, true);
            return;
        }

        const binary_op_t change = node.decrement ? binary_op_t::SUBTRACT : binary_op_t::ADD;
        if (const std::optional<symbol_id_t> property = property_of(node.target)) {
            // The Get runs between the receiver and the Set, and can change it.
            set_property(node.target, *property, true, [&](const auto& read) {
                const operand_t current = read();
                return combine(change, current, one(type_of(current)), span);
            });
            return;
        }

        const place_t target = lower_place(node.target);
        const binary_t changed{
            .op = change,
            .left = target,
            .right = one(type_of(target)),
            .span = span,
        };
        emit(assign_t{ .target = target, .value = changed });
        finish_statement();
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

    /// "Lock m As s": takes the Mutex where it is, and lets go of it on every way out of the
    /// body, whose s is the value inside it. An object holding the Mutex is kept until then, so
    /// the body can't make it go, or reach another object's Mutex through the same name.
    void lower_stmt(const stmt_id_t id, span_t /*span*/, const lock_stmt_t& node) {
        // The Mutex is reached once, and the object it's in, if it's in one, is held for the
        // block, so the block can't free it by changing where it came from.
        push_scope();
        place_t mutex = lower_place(node.mutex);
        for (std::size_t count = mutex.steps.size(); count-- > 0;) {
            place_t prefix = mutex;
            prefix.steps.erase(prefix.steps.begin() + static_cast<std::ptrdiff_t>(count),
                               prefix.steps.end());
            const type_id_t type = type_of(prefix);
            if (!is_object(type)) {
                continue;
            }

            const auto* root = std::get_if<local_id_t>(&prefix.root);
            if (prefix.steps.empty() && root != nullptr && *root == m_self) {
                break; // Self's object outlives the Method
            }

            const local_id_t holder = add_local(type);
            initialize(holder, prefix);
            m_scopes.back().emplace_back(holder);
            place_t held{ holder };
            held.steps.insert(held.steps.end(),
                              mutex.steps.begin() + static_cast<std::ptrdiff_t>(count),
                              mutex.steps.end());
            mutex = std::move(held);
            break;
        }

        finish_statement();
        emit(evaluate_t{ lock_t{ .target = mutex, .span = span_of(node.mutex) } });
        m_scopes.back().emplace_back(unlock_t{ .mutex = mutex });
        place_t value = mutex;
        value.steps.push_back({ .field = 2 });
        m_aliases.insert_or_assign(expect(m_analysis.stmt(id).symbol), std::move(value));
        lower_block(node.body);
        pop_scope();
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

    /// "Parallel For i = a Until b": the body's closure, which the runtime runs for chunks of
    /// the indexes from a up to b, or through b for To, when there are any. They compare as the
    /// loop variable's type, signed or not, and the runtime counts up from the first, wrapping
    /// past an Int64's end, so a range at a type's end has its passes too.
    void lower_parallel_for(const stmt_id_t id, const span_t span, const for_stmt_t& node) {
        const closure_t& closure = m_analysis.closures[expect(m_analysis.stmt(id).closure)];
        const auto wide = [&](const operand_t& value) {
            return temporary(cast_t{ .value = value, .type = type_id_t::INT64, .span = span },
                             type_id_t::INT64);
        };

        const operand_t first = lower_value(node.start);
        const operand_t end = lower_value(node.end);
        const operand_t body = make_closure(closure, span);
        const operand_t any =
            compare(node.inclusive ? binary_op_t::LESS_EQUAL : binary_op_t::LESS, first, end, span);
        const block_id_t run = new_block();
        const block_id_t done = new_block();
        terminate(branch_t{ .condition = any, .if_true = run, .if_false = done });
        start(run);
        operand_t last = end;
        if (!node.inclusive) {
            last = wrapping(binary_op_t::SUBTRACT, end, one(type_of(end)), span);
        }

        runtime_call_t call{
            .function = runtime_function_t::PARALLEL_FOR,
            .arguments = { wide(first), wide(last), body },
        };
        emit(evaluate_t{ std::move(call) });
        jump(done);
        start(done);
        finish_statement();
    }

    void lower_stmt(const stmt_id_t id, const span_t span, const for_stmt_t& node) {
        if (node.parallel) {
            lower_parallel_for(id, span, node);
            return;
        }

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

    /// "For x In items": the array is evaluated once and held for the loop, so the body can't
    /// change or free what it goes over, and a counter goes through its elements, checking a
    /// T[]'s Count before each pass. Each pass's x is a Local of its own, with its own references
    /// to the element's.
    void lower_stmt(const stmt_id_t id, const span_t span, const for_in_stmt_t& node) {
        const stmt_info_t& info = m_analysis.stmt(id);
        if (info.next) {
            lower_iteration(id, span, node);
            return;
        }

        const type_id_t type = converted_type(node.sequence);
        if (is_channel(type)) {
            lower_channel_iteration(id, span, node, type);
            return;
        }

        // A fixed array is held as a copy, unless it's a whole temporary already, which the
        // loop takes over.
        push_scope();
        const operand_t value = lower_value(node.sequence);
        place_t sequence = as_place(value);
        const auto* root = std::get_if<local_id_t>(&sequence.root);
        if (!is_array(type) && root != nullptr && sequence.steps.empty() &&
            !holds_variable(*root)) {
            if (take_temporary(value)) {
                m_scopes.back().emplace_back(*root);
            }
        } else {
            const local_id_t held = add_local(type);
            initialize(held, value);
            if (has_references(type)) {
                m_scopes.back().emplace_back(held);
            }

            sequence = held;
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

    /// "For Field f In T" or "For Method m In T": a copy of the body for each of T's fields or
    /// Methods, in order, with the field's type in place of the loop's type parameter for it.
    /// Continue goes on to the next copy, and Break past the last.
    void lower_stmt(const stmt_id_t id, const span_t span, const for_member_stmt_t& node) {
        const stmt_info_t& info = m_analysis.stmt(id);
        const type_id_t type = concrete(info.type_arguments.at(0));
        const symbol_id_t variable = expect(info.symbol);
        std::vector<block_id_t> breaks;
        const auto copy = [&](reflected_t member, const type_id_t field) {
            member.owner = type;
            m_reflected[variable] = std::move(member);
            const bool typed =
                field != type_id_t::INVALID && info.type_arguments.at(1) != type_id_t::INVALID;
            if (typed) {
                m_parameters.push_back(info.type_arguments.at(1));
                m_arguments.push_back(field);
            }

            m_copies.push_back(m_next_copy++);
            loop_t loop = lower_loop_body(id, node.body);
            m_copies.pop_back();
            if (typed) {
                m_parameters.pop_back();
                m_arguments.pop_back();
            }

            leave(loop.continues);
            join(loop.continues);
            std::ranges::copy(loop.breaks, std::back_inserter(breaks));
        };

        if (node.kind == member_kind_t::METHOD) {
            const type_id_t signature = concrete(info.type_arguments.at(2));
            for (const symbol_id_t method : reflected_methods(type, signature, span.file)) {
                reflected_t member{ .name = std::string(m_analysis.symbol(method).name) };
                member.symbol = method;
                copy(std::move(member), signature);
            }
        } else if (node.kind == member_kind_t::PROPERTY ||
                   node.kind == member_kind_t::STATIC_FIELD) {
            for (const symbol_id_t found : reflected_members(type, node.kind, span.file)) {
                const symbol_t& declared = m_analysis.symbol(found);
                reflected_t member{ .name = std::string(declared.name) };
                member.symbol = found;
                copy(std::move(member),
                     types().substitute(declared.type, type_parameters_of(declared),
                                        owner_arguments(type, declared.owner)));
            }
        } else if (is_struct(type) || is_class(type) || is_tuple(type)) {
            const std::vector<field_t> fields = types().get(type).fields;
            for (std::uint32_t i = 0; i < fields.size(); ++i) {
                // An Atomic or Mutex can't be copied, so the loop leaves it out.
                const type_kind_t kind = types().get(fields[i].type).kind;
                if (kind == type_kind_t::ATOMIC || kind == type_kind_t::MUTEX) {
                    continue;
                }

                reflected_t member{ .name = fields[i].name, .index = i };
                member.symbol = field_symbol(type, i);
                copy(std::move(member), fields[i].type);
            }
        }

        m_reflected.erase(variable);
        leave(breaks);
        join(breaks);
    }

    /// The symbol of a Struct's or Class's field, by its position, which may be in a base Class.
    [[nodiscard]] std::optional<symbol_id_t> field_symbol(const type_id_t type,
                                                          const std::uint32_t index) const {
        for (type_id_t at = type; at != type_id_t::INVALID;
             at = types().get(definition_of(at)).base) {
            const auto found = m_declared_types.find(definition_of(at));
            if (found == m_declared_types.end()) {
                return std::nullopt;
            }

            for (const symbol_id_t member : m_analysis.symbol(found->second).members) {
                const symbol_t& field = m_analysis.symbol(member);
                if (field.kind == symbol_kind_t::FIELD && field.field == index) {
                    return member;
                }
            }
        }

        return std::nullopt;
    }

    /// Whether an object of `type` has a member that Where lines on its Struct's or Class's type
    /// parameters make conditional, like what an Extend with one adds: they hold for the type
    /// arguments of the instance `type` is or extends, as analysis checks at a use.
    [[nodiscard]] bool has_member(const type_id_t type, const symbol_t& member) const {
        if (member.constraints.empty() || member.owner == type_id_t::INVALID) {
            return true;
        }

        const type_id_t owner = definition_of(member.owner);
        const std::vector<type_id_t> parameters = types().get(owner).parameters;
        const std::vector<type_id_t> arguments = owner_arguments(type, owner);
        if (parameters.empty() || arguments.size() != parameters.size()) {
            return true;
        }

        return std::ranges::all_of(member.constraints, [&](const bound_t& bound) {
            const auto at = std::ranges::find(parameters, bound.parameter);
            if (at == parameters.end() || bound.field != type_id_t::INVALID) {
                return true;
            }

            const type_id_t argument = arguments[static_cast<std::size_t>(at - parameters.begin())];
            return fb::implements(m_analysis, types(), argument,
                                  types().substitute(bound.interface, parameters, arguments));
        });
    }

    /// The members of each Struct or Class in a chain from `type` through the Classes it extends,
    /// the base Class's first: its declaration's, with what an Extend of that one instance adds,
    /// like Box<Int>'s, without what an Extend that code in `file` doesn't see adds, as analysis
    /// decides for a use there.
    [[nodiscard]] std::vector<std::vector<symbol_id_t>>
    member_chain(const type_id_t type, const bool own_only, const file_id_t file) const {
        std::vector<std::vector<symbol_id_t>> chain;
        for (type_id_t at = type; at != type_id_t::INVALID; at = types().get(at).base) {
            const auto found = m_declared_types.find(definition_of(at));
            if (found == m_declared_types.end() || !(is_struct(at) || is_class(at))) {
                break;
            }

            std::vector<symbol_id_t> members = m_analysis.symbol(found->second).members;
            if (const auto added = m_analysis.instance_members.find(at);
                added != m_analysis.instance_members.end()) {
                std::ranges::copy(added->second, std::back_inserter(members));
            }

            std::erase_if(members, [&](const symbol_id_t member) {
                return !sees_extension(m_analysis.symbol(member), file);
            });
            chain.push_back(std::move(members));
            if (own_only) {
                break;
            }
        }

        std::ranges::reverse(chain);
        return chain;
    }

    /// Whether code in `file` sees a member: what an Extend adds, the defaults it gives
    /// included, only where the Extend's module is, or is imported.
    [[nodiscard]] bool sees_extension(const symbol_t& member, const file_id_t file) const {
        if (!member.extended) {
            return true;
        }

        const module_table_t& modules = m_ast.modules();
        const std::optional<module_id_t> here = modules.of(file);
        const std::optional<module_id_t> there = modules.of(member.span.file);
        if (!here || !there || *here == *there) {
            return true;
        }

        return std::ranges::any_of(modules.get(*here).imports,
                                   [&](const module_import_t& imported) {
                                       return std::ranges::contains(imported.modules, *there);
                                   });
    }

    /// The Methods a For Method loop goes over: a Struct's or Class's own and inherited ones
    /// that take no arguments, give nothing, and throw nothing, the base Class's first, each in
    /// the place of what it overrides. Destroy and Unsafe Methods aren't among them, nor Async
    /// ones, which give a Task, nor ones with a ByRef parameter, which no Func type has, nor what
    /// the instance's type arguments don't meet the Where lines of, nor what an Extend the loop's
    /// file doesn't see adds.
    [[nodiscard]] std::vector<symbol_id_t>
    reflected_methods(const type_id_t type, const type_id_t signature, const file_id_t file) {
        const auto by_ref = [&](const symbol_id_t parameter) {
            return m_analysis.symbol(parameter).by_ref;
        };
        std::vector<symbol_id_t> methods;
        for (const std::vector<symbol_id_t>& level : member_chain(type, false, file)) {
            for (const symbol_id_t member : level) {
                const symbol_t& method = m_analysis.symbol(member);
                if (method.kind != symbol_kind_t::METHOD || method.modifiers.is_static ||
                    method.modifiers.is_unsafe || method.modifiers.is_async || method.by_ref ||
                    std::ranges::any_of(method.parameters, by_ref) ||
                    !method.type_parameters.empty() || !method.throws.empty() ||
                    method.name == "Destroy" || method.name == "[]" || method.name == "[]=" ||
                    !has_member(type, method) || method_type(type, member) != signature) {
                    continue;
                }

                const auto overridden = method.overrides
                                            ? std::ranges::find(methods, *method.overrides)
                                            : methods.end();
                if (overridden != methods.end()) {
                    *overridden = member;
                } else {
                    methods.push_back(member);
                }
            }
        }

        return methods;
    }

    /// A Method's Func type, as an object of `type` has it: what it takes, besides the object,
    /// and what it gives.
    type_id_t method_type(const type_id_t type, const symbol_id_t id) {
        const symbol_t& method = m_analysis.symbol(id);
        const std::vector<type_id_t> parameters = type_parameters_of(method);
        const std::vector<type_id_t> arguments = owner_arguments(type, method.owner);
        std::vector<type_id_t> takes;
        for (const symbol_id_t parameter : method.parameters) {
            takes.push_back(
                types().substitute(m_analysis.symbol(parameter).type, parameters, arguments));
        }

        return types().function(takes, types().substitute(method.type, parameters, arguments));
    }

    /// The Properties a For Property loop goes over, the inherited ones first, each in the place
    /// of what it overrides, or the Static Fields of a For Static Field loop, the type's own;
    /// what an Extend the loop's file doesn't see adds isn't among them.
    [[nodiscard]] std::vector<symbol_id_t>
    reflected_members(const type_id_t type, const member_kind_t kind, const file_id_t file) const {
        std::vector<symbol_id_t> members;
        for (const std::vector<symbol_id_t>& level :
             member_chain(type, kind == member_kind_t::STATIC_FIELD, file)) {
            for (const symbol_id_t member : level) {
                const symbol_t& found = m_analysis.symbol(member);
                const bool wanted =
                    kind == member_kind_t::PROPERTY
                        ? found.kind == symbol_kind_t::PROPERTY && !found.modifiers.is_static
                        : found.kind == symbol_kind_t::GLOBAL;
                const type_kind_t held = types().get(found.type).kind;
                if (!wanted || held == type_kind_t::ATOMIC || held == type_kind_t::MUTEX ||
                    !has_member(type, found)) {
                    continue;
                }

                // An Override takes the place of what it overrides. Another Property of the
                // name, like what an Extend adds beside one its code can't use, is its own.
                const auto same =
                    found.overrides ? std::ranges::find(members, *found.overrides) : members.end();
                if (same != members.end()) {
                    *same = member;
                } else {
                    members.push_back(member);
                }
            }
        }

        return members;
    }

    /// "For x In items" over an Iterator, or the one an Iterate Method gives: it's held for the
    /// loop, a copy of a Struct, and each pass calls its Next, until it gives Null. Each pass's x
    /// is a Local of its own, which takes the references of what Next gave.
    void lower_iteration(const stmt_id_t id, const span_t span, const for_in_stmt_t& node) {
        const stmt_info_t& info = m_analysis.stmt(id);
        push_scope();
        operand_t value = lower_value(node.sequence);
        if (info.iterate) {
            value =
                call_method(*info.iterate, as_place(value), type_of(value), span_of(node.sequence));
        }

        const type_id_t type = type_of(value);
        const local_id_t held = add_local(type);
        initialize(held, value);
        if (has_references(type)) {
            m_scopes.back().emplace_back(held);
        }

        finish_statement();
        const block_id_t head = new_block();
        jump(head);
        start(head);
        const operand_t next = call_method(expect(info.next), place_t{ held }, type);
        (void)take_temporary(next);
        const operand_t present = has_value(next, span);
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        m_loops.push_back({ .stmt = id, .scope_depth = m_scopes.size() });
        push_scope();
        const auto declare = [&](const symbol_id_t variable, const std::string_view name,
                                 const operand_t& part) {
            const type_id_t variable_type = concrete(m_analysis.symbol(variable).type);
            const local_id_t local = add_local(variable_type, name);
            m_locals[variable] = local;
            emit(assign_t{ .target = local, .value = use_t{ .value = part } });
            if (has_references(variable_type)) {
                m_scopes.back().emplace_back(local);
            }
        };

        // With two names, each takes one part of the element.
        const operand_t element = unwrap(next);
        if (info.index) {
            const place_t pair = as_place(element);
            declare(*info.index, expect(node.index).text, element_of(pair, 0));
            declare(expect(info.symbol), node.value.text, element_of(pair, 1));
        } else {
            declare(expect(info.symbol), node.value.text, element);
        }

        lower_statements(node.body);
        pop_scope();
        loop_t loop = std::move(m_loops.back());
        m_loops.pop_back();
        leave(loop.continues);
        join(loop.continues);
        jump(head);

        const block_id_t exit = join(loop.breaks);
        block_at(test).terminator =
            branch_t{ .condition = present, .if_true = body, .if_false = exit };
        pop_scope();
    }

    /// Calls a Method without arguments on `receiver`, a variable of `type`: an Interface's
    /// through what implements it, or through the table of an Interface's object, and a Class's
    /// through its Method table. A Struct's ByRef Method gets the variable itself.
    operand_t call_method(const symbol_id_t id, const place_t& receiver, const type_id_t type,
                          const span_t span = {}) {
        const symbol_t& method = m_analysis.symbol(id);
        if (is_interface(method.owner) && is_interface(type)) {
            const type_id_t interface = extended_instance(type, method.owner);
            std::vector<type_id_t> arguments{ interface };
            std::ranges::copy(types().get(interface).arguments, std::back_inserter(arguments));
            const type_id_t result =
                types().substitute(method.type, type_parameters_of(method), arguments);
            call_t call{
                .function = function_of(id, arguments),
                .arguments = { { .value = receiver } },
                .slot = interface_slot(id),
                .interface = interface,
            };
            return checked_call(std::move(call), result, takes_error(id), span);
        }

        const symbol_id_t target =
            is_interface(method.owner) ? expect(implementation(type, id)) : id;
        const symbol_t& callee = m_analysis.symbol(target);
        const std::vector<type_id_t> arguments = implementation_arguments(type, target);
        const type_id_t result =
            types().substitute(callee.type, type_parameters_of(callee), arguments);
        call_t call{
            .function = function_of(target, arguments),
            .arguments = { { .value = receiver, .by_ref = callee.by_ref && !is_object(type) } },
        };
        if (is_virtual(callee)) {
            call.slot = slot_of(target);
        }

        return checked_call(std::move(call), result, takes_error(target), span);
    }

    /// A temporary for a call's result. One that `throws` gets a variable for its error, which
    /// is checked right after, and its result is only the caller's once it didn't throw.
    operand_t checked_call(call_t call, const type_id_t result, const bool throws,
                           const span_t span) {
        std::optional<local_id_t> error;
        if (throws) {
            error = error_variable();
            call.arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        const operand_t value = temporary(std::move(call), result);
        if (error) {
            check_error(*error, span);
        }

        if (has_references(result)) {
            m_temporaries.push_back(std::get<local_id_t>(std::get<place_t>(value).root));
        }

        return value;
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

    /// A temporary for a runtime call's result. `owned` when it gives a new reference, like a
    /// String it makes or a Weak's object.
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
                const type_id_t match = concrete(m_analysis.symbol(*variable).type);
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

    /// Break or Continue: leaves the scopes inside the loop, releasing their variables, and jumps
    /// to a target the loop fills in later. The loop is found by its position, since a Finally
    /// on the way out can have loops of its own, which move m_loops.
    void lower_jump(const stmt_id_t id, const bool is_break) {
        const stmt_id_t target = expect(m_analysis.stmt(id).loop);
        const auto at = static_cast<std::size_t>(std::ranges::distance(
            m_loops.begin(),
            std::ranges::find_if(m_loops, [target](const loop_t& l) { return l.stmt == target; })));
        release_scopes(m_loops[at].scope_depth);
        leave(is_break ? m_loops[at].breaks : m_loops[at].continues);
    }

    void lower_stmt(stmt_id_t /*id*/, span_t /*span*/, const return_stmt_t& node) {
        if (m_returns_self) {
            return_self();
            return;
        }

        // An Async Function's result goes in its Task.
        if (const std::optional<local_id_t> task = m_async_task) {
            if (node.value) {
                const operand_t result = lower_value(*node.value);
                take(result);
                emit(assign_t{
                    .target = element_of(place_t{ *task }, 2),
                    .value = use_t{ .value = result },
                });
            }

            finish_statement();
            release_scopes(0);
            finish_async(*task);
            terminate(return_t{});
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

            result = outliving(*result);
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

    /// How a Property's Get or Set is called on a type: which one, with what type arguments, and
    /// whether through the object's Method table or the table its Class has for an Interface.
    struct accessor_call_t {
        symbol_id_t accessor;
        std::vector<type_id_t> arguments;
        /// The Property's type, with the type arguments in it.
        type_id_t type = type_id_t::INVALID;
        std::optional<std::uint32_t> slot;
        type_id_t interface = type_id_t::INVALID;
    };

    /// The Get, or the Set, of a Property used on `self`. An Interface's is the one of the
    /// Property that implements it, or through an Interface's object, the one in the table.
    accessor_call_t accessor_of(const symbol_id_t property_id, const type_id_t self,
                                const bool set) {
        const symbol_t& property = m_analysis.symbol(property_id);
        if (is_interface(property.owner) && !is_interface(self)) {
            return accessor_of(expect(implementation(self, property_id)), self, set);
        }

        if (is_interface(property.owner)) {
            const symbol_id_t accessor = set ? expect(property.setter) : expect(property.getter);
            const type_id_t interface = extended_instance(self, property.owner);
            std::vector<type_id_t> arguments{ interface };
            std::ranges::copy(types().get(interface).arguments, std::back_inserter(arguments));
            return {
                .accessor = accessor,
                .arguments = arguments,
                .type = types().substitute(property.type, type_parameters_of(property), arguments),
                .slot = interface_slot(accessor),
                .interface = interface,
            };
        }

        const symbol_id_t accessor = set ? setter_of(property) : expect(property.getter);
        std::vector<type_id_t> arguments;
        if (!type_parameters_of(property).empty()) {
            arguments = implementation_arguments(self, property_id);
        }

        accessor_call_t call{
            .accessor = accessor,
            .arguments = arguments,
            .type = types().substitute(property.type, type_parameters_of(property), arguments),
        };
        if (is_virtual(m_analysis.symbol(accessor))) {
            call.slot = slot_of(accessor);
        }

        return call;
    }

    /// Reading a Property calls its Get on `receiver`, which a Static Property doesn't have.
    /// `self` is the type it's used on.
    operand_t get_property(const symbol_id_t property, const std::optional<operand_t>& receiver,
                           const type_id_t self) {
        const accessor_call_t access = accessor_of(property, self, false);
        std::vector<call_argument_t> arguments;
        if (receiver && !m_analysis.symbol(access.accessor).modifiers.is_static) {
            arguments.push_back({ .value = *receiver });
        }

        return expect(call_accessor(access, std::move(arguments), access.type));
    }

    /// The type a Static Property is used on: its own, or for an Interface's, the type for Self,
    /// which analysis gave the expression that uses it. Used by name in the Interface, it's
    /// Self.
    type_id_t static_self(const symbol_t& property, const expr_id_t use) {
        // A generic type's is the instance it's used on.
        if (!is_interface(property.owner) && !types().get(property.owner).parameters.empty()) {
            std::vector<type_id_t> arguments;
            for (const type_id_t argument : m_analysis.expr(use).type_arguments) {
                arguments.push_back(concrete(argument));
            }

            return types().instance(property.owner, arguments);
        }

        if (!is_interface(property.owner)) {
            return property.owner;
        }

        const std::vector<type_id_t>& arguments = m_analysis.expr(use).type_arguments;
        return concrete(arguments.empty() ? types().get(property.owner).inner : arguments.front());
    }

    /// Calls a Property's Get or Set, which returns `type`, or nothing.
    std::optional<operand_t> call_accessor(const accessor_call_t& access,
                                           std::vector<call_argument_t> arguments,
                                           const type_id_t type) {
        call_t call{
            .function = function_of(access.accessor, access.arguments),
            .arguments = std::move(arguments),
            .slot = access.slot,
            .interface = access.interface,
        };
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

    /// "x.P = value", "x.P += value", or "x.P++": the receiver first, then `value`, with a
    /// function that reads the Property, then the Set. A Struct's Set takes the variable itself,
    /// like a ByRef Method, a Property used by name is Self's, and a Static one has none. The
    /// receiver is settled when the value `changes` what it reads.
    template <typename Value>
    void set_property(const expr_id_t target, const symbol_id_t property_id, const bool changes,
                      const Value& value) {
        const symbol_t& property = m_analysis.symbol(property_id);
        const auto* member = std::get_if<member_expr_t>(&m_ast.get(target).node);
        std::optional<call_argument_t> receiver;
        type_id_t self = type_id_t::INVALID;
        if (property.modifiers.is_static) {
            self = static_self(property, target);
        } else if (member == nullptr) {
            self = type_of(place_t{ expect(m_self) });
            receiver = { .value = place_t{ expect(m_self) }, .by_ref = !is_object(self) };
        } else {
            self = converted_type(member->object);
            if (is_object(self)) {
                receiver = { .value = settled(lower_value(member->object), changes) };
            } else {
                const place_t place = lower_place(member->object);
                receiver = {
                    .value = changes ? settled_place(place, false) : place,
                    .by_ref = true,
                };
            }
        }

        const std::optional<operand_t> on =
            receiver ? std::optional(receiver->value) : std::nullopt;
        const auto read = [&] { return get_property(property_id, on, self); };
        const accessor_call_t access = accessor_of(property_id, self, true);
        const operand_t assigned = convert(value(read), access.type, span_of(target));
        std::vector<call_argument_t> arguments;
        if (receiver) {
            arguments.push_back(*receiver);
        }

        arguments.push_back({ .value = assigned });
        call_accessor(access, std::move(arguments), type_id_t::NONE);
        finish_statement();
    }

    /// `left op right`, as "x op= y" computes it: Strings add through the runtime, and a Ptr
    /// steps by its T's size.
    operand_t combine(const binary_op_t op, const operand_t& left, const operand_t& right,
                      const span_t span) {
        if (type_of(left) == type_id_t::STRING) {
            return runtime_call(runtime_function_t::CONCAT, { left, right }, type_id_t::STRING,
                                true);
        }

        if (is_pointer(type_of(left))) {
            check_steps(type_of(left), span);
            return temporary(pointer_t{ .op = op, .left = left, .right = right }, type_of(left));
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
        // The error is from here, unless an earlier Throw threw it.
        emit(evaluate_t{
            .value =
                runtime_call_t{
                    .function = runtime_function_t::ERROR_THROWN,
                    .arguments = { value },
                    .span = span,
                },
        });
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

        // The error is held while the Defers and Finallys on its way run, so one that Awaits and
        // is never resumed lets go of it too.
        m_raising.push_back(error);
        if (!m_handlers.empty()) {
            release_scopes(m_handlers.back().scope_depth);
            emit(assign_t{
                .target = m_handlers.back().error,
                .value = use_t{ .value = place_t{ error } },
            });
            leave(m_handlers.back().exits);
        } else if (const std::optional<local_id_t> task = m_async_task) {
            // An Async Function's error goes in its Task, which Wait and Await re-throw.
            release_scopes(0);
            emit(assign_t{
                .target = error_field(*task),
                .value = use_t{ .value = place_t{ error } },
            });
            finish_async(*task);
            terminate(return_t{});
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
            // top-level code, or a ThreadLocal's initializer, lets out ends the program, after its
            // Defers and Finallys.
            if (m_function == m_module.entry || m_ending_functions.contains(m_function)) {
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

        m_raising.pop_back();
        m_temporaries = temporaries;
    }

    /// What a function returns when it throws, which its caller never reads: its type's zero.
    operand_t placeholder(const type_id_t type) {
        if (has_fields(type) || is_object(type) || is_array(type) || is_fixed_array(type) ||
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
        if (const auto found = m_aliases.find(symbol); found != m_aliases.end()) {
            return found->second;
        }

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

        if (const auto found = m_globals.find(symbol); found != m_globals.end()) {
            return found->second;
        }

        return declare_global(symbol); // a Const known at compile time, whose place code needs
    }

    /// A variable's place. When analysis narrowed its T? to a T, it's the T inside. A Property
    /// used by name is Self's, read into a temporary.
    place_t variable_place(const expr_id_t id) {
        const symbol_id_t symbol = expect(m_analysis.expr(id).symbol);
        if (m_analysis.symbol(symbol).kind == symbol_kind_t::GLOBAL && !m_locals.contains(symbol)) {
            return global_of(symbol, id);
        }

        if (const symbol_t& property = m_analysis.symbol(symbol);
            property.kind == symbol_kind_t::PROPERTY) {
            if (property.modifiers.is_static) {
                return as_place(get_property(symbol, std::nullopt, static_self(property, id)));
            }

            const place_t self{ expect(m_self) };
            return as_place(get_property(symbol, self, type_of(self)));
        }

        place_t place = place_of(symbol);
        // A top-level Const that main sets.
        if (const auto* global = std::get_if<global_id_t>(&place.root)) {
            ready(*global, id);
        }

        // Narrowed, it's the T inside each T? that analysis took off.
        type_id_t held = concrete(m_analysis.symbol(symbol).type);
        const type_id_t read = expr_type(id);
        while (is_nullable(held) && held != read) {
            place.steps.push_back({ .field = 0 });
            held = types().get(held).inner;
        }

        return place;
    }

    /// Where a variable, Self, or a field inside one lives, without copying it. Anything else,
    /// like a call, gives a temporary.
    place_t lower_place(const expr_id_t id) {
        if (const auto reused = m_reused.find(id); reused != m_reused.end()) {
            return as_place(reused->second);
        }

        const scoped_value_t<std::optional<span_t>> at(m_span, span_of(id));
        const expr_node_t& node = m_ast.get(id).node;
        // "game.score" is the variable, as its name alone would be.
        if (m_analysis.expr(id).qualified) {
            return variable_place(id);
        }

        // A member without a symbol, like a String's Bytes, is a value of its own.
        if (const auto* member = std::get_if<member_expr_t>(&node);
            member != nullptr && !member->null_safe && m_analysis.expr(id).symbol) {
            // A Static Field is a global, and a Property's value comes from its Get.
            const symbol_t& found = m_analysis.symbol(expect(m_analysis.expr(id).symbol));
            if (found.kind == symbol_kind_t::GLOBAL) {
                return global_of(expect(m_analysis.expr(id).symbol), id);
            }

            if (found.kind == symbol_kind_t::MEMBER_VARIABLE) {
                return as_place(lower_value(id));
            }

            if (found.kind == symbol_kind_t::PROPERTY && found.modifiers.is_static) {
                const symbol_id_t property = expect(m_analysis.expr(id).symbol);
                return as_place(get_property(property, std::nullopt, static_self(found, id)));
            }

            if (found.kind == symbol_kind_t::PROPERTY) {
                const operand_t object = lower_value(member->object);
                return as_place(
                    get_property(expect(m_analysis.expr(id).symbol), object, type_of(object)));
            }

            // A Ptr's Value is a step through it, which panics where the Ptr is Null.
            place_t place = lower_place(member->object);
            place.steps.push_back({ .field = found.field, .span = span_of(id) });
            return place;
        }

        if (std::holds_alternative<self_expr_t>(node)) {
            return expect(m_self);
        }

        if (std::holds_alternative<name_expr_t>(node)) {
            return variable_place(id);
        }

        // Each index goes in a local of its own, so the place keeps the value the index had.
        // Through an Operator [], the element is what its call gives.
        if (const auto* element = std::get_if<index_expr_t>(&node);
            element != nullptr && m_analysis.expr(element->call).symbol) {
            return as_place(lower_value(element->call));
        }

        if (const auto* element = std::get_if<index_expr_t>(&node)) {
            place_t place = lower_place(element->object);
            // Operands go left to right: the object or T[] the element is in is read before an
            // index that could change which one it is.
            if (std::ranges::any_of(element->indices, [this](const expr_id_t position) {
                    return may_change(position);
                })) {
                place = settled_place(place, true);
            }

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
        if (const auto reused = m_reused.find(id); reused != m_reused.end()) {
            return reused->second;
        }

        const scoped_value_t<std::optional<span_t>> at(m_span, span_of(id));
        const expr_info_t& info = m_analysis.expr(id);
        if (info.value) {
            return concrete(*info.value);
        }

        // TypeName<T>() names the instance's type.
        if (info.type_name) {
            return make_string(std::string(types().name(concrete(*info.type_name))));
        }

        if (info.size_of) {
            return size_of(concrete(*info.size_of));
        }

        const operand_t value =
            std::visit([this, id](const auto& node) { return this->lower_expr(id, node); },
                       m_ast.get(id).node);
        return convert(value, converted_type(id), span_of(id));
    }

    /// Reports, once, a step of a Ptr at a T that the instance makes more bytes than an Int64
    /// counts, as analysis does for a T without type parameters.
    void check_steps(const type_id_t pointer, const span_t span) {
        const type_id_t element = types().get(pointer).inner;
        if (m_module.error || element == type_id_t::NONE ||
            c_layout(types(), element, build_c_integers()).size <=
                static_cast<std::uint64_t>(INT64_MAX)) {
            return;
        }

        m_module.error = mir_error_t{
            .message = std::format("{} takes more bytes than an Int64 counts here, so a Ptr at it "
                                   "doesn't step",
                                   types().name(element)),
            .span = span,
        };
    }

    /// SizeOf<T>() of an instance's T, which CData keeps to C's, as the target programs are
    /// built for lays it out, as analysis measures one without type parameters. One that takes
    /// more bytes than an Int64 counts is reported, once.
    const_value_t size_of(const type_id_t type) {
        const std::uint64_t size = c_layout(types(), type, build_c_integers()).size;
        if (size > static_cast<std::uint64_t>(INT64_MAX) && !m_module.error) {
            m_module.error = mir_error_t{
                .message = std::format("SizeOf measures {} here, which takes more bytes than an "
                                       "Int64 counts",
                                       types().name(type)),
                .span = m_span.value_or(span_t{}),
            };
        }

        return { .type = type_id_t::INT64, .data = static_cast<std::int64_t>(size) };
    }

    /// The expressions without a lowering of their own always have a value analysis knew, like a
    /// literal's, or an overload that lowers them, so none comes here.
    template <typename Node>
    operand_t lower_expr(expr_id_t /*id*/, const Node& /*node*/) {
        assert(false);
        return const_value_t{};
    }

    /// A variable, or a Function's name alone, which is a function value. A Const type
    /// parameter is the value its instance has.
    operand_t lower_expr(const expr_id_t id, const name_expr_t& node) {
        if (const type_id_t parameter = m_analysis.expr(id).parameter;
            parameter != type_id_t::INVALID) {
            const type_t& value = types().get(concrete(parameter));
            return *fold_cast({ .type = type_id_t::INT64, .data = value.number }, value.inner);
        }

        if (const std::optional<std::size_t> closure = m_analysis.expr(id).closure) {
            return make_closure(m_analysis.closures[*closure], span_of(id));
        }

        // A Function's name where a Callback is expected is its code's address, by the C ABI.
        if (m_analysis.expr(id).callback) {
            const type_id_t type = concrete(expr_type(id));
            const symbol_id_t symbol = expect(m_analysis.expr(id).symbol);
            const callback_t code{
                .function = function_of(symbol),
                .type = type,
                .span = m_analysis.symbol(symbol).span,
            };
            return temporary(code, type);
        }

        // Count or Capacity alone, in an Extend of T[], is Self's, which a lambda has captured.
        if (m_analysis.expr(id).count) {
            const symbol_id_t self = expect(m_analysis.expr(id).symbol);
            return array_size(
                place_t{ m_locals.contains(self) ? m_locals.at(self) : expect(m_self) }, node.name);
        }

        return variable_place(id);
    }

    /// A field, an array's Count or Capacity, or a String's Chars or Bytes. A For Field's
    /// "f.Name" and "f.Index" are constants in each copy of its body.
    operand_t lower_expr(const expr_id_t id, const member_expr_t& node) {
        // A name after a module's qualifier is what the name alone would be.
        if (m_analysis.expr(id).qualified) {
            return lower_expr(id, name_expr_t{});
        }

        if (const std::optional<symbol_id_t> variable = m_analysis.expr(id).symbol;
            variable && m_analysis.symbol(*variable).kind == symbol_kind_t::MEMBER_VARIABLE) {
            const reflected_t& member = m_reflected.at(*variable);
            if (node.member.text == "Index") {
                const auto position = static_cast<std::int64_t>(member.index);
                return const_value_t{ .type = type_id_t::INT32, .data = position };
            }

            return const_value_t{ .type = type_id_t::STRING, .data = member.name };
        }

        // An array's Count or Capacity, unless it's a Property an Extend gives it.
        const std::optional<symbol_id_t> found = m_analysis.expr(id).symbol;
        const bool property = found && m_analysis.symbol(*found).kind == symbol_kind_t::PROPERTY;
        if (!node.null_safe && is_array(converted_type(node.object)) && !property) {
            return array_size(lower_value(node.object), node.member.text);
        }

        // A fixed array's Count, when its size is a Const type parameter's, is the instance's.
        if (!node.null_safe && is_fixed_array(converted_type(node.object)) && !property) {
            const auto count =
                static_cast<std::int64_t>(types().element_count(converted_type(node.object)));
            return const_value_t{ .type = type_id_t::INT32, .data = count };
        }

        if (!node.null_safe && converted_type(node.object) == type_id_t::STRING && !property) {
            return string_parts(lower_value(node.object), node.member.text, expr_type(id));
        }

        if (!node.null_safe) {
            return lower_place(id);
        }

        const std::optional<symbol_id_t> field = m_analysis.expr(id).symbol;
        return expect(lower_safe(id, node.object, [&](const operand_t& object) {
            const type_id_t type = type_of(object);
            if (is_array(type) && !property) {
                return std::optional<operand_t>(array_size(object, node.member.text));
            }

            if (is_fixed_array(type) && !property) {
                const auto count = static_cast<std::int64_t>(types().element_count(type));
                return std::optional<operand_t>(
                    const_value_t{ .type = type_id_t::INT32, .data = count });
            }

            if (type == type_id_t::STRING && !property) {
                return std::optional<operand_t>(
                    string_parts(object, node.member.text, types().get(expr_type(id)).inner));
            }

            if (m_analysis.symbol(expect(field)).kind == symbol_kind_t::PROPERTY) {
                return std::optional<operand_t>(
                    get_property(expect(field), object, type_of(object)));
            }

            place_t place = as_place(object);
            place.steps.push_back({ .field = m_analysis.symbol(expect(field)).field });
            return std::optional<operand_t>(place);
        }));
    }

    /// A String's code points, for "s.Chars", or its UTF-8 bytes, for "s.Bytes", in a new array
    /// of `type`.
    operand_t string_parts(const operand_t& text, const std::string_view member,
                           const type_id_t type) {
        const operand_t made = temporary(array_t{ .type = type }, type, true);
        runtime_call_t call{
            .function = member == "Chars" ? runtime_function_t::STRING_CHARS
                                          : runtime_function_t::STRING_BYTES,
            .arguments = { text, made },
        };
        emit(evaluate_t{ .value = std::move(call) });
        return made;
    }

    /// An element, or for a type with an Operator [], what its call gives.
    operand_t lower_expr(const expr_id_t id, const index_expr_t& node) {
        if (m_analysis.expr(node.call).symbol) {
            return lower_value(node.call);
        }

        return lower_place(id);
    }

    /// "start To end Step step": a Range<T> of the values "For i = start To end Step step"
    /// counts: its first value, its step, how many steps come after that value, and whether it
    /// has values at all; an empty range is a Range's zero. The start, end, and step are
    /// evaluated once, in that order, and a Step only known when the program runs is checked
    /// then, as a For loop's is.
    operand_t lower_expr(const expr_id_t id, const range_expr_t& node) {
        const span_t span = span_of(id);
        const type_id_t type = concrete(expr_type(id));
        const type_id_t element = types().get(type).arguments.front();
        const operand_t first = stable(lower_value(node.start));
        const operand_t end = stable(lower_value(node.end));
        const operand_t step = node.step ? stable(lower_value(*node.step)) : one(element);

        std::optional<bool> up;
        operand_t up_at_run_time;
        if (const auto* constant = std::get_if<const_value_t>(&step)) {
            up = !is_negative(*constant);
        } else {
            const operand_t is_zero = compare(binary_op_t::EQUAL, step, zero(element), span);
            const block_id_t fail = new_block();
            const block_id_t ok = new_block();
            terminate(branch_t{ .condition = is_zero, .if_true = fail, .if_false = ok });
            start(fail);
            terminate(panic_t{ .message = "Step can't be 0", .span = span_of(expect(node.step)) });
            start(ok);
            if (is_signed(element)) {
                up_at_run_time = compare(binary_op_t::GREATER, step, zero(element), span);
            } else {
                up = true;
            }
        }

        const auto by_direction = [&](const auto& make) {
            if (up) {
                return make(*up);
            }

            const operand_t if_up = make(true);
            const operand_t if_down = make(false);
            return choose(up_at_run_time, if_up, if_down, span);
        };

        // Whether the first value is in the range, as a For loop asks before its first pass.
        const operand_t more = by_direction([&](const bool going_up) {
            binary_op_t op = node.inclusive ? binary_op_t::LESS_EQUAL : binary_op_t::LESS;
            if (!going_up) {
                op = node.inclusive ? binary_op_t::GREATER_EQUAL : binary_op_t::GREATER;
            }

            return compare(op, first, end, span);
        });

        const local_id_t made = add_local(type);
        const block_id_t filled = new_block();
        const block_id_t empty = new_block();
        const block_id_t joined = new_block();
        terminate(branch_t{ .condition = more, .if_true = filled, .if_false = empty });
        start(empty);
        emit(assign_t{ .target = made, .value = zero_t{ .type = type } });
        jump(joined);

        // How many steps come after the first value: the distance to the end, counted without
        // overflowing, over the step's size.
        start(filled);
        const auto steps_left = [&](const bool going_up) {
            const type_id_t distance_type = unsigned_of(element);
            const operand_t distance = going_up ? wrapping(binary_op_t::SUBTRACT, end, first, span)
                                                : wrapping(binary_op_t::SUBTRACT, first, end, span);
            operand_t steps = cast(distance, distance_type, span);
            if (!node.inclusive) {
                steps = wrapping(binary_op_t::SUBTRACT, steps, one(distance_type), span);
            }

            const operand_t size =
                cast(going_up ? step : wrapping(binary_op_t::SUBTRACT, zero(element), step, span),
                     distance_type, span);
            const operand_t count = temporary(
                binary_t{ .op = binary_op_t::DIVIDE, .left = steps, .right = size, .span = span },
                distance_type);
            return cast(count, type_id_t::UINT64, span);
        };

        // With the direction only known when the program runs, a branch picks the count.
        operand_t left;
        if (up) {
            left = steps_left(*up);
        } else {
            const local_id_t chosen = add_local(type_id_t::UINT64);
            const block_id_t counting_up = new_block();
            const block_id_t counting_down = new_block();
            const block_id_t counted = new_block();
            terminate(branch_t{
                .condition = up_at_run_time,
                .if_true = counting_up,
                .if_false = counting_down,
            });
            start(counting_up);
            emit(assign_t{ .target = chosen, .value = use_t{ .value = steps_left(true) } });
            jump(counted);
            start(counting_down);
            emit(assign_t{ .target = chosen, .value = use_t{ .value = steps_left(false) } });
            jump(counted);
            start(counted);
            left = place_t{ chosen };
        }

        emit(assign_t{
            .target = made,
            .value = aggregate_t{ .type = type, .fields = { first, step, left, make_bool(true) } },
        });
        jump(joined);
        start(joined);
        return place_t{ made };
    }

    /// "a[from..to]": a new T[] of the elements from `from` up to `to`, each with its own
    /// references. The bounds are checked first; out of range, the program panics with them.
    operand_t lower_expr(const expr_id_t id, const slice_expr_t& node) {
        const type_id_t type = expr_type(id);
        const span_t span = span_of(id);
        const bool from_changes = node.from && may_change(*node.from);
        const bool to_changes = node.to && may_change(*node.to);
        const operand_t object = settled(lower_value(node.object), from_changes || to_changes);
        const place_t source = as_place(object);
        // The bounds and count are compared as Int64s. A bound known at compile time is one
        // already.
        const auto wide = [&](const operand_t& value) -> operand_t {
            const auto* constant = std::get_if<const_value_t>(&value);
            if (constant != nullptr && std::holds_alternative<std::int64_t>(constant->data)) {
                return const_value_t{ .type = type_id_t::INT64, .data = constant->data };
            }

            return type_of(value) == type_id_t::INT64 ? value : cast(value, type_id_t::INT64, span);
        };

        // A panic shows the bounds as they are, which a UInt64's past an Int64's range isn't once
        // it's one.
        const std::optional<operand_t> from_written =
            node.from ? std::optional(settled(lower_value(*node.from), to_changes)) : std::nullopt;
        const operand_t from = from_written ? wide(*from_written) : zero(type_id_t::INT64);
        const std::optional<operand_t> to_written =
            node.to ? std::optional(lower_value(*node.to)) : std::nullopt;
        const std::optional<operand_t> written =
            to_written ? std::optional(wide(*to_written)) : std::nullopt;
        const type_id_t source_type = type_of(object);
        const auto fixed = static_cast<std::int64_t>(
            is_array(source_type) ? 0 : types().element_count(source_type));
        const operand_t count = is_array(source_type)
                                    ? wide(count_of(source))
                                    : const_value_t{ .type = type_id_t::INT64, .data = fixed };
        const operand_t to = written.value_or(count);

        // 0 <= from <= to <= count.
        const operand_t starts =
            compare(binary_op_t::GREATER_EQUAL, from, zero(type_id_t::INT64), span);
        const operand_t ordered = compare(binary_op_t::LESS_EQUAL, from, to, span);
        const operand_t ends = compare(binary_op_t::LESS_EQUAL, to, count, span);
        const operand_t fits =
            compare(binary_op_t::AND, compare(binary_op_t::AND, starts, ordered, span), ends, span);
        const block_id_t fail = new_block();
        const block_id_t ok = new_block();
        terminate(branch_t{ .condition = fits, .if_true = ok, .if_false = fail });
        start(fail);
        const std::size_t mark = m_temporaries.size();
        const auto text = [&](const operand_t& number) {
            return runtime_call(runtime_function_t::TO_TEXT, { number }, type_id_t::STRING, true);
        };
        const operand_t first = text(from_written.value_or(from));
        const operand_t last = text(to_written.value_or(to));
        std::vector<operand_t> parts{
            make_string("slice "),
            first,
            make_string(".."),
            last,
            make_string(" is out of range for "),
            text(count),
            make_string(" elements"),
        };
        const operand_t message =
            runtime_call(runtime_function_t::JOIN, std::move(parts), type_id_t::STRING, true);
        terminate(panic_t{ .span = span, .text = message });
        release_temporaries_from(mark); // the program stops, so they're only dropped

        start(ok);
        const operand_t made = temporary(array_t{ .type = type }, type, true);
        const local_id_t at = add_local(type_id_t::INT64);
        emit(assign_t{ .target = at, .value = use_t{ .value = from } });
        const block_id_t head = new_block();
        jump(head);
        start(head);
        const operand_t more = compare(binary_op_t::LESS, at, to, span);
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        place_t element = source;
        element.steps.push_back({ .indices = { at }, .span = span });
        take(element);
        runtime_call_t add{
            .function = runtime_function_t::ARRAY_ADD,
            .arguments = { made, element },
        };
        emit(evaluate_t{ .value = std::move(add) });
        const binary_t next{
            .op = binary_op_t::ADD,
            .left = at,
            .right = one(type_id_t::INT64),
            .span = span,
            .wrapping = true,
        };
        emit(assign_t{ .target = at, .value = next });
        jump(head);

        const block_id_t exit = new_block();
        block_at(test).terminator =
            branch_t{ .condition = more, .if_true = body, .if_false = exit };
        start(exit);
        return made;
    }

    /// "[a, b, c]": a new T[] that holds the elements' references, or a fixed array's value.
    operand_t lower_expr(const expr_id_t id, const array_expr_t& node) {
        return array_of(node.elements, expr_type(id));
    }

    /// "{key: value}": a new Map, which its New() sets up, as Map() would, with each entry passed
    /// to its Add, in order.
    operand_t lower_expr(const expr_id_t id, const map_expr_t& node) {
        const type_id_t type = expr_type(id);
        const operand_t map = new_object(type);
        const symbol_id_t declared = m_declared_types.at(definition_of(type));
        for (const symbol_id_t member : m_analysis.symbol(declared).members) {
            const symbol_t& found = m_analysis.symbol(member);
            if (found.kind == symbol_kind_t::CONSTRUCTOR && found.parameters.empty()) {
                set_up(member, as_place(map), {}, span_of(id));
                break;
            }
        }

        const symbol_id_t add = expect(m_analysis.expr(id).symbol);
        const function_id_t function = function_of(add, implementation_arguments(type, add));
        for (const map_entry_t& entry : node.entries) {
            call_t call{
                .function = function,
                .arguments = {
                    { .value = map },
                    { .value = settled(lower_value(entry.key), may_change(entry.value)) },
                    { .value = lower_value(entry.value) },
                },
            };
            if (is_virtual(m_analysis.symbol(add))) {
                call.slot = slot_of(add);
            }

            // Add can get an error variable, from an Override that throws, though std's never
            // sets it.
            give_error_variable(call, add);
            emit(evaluate_t{ .value = std::move(call) });
        }

        return map;
    }

    /// A new array of `type` made of the elements, holding their references, or a fixed array's
    /// value made of them.
    operand_t array_of(const std::vector<expr_id_t>& elements, const type_id_t type) {
        array_t value{ .type = type };
        const std::vector<bool> later = changes_after(elements);
        for (std::size_t i = 0; i < elements.size(); ++i) {
            value.elements.push_back(settled(lower_value(elements[i]), later[i]));
        }

        for (const operand_t& element : value.elements) {
            take(element);
        }

        return temporary(std::move(value), type, has_references(type));
    }

    /// A new String[], `type`, of each element's text, as a string's "{x}" shows it, for a
    /// parameter written "values: Printable...".
    operand_t texts_of(const std::vector<expr_id_t>& elements, const type_id_t type) {
        array_t value{ .type = type };
        const std::vector<bool> later = changes_after(elements);
        for (std::size_t i = 0; i < elements.size(); ++i) {
            std::vector<operand_t> parts;
            add_text(parts, settled(lower_value(elements[i]), later[i]),
                     converted_type(elements[i]));
            value.elements.push_back(parts.size() == 1
                                         ? parts.front()
                                         : runtime_call(runtime_function_t::JOIN, std::move(parts),
                                                        type_id_t::STRING, true));
        }

        for (const operand_t& element : value.elements) {
            take(element);
        }

        return temporary(std::move(value), type, true);
    }

    /// "(a, b)": a tuple, whose elements take their own references.
    operand_t lower_expr(const expr_id_t id, const tuple_expr_t& node) {
        const type_id_t type = expr_type(id);
        aggregate_t value{ .type = type };
        const std::vector<bool> later = changes_after(node.elements);
        for (std::size_t i = 0; i < node.elements.size(); ++i) {
            value.fields.push_back(settled(lower_value(node.elements[i]), later[i]));
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

    /// A T[]'s Count or Capacity, by `member`, as an Int.
    operand_t array_size(const operand_t& array, const std::string_view member) {
        if (member == "Capacity") {
            return runtime_call(runtime_function_t::ARRAY_CAPACITY, { array }, type_id_t::INT32);
        }

        return count_of(array);
    }

    /// An integer's Wrapping or Checked Method, with the integer as the first argument: what
    /// wraps around, or for a Checked one, a T? that's Null when it would overflow or divide by
    /// zero.
    operand_t integer_call(const symbol_t& method, const std::vector<call_argument_t>& arguments,
                           const span_t span) {
        const integer_method_t operation = expect(integer_method(method.name));
        const type_id_t type = method.owner;
        const operand_t left = operation.negate ? zero(type) : arguments[0].value;
        const operand_t right = operation.negate ? arguments[0].value : arguments[1].value;
        if (!operation.checked) {
            return wrapping(operation.op, left, right, span);
        }

        const type_id_t result_type = types().find_nullable(type);
        const local_id_t result = add_local(result_type);
        const operand_t overflows = temporary(
            binary_t{
                .op = operation.op, .left = left, .right = right, .span = span, .overflows = true },
            type_id_t::BOOL);
        const block_id_t test = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        const block_id_t fails = new_block();
        start(fails);
        emit(assign_t{ .target = result, .value = use_t{ .value = null_of(result_type) } });
        leave(exits);
        const block_id_t works = new_block();
        start(works);
        const operand_t value = wrapping(operation.op, left, right, span);
        emit(assign_t{ .target = result,
                       .value = use_t{ .value = convert(value, result_type, span) } });
        leave(exits);
        block_at(test).terminator =
            branch_t{ .condition = overflows, .if_true = fails, .if_false = works };
        join(exits);
        return place_t{ result };
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
        } else if (method.name == "Reserve") {
            run(runtime_function_t::ARRAY_RESERVE, { array, arguments[1].value });
        } else if (method.name == "TrimExcess") {
            run(runtime_function_t::ARRAY_TRIM, { array });
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

    /// "Function(x) => ...": a new function value, or where a Callback is expected, its code's
    /// address, by the C ABI.
    operand_t lower_expr(const expr_id_t id, const lambda_expr_t& /*node*/) {
        const closure_t& closure = m_analysis.closures[expect(m_analysis.expr(id).closure)];
        if (m_analysis.expr(id).callback) {
            const type_id_t type = concrete(expr_type(id));
            const function_id_t code = code_of(closure, closure_copy(concrete(closure.type)));
            return temporary(
                callback_t{ .function = code, .closure = true, .type = type, .span = span_of(id) },
                type);
        }

        return make_closure(closure, span_of(id));
    }

    /// A new object of a closure's Class, which is the function value, as its Func type. It holds
    /// what the lambda captures, and its code is in the Class's Method table.
    operand_t make_closure(const closure_t& closure, const span_t span) {
        // In generic code, the closure's Class is an instance too, and in a copy of a loop over
        // a type's members, the copy's own.
        const type_id_t type = closure_copy(concrete(closure.type));
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

    /// A closure Class for the copies of loop bodies being lowered: one with the same fields,
    /// named after them.
    type_id_t closure_copy(const type_id_t type) {
        if (m_copies.empty()) {
            return type;
        }

        const auto key = std::pair(type, m_copies);
        if (const auto found = m_closure_copies.find(key); found != m_closure_copies.end()) {
            return found->second;
        }

        std::string name(types().name(type));
        for (const std::uint32_t copy : m_copies) {
            name += std::format(" #{}", copy);
        }

        const type_id_t made = types().add_class(std::move(name), types().get(type).module);
        types().set_fields(made, types().get(type).fields);
        m_closure_copies[key] = made;
        return made;
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
            .name = types().qualified_name(type),
            .return_type = types().get(concrete(closure.func)).inner,
        });
        m_closure_code[type] = id;
        add_runtime_class(type, { id });
        m_closures.push_back({
            .closure = closure,
            .type = type,
            .parameters = m_parameters,
            .arguments = m_arguments,
            .reflected = m_reflected,
            .origin = m_origin,
        });

        // An Async lambda's code starts its coroutine, which is a function of its own.
        if (closure.lambda && m_analysis.symbol(closure.function).modifiers.is_async) {
            m_coroutine_code[type] = static_cast<function_id_t>(m_module.functions.size());
            m_module.functions.push_back({
                .name = std::format("{} (Async)", types().qualified_name(type)),
                .is_async = true,
            });
            m_closures.push_back({
                .closure = closure,
                .type = type,
                .parameters = m_parameters,
                .arguments = m_arguments,
                .reflected = m_reflected,
                .coroutine = true,
                .origin = m_origin,
            });
        }

        return id;
    }

    /// What a closure's code captured, from its object into locals, which borrow the object's
    /// references.
    void load_captures(const pending_closure_t& pending, const local_id_t object) {
        // The type table keeps each type where it is, so the locals can use the fields' names.
        const closure_t& closure = pending.closure;
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
            if (capture.kind == capture_kind_t::VALUE) {
                m_unchanging.push_back(local);
            }

            if (self) {
                m_self = local;
            } else {
                m_locals[capture.inside] = local;
            }
        }
    }

    /// A Parallel For's body's code takes its object and a chunk of the indexes, from `first` up
    /// to `last`, and runs the body for each, as the loop variable. Every thread running a chunk
    /// shares the object, which only borrows.
    void lower_parallel_body(const pending_closure_t& pending, const local_id_t object) {
        const closure_t& closure = pending.closure;
        const local_id_t first = add_local(type_id_t::INT64, "first");
        const local_id_t count = add_local(type_id_t::INT64, "count");
        current().parameter_count = current().locals.size();
        load_captures(pending, object);

        const stmt_id_t id = expect(closure.body);
        const auto& node = std::get<for_stmt_t>(m_ast.get(id).node);
        const span_t span = m_ast.get(id).span;
        const symbol_id_t symbol_id = m_analysis.symbol(closure.function).parameters.front();
        const type_id_t type = concrete(m_analysis.symbol(symbol_id).type);
        const local_id_t position = add_local(type_id_t::INT64, "index");
        emit(assign_t{ .target = position, .value = use_t{ .value = place_t{ first } } });

        const block_id_t head = new_block();
        jump(head);
        start(head);
        const operand_t more =
            compare(binary_op_t::NOT_EQUAL, place_t{ count }, zero(type_id_t::INT64), span);
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        const local_id_t variable = add_local(type, node.variable.text);
        m_locals[symbol_id] = variable;
        emit(assign_t{
            .target = variable,
            .value = cast_t{ .value = place_t{ position }, .type = type, .span = span },
        });
        loop_t loop = lower_loop_body(id, node.body);
        leave(loop.continues);
        join(loop.continues);
        // The index wraps past an Int64's end, where an unsigned range can go.
        binary_t next{
            .op = binary_op_t::ADD,
            .left = place_t{ position },
            .right = one(type_id_t::INT64),
            .span = span,
            .wrapping = true,
        };
        emit(assign_t{ .target = position, .value = std::move(next) });
        binary_t fewer{
            .op = binary_op_t::SUBTRACT,
            .left = place_t{ count },
            .right = one(type_id_t::INT64),
            .span = span,
        };
        emit(assign_t{ .target = count, .value = std::move(fewer) });
        jump(head);

        const block_id_t done = new_block();
        block_at(test).terminator =
            branch_t{ .condition = more, .if_true = body, .if_false = done };
        start(done);
        terminate(return_t{});
    }

    /// A closure's code takes its object, then the arguments. It reads what it captured from the
    /// object, which lives as long as the call. A Function used by name just calls it.
    void lower_closure(const pending_closure_t& pending) {
        if (pending.coroutine) {
            lower_async_closure(pending);
            return;
        }

        const closure_t& closure = pending.closure;
        m_parameters = pending.parameters;
        m_arguments = pending.arguments;
        m_reflected = pending.reflected;
        begin_function(m_closure_code.at(pending.type));
        const local_id_t object = add_local(pending.type, "closure");
        const symbol_t& function = m_analysis.symbol(closure.function);

        if (closure.body) {
            lower_parallel_body(pending, object);
            return;
        }

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
        if (!closure.lambda && external_of(function) != nullptr) {
            const std::optional<operand_t> value =
                extern_call(closure.function, std::move(arguments), function.span);
            if (value) {
                take(*value); // the caller owns the references it gets back
            }

            const std::optional<operand_t> returned =
                value ? std::optional(outliving(*value)) : std::nullopt;
            finish_statement();
            terminate(return_t{ .value = returned });
            return;
        }

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

        // An Async lambda's code makes its Task and starts the coroutine, which runs until it
        // first Awaits, as an Async Function's call does.
        if (function.modifiers.is_async) {
            const type_id_t type = types().get(concrete(closure.func)).inner;
            generated(generated_kind_t::RUN, type);
            const operand_t task = temporary(new_t{ .type = type }, type, true);
            runtime_call_t begin{
                .function = runtime_function_t::ASYNC_BEGIN,
                .arguments = { task },
            };
            emit(evaluate_t{ std::move(begin) });
            arguments.insert(arguments.begin(), { .value = task });
            arguments.insert(arguments.begin(), { .value = place_t{ object } });
            call_t start{
                .function = m_coroutine_code.at(pending.type),
                .arguments = std::move(arguments),
            };
            emit(evaluate_t{ std::move(start) });
            take(task);
            finish_statement();
            terminate(return_t{ .value = task });
            return;
        }

        load_captures(pending, object);
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
        const operand_t returned = outliving(value);
        finish_statement();
        release_scopes(0);
        terminate(return_t{ .value = returned });
    }

    /// An Async lambda's coroutine takes its object, its Task, then the arguments, and keeps its
    /// own references to them until it's done, as an Async Function does.
    void lower_async_closure(const pending_closure_t& pending) {
        const closure_t& closure = pending.closure;
        m_parameters = pending.parameters;
        m_arguments = pending.arguments;
        m_reflected = pending.reflected;
        begin_function(m_coroutine_code.at(pending.type));
        const local_id_t object = add_local(pending.type, "closure");
        const symbol_t& function = m_analysis.symbol(closure.function);
        const type_id_t result = concrete(function.type);
        const local_id_t task = add_local(types().task(result), "task");
        m_async_task = task;
        for (const symbol_id_t parameter_id : function.parameters) {
            const symbol_t& parameter = m_analysis.symbol(parameter_id);
            m_locals[parameter_id] = add_local(concrete(parameter.type), parameter.name);
        }

        current().parameter_count = current().locals.size();
        for (std::size_t i = 0; i < current().parameter_count; ++i) {
            const auto local = static_cast<local_id_t>(i);
            if (has_references(current().locals[i].type)) {
                emit(retain_t{ .value = place_t{ local } });
                m_async_kept.push_back(local);
            }
        }

        load_captures(pending, object);
        const auto& node = std::get<lambda_expr_t>(m_ast.get(expect(closure.lambda)).node);
        if (!node.value) {
            lower_statements(node.body);
            finish_function();
            return;
        }

        if (result == type_id_t::NONE) {
            lower_effect(*node.value);
        } else {
            const operand_t value = lower_value(*node.value);
            take(value);
            emit(assign_t{
                .target = element_of(place_t{ task }, 2),
                .value = use_t{ .value = value },
            });
        }

        finish_statement();
        release_scopes(0);
        finish_async(task);
        terminate(return_t{});
    }

    /// Interpolation: every part becomes a String, then they're joined. The parts go left to
    /// right, so a String read in one is copied first if a later one could change it.
    operand_t lower_expr(expr_id_t /*id*/, const string_expr_t& node) {
        std::vector<expr_id_t> inners;
        for (const string_part_t& part : node.parts) {
            if (const auto* inner = std::get_if<expr_id_t>(&part)) {
                inners.push_back(*inner);
            }
        }

        const std::vector<bool> later = changes_after(inners);
        std::vector<operand_t> parts;
        std::size_t next = 0;
        for (const string_part_t& part : node.parts) {
            if (const auto* text = std::get_if<std::string>(&part)) {
                parts.emplace_back(make_string(*text));
                continue;
            }

            const expr_id_t inner = std::get<expr_id_t>(part);
            add_text(parts, settled(lower_value(inner), later[next++]), converted_type(inner));
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

        // An array's text is its elements', from a function made for its type.
        if (is_any_array(type)) {
            call_t call{
                .function = generated(generated_kind_t::TEXT, type),
                .arguments = { { .value = value } },
            };
            parts.push_back(temporary(std::move(call), type_id_t::STRING, true));
            return;
        }

        // A Struct or Class shows what its ToString gives, and so does an Interface's object.
        if (is_struct(type) || is_class(type) || is_interface(type)) {
            parts.push_back(to_string(value, type));
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

    /// A Struct's, Class's, or Interface's object's text: what Printable's ToString gives, called
    /// through the Interface for an Interface's object.
    operand_t to_string(const operand_t& value, const type_id_t type) {
        const type_id_t printable = m_analysis.interfaces.printable;
        const symbol_id_t method = builtin_member(printable, "ToString");
        call_t call{ .arguments = { { .value = value } } };
        if (is_interface(type)) {
            call.function = function_of(method, { printable });
            call.slot = interface_slot(method);
            call.interface = printable;
        } else {
            const symbol_id_t found = expect(implementation(type, method));
            call.function = function_of(found, implementation_arguments(type, found));
            if (is_virtual(m_analysis.symbol(found))) {
                call.slot = slot_of(found);
            }

            give_error_variable(call, found);
        }

        return temporary(std::move(call), type_id_t::STRING, true);
    }

    /// What implements an Interface's Method that doesn't throw gets an error variable all the
    /// same where it takes one, as what implements another Interface's Method that throws does,
    /// which nothing sets.
    void give_error_variable(call_t& call, const symbol_id_t target) {
        if (takes_error(target)) {
            call.arguments.push_back({ .value = place_t{ error_variable() }, .by_ref = true });
        }
    }

    /// A value's Hash: a Struct's or Class's own, or else the built-in one. A built-in type's
    /// comes from the runtime, and so does a Class's object's. A Struct or tuple combines its
    /// fields', an array its elements', and a T? its value's, or 0 for Null.
    operand_t hash_of(const operand_t& value, const type_id_t type) {
        if (is_struct(type) || is_class(type)) {
            const symbol_id_t method = builtin_member(m_analysis.interfaces.hashable, "Hash");
            if (const std::optional<symbol_id_t> own = implementation(type, method)) {
                call_t call{
                    .function = function_of(*own, implementation_arguments(type, *own)),
                    .arguments = { { .value = value } },
                };
                if (is_virtual(m_analysis.symbol(*own))) {
                    call.slot = slot_of(*own);
                }

                give_error_variable(call, *own);
                return temporary(std::move(call), type_id_t::UINT64);
            }
        }

        if (has_fields(type)) {
            const place_t whole = as_place(value);
            const std::vector<field_t>& fields = types().get(type).fields;
            operand_t hash = const_value_t{ .type = type_id_t::UINT64, .data = std::uint64_t{ 0 } };
            for (std::uint32_t i = 0; i < fields.size(); ++i) {
                const operand_t part = hash_of(element_of(whole, i), fields[i].type);
                hash = runtime_call(runtime_function_t::HASH_COMBINE, { hash, part },
                                    type_id_t::UINT64);
            }

            return hash;
        }

        if (is_nullable(type)) {
            const local_id_t hash = add_local(type_id_t::UINT64);
            emit(assign_t{ .target = hash, .value = use_t{ .value = zero(type_id_t::UINT64) } });
            const block_id_t some = new_block();
            const block_id_t done = new_block();
            terminate(
                branch_t{ .condition = has_value(value, {}), .if_true = some, .if_false = done });
            start(some);
            const operand_t inner = hash_of(unwrap(value), types().get(type).inner);
            emit(assign_t{ .target = hash, .value = use_t{ .value = inner } });
            jump(done);
            start(done);
            return place_t{ hash };
        }

        if (is_any_array(type)) {
            call_t call{
                .function = generated(generated_kind_t::HASH, type),
                .arguments = { { .value = value } },
            };
            return temporary(std::move(call), type_id_t::UINT64);
        }

        return runtime_call(runtime_function_t::HASH, { value }, type_id_t::UINT64);
    }

    /// A built-in Interface's Method on a type without one of its own: a Hash, a ToString that
    /// gives what a string shows, Numeric's Zero or One, Defaultable's DefaultValue, which is
    /// the zero, or an array's Clone for Cloneable's.
    operand_t builtin_method(const symbol_t& method, const type_id_t type,
                             const std::optional<operand_t>& receiver) {
        if (method.name == "Zero" || method.name == "DefaultValue") {
            return zero_value(type);
        }

        if (method.name == "Clone") {
            return runtime_call(runtime_function_t::ARRAY_CLONE, { expect(receiver) }, type, true);
        }

        if (method.name == "One") {
            return one(type);
        }

        if (method.name == "Hash") {
            return hash_of(expect(receiver), type);
        }

        std::vector<operand_t> parts;
        add_text(parts, expect(receiver), type);
        return runtime_call(runtime_function_t::JOIN, std::move(parts), type_id_t::STRING, true);
    }

    /// The function that compares, hashes, or gives the text of arrays of `type`, declared the
    /// first time it's needed.
    function_id_t generated(const generated_kind_t kind, const type_id_t type) {
        const auto found = std::ranges::find_if(m_generated, [&](const generated_t& function) {
            return function.kind == kind && function.type == type;
        });
        if (found != m_generated.end()) {
            return found->function;
        }

        const std::string name = types().qualified_name(type);
        std::string full;
        type_id_t result = type_id_t::BOOL;
        if (kind == generated_kind_t::EQUAL) {
            full = std::format("{}.Operator ==({}, {})", name, name, name);
        } else if (kind == generated_kind_t::HASH) {
            full = std::format("{}.Hash()", name);
            result = type_id_t::UINT64;
        } else if (kind == generated_kind_t::RUN) {
            full = std::format("{}.Run()", name);
            result = type_id_t::NONE;
        } else {
            full = std::format("{}.ToString()", name);
            result = type_id_t::STRING;
        }

        const auto id = static_cast<function_id_t>(m_module.functions.size());
        m_module.functions.push_back({ .name = std::move(full), .return_type = result });
        m_generated.push_back({ .kind = kind, .type = type, .function = id });
        // A Task's Class has its work first in its Method table, where the runtime finds it.
        if (kind == generated_kind_t::RUN) {
            add_runtime_class(type, { id });
        }

        return id;
    }

    /// An array function's body. An array's are equal when they have as many elements, each
    /// equal to the other's; its Hash combines its count and its elements'; and its text is
    /// like "[1, 2, 3]".
    void lower_generated(const generated_t& generated) {
        begin_function(generated.function);
        const type_id_t type = generated.type;
        const type_id_t element = types().get(type).inner;
        const local_id_t array =
            add_local(type, generated.kind == generated_kind_t::EQUAL ? "a" : "Self");
        const local_id_t other =
            generated.kind == generated_kind_t::EQUAL ? add_local(type, "b") : array;
        current().parameter_count = current().locals.size();
        const span_t nowhere;
        switch (generated.kind) {
        case generated_kind_t::EQUAL: {
            if (is_array(type)) {
                const operand_t count = count_of(place_t{ array });
                const operand_t other_count = count_of(place_t{ other });
                return_false_unless(compare(binary_op_t::EQUAL, count, other_count, nowhere));
            }

            each_element(array, type, [&](const place_t& left, const local_id_t counter) {
                place_t right{ other };
                right.steps.push_back({ .indices = { counter }, .span = nowhere });
                return_false_unless(equal(left, right, nowhere));
            });
            terminate(return_t{ .value = make_bool(true) });
            break;
        }
        case generated_kind_t::HASH: {
            const local_id_t hash = add_local(type_id_t::UINT64);
            const runtime_call_t count{
                .function = runtime_function_t::HASH,
                .arguments = { element_count_of(array, type) },
            };
            emit(assign_t{ .target = hash, .value = count });
            each_element(array, type, [&](const place_t& item, local_id_t /*counter*/) {
                const runtime_call_t combined{
                    .function = runtime_function_t::HASH_COMBINE,
                    .arguments = { place_t{ hash }, hash_of(item, element) },
                };
                emit(assign_t{ .target = hash, .value = combined });
            });
            terminate(return_t{ .value = place_t{ hash } });
            break;
        }
        case generated_kind_t::TEXT: {
            const local_id_t text = add_local(type_id_t::STRING, "text");
            initialize(text, make_string("["));
            m_scopes.back().emplace_back(text);
            each_element(array, type, [&](const place_t& item, const local_id_t counter) {
                // ", " goes before each element after the first.
                const operand_t later =
                    compare(binary_op_t::GREATER, counter, zero(type_id_t::INT32), nowhere);
                const block_id_t separate = new_block();
                const block_id_t next = new_block();
                terminate(branch_t{ .condition = later, .if_true = separate, .if_false = next });
                start(separate);
                store(text, runtime_call(runtime_function_t::CONCAT,
                                         { place_t{ text }, make_string(", ") }, type_id_t::STRING,
                                         true));
                finish_statement();
                jump(next);
                start(next);
                std::vector<operand_t> parts{ place_t{ text } };
                add_text(parts, item, element);
                store(text, runtime_call(runtime_function_t::JOIN, std::move(parts),
                                         type_id_t::STRING, true));
                finish_statement();
            });
            store(text,
                  runtime_call(runtime_function_t::CONCAT, { place_t{ text }, make_string("]") },
                               type_id_t::STRING, true));
            finish_statement();
            take(place_t{ text });
            release_scopes(0);
            terminate(return_t{ .value = place_t{ text } });
            break;
        }
        case generated_kind_t::RUN: {
            // The closure runs once, so the work takes it from the object, and lets go of it
            // when it's done. A Task keeps the result.
            const type_id_t func = types().get(type).fields[1].type;
            const local_id_t work = add_local(func, "work");
            const place_t closure = element_of(place_t{ array }, 1);
            emit(assign_t{ .target = work, .value = use_t{ .value = closure } });
            emit(assign_t{
                .target = closure,
                .value = use_t{ .value = const_value_t{ .type = func, .data = nullptr } },
            });
            invoke_t call{ .callee = place_t{ work } };
            if (element == type_id_t::NONE) {
                emit(evaluate_t{ std::move(call) });
            } else {
                emit(assign_t{
                    .target = element_of(place_t{ array }, 2),
                    .value = std::move(call),
                });
            }

            emit(release_t{ .target = work });
            terminate(return_t{});
            break;
        }
        }
    }

    /// Returns False from the function unless `condition` holds, and goes on where it does.
    void return_false_unless(const operand_t& condition) {
        const block_id_t next = new_block();
        const block_id_t differ = new_block();
        terminate(branch_t{ .condition = condition, .if_true = next, .if_false = differ });
        start(differ);
        terminate(return_t{ .value = make_bool(false) });
        start(next);
    }

    /// How many elements an array has: a T[]'s count, or a fixed array's.
    operand_t element_count_of(const local_id_t array, const type_id_t type) {
        if (is_array(type)) {
            return count_of(place_t{ array });
        }

        const auto count = static_cast<std::int64_t>(types().element_count(type));
        return const_value_t{ .type = type_id_t::INT32, .data = count };
    }

    /// Runs `body` with each element of the array in `array`, in order, and a local with its
    /// index, then goes on after the loop.
    template <typename Body>
    void each_element(const local_id_t array, const type_id_t type, const Body& body) {
        const span_t nowhere;
        const local_id_t counter = add_local(type_id_t::INT32);
        emit(assign_t{ .target = counter, .value = use_t{ .value = zero(type_id_t::INT32) } });
        const block_id_t head = new_block();
        jump(head);
        start(head);
        const operand_t in_range =
            compare(binary_op_t::LESS, counter, element_count_of(array, type), nowhere);
        const block_id_t loop = new_block();
        const block_id_t exit = new_block();
        terminate(branch_t{ .condition = in_range, .if_true = loop, .if_false = exit });
        start(loop);
        place_t element{ array };
        element.steps.push_back({ .indices = { counter }, .span = nowhere });
        body(element, counter);
        count_up(counter, nowhere);
        jump(head);
        start(exit);
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

        const operand_t left = settled(lower_value(node.left), may_change(node.right));
        const operand_t right = lower_value(node.right);
        const type_id_t type = type_of(left);
        // A Ptr steps by its T's size.
        if (is_pointer(type) && (node.op == binary_op_t::ADD || node.op == binary_op_t::SUBTRACT)) {
            check_steps(type, span_of(id));
            return temporary(pointer_t{ .op = node.op, .left = left, .right = right },
                             expr_type(id));
        }

        // Strings add through the runtime.
        if (type == type_id_t::STRING && node.op == binary_op_t::ADD) {
            return runtime_call(runtime_function_t::CONCAT, { left, right }, type_id_t::STRING,
                                true);
        }

        // Strings order by their text.
        if (type == type_id_t::STRING && is_ordering(node.op)) {
            return order(node.op, left, right, span_of(id));
        }

        // Structs, tuples, arrays, Strings, and the values in T?s only have == and !=, which take
        // steps of their own.
        if (has_fields(type) || is_any_array(type) || type == type_id_t::STRING ||
            compares_values(left, right)) {
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

    /// A Bool: whether two values of one type are equal, the way == compares them: with the
    /// type's Operator == if it has one, and otherwise a Struct or tuple compares its fields, a
    /// String its text, and a T? is equal when both are Null or both hold equal values. A Class,
    /// or Class?, without an Operator == compares references.
    operand_t equal(const operand_t& left, const operand_t& right, const span_t span) {
        const type_id_t type = type_of(left);
        if (const auto defined = equal_operator(type)) {
            return call_operator(defined->first, defined->second, { left, right }, span);
        }

        if (has_fields(type)) {
            return equal_fields(std::get<place_t>(left), std::get<place_t>(right), span);
        }

        // Arrays compare their elements, in a function made for their type.
        if (is_any_array(type)) {
            call_t call{
                .function = generated(generated_kind_t::EQUAL, type),
                .arguments = { { .value = left }, { .value = right } },
            };
            return temporary(std::move(call), type_id_t::BOOL);
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
        std::vector<expr_id_t> written;
        written.reserve(info.arguments.size());
        for (const std::optional<expr_id_t> operand : info.arguments) {
            written.push_back(expect(operand));
        }

        const std::vector<bool> later = changes_after(written);
        std::vector<operand_t> operands;
        operands.reserve(written.size());
        for (std::size_t i = 0; i < written.size(); ++i) {
            operands.push_back(settled(lower_value(written[i]), later[i]));
        }

        operand_t result = call_operator(expect(info.symbol), concrete(info.type_arguments),
                                         operands, span_of(id));
        if (!negate) {
            return result;
        }

        return temporary(unary_t{ .op = unary_op_t::NOT, .operand = result, .span = span_of(id) },
                         type_id_t::BOOL);
    }

    /// Calls an Operator, with the type arguments of its Struct or Class. An Interface's is the
    /// one of the type for Self that implements it.
    operand_t call_operator(const symbol_id_t id, const std::vector<type_id_t>& arguments,
                            const std::vector<operand_t>& operands, const span_t span) {
        const symbol_t& function = m_analysis.symbol(id);
        if (is_interface(function.owner)) {
            const std::optional<symbol_id_t> found = implementation(arguments.front(), id);
            if (!found) {
                return builtin_operator(function.name, arguments.front(), operands, span);
            }

            return call_operator(*found, implementation_arguments(arguments.front(), *found),
                                 operands, span);
        }

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

    /// A built-in Interface's Operator on a type without one of its own: the operator a
    /// built-in type has, or == on a Struct's fields or a Class's objects.
    operand_t builtin_operator(const std::string_view name, const type_id_t type,
                               const std::vector<operand_t>& operands, const span_t span) {
        const operand_t left = convert(operands[0], type, span);
        const operand_t right = convert(operands[1], type, span);
        static constexpr std::array<std::pair<std::string_view, binary_op_t>, 9> operators{
            {
                { "==", binary_op_t::EQUAL },
                { "<", binary_op_t::LESS },
                { ">", binary_op_t::GREATER },
                { "<=", binary_op_t::LESS_EQUAL },
                { ">=", binary_op_t::GREATER_EQUAL },
                { "+", binary_op_t::ADD },
                { "-", binary_op_t::SUBTRACT },
                { "*", binary_op_t::MULTIPLY },
                { "/", binary_op_t::DIVIDE },
            },
        };
        const binary_op_t op =
            std::ranges::find(operators, name, &std::pair<std::string_view, binary_op_t>::first)
                ->second;
        if (op == binary_op_t::EQUAL) {
            return equal(left, right, span);
        }

        if (is_ordering(op)) {
            return order(op, left, right, span);
        }

        return temporary(binary_t{ .op = op, .left = left, .right = right, .span = span }, type);
    }

    [[nodiscard]] static bool is_ordering(const binary_op_t op) {
        return op == binary_op_t::LESS || op == binary_op_t::LESS_EQUAL ||
               op == binary_op_t::GREATER || op == binary_op_t::GREATER_EQUAL;
    }

    /// "a < b" and the rest: a number's comparison, or for Strings, the runtime's order of their
    /// text.
    operand_t order(const binary_op_t op, const operand_t& left, const operand_t& right,
                    const span_t span) {
        if (type_of(left) != type_id_t::STRING) {
            return compare(op, left, right, span);
        }

        const operand_t order =
            runtime_call(runtime_function_t::COMPARE, { left, right }, type_id_t::INT32);
        return compare(op, order, zero(type_id_t::INT32), span);
    }

    /// The Operator == that a Struct or Class has for two of itself, which every == on it uses:
    /// in a Struct's or tuple's fields, a T[]'s Contains, and a T?'s value too, with the type
    /// arguments to call it with. It's the one analysis picks for a == written on the type: one
    /// that an Extend of the instance adds, like Box<Int32>'s, before the generic type's own,
    /// and else the nearest Class's it extends. One that a Where line makes conditional, like
    /// what an Extend with one adds or a default it gives, is only the instances' that meet it,
    /// and the others compare the built-in way.
    [[nodiscard]] std::optional<std::pair<symbol_id_t, std::vector<type_id_t>>>
    equal_operator(const type_id_t type) const {
        if (!has_fields(type) && !is_class(type)) {
            return std::nullopt;
        }

        for (type_id_t current = type; current != type_id_t::INVALID;
             current = types().get(current).base) {
            if (const auto added = m_analysis.instance_members.find(current);
                added != m_analysis.instance_members.end()) {
                for (const symbol_id_t member : added->second) {
                    if (compares_two(member, current)) {
                        return std::pair{ member, std::vector<type_id_t>{} };
                    }
                }
            }

            const type_id_t definition = definition_of(current);
            const auto declared = m_declared_types.find(definition);
            if (declared == m_declared_types.end()) {
                break;
            }

            for (const symbol_id_t member : m_analysis.symbol(declared->second).members) {
                if (compares_two(member, definition) &&
                    has_member(current, m_analysis.symbol(member))) {
                    return std::pair{ member, types().get(current).arguments };
                }
            }
        }

        return std::nullopt;
    }

    /// Whether a member is an Operator == that takes two of `type`.
    [[nodiscard]] bool compares_two(const symbol_id_t member, const type_id_t type) const {
        const symbol_t& candidate = m_analysis.symbol(member);
        return candidate.kind == symbol_kind_t::METHOD && candidate.name == "==" &&
               candidate.modifiers.is_static && candidate.parameters.size() == 2 &&
               m_analysis.symbol(candidate.parameters[0]).type == type &&
               m_analysis.symbol(candidate.parameters[1]).type == type;
    }

    /// Whether == on two T? values compares the values inside. Comparing with Null only asks
    /// whether the other holds a value, and a Class? compares references, unless its Class has an
    /// Operator ==, which every == on its values uses, narrowed or not.
    [[nodiscard]] bool compares_values(const operand_t& left, const operand_t& right) {
        const type_id_t type = type_of(left);
        if (!is_nullable(type) || is_null(left) || is_null(right)) {
            return false;
        }

        const type_id_t inner = types().get(type).inner;
        return !is_object(inner) || equal_operator(inner).has_value();
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

    /// "Await t": the scheduler resumes this coroutine once the Task is done, and its result is
    /// in the Task.
    std::optional<operand_t> lower_await(const expr_id_t id) {
        const auto& node = std::get<await_expr_t>(m_ast.get(id).node);
        // What runs meanwhile can change a variable that holds the Task, so the wait and the
        // result go through a reference of its own.
        const operand_t task = settled(lower_value(node.operand), true);
        // Top-level code waits as Wait does, while the main scheduler runs what can go on.
        if (!m_async_task) {
            emit(evaluate_t{
                .value =
                    runtime_call_t{
                        .function = runtime_function_t::TASK_WAIT,
                        .arguments = { task },
                    },
            });
            rethrow(task, span_of(id));
            if (types().get(concrete(type_of(task))).inner == type_id_t::NONE) {
                return std::nullopt;
            }

            return element_of(as_place(task), 2);
        }

        runtime_call_t wait{
            .function = runtime_function_t::AWAIT,
            .arguments = { task, place_t{ expect(m_async_task) } },
        };
        emit(evaluate_t{ std::move(wait) });
        const block_id_t resume = new_block();
        const block_id_t cancel = new_block();
        terminate(suspend_t{ .resume = resume, .cancel = cancel });
        // Destroyed here, the coroutine lets go of what it holds, without running its Defers or
        // Finally blocks, and its Task is never done. In a deferred statement or a Finally on a
        // way out, what the way out has let go of already is gone, and an error on its way out
        // is let go of too.
        start(cancel);
        for (const local_id_t temporary : std::views::reverse(m_temporaries)) {
            emit(release_t{ .target = temporary });
        }

        for (const local_id_t error : std::views::reverse(m_raising)) {
            emit(release_t{ .target = error });
        }

        for (std::size_t depth = m_scopes.size(); depth-- > 0;) {
            for (std::size_t position = m_scopes[depth].size(); position-- > 0;) {
                const auto* local = std::get_if<local_id_t>(&m_scopes[depth][position].action);
                if (local != nullptr && !left(depth, position)) {
                    emit(release_t{ .target = *local });
                }
            }
        }

        for (const local_id_t kept : std::views::reverse(m_async_kept)) {
            emit(release_t{ .target = kept });
        }

        terminate(return_t{});
        start(resume);
        rethrow(task, span_of(id));
        if (types().get(concrete(type_of(task))).inner == type_id_t::NONE) {
            return std::nullopt;
        }

        return element_of(as_place(task), 2);
    }

    operand_t lower_expr(const expr_id_t id, const await_expr_t& /*node*/) {
        return expect(lower_await(id));
    }

    /// A call, or a Try, for its effects: its result, or nullopt when it gives nothing.
    std::optional<operand_t> lower_effect(const expr_id_t id) {
        if (std::holds_alternative<try_expr_t>(m_ast.get(id).node)) {
            return lower_try(id);
        }

        if (std::holds_alternative<await_expr_t>(m_ast.get(id).node)) {
            return lower_await(id);
        }

        // SizeOf<T>() and TypeName<T>() only give a value.
        if (m_analysis.expr(id).value || m_analysis.expr(id).type_name ||
            m_analysis.expr(id).size_of) {
            return lower_value(id);
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
                !std::holds_alternative<try_expr_t>(inner) &&
                !std::holds_alternative<await_expr_t>(inner)) {
                return lower_value(node.operand);
            }

            // A call or an Await that gives nothing, like Super.New, is lowered for its effects.
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
    /// its message, or one saying the assertion failed, and so does DebugAssert, in debug builds
    /// only. Unreachable and Todo say what they are first. The message is only made then.
    void lower_panic(const expr_id_t id, const symbol_t& function) {
        const expr_info_t& info = m_analysis.expr(id);
        const bool asserts = function.name == "Assert" || function.name == "DebugAssert";
        std::optional<block_id_t> ok;
        if (asserts) {
            ok = new_block();
            // A release build goes past a DebugAssert, whose code it then leaves out.
            if (function.name == "DebugAssert") {
                const operand_t debug = temporary(
                    runtime_call_t{ .function = runtime_function_t::DEBUG_BUILD }, type_id_t::BOOL);
                const block_id_t check = new_block();
                terminate(branch_t{ .condition = debug, .if_true = check, .if_false = *ok });
                start(check);
            }

            const std::size_t made = m_temporaries.size();
            operand_t condition = lower_value(expect(info.arguments[0]));
            // A release build never makes what a DebugAssert's condition does, so it's let go
            // before the two ways join, the Bool kept apart from it.
            if (function.name == "DebugAssert" && m_temporaries.size() > made) {
                condition = temporary(use_t{ .value = condition }, type_id_t::BOOL);
                release_temporaries_from(made);
            }

            const block_id_t fail = new_block();
            terminate(branch_t{ .condition = condition, .if_true = *ok, .if_false = fail });
            start(fail);
        }

        std::string_view first;
        if (function.name == "Unreachable") {
            first = "this code isn't meant to be reached";
        } else if (function.name == "Todo") {
            first = "this isn't done yet";
        } else if (function.name == "DebugAssert") {
            first = "debug assertion failed";
        } else {
            first = "assertion failed";
        }

        // Unreachable's and Todo's message goes after what they say.
        const bool after = function.name == "Unreachable" || function.name == "Todo";
        panic_t panic{ .message = std::string(first), .span = span_of(id) };
        const std::size_t mark = m_temporaries.size();
        if (const std::optional<expr_id_t> message = info.arguments[asserts ? 1 : 0]) {
            operand_t text = lower_value(*message);
            if (after) {
                const auto* constant = std::get_if<const_value_t>(&text);
                text = constant != nullptr
                           ? operand_t{ make_string(std::format(
                                 "{}: {}", first, std::get<std::string>(constant->data))) }
                           : runtime_call(runtime_function_t::CONCAT,
                                          { make_string(std::format("{}: ", first)), text },
                                          type_id_t::STRING, true);
            }

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

    /// "Atomic<Int>(0)" or "Mutex(value)": what a Global or Field starts with. A Mutex starts
    /// with no thread holding it.
    operand_t make_in_place(const expr_id_t id) {
        const type_id_t type = concrete(expr_type(id));
        const operand_t value = lower_value(expect(m_analysis.expr(id).arguments.front()));
        if (types().get(type).kind == type_kind_t::ATOMIC) {
            return temporary(
                atomic_t{ .op = atomic_op_t::MAKE, .operands = { value }, .type = type }, type);
        }

        take(value);
        return temporary(
            aggregate_t{
                .type = type,
                .fields = { zero(type_id_t::INT32), zero(type_id_t::UINT32), value },
            },
            type, true);
    }

    /// An Atomic's Methods, on the Atomic where it is, which comes first.
    std::optional<operand_t> atomic_call(const symbol_t& method,
                                         const std::vector<call_argument_t>& arguments) {
        constexpr auto names = std::to_array<std::pair<std::string_view, atomic_op_t>>({
            { "Load", atomic_op_t::LOAD },
            { "Store", atomic_op_t::STORE },
            { "Add", atomic_op_t::ADD },
            { "Subtract", atomic_op_t::SUBTRACT },
            { "Exchange", atomic_op_t::EXCHANGE },
            { "CompareExchange", atomic_op_t::COMPARE_EXCHANGE },
        });
        const atomic_op_t op =
            std::ranges::find(names, method.name, &std::pair<std::string_view, atomic_op_t>::first)
                ->second;
        const type_id_t type = concrete(method.owner);
        atomic_t operation{
            .op = op,
            .target = std::get<place_t>(arguments.front().value),
            .type = type,
        };
        for (const call_argument_t& argument : arguments | std::views::drop(1)) {
            operation.operands.push_back(argument.value);
        }

        if (op == atomic_op_t::STORE) {
            emit(evaluate_t{ std::move(operation) });
            return std::nullopt;
        }

        return temporary(std::move(operation), op == atomic_op_t::COMPARE_EXCHANGE
                                                   ? type_id_t::BOOL
                                                   : types().get(type).inner);
    }

    /// "Spawn(work)" or "Thread.Start(work)": a new object of the Task's or Thread's Class,
    /// holding the work's closure, which the runtime starts.
    operand_t spawn(const expr_id_t id) {
        const type_id_t type = concrete(expr_type(id));
        const operand_t work = lower_value(expect(m_analysis.expr(id).arguments.front()));
        generated(generated_kind_t::RUN, type);
        const operand_t task = temporary(new_t{ .type = type }, type, true);
        take(work);
        emit(assign_t{
            .target = element_of(std::get<place_t>(task), 1),
            .value = use_t{ .value = work },
        });
        const bool thread = types().get(type).kind == type_kind_t::THREAD;
        runtime_call_t start{
            .function = thread ? runtime_function_t::THREAD_START : runtime_function_t::TASK_START,
            .arguments = { task },
        };
        emit(evaluate_t{ std::move(start) });
        return task;
    }

    /// A Channel's Methods. What's sent goes in with its references, and what's received comes
    /// out with them.
    std::optional<operand_t> channel_call(const symbol_t& method,
                                          const std::vector<call_argument_t>& arguments,
                                          const span_t span) {
        const operand_t& channel = arguments.front().value;
        const type_id_t element = types().get(concrete(method.owner)).inner;
        const auto run = [&](const runtime_function_t function, const local_id_t slot) {
            return runtime_call_t{
                .function = function,
                .arguments = { channel, place_t{ slot } },
                .span = span,
            };
        };

        // `then` runs when `condition` holds, and the code goes on after.
        const auto when = [&](const operand_t& condition, const auto& then) {
            const block_id_t test = here();
            m_block.reset();
            std::vector<block_id_t> exits;
            const block_id_t yes = new_block();
            start(yes);
            then();
            leave(exits);
            const block_id_t no = new_block();
            start(no);
            leave(exits);
            block_at(test).terminator =
                branch_t{ .condition = condition, .if_true = yes, .if_false = no };
            join(exits);
        };

        if (method.name == "Close") {
            runtime_call_t close{
                .function = runtime_function_t::CHANNEL_CLOSE,
                .arguments = { channel },
            };
            emit(evaluate_t{ std::move(close) });
            return std::nullopt;
        }

        if (method.name == "Send" || method.name == "TrySend") {
            const local_id_t slot = add_local(element);
            take(arguments[1].value);
            emit(assign_t{ .target = slot, .value = use_t{ .value = arguments[1].value } });
            if (method.name == "Send") {
                emit(evaluate_t{ run(runtime_function_t::CHANNEL_SEND, slot) });
                return std::nullopt;
            }

            // What wasn't sent is still this code's.
            operand_t sent =
                temporary(run(runtime_function_t::CHANNEL_TRY_SEND, slot), type_id_t::BOOL);
            if (has_references(element)) {
                const operand_t kept =
                    temporary(unary_t{ .op = unary_op_t::NOT, .operand = sent, .span = span },
                              type_id_t::BOOL);
                when(kept, [&] { emit(release_t{ .target = slot }); });
            }

            return sent;
        }

        if (method.name == "TryReceive") {
            const place_t target = std::get<place_t>(arguments[1].value);
            const local_id_t slot = add_local(element);
            operand_t got =
                temporary(run(runtime_function_t::CHANNEL_TRY_RECEIVE, slot), type_id_t::BOOL);
            when(got, [&] {
                if (has_references(element)) {
                    emit(release_t{ .target = target });
                }

                emit(assign_t{ .target = target, .value = use_t{ .value = place_t{ slot } } });
            });
            return got;
        }

        // Receive: Null once it's closed and empty.
        const local_id_t slot = add_local(element);
        const operand_t got =
            temporary(run(runtime_function_t::CHANNEL_RECEIVE, slot), type_id_t::BOOL);
        const type_id_t result_type = concrete(method.type);
        const local_id_t result = add_local(result_type);
        emit(assign_t{ .target = result, .value = use_t{ .value = null_of(result_type) } });
        when(got, [&] {
            const operand_t value = convert(place_t{ slot }, result_type, span);
            emit(assign_t{ .target = result, .value = use_t{ .value = value } });
        });
        if (has_references(result_type)) {
            m_temporaries.push_back(result);
        }

        return place_t{ result };
    }

    /// "For x In ch": each pass receives one, until the Channel is closed and empty, and x takes
    /// its references.
    void lower_channel_iteration(const stmt_id_t id, const span_t span, const for_in_stmt_t& node,
                                 const type_id_t type) {
        const stmt_info_t& info = m_analysis.stmt(id);
        push_scope();
        const local_id_t channel = add_local(type);
        initialize(channel, lower_value(node.sequence));
        m_scopes.back().emplace_back(channel);
        finish_statement();

        const symbol_id_t variable = expect(info.symbol);
        const type_id_t element = concrete(m_analysis.symbol(variable).type);
        const local_id_t slot = add_local(element);
        const block_id_t head = new_block();
        jump(head);
        start(head);
        runtime_call_t receive{
            .function = runtime_function_t::CHANNEL_RECEIVE,
            .arguments = { place_t{ channel }, place_t{ slot } },
            .span = span,
        };
        const operand_t received = temporary(std::move(receive), type_id_t::BOOL);
        const block_id_t test = here();
        m_block.reset();

        const block_id_t body = new_block();
        start(body);
        m_loops.push_back({ .stmt = id, .scope_depth = m_scopes.size() });
        push_scope();
        const local_id_t local = add_local(element, node.value.text);
        m_locals[variable] = local;
        emit(assign_t{ .target = local, .value = use_t{ .value = place_t{ slot } } });
        if (has_references(element)) {
            m_scopes.back().emplace_back(local);
        }

        lower_statements(node.body);
        pop_scope();
        loop_t loop = std::move(m_loops.back());
        m_loops.pop_back();
        leave(loop.continues);
        join(loop.continues);
        jump(head);

        const block_id_t exit = join(loop.breaks);
        block_at(test).terminator =
            branch_t{ .condition = received, .if_true = body, .if_false = exit };
        pop_scope();
    }

    /// A T?'s Expect, which gives its value or panics with the message where it's called, and
    /// OrElse, which gives its value or what its function gives, called only when it's Null.
    operand_t nullable_call(const symbol_t& method, const std::vector<call_argument_t>& arguments,
                            const span_t span) {
        const operand_t value = arguments.front().value;
        const type_id_t type = concrete(method.type);
        // A variable declared as a T? that holds a value here is its value.
        if (!is_nullable(type_of(value))) {
            return convert(value, type, span);
        }

        const local_id_t result = add_local(type);
        const operand_t present = has_value(value, span);
        const block_id_t test = here();
        m_block.reset();

        std::vector<block_id_t> exits;
        const block_id_t holds = new_block();
        start(holds);
        const operand_t inner = convert(unwrap(value), type, span);
        take(inner);
        emit(assign_t{ .target = result, .value = use_t{ .value = inner } });
        leave(exits);

        const block_id_t missing = new_block();
        start(missing);
        if (method.name == "Expect") {
            terminate(panic_t{ .span = span, .text = arguments[1].value });
        } else {
            const std::size_t mark = m_temporaries.size();
            const operand_t made = temporary(invoke_t{ .callee = arguments[1].value, .span = span },
                                             type, has_references(type));
            take(made);
            emit(assign_t{ .target = result, .value = use_t{ .value = made } });
            release_temporaries_from(mark);
            leave(exits);
        }

        block_at(test).terminator =
            branch_t{ .condition = present, .if_true = holds, .if_false = missing };
        join(exits);
        if (has_references(type)) {
            m_temporaries.push_back(result);
        }

        return place_t{ result };
    }

    /// A Task's Wait and a Thread's Join, which give the result a Task keeps, and IsDone.
    std::optional<operand_t> task_call(const symbol_t& method,
                                       const std::vector<call_argument_t>& arguments,
                                       const span_t span) {
        const operand_t task = settled(arguments.front().value, method.name != "IsDone");
        if (method.name == "IsDone") {
            return temporary(
                runtime_call_t{ .function = runtime_function_t::TASK_DONE, .arguments = { task } },
                type_id_t::BOOL);
        }

        runtime_call_t wait{ .function = runtime_function_t::TASK_WAIT, .arguments = { task } };
        emit(evaluate_t{ std::move(wait) });
        rethrow(task, span);
        if (concrete(method.type) == type_id_t::NONE) {
            return std::nullopt;
        }

        return element_of(as_place(task), 2);
    }

    /// NextFrame, Wait, and RealWait make a Task for the scheduler running the code to finish,
    /// and MainThread one that's done. A Wait's time is std.time's Duration, whose one Field is
    /// its nanoseconds.
    operand_t timing_call(const expr_id_t id, const symbol_t& function,
                          const std::vector<call_argument_t>& arguments) {
        const type_id_t type = concrete(expr_type(id));
        generated(generated_kind_t::RUN, type);
        const operand_t task = temporary(new_t{ .type = type }, type, true);
        runtime_call_t begin{ .function = runtime_function_t::ASYNC_BEGIN, .arguments = { task } };
        emit(evaluate_t{ std::move(begin) });
        runtime_call_t timer{ .function = runtime_function_t::ASYNC_FINISH, .arguments = { task } };
        if (function.name == "NextFrame") {
            timer.function = runtime_function_t::WAIT_FRAME;
        } else if (function.name != "MainThread") {
            const const_value_t real{
                .type = type_id_t::BOOL,
                .data = function.name == "RealWait",
            };
            timer.function = runtime_function_t::WAIT_TIME;
            timer.arguments = { task, element_of(as_place(arguments[0].value), 0), real };
        }

        emit(evaluate_t{ std::move(timer) });
        return task;
    }

    /// A Scheduler's Run, Tick, and IsIdle.
    std::optional<operand_t> scheduler_call(const symbol_t& method,
                                            const std::vector<call_argument_t>& arguments) {
        const operand_t& scheduler = arguments.front().value;
        if (method.name == "IsIdle") {
            return temporary(
                runtime_call_t{
                    .function = runtime_function_t::SCHEDULER_IDLE,
                    .arguments = { scheduler },
                },
                type_id_t::BOOL);
        }

        runtime_call_t call{
            .function = method.name == "Run" ? runtime_function_t::SCHEDULER_RUN
                                             : runtime_function_t::SCHEDULER_TICK,
            .arguments = { scheduler, arguments[1].value },
        };
        emit(evaluate_t{ std::move(call) });
        return std::nullopt;
    }

    /// A call's result, or nullopt when it returns nothing. A result with references is released
    /// after the statement unless something takes it. "a?.M()" only calls when a holds a value.
    std::optional<operand_t> lower_call(const expr_id_t id) {
        const scoped_value_t<std::optional<span_t>> at(m_span, span_of(id));
        const auto& node = std::get<call_expr_t>(m_ast.get(id).node);
        // AddressOf(x) is x's place's address.
        if (m_analysis.expr(id).address_of) {
            return temporary(address_of_t{ .place = lower_place(node.arguments.front().value) },
                             concrete(expr_type(id)));
        }

        // A CString's ToString copies its text, and its NULL has none.
        if (m_analysis.expr(id).symbol && m_analysis.expr(id).symbol == m_analysis.cstring_text) {
            // "cs.ToString()", or "ToString()" by name in an Extend of CString, Self's.
            const auto* method = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
            const operand_t text = method != nullptr ? lower_value(method->object)
                                                     : operand_t{ place_t{ expect(m_self) } };
            const block_id_t fail = new_block();
            const block_id_t ok = new_block();
            const binary_t compare{
                .op = binary_op_t::EQUAL,
                .left = text,
                .right = const_value_t{ .type = type_id_t::CSTRING, .data = nullptr },
                .span = span_of(id),
            };
            const operand_t null = temporary(compare, type_id_t::BOOL);
            terminate(branch_t{ .condition = null, .if_true = fail, .if_false = ok });
            start(fail);
            terminate(panic_t{
                .message = "the CString is Null, so it has no text",
                .span = span_of(id),
            });
            start(ok);
            const operand_t copy = runtime_call(runtime_function_t::STRING_FROM_CSTRING, { text },
                                                types().nullable(type_id_t::STRING), true);
            return unwrap(copy);
        }

        // No function gives back an Atomic or Mutex, so a call that does makes one.
        if (const type_kind_t kind = types().get(concrete(expr_type(id))).kind;
            kind == type_kind_t::ATOMIC || kind == type_kind_t::MUTEX) {
            return make_in_place(id);
        }

        // Spawn and Thread.Start are built in, without a symbol, and so is making a Channel. A
        // function value that gives a Task has no symbol either.
        if (is_task(concrete(expr_type(id))) && !m_analysis.expr(id).symbol &&
            !is_func(expr_type(node.callee))) {
            return spawn(id);
        }

        if (is_scheduler(concrete(expr_type(id))) && !m_analysis.expr(id).symbol) {
            const type_id_t type = concrete(expr_type(id));
            return temporary(
                channel_t{ .type = type, .capacity = zero(type_id_t::INT64), .span = span_of(id) },
                type, true);
        }

        if (is_channel(concrete(expr_type(id))) && !m_analysis.expr(id).symbol) {
            const type_id_t type = concrete(expr_type(id));
            const operand_t capacity = lower_value(expect(m_analysis.expr(id).arguments.front()));
            const operand_t wide = temporary(
                cast_t{ .value = capacity, .type = type_id_t::INT64, .span = span_of(id) },
                type_id_t::INT64);
            return temporary(channel_t{ .type = type, .capacity = wide, .span = span_of(id) }, type,
                             true);
        }

        if (const std::optional<symbol_id_t> variable = m_analysis.expr(id).symbol;
            variable && m_analysis.symbol(*variable).kind == symbol_kind_t::MEMBER_VARIABLE) {
            return lower_reflection(id, node, m_reflected.at(*variable));
        }

        // A type's own attributes, of its declaration.
        if (const type_id_t reflected = m_analysis.expr(id).reflected;
            reflected != type_id_t::INVALID) {
            const auto declared = m_declared_types.find(definition_of(concrete(reflected)));
            const bool has =
                std::get<member_expr_t>(m_ast.get(node.callee).node).member.text == "Has";
            return attribute_of(id,
                                declared != m_declared_types.end()
                                    ? &m_analysis.symbol(declared->second).attributes
                                    : nullptr,
                                has);
        }

        // A function value, or a Callback, which is called by the C ABI.
        if (is_func(expr_type(node.callee)) ||
            types().get(expr_type(node.callee)).kind == type_kind_t::CALLBACK) {
            return invoke(node, span_of(id));
        }

        if (const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
            member != nullptr && member->null_safe) {
            return lower_safe(id, member->object,
                              [&](const operand_t& receiver) { return call_on(id, receiver); });
        }

        return call_on(id, std::nullopt);
    }

    /// Why "f.Set" at `span` can't change `field`, which the code there couldn't assign by name
    /// either, as analysis has visibility: one that's Private outside its file, Package outside
    /// its package, or Protected outside its file and the Classes extending its own. nullopt
    /// when it can.
    [[nodiscard]] std::optional<std::string> unsettable(const symbol_t& field, const span_t span) {
        if (field.kind != symbol_kind_t::FIELD || field.span.file == span.file) {
            return std::nullopt;
        }

        const module_table_t& modules = m_ast.modules();
        const auto package = [&](const file_id_t file) -> std::optional<package_id_t> {
            const std::optional<module_id_t> module = modules.of(file);
            return module ? std::optional(modules.get(*module).package) : std::nullopt;
        };
        switch (field.modifiers.visibility) {
        case visibility_t::PUBLIC:
            return std::nullopt;
        case visibility_t::PACKAGE:
            if (package(field.span.file) == package(span.file)) {
                return std::nullopt;
            }

            return std::format("'{}' is Package, so f.Set outside its package can't change it",
                               field.name);
        case visibility_t::PROTECTED:
            // In the code of a Class extending its own.
            for (type_id_t code = m_self ? type_of(place_t{ *m_self }) : type_id_t::INVALID;
                 code != type_id_t::INVALID && is_class(code); code = types().get(code).base) {
                if (definition_of(code) == definition_of(field.owner)) {
                    return std::nullopt;
                }
            }

            return std::format("'{}' is Protected, so f.Set outside its file and the Classes "
                               "extending {} can't change it",
                               field.name, types().name(field.owner));
        case visibility_t::PRIVATE:
            break;
        }

        return std::format("'{}' is Private, so f.Set outside its file can't change it",
                           field.name);
    }

    /// "f.Get(value)", "f.Set(ByRef value, x)", "f.Has(A)", "f.Attribute(A)", or
    /// "m.Call(value)", for the field or Method of this copy of the loop's body. An attribute's
    /// values are constants.
    std::optional<operand_t> lower_reflection(const expr_id_t id, const call_expr_t& node,
                                              const reflected_t& member) {
        const std::string_view name =
            std::get<member_expr_t>(m_ast.get(node.callee).node).member.text;
        const symbol_t* declared = member.symbol ? &m_analysis.symbol(*member.symbol) : nullptr;
        // A Static Field is a global, the instance's for a generic type's.
        if (name == "Get" && declared != nullptr && declared->kind == symbol_kind_t::GLOBAL) {
            const std::vector<type_id_t> arguments = owner_arguments(member.owner, declared->owner);
            return place_t{ arguments.empty() ? ready(m_globals.at(*member.symbol), id)
                                              : instance_global(*member.symbol, arguments) };
        }

        if (name == "Get" && declared != nullptr && declared->kind == symbol_kind_t::PROPERTY) {
            const operand_t object = lower_value(node.arguments[0].value);
            return get_property(*member.symbol, object, type_of(object));
        }

        // A ReadOnly Field changes only in its Class's New, which a loop's copy can't tell it's
        // in, so f.Set can't change one.
        if (name == "Set" && declared != nullptr && declared->modifiers.is_read_only) {
            if (!m_module.error) {
                m_module.error = mir_error_t{
                    .message =
                        std::format("'{}' is ReadOnly, so f.Set can't change it", declared->name),
                    .span = span_of(id),
                };
            }

            return std::nullopt;
        }

        // Nor one that visibility hides from the code, which couldn't assign it by name: what a
        // type keeps to itself stays as its own code has it.
        if (name == "Set" && declared != nullptr) {
            if (const std::optional<std::string> why = unsettable(*declared, span_of(id))) {
                if (!m_module.error) {
                    m_module.error = mir_error_t{ .message = *why, .span = span_of(id) };
                }

                return std::nullopt;
            }
        }

        if (name == "Get" || name == "Set") {
            place_t place = lower_place(node.arguments[0].value);
            place.steps.push_back({ .field = member.index });
            if (name == "Get") {
                return place;
            }

            // The variable's object, if it's in one, goes before the value.
            if (may_change(node.arguments[1].value)) {
                place = settled_place(place, false);
            }

            store(place, lower_value(node.arguments[1].value));
            return std::nullopt;
        }

        if (name == "Call") {
            std::vector<expr_id_t> operands;
            operands.reserve(node.arguments.size());
            for (const argument_t& argument : node.arguments) {
                operands.push_back(argument.value);
            }

            const std::vector<bool> later = changes_after(operands);
            const symbol_id_t method = expect(member.symbol);
            const operand_t value = settled(lower_value(operands[0]), later[0]);
            const type_id_t type = type_of(value);
            call_t call{
                .function = function_of(method, implementation_arguments(type, method)),
                .arguments = { { .value = value } },
            };
            for (std::size_t i = 1; i < operands.size(); ++i) {
                call.arguments.push_back({ .value = settled(lower_value(operands[i]), later[i]) });
            }

            if (is_virtual(m_analysis.symbol(method))) {
                call.slot = slot_of(method);
            }

            // A reflected Method doesn't throw, so its error variable is never set.
            give_error_variable(call, method);
            const type_id_t result = types().get(method_type(type, method)).inner;
            if (result == type_id_t::NONE) {
                emit(evaluate_t{ .value = std::move(call) });
                return std::nullopt;
            }

            return temporary(std::move(call), result, has_references(result));
        }

        return attribute_of(id, declared != nullptr ? &declared->attributes : nullptr,
                            name == "Has");
    }

    /// "Has(A)", whether the attributes have an A, or "Attribute(A)", its values, or Null.
    operand_t attribute_of(const expr_id_t id, const std::vector<attribute_value_t>* attributes,
                           const bool has) {
        const type_id_t attribute = m_analysis.expr(id).tested;
        const attribute_value_t* found = nullptr;
        if (attributes != nullptr) {
            const auto at = std::ranges::find(*attributes, attribute, &attribute_value_t::type);
            found = at != attributes->end() ? &*at : nullptr;
        }

        if (has) {
            return const_value_t{ .type = type_id_t::BOOL, .data = found != nullptr };
        }

        if (found == nullptr) {
            return null_of(expr_type(id));
        }

        aggregate_t value{ .type = attribute };
        for (const const_value_t& field : found->values) {
            value.fields.emplace_back(concrete(field));
        }

        return temporary(std::move(value), attribute, has_references(attribute));
    }

    /// Calling a function value runs its object's code.
    std::optional<operand_t> invoke(const call_expr_t& node, const span_t span) {
        std::vector<expr_id_t> operands{ node.callee };
        for (const argument_t& argument : node.arguments) {
            operands.push_back(argument.value);
        }

        const std::vector<bool> later = changes_after(operands);
        const operand_t callee = settled(lower_value(node.callee), later.front());
        invoke_t call{ .callee = callee, .span = span };
        for (std::size_t i = 0; i < node.arguments.size(); ++i) {
            call.arguments.push_back(settled(lower_value(node.arguments[i].value), later[i + 1]));
        }

        const type_id_t result = types().get(type_of(callee)).inner;
        if (result == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
            return std::nullopt;
        }

        operand_t given = temporary(std::move(call), result, has_references(result));
        // An Enum C's code gives has to be one of its values, as an Extern Function's does.
        if (types().get(type_of(callee)).kind == type_kind_t::CALLBACK && is_enum(result) &&
            !types().get(result).flags) {
            return to_enum(given, result, span,
                           std::format("a Callback gave a value that isn't one of {}'s",
                                       types().name(result)));
        }

        return given;
    }

    /// A call's arguments in the order they're written, packed ones too.
    [[nodiscard]] std::vector<expr_id_t> written_arguments(const expr_id_t id) const {
        const expr_info_t& info = m_analysis.expr(id);
        const auto& node = std::get<call_expr_t>(m_ast.get(id).node);
        std::vector<expr_id_t> written;
        written.reserve(node.arguments.size());
        for (const argument_t& argument : node.arguments) {
            if (std::ranges::contains(info.arguments, std::optional(argument.value)) ||
                std::ranges::contains(info.packed, argument.value)) {
                written.push_back(argument.value);
            }
        }

        return written;
    }

    /// Whether a call's arguments could change what its object reads.
    [[nodiscard]] bool arguments_change(const expr_id_t id) const {
        return std::ranges::any_of(written_arguments(id), [this](const expr_id_t argument) {
            return may_change(argument);
        });
    }

    /// A Method's object, which goes before its arguments: the variable itself for a ByRef
    /// Method, reached through what it was in, else its value, copied first if an argument could
    /// change it. After a '?.', it's `receiver`, and called by name, it's Self.
    operand_t object_of(const std::optional<operand_t>& receiver, const member_expr_t* member,
                        const bool by_ref, const bool changes_later) {
        if (receiver) {
            return by_ref ? *receiver : settled(*receiver, changes_later);
        }

        if (member == nullptr) {
            const operand_t self = place_t{ expect(m_self) };
            return by_ref ? self : settled(self, changes_later);
        }

        if (by_ref) {
            const place_t place = lower_place(member->object);
            return changes_later ? settled_place(place, false) : place;
        }

        return settled(lower_value(member->object), changes_later);
    }

    /// A call's arguments for its parameters in order, after any object in `arguments`. They're
    /// evaluated in the order they're written, each copied first if one written after it could
    /// change what it reads. A missing one gets the parameter's default, which is known at
    /// compile time, or a field's zero, and a variadic parameter gets its packed ones, which are
    /// written last. ByRef passes the variable itself.
    void lower_arguments(const expr_id_t id, const symbol_t& function,
                         const std::vector<type_id_t>& type_arguments,
                         std::vector<call_argument_t>& arguments) {
        const expr_info_t& info = m_analysis.expr(id);
        std::vector<std::optional<call_argument_t>> lowered(info.arguments.size());
        const auto lower_one = [&](const std::size_t index, const bool changes_later) {
            const expr_id_t argument = expect(info.arguments[index]);
            if (!m_analysis.symbol(function.parameters[index]).by_ref) {
                lowered[index] = { .value = settled(lower_value(argument), changes_later) };
                return;
            }

            const place_t place = lower_place(argument);
            lowered[index] = {
                .value = changes_later ? settled_place(place, false) : place,
                .by_ref = true,
            };
        };
        const std::vector<expr_id_t> written = written_arguments(id);
        const std::vector<bool> later = changes_after(written);
        for (std::size_t i = 0; i < written.size(); ++i) {
            const auto found = std::ranges::find(info.arguments, std::optional(written[i]));
            if (found != info.arguments.end()) {
                lower_one(static_cast<std::size_t>(found - info.arguments.begin()), later[i]);
            }
        }

        const std::vector<type_id_t> type_parameters = type_parameters_of(function);
        for (std::size_t i = 0; i < info.arguments.size(); ++i) {
            if (!lowered[i] && info.arguments[i]) {
                lower_one(i, false);
            }

            if (const std::optional<call_argument_t>& argument = lowered[i]) {
                arguments.push_back(*argument);
                continue;
            }

            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            const type_id_t type =
                types().substitute(parameter.type, type_parameters, type_arguments);
            operand_t value;
            if (parameter.shows) {
                value = texts_of(info.packed, type);
            } else if (parameter.variadic) {
                value = array_of(info.packed, type);
            } else {
                value = default_of(parameter, type);
            }

            arguments.push_back({ .value = value });
        }
    }

    /// A call of an Extern Function. A String for a CString parameter goes as a copy with a NUL
    /// after it, which C code reads during the call, and which is freed right after it. A String
    /// C gives is a copy of its text, made before that, and Null for a String? where C gives
    /// NULL, which panics for a String.
    std::optional<operand_t>
    extern_call(const symbol_id_t id, std::vector<call_argument_t> arguments, const span_t span) {
        const symbol_t& function = m_analysis.symbol(id);
        const external_t* external = external_of(function);
        const bool text = external != nullptr && external->convention != convention_t::RUNTIME &&
                          is_text(function.type);
        std::vector<operand_t> copies;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (m_analysis.symbol(function.parameters[i]).converts_string &&
                type_of(arguments[i].value) == type_id_t::STRING) {
                arguments[i].value = runtime_call(runtime_function_t::CSTRING_NEW,
                                                  { arguments[i].value }, type_id_t::CSTRING);
                copies.push_back(arguments[i].value);
            }
        }

        std::vector<operand_t> called;
        called.reserve(arguments.size());
        for (const call_argument_t& argument : arguments) {
            called.push_back(argument.value);
        }

        call_t call{ .function = function_of(id), .arguments = std::move(arguments) };
        std::optional<operand_t> result;
        if (function.gives_task) {
            // The call makes the Task, which the runtime finishes later.
            const type_id_t type = concrete(function.type);
            generated(generated_kind_t::RUN, type);
            const operand_t task = temporary(new_t{ .type = type }, type, true);
            emit(evaluate_t{
                .value =
                    runtime_call_t{
                        .function = runtime_function_t::ASYNC_BEGIN,
                        .arguments = { task },
                    },
            });
            call.arguments.insert(call.arguments.begin(), { .value = task });
            emit(evaluate_t{ .value = std::move(call) });
            result = task;
        } else if (function.type == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
        } else if (text) {
            const operand_t given = temporary(std::move(call), type_id_t::CSTRING);
            result = runtime_call(runtime_function_t::STRING_FROM_CSTRING, { given },
                                  types().nullable(type_id_t::STRING), true);
        } else {
            result = temporary(std::move(call), function.type, has_references(function.type));
        }

        for (const operand_t& copy : copies) {
            const runtime_call_t free{
                .function = runtime_function_t::CSTRING_FREE,
                .arguments = { copy },
            };
            emit(evaluate_t{ .value = free });
        }

        // An Enum C gives, or sets ByRef, has to be one of its values.
        const auto checked_enum = [&](const type_id_t type) {
            return is_enum(type) && !types().get(type).flags;
        };
        const std::string gave =
            std::format("'{}' gave a value that isn't one of {}'s", function.name,
                        checked_enum(function.type) ? types().name(function.type) : "");
        if (result && checked_enum(function.type)) {
            result = to_enum(*result, function.type, span, gave);
        }

        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            if (parameter.by_ref && checked_enum(parameter.type)) {
                (void)to_enum(called[i], parameter.type, span,
                              std::format("'{}' set '{}' to a value that isn't one of {}'s",
                                          function.name, parameter.name,
                                          types().name(parameter.type)));
            }
        }

        // And so is each one in a Struct it gives or sets ByRef.
        if (result && is_struct(function.type)) {
            const std::string gives(types().name(function.type));
            check_enum_fields(std::get<place_t>(*result), function.type, {}, span,
                              [&](const std::string& path, const std::string_view enumeration) {
                                  return std::format(
                                      "'{}' gave {}'s Field {} a value that isn't one of {}'s",
                                      function.name, gives, path, enumeration);
                              });
        }

        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[i]);
            if (parameter.by_ref && is_struct(parameter.type)) {
                check_enum_fields(
                    std::get<place_t>(called[i]), parameter.type, {}, span,
                    [&](const std::string& path, const std::string_view enumeration) {
                        return std::format(
                            "'{}' set Field {} of '{}' to a value that isn't one of {}'s",
                            function.name, path, parameter.name, enumeration);
                    });
            }
        }

        if (result && text && function.type == type_id_t::STRING) {
            const operand_t given = has_value(*result, span);
            const block_id_t fail = new_block();
            const block_id_t ok = new_block();
            terminate(branch_t{ .condition = given, .if_true = ok, .if_false = fail });
            start(fail);
            terminate(panic_t{
                .message = std::format("'{}' gave a null CString for its String", function.name),
                .span = span,
            });
            start(ok);
            result = unwrap(*result);
        }

        return result;
    }

    /// A call, on `receiver` when it's a Method called after a '?.'.
    std::optional<operand_t> call_on(const expr_id_t id, const std::optional<operand_t>& receiver) {
        const expr_info_t& info = m_analysis.expr(id);
        const symbol_t& function = m_analysis.symbol(expect(info.symbol));
        const auto& node = std::get<call_expr_t>(m_ast.get(id).node);
        if (function.kind == symbol_kind_t::BUILTIN_FUNCTION &&
            (function.name == "Panic" || function.name == "Assert" ||
             function.name == "DebugAssert" || function.name == "Unreachable" ||
             function.name == "Todo")) {
            lower_panic(id, function);
            return std::nullopt;
        }

        if (function.owner != type_id_t::INVALID && is_interface(function.owner)) {
            return call_interface_member(id, receiver);
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
            const operand_t value =
                object_of(receiver, member, function.by_ref, arguments_change(id));
            if (function.by_ref) {
                arguments.push_back({ .value = as_place(value), .by_ref = true });
            } else {
                arguments.push_back({ .value = value });
            }

            object = type_of(value);
        }

        const std::vector<type_id_t> type_arguments = call_arguments(function, info, object);
        lower_arguments(id, function, type_arguments, arguments);
        if (external_of(function) != nullptr) {
            return extern_call(expect(info.symbol), std::move(arguments), span_of(id));
        }

        // A T[]'s own Methods, and a @Flags Enum's Has, are built in; an Extend's aren't.
        if (function.kind == symbol_kind_t::METHOD && is_array(function.owner) &&
            !function.declaration) {
            return array_method(function, arguments, span_of(id));
        }

        // So are an integer's Wrapping and Checked Methods.
        if (function.kind == symbol_kind_t::METHOD && is_integer(function.owner) &&
            !function.declaration) {
            return integer_call(function, arguments, span_of(id));
        }

        // And an Atomic's, a Task's, and a Thread's.
        if (function.kind == symbol_kind_t::METHOD &&
            types().get(function.owner).kind == type_kind_t::ATOMIC) {
            return atomic_call(function, arguments);
        }

        if (function.kind == symbol_kind_t::METHOD && is_task(function.owner) &&
            !function.declaration) {
            return task_call(function, arguments, span_of(id));
        }

        if (function.kind == symbol_kind_t::METHOD && is_nullable(function.owner) &&
            !function.declaration) {
            return nullable_call(function, arguments, span_of(id));
        }

        if (function.kind == symbol_kind_t::METHOD && is_channel(function.owner) &&
            !function.declaration) {
            return channel_call(function, arguments, span_of(id));
        }

        if (function.kind == symbol_kind_t::METHOD && is_scheduler(function.owner)) {
            return scheduler_call(function, arguments);
        }

        // A @Flags Enum's Has: whether every one of the flags is set.
        if (function.kind == symbol_kind_t::METHOD && is_enum(function.owner) &&
            !function.declaration) {
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

        if (function.kind == symbol_kind_t::BUILTIN_FUNCTION) {
            return timing_call(id, function, arguments);
        }

        // An Async Function's call makes its Task and starts its coroutine, which runs until it
        // first Awaits. The Task comes first, after a Method's object. "a?.M()" gives a Task<T>?.
        if (function.modifiers.is_async) {
            const type_id_t type = concrete(non_null(expr_type(id)));
            generated(generated_kind_t::RUN, type);
            operand_t task = temporary(new_t{ .type = type }, type, true);
            runtime_call_t begin{
                .function = runtime_function_t::ASYNC_BEGIN,
                .arguments = { task },
            };
            emit(evaluate_t{ std::move(begin) });
            const bool has_self =
                function.kind == symbol_kind_t::METHOD && !function.modifiers.is_static;
            arguments.insert(arguments.begin() + (has_self ? 1 : 0), { .value = task });
            call_t start{
                .function = function_of(expect(info.symbol), type_arguments),
                .arguments = std::move(arguments),
            };
            if (is_virtual(function) && !on_super) {
                start.slot = slot_of(expect(info.symbol));
            }

            emit(evaluate_t{ std::move(start) });
            return task;
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
        if (takes_error(expect(info.symbol))) {
            error = error_variable();
            call.arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        const type_id_t result_type =
            types().substitute(function.type, type_parameters_of(function), type_arguments);
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

    /// A call of an Interface's member. Through an Interface's object, it goes through the table
    /// the object's Class has for the Interface. Anywhere else, the type for Self is known, and
    /// the call goes to its member that implements this one.
    std::optional<operand_t> call_interface_member(const expr_id_t id,
                                                   const std::optional<operand_t>& receiver) {
        const expr_info_t& info = m_analysis.expr(id);
        const symbol_id_t required = expect(info.symbol);
        const symbol_t& function = m_analysis.symbol(required);
        const auto& node = std::get<call_expr_t>(m_ast.get(id).node);
        const auto* member = std::get_if<member_expr_t>(&m_ast.get(node.callee).node);
        // The type for Self, then a generic Interface's type arguments.
        std::vector<type_id_t> type_arguments;
        type_arguments.reserve(info.type_arguments.size());
        for (const type_id_t argument : info.type_arguments) {
            type_arguments.push_back(concrete(argument));
        }

        const type_id_t self = type_arguments.front();
        const bool dynamic = is_interface(self) && !outside_table(function);
        const std::optional<symbol_id_t> found = dynamic || is_interface(self)
                                                     ? std::optional(required)
                                                     : implementation(self, required);
        // A built-in type, and a Struct or Class without its own Hash, has the built-in one.
        if (!found) {
            std::optional<operand_t> object = receiver;
            if (!object && member == nullptr && !function.modifiers.is_static) {
                object = place_t{ expect(m_self) };
            } else if (!object && !function.modifiers.is_static) {
                object = lower_value(member->object);
            }

            return builtin_method(function, self, object);
        }

        const symbol_id_t target = *found;
        const symbol_t& callee = m_analysis.symbol(target);

        // The object first, or the variable itself for a Struct's ByRef Method. Called by name,
        // it's Self.
        std::vector<call_argument_t> arguments;
        if (!callee.modifiers.is_static) {
            const bool by_ref = callee.by_ref && !is_object(self);
            const operand_t value = object_of(receiver, member, by_ref, arguments_change(id));
            arguments.push_back(by_ref ? call_argument_t{ .value = as_place(value), .by_ref = true }
                                       : call_argument_t{ .value = value });
        }

        lower_arguments(id, function, type_arguments, arguments);

        // Through an object, the call has the Interface for Self, and goes through its table
        // for the instance of a generic one.
        const type_id_t interface = interface_instance(function, type_arguments);
        std::vector<type_id_t> abstract{ interface };
        abstract.insert(abstract.end(), type_arguments.begin() + 1, type_arguments.end());
        // A generic Method's own type arguments come after the Interface's.
        std::vector<type_id_t> concrete_arguments = implementation_arguments(self, target);
        const std::size_t owned = types().get(function.owner).parameters.size() + 1;
        if (type_arguments.size() > owned) {
            concrete_arguments.insert(concrete_arguments.end(),
                                      type_arguments.begin() + static_cast<std::ptrdiff_t>(owned),
                                      type_arguments.end());
        }

        call_t call{
            .function = function_of(target, is_interface(self) ? abstract : concrete_arguments),
            .arguments = std::move(arguments),
        };
        if (dynamic) {
            call.slot = interface_slot(required);
            call.interface = interface;
        } else if (is_virtual(callee)) {
            call.slot = slot_of(target);
        }

        if (function.modifiers.is_async) {
            return start_async_member(id, std::move(call), !callee.modifiers.is_static);
        }

        // A table's slot is called as the Interface's Method is, and a Method itself as it is.
        std::optional<local_id_t> error;
        if (takes_error(dynamic ? required : target)) {
            error = error_variable();
            call.arguments.push_back({ .value = place_t{ *error }, .by_ref = true });
        }

        const type_id_t result =
            types().substitute(function.type, type_parameters_of(function), type_arguments);
        // A Struct the type's Method gives where the Interface's gives an Iterator<T> goes in a
        // box, and a Class's object is one already.
        const type_id_t given =
            is_interface(self)
                ? result
                : types().substitute(callee.type, type_parameters_of(callee), concrete_arguments);
        std::optional<operand_t> value;
        if (result == type_id_t::NONE) {
            emit(evaluate_t{ .value = std::move(call) });
        } else {
            value = temporary(std::move(call), given);
        }

        if (error) {
            check_error(*error, span_of(id));
        }

        if (value && has_references(given)) {
            m_temporaries.push_back(std::get<local_id_t>(std::get<place_t>(*value).root));
        }

        if (value && given != result) {
            value = boxed(*value, result, span_of(id));
        }

        return value;
    }

    /// An Async Interface member's call, through its table or to the type's own Method, as an
    /// Async Method's is: it makes the Task, which goes after the object when `has_self`, and
    /// starts the coroutine, which runs until it first Awaits. The call gives the Task.
    operand_t start_async_member(const expr_id_t id, call_t call, const bool has_self) {
        const type_id_t type = concrete(non_null(expr_type(id)));
        generated(generated_kind_t::RUN, type);
        operand_t task = temporary(new_t{ .type = type }, type, true);
        runtime_call_t begin{
            .function = runtime_function_t::ASYNC_BEGIN,
            .arguments = { task },
        };
        emit(evaluate_t{ std::move(begin) });
        call.arguments.insert(call.arguments.begin() + (has_self ? 1 : 0), { .value = task });
        emit(evaluate_t{ std::move(call) });
        return task;
    }

    /// The Interface an Interface's member belongs to, with the type arguments after Self in
    /// `type_arguments`: an instance for a generic one.
    type_id_t interface_instance(const symbol_t& member,
                                 const std::vector<type_id_t>& type_arguments) {
        const std::size_t owned = types().get(definition_of(member.owner)).parameters.size();
        return types().instance(member.owner,
                                std::vector<type_id_t>(type_arguments.begin() + 1,
                                                       type_arguments.begin() + 1 +
                                                           static_cast<std::ptrdiff_t>(owned)));
    }

    /// The instance of a generic Interface among an Interface and the ones it extends, like the
    /// Iterator<Int32> of "Numbers Extends Iterator<Int32>". One that isn't generic is its own.
    [[nodiscard]] type_id_t extended_instance(const type_id_t interface,
                                              const type_id_t definition) const {
        std::vector<type_id_t> pending{ interface };
        while (!pending.empty()) {
            const type_id_t current = pending.back();
            pending.pop_back();
            if (definition_of(current) == definition) {
                return current;
            }

            std::ranges::copy(types().get(current).interfaces, std::back_inserter(pending));
        }

        return definition;
    }

    /// 'As' between Classes checks the object's Class at run time, unless it converts anyway.
    /// So does 'As' from a T? to a T, for Null.
    operand_t lower_expr(const expr_id_t id, const cast_expr_t& node) {
        const operand_t value = lower_value(node.value);
        const type_id_t type = expr_type(id);
        use_class(type);
        // 'As?' can give Null, so it borrows the object, whose temporary keeps its reference.
        if (node.checked) {
            return temporary(
                downcast_t{ .value = value, .type = type, .span = span_of(id), .or_null = true },
                type);
        }

        const type_t& from = types().get(type_of(value));
        const type_id_t source = from.kind == type_kind_t::NULLABLE ? from.inner : type_of(value);
        const type_id_t target = is_nullable(type) ? types().get(type).inner : type;
        const bool checks = is_object(source) && is_object(target) &&
                            (!upcasts(source, target) ||
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

    /// Whether a value of `type` has an Enum inside that C could set to another value: one that
    /// isn't @Flags, in a Struct's fields or a fixed array's elements.
    [[nodiscard]] bool holds_checked_enum(const type_id_t type) const {
        if (is_enum(type)) {
            return !types().get(type).flags;
        }

        if (is_fixed_array(type)) {
            return holds_checked_enum(types().get(type).inner);
        }

        return is_struct(type) &&
               std::ranges::any_of(types().get(type).fields, [this](const field_t& field) {
                   return holds_checked_enum(field.type);
               });
    }

    /// Checks each Enum in a Struct that C gave, at `path` inside it, whose fields and fixed
    /// arrays' elements it walks: each has to be one of its values, or the program panics with
    /// what `message` says of the Field's path and the Enum's name.
    template <typename Message>
    void check_enum_fields(const place_t& place, const type_id_t type, const std::string& path,
                           const span_t span, const Message& message) {
        if (!holds_checked_enum(type)) {
            return;
        }

        if (is_enum(type)) {
            (void)to_enum(place, type, span, message(path, types().name(type)));
            return;
        }

        if (is_fixed_array(type)) {
            const local_id_t counter = add_local(type_id_t::INT32);
            emit(assign_t{ .target = counter, .value = use_t{ .value = zero(type_id_t::INT32) } });
            const block_id_t head = new_block();
            jump(head);
            start(head);
            const auto count = static_cast<std::int64_t>(types().element_count(type));
            const operand_t in_range =
                compare(binary_op_t::LESS, counter,
                        const_value_t{ .type = type_id_t::INT32, .data = count }, span);
            const block_id_t loop = new_block();
            const block_id_t exit = new_block();
            terminate(branch_t{ .condition = in_range, .if_true = loop, .if_false = exit });
            start(loop);
            place_t element = place;
            element.steps.push_back({ .indices = { counter }, .span = span });
            check_enum_fields(element, types().get(type).inner, path, span, message);
            count_up(counter, span);
            jump(head);
            start(exit);
            return;
        }

        const std::vector<field_t>& fields = types().get(type).fields;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            place_t field = place;
            field.steps.push_back({ .field = static_cast<std::uint32_t>(i), .span = span });
            check_enum_fields(field, fields[i].type,
                              path.empty() ? fields[i].name : path + "." + fields[i].name, span,
                              message);
        }
    }

    /// An integer as one of an Enum's values, which it has to be, or the program panics with
    /// `message`. Values in a run need only a check that it's in range.
    operand_t to_enum(const operand_t& value, const type_id_t type, const span_t span,
                      std::string message = {}) {
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
        // An Enum sema found no values for has none to be.
        bool run = !values.empty();
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
        if (message.empty()) {
            message = std::format("the value isn't one of {}'s", types().name(type));
        }

        terminate(panic_t{ .message = std::move(message), .span = span });
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
        const std::string name = types().qualified_name(type);
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

mir_module_t lower_function(const ast_t& ast, const analysis_t& analysis,
                            const symbol_id_t function,
                            const std::vector<type_id_t>& type_arguments) {
    return lowerer_t(ast, analysis).run_one(function, type_arguments);
}

} // namespace fb
