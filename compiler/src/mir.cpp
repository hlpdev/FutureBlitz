#include "fb/mir.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// The MIR types are all in mir.hpp. This file prints a module as text, for --show mir and the
// tests.

namespace fb {
namespace {

std::string_view runtime_name(const runtime_function_t function) {
    switch (function) {
    case runtime_function_t::CONCAT:
        return "concat";
    case runtime_function_t::JOIN:
        return "join";
    case runtime_function_t::EQUAL:
        return "equal";
    case runtime_function_t::TO_TEXT:
        return "to_text";
    case runtime_function_t::CSTRING_NEW:
        return "cstring_new";
    case runtime_function_t::CSTRING_FREE:
        return "cstring_free";
    case runtime_function_t::STRING_FROM_CSTRING:
        return "string_from_cstring";
    case runtime_function_t::ARRAY_COUNT:
        return "count";
    case runtime_function_t::ARRAY_ADD:
        return "add";
    case runtime_function_t::ARRAY_INSERT:
        return "insert";
    case runtime_function_t::ARRAY_REMOVE_AT:
        return "remove_at";
    case runtime_function_t::ARRAY_CLEAR:
        return "clear";
    case runtime_function_t::ARRAY_CLONE:
        return "clone";
    case runtime_function_t::ARRAY_CAPACITY:
        return "capacity";
    case runtime_function_t::ARRAY_RESERVE:
        return "reserve";
    case runtime_function_t::ARRAY_TRIM:
        return "trim";
    case runtime_function_t::ERROR_UNCAUGHT:
        return "uncaught";
    case runtime_function_t::ERROR_THROWN:
        return "thrown";
    case runtime_function_t::DEBUG_BUILD:
        return "debug_build";
    case runtime_function_t::WEAK_LOAD:
        return "load_weak";
    case runtime_function_t::OBJECT_DISCARD:
        return "discard";
    case runtime_function_t::COMPARE:
        return "compare";
    case runtime_function_t::HASH:
        return "hash";
    case runtime_function_t::HASH_COMBINE:
        return "hash_combine";
    case runtime_function_t::STRING_CHARS:
        return "string_chars";
    case runtime_function_t::STRING_BYTES:
        return "string_bytes";
    case runtime_function_t::TASK_START:
        return "task_start";
    case runtime_function_t::THREAD_START:
        return "thread_start";
    case runtime_function_t::TASK_WAIT:
        return "task_wait";
    case runtime_function_t::TASK_DONE:
        return "task_done";
    case runtime_function_t::PARALLEL_FOR:
        return "parallel_for";
    case runtime_function_t::CHANNEL_SEND:
        return "send";
    case runtime_function_t::CHANNEL_TRY_SEND:
        return "try_send";
    case runtime_function_t::CHANNEL_RECEIVE:
        return "receive";
    case runtime_function_t::CHANNEL_TRY_RECEIVE:
        return "try_receive";
    case runtime_function_t::CHANNEL_CLOSE:
        return "close";
    case runtime_function_t::ASYNC_BEGIN:
        return "async_begin";
    case runtime_function_t::ASYNC_FINISH:
        return "async_finish";
    case runtime_function_t::AWAIT:
        return "await";
    case runtime_function_t::WAIT_FRAME:
        return "wait_frame";
    case runtime_function_t::WAIT_SECONDS:
        return "wait_seconds";
    case runtime_function_t::SCHEDULER_RUN:
        return "scheduler_run";
    case runtime_function_t::SCHEDULER_TICK:
        return "scheduler_tick";
    case runtime_function_t::SCHEDULER_IDLE:
        return "scheduler_idle";
    }

    std::unreachable();
}

/// Constants print as they would in source code.
std::string constant(const const_value_t& value) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (text == nullptr) {
        return to_text(value);
    }

    std::string out = "\"";
    for (const char c : *text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }

    return out + '"';
}

/// Globals first, then each function: its locals, then its blocks.
class dumper_t {
public:
    dumper_t(const mir_module_t& module, const style_t& style) : m_module(module), m_style(style) {}

    std::string run() {
        std::string out;
        for (const global_t& global : m_module.globals) {
            const std::string initial =
                global.initial ? paint(literal(*global.initial), tone_t::LITERAL) : keyword("zero");
            out += std::format("{} {}: {} = {}\n", keyword("global"),
                               paint(global.name, tone_t::NAME), type(global.type), initial);
        }

        // "class Boss Extends Enemy, Destroy: Boss.Destroy()", then its Method table.
        for (const mir_class_t& info : m_module.classes) {
            out += std::format("{} {}", keyword("class"), type(info.type));
            if (const type_id_t base = m_module.types->get(info.type).base;
                base != type_id_t::INVALID) {
                out += std::format(" {} {}", keyword("Extends"), type(base));
            }

            if (info.destroy) {
                out += std::format(", Destroy: {}", function_name(*info.destroy));
            }

            out += '\n';
            for (std::size_t i = 0; i < info.methods.size(); ++i) {
                out += std::format("    {}: {}\n", i, function_name(info.methods[i]));
            }

            // Each Interface's table, like "Shape 0: Square.Area()".
            for (const mir_implementation_t& implementation : info.interfaces) {
                for (std::size_t i = 0; i < implementation.methods.size(); ++i) {
                    out += std::format("    {} {}: {}\n", type(implementation.interface), i,
                                       function_name(implementation.methods[i]));
                }
            }
        }

        // An array type's tables, like "array Int32[]" and "Iterable<Int32> 0: ...".
        for (const mir_class_t& info : m_module.array_classes) {
            out += std::format("{} {}\n", keyword("array"), type(info.type));
            for (const mir_implementation_t& implementation : info.interfaces) {
                for (std::size_t i = 0; i < implementation.methods.size(); ++i) {
                    out += std::format("    {} {}: {}\n", type(implementation.interface), i,
                                       function_name(implementation.methods[i]));
                }
            }
        }

        for (const mir_function_t& function : m_module.functions) {
            if (!out.empty()) {
                out += '\n';
            }

            out += dump(function);
        }

        return out;
    }

private:
    const mir_module_t& m_module;
    const style_t& m_style;
    /// The function being dumped, whose locals places refer to.
    const mir_function_t* m_function = nullptr;

    [[nodiscard]] std::string paint(const std::string_view text, const tone_t tone) const {
        return m_style.paint(text, tone);
    }

    [[nodiscard]] std::string keyword(const std::string_view text) const {
        return paint(text, tone_t::KEYWORD);
    }

    /// Types outside the main module start with their module, as LLVM names them.
    [[nodiscard]] std::string type(const type_id_t id) const {
        return paint(m_module.types->qualified_name(id), tone_t::TYPE);
    }

    /// A constant. One of a T? prints as its T would, or as Null, and one of an Enum by its
    /// member's name, like "State.Walk", or else as its integer.
    [[nodiscard]] std::string literal(const const_value_t& value) const {
        const type_t& info = m_module.types->get(value.type);
        if (info.kind == type_kind_t::ENUM) {
            for (const enum_value_t& member : info.values) {
                if (const_value_t{ .type = value.type, .data = member_data(member, info) } ==
                    value) {
                    return std::format("{}.{}", m_module.types->qualified_name(value.type),
                                       member.name);
                }
            }

            return to_text(value);
        }

        if (info.kind == type_kind_t::NULLABLE &&
            !std::holds_alternative<std::nullptr_t>(value.data)) {
            return constant({ .type = info.inner, .data = value.data });
        }

        return constant(value);
    }

    [[nodiscard]] std::string function_name(const function_id_t id) const {
        return paint(m_module.functions[std::to_underlying(id)].name, tone_t::BOLD);
    }

    /// An Enum member's bits as a constant's data: signed for a signed integer type.
    static decltype(const_value_t::data) member_data(const enum_value_t& member,
                                                     const type_t& info) {
        if (is_signed(info.inner)) {
            return static_cast<std::int64_t>(member.bits);
        }

        return member.bits;
    }

    [[nodiscard]] std::string label(const block_id_t block) const {
        return paint(std::format("bb{}", std::to_underlying(block)), tone_t::BOLD);
    }

    [[nodiscard]] std::string dump(const mir_function_t& function) {
        m_function = &function;
        std::string out = function.is_inline ? keyword("@Inline") + " " : std::string();
        if (function.panics_at_caller) {
            out += keyword("@PanicsAtCaller") + " ";
        }

        if (function.external) {
            out += keyword(function.external->c_abi ? "extern" : "extern runtime") + " ";
        }

        out += keyword("function") + " " + paint(function.name, tone_t::BOLD);
        if (function.return_type != type_id_t::NONE) {
            out += " -> " + type(function.return_type);
        }

        if (function.external) {
            out += " = " + paint(std::format("\"{}\"", function.external->symbol), tone_t::LITERAL);
        }

        out += '\n';
        for (std::size_t i = 0; i < function.locals.size(); ++i) {
            // "_0 a: Int32", "_1 ByRef b: Int32", or "_2: Int32" for a temporary
            const local_t& local = function.locals[i];
            std::string name;
            if (!local.name.empty()) {
                name = std::format(" {}{}", local.by_ref ? keyword("ByRef") + " " : "", local.name);
            }

            out += std::format("    {}{}: {}\n", paint(std::format("_{}", i), tone_t::NAME), name,
                               type(local.type));
        }

        for (std::size_t i = 0; i < function.blocks.size(); ++i) {
            out += paint(std::format("bb{}:", i), tone_t::BOLD) + '\n';
            for (const statement_t& statement : function.blocks[i].statements) {
                out +=
                    "    " +
                    std::visit([this](const auto& node) { return this->dump(node); }, statement) +
                    '\n';
            }

            out += "    " +
                   std::visit([this](const auto& node) { return this->dump(node); },
                              function.blocks[i].terminator) +
                   '\n';
        }

        return out;
    }

    /// "_3" for a local and "@score" for a global, with any fields and elements after:
    /// "_3.pos.x" or "_3[_4]". The T in a T? is its "value".
    [[nodiscard]] std::string place(const place_t& target) const {
        std::string text;
        type_id_t current = type_id_t::INVALID;
        if (const auto* local = std::get_if<local_id_t>(&target.root)) {
            text = std::format("_{}", std::to_underlying(*local));
            current = m_function->locals[std::to_underlying(*local)].type;
        } else {
            const global_t& global =
                m_module.globals[std::to_underlying(std::get<global_id_t>(target.root))];
            text = std::format("@{}", global.name);
            current = global.type;
        }

        for (const step_t& step : target.steps) {
            const type_t& info = m_module.types->get(current);
            if (step.indices.empty()) {
                text += "." + info.fields[step.field].name;
                current = info.fields[step.field].type;
                continue;
            }

            std::string indices;
            for (const local_id_t local : step.indices) {
                indices +=
                    std::format("{}_{}", indices.empty() ? "" : ", ", std::to_underlying(local));
            }

            text += std::format("[{}]", indices);
            current = info.inner;
        }

        return paint(text, tone_t::NAME);
    }

    [[nodiscard]] std::string operand(const operand_t& value) const {
        if (const auto* constant_value = std::get_if<const_value_t>(&value)) {
            return paint(literal(*constant_value), tone_t::LITERAL);
        }

        return place(std::get<place_t>(value));
    }

    [[nodiscard]] std::string rvalue(const rvalue_t& value) const {
        return std::visit([this](const auto& node) { return this->dump(node); }, value);
    }

    // Rvalues

    [[nodiscard]] std::string dump(const use_t& node) const {
        return operand(node.value);
    }

    [[nodiscard]] std::string dump(const unary_t& node) const {
        return std::format("{}{}", spelling(node.op), operand(node.operand));
    }

    [[nodiscard]] std::string dump(const binary_t& node) const {
        const std::string text =
            std::format("{} {} {}", operand(node.left), spelling(node.op), operand(node.right));
        if (node.overflows) {
            return std::format("{}({})", keyword("overflows"), text);
        }

        return node.wrapping ? std::format("{}({})", keyword("wrapping"), text) : text;
    }

    [[nodiscard]] std::string dump(const address_of_t& node) const {
        return std::format("{} {}", keyword("AddressOf"), operand(node.place));
    }

    [[nodiscard]] std::string dump(const callback_t& node) const {
        return std::format("{} {}{}", keyword("Callback"),
                           m_module.functions[std::to_underlying(node.function)].name,
                           node.closure ? " (closure)" : "");
    }

    [[nodiscard]] std::string dump(const pointer_t& node) const {
        return std::format("{} {} {} {}", keyword("Ptr"), operand(node.left), spelling(node.op),
                           operand(node.right));
    }

    [[nodiscard]] std::string dump(const cast_t& node) const {
        return std::format("{} {} {}", operand(node.value), keyword("As"), type(node.type));
    }

    [[nodiscard]] std::string dump(const call_t& node) const {
        std::string arguments;
        for (const call_argument_t& argument : node.arguments) {
            arguments += arguments.empty() ? "" : ", ";
            arguments += argument.by_ref ? keyword("ByRef") + " " : "";
            arguments += operand(argument.value);
        }

        // "call virtual" goes through the object's Method table, and "call interface" through
        // the table its Class has for an Interface.
        std::string_view call = node.slot ? "call virtual" : "call";
        if (node.interface != type_id_t::INVALID) {
            call = "call interface";
        }

        return std::format("{} {}({})", keyword(call), function_name(node.function), arguments);
    }

    /// "invoke _3(1, _2)"
    [[nodiscard]] std::string dump(const invoke_t& node) const {
        std::string arguments;
        for (const operand_t& argument : node.arguments) {
            arguments += (arguments.empty() ? "" : ", ") + operand(argument);
        }

        return std::format("{} {}({})", keyword("invoke"), operand(node.callee), arguments);
    }

    [[nodiscard]] std::string dump(const runtime_call_t& node) const {
        std::string arguments;
        for (const operand_t& argument : node.arguments) {
            arguments += arguments.empty() ? "" : ", ";
            arguments += operand(argument);
        }

        return std::format("{}({})", paint(runtime_name(node.function), tone_t::BOLD), arguments);
    }

    /// "Point { x: _1, y: 2 }", or "(_1, 2)" for a tuple.
    [[nodiscard]] std::string dump(const aggregate_t& node) const {
        const type_t& info = m_module.types->get(node.type);
        if (info.kind == type_kind_t::TUPLE) {
            std::string elements;
            for (const operand_t& element : node.fields) {
                elements += (elements.empty() ? "" : ", ") + operand(element);
            }

            return std::format("({})", elements);
        }

        std::string fields;
        for (std::size_t i = 0; i < node.fields.size(); ++i) {
            fields += i == 0 ? "" : ", ";
            fields += std::format("{}: {}", info.fields[i].name, operand(node.fields[i]));
        }

        return std::format("{} {{ {} }}", type(node.type), fields);
    }

    /// "Int32[] [1, _2]"
    [[nodiscard]] std::string dump(const array_t& node) const {
        std::string elements;
        for (const operand_t& element : node.elements) {
            elements += (elements.empty() ? "" : ", ") + operand(element);
        }

        return std::format("{} [{}]", type(node.type), elements);
    }

    [[nodiscard]] std::string dump(const zero_t& node) const {
        return std::format("{} {}", keyword("zero"), type(node.type));
    }

    [[nodiscard]] std::string dump(const new_t& node) const {
        return std::format("{} {}", keyword("new"), type(node.type));
    }

    /// "atomic add _1.hits(1)", or "atomic make Atomic<Int32>(0)".
    [[nodiscard]] std::string dump(const atomic_t& node) const {
        constexpr auto names = std::to_array<std::string_view>(
            { "make", "load", "store", "add", "subtract", "exchange", "compare-exchange" });
        std::string operands;
        for (const operand_t& value : node.operands) {
            operands += (operands.empty() ? "" : ", ") + operand(value);
        }

        const std::string target = node.target ? place(*node.target) : std::string(type(node.type));
        return std::format("{} {} {}({})", keyword("atomic"), names[std::to_underlying(node.op)],
                           target, operands);
    }

    /// "new Channel<Int32>(64)"
    [[nodiscard]] std::string dump(const channel_t& node) const {
        return std::format("{} {}({})", keyword("new"), type(node.type), operand(node.capacity));
    }

    /// "lock global.score", or "unlock global.score".
    [[nodiscard]] std::string dump(const lock_t& node) const {
        return std::format("{} {}", keyword(node.unlock ? "unlock" : "lock"), place(node.target));
    }

    [[nodiscard]] std::string dump(const is_t& node) const {
        return std::format("{} {} {}", operand(node.value), keyword("Is"), type(node.type));
    }

    /// "downcast _1 As Boss", or "_1 As? Boss?".
    [[nodiscard]] std::string dump(const downcast_t& node) const {
        if (node.or_null) {
            return std::format("{} {} {}", operand(node.value), keyword("As?"), type(node.type));
        }

        return std::format("{} {} {} {}", keyword("downcast"), operand(node.value), keyword("As"),
                           type(node.type));
    }

    // Statements

    [[nodiscard]] std::string dump(const assign_t& node) const {
        return std::format("{} = {}", place(node.target), rvalue(node.value));
    }

    [[nodiscard]] std::string dump(const evaluate_t& node) const {
        return rvalue(node.value);
    }

    [[nodiscard]] std::string dump(const retain_t& node) const {
        return keyword("retain") + " " + operand(node.value);
    }

    [[nodiscard]] std::string dump(const release_t& node) const {
        return keyword("release") + " " + place(node.target);
    }

    [[nodiscard]] std::string dump(const ready_t& node) const {
        return keyword(node.mark ? "ready" : "check ready") + " " + place(place_t{ node.global });
    }

    // Terminators

    [[nodiscard]] std::string dump(const goto_t& node) const {
        return keyword("goto") + " " + label(node.target);
    }

    [[nodiscard]] std::string dump(const branch_t& node) const {
        return std::format("{} {}, {}, {}", keyword("branch"), operand(node.condition),
                           label(node.if_true), label(node.if_false));
    }

    [[nodiscard]] std::string dump(const return_t& node) const {
        return node.value ? keyword("return") + " " + operand(*node.value) : keyword("return");
    }

    [[nodiscard]] std::string dump(const panic_t& node) const {
        if (node.text) {
            return keyword("panic") + " " + operand(*node.text);
        }

        return keyword("panic") + " " + paint(std::format("\"{}\"", node.message), tone_t::LITERAL);
    }

    [[nodiscard]] std::string dump(const unreachable_t& /*node*/) const {
        return keyword("unreachable");
    }

    /// "suspend, then bb3"
    [[nodiscard]] std::string dump(const suspend_t& node) const {
        return std::format("{}, {} {}, {} {}", keyword("suspend"), keyword("then"),
                           label(node.resume), keyword("cancel"), label(node.cancel));
    }
};

} // namespace

std::string dump(const mir_module_t& module, const style_t& style) {
    return dumper_t(module, style).run();
}

} // namespace fb
