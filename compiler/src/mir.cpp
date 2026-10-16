#include "fb/mir.hpp"

#include <format>
#include <utility>

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
    case runtime_function_t::PRINT:
        return "print";
    }

    std::unreachable();
}

/// Constants print as they would in source code.
std::string constant(const const_value_t& value) {
    if (value.type != type_id_t::STRING) {
        return to_text(value);
    }

    std::string out = "\"";
    for (const char c : std::get<std::string>(value.data)) {
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
            out +=
                std::format("{} {}: {} = {}\n", keyword("global"), paint(global.name, tone_t::NAME),
                            type(global.type), paint(constant(global.initial), tone_t::LITERAL));
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

    [[nodiscard]] std::string paint(const std::string_view text, const tone_t tone) const {
        return m_style.paint(text, tone);
    }

    [[nodiscard]] std::string keyword(const std::string_view text) const {
        return paint(text, tone_t::KEYWORD);
    }

    [[nodiscard]] std::string type(const type_id_t id) const {
        return paint(type_name(id), tone_t::TYPE);
    }

    [[nodiscard]] std::string label(const block_id_t block) const {
        return paint(std::format("bb{}", std::to_underlying(block)), tone_t::BOLD);
    }

    [[nodiscard]] std::string dump(const mir_function_t& function) const {
        std::string out = keyword("function") + " " + paint(function.name, tone_t::BOLD);
        if (function.return_type != type_id_t::NONE) {
            out += " -> " + type(function.return_type);
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

    /// "_3" for a local, "@score" for a global.
    [[nodiscard]] std::string place(const place_t& target) const {
        if (const auto* local = std::get_if<local_id_t>(&target)) {
            return paint(std::format("_{}", std::to_underlying(*local)), tone_t::NAME);
        }

        const auto global = std::to_underlying(std::get<global_id_t>(target));
        return paint(std::format("@{}", m_module.globals[global].name), tone_t::NAME);
    }

    [[nodiscard]] std::string operand(const operand_t& value) const {
        if (const auto* constant_value = std::get_if<const_value_t>(&value)) {
            return paint(constant(*constant_value), tone_t::LITERAL);
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
        return node.wrapping ? std::format("{}({})", keyword("wrapping"), text) : text;
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

        const mir_function_t& function = m_module.functions[std::to_underlying(node.function)];
        return std::format("{} {}({})", keyword("call"), paint(function.name, tone_t::BOLD),
                           arguments);
    }

    [[nodiscard]] std::string dump(const runtime_call_t& node) const {
        std::string arguments;
        for (const operand_t& argument : node.arguments) {
            arguments += arguments.empty() ? "" : ", ";
            arguments += operand(argument);
        }

        return std::format("{}({})", paint(runtime_name(node.function), tone_t::BOLD), arguments);
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
        return keyword("panic") + " " + paint(std::format("\"{}\"", node.message), tone_t::LITERAL);
    }

    [[nodiscard]] std::string dump(const unreachable_t& /*node*/) const {
        return keyword("unreachable");
    }
};

} // namespace

std::string dump(const mir_module_t& module, const style_t& style) {
    return dumper_t(module, style).run();
}

} // namespace fb
