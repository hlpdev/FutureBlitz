#include "fb/evaluate.hpp"

#include "fb/types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>
#include <variant>

namespace fb {
namespace {

/// A value while a function runs at compile time: a number, Bool, String, or Null, or the parts
/// of a Struct, tuple, or fixed array. A T? that holds a value is that value.
struct value_t {
    std::variant<const_value_t, std::vector<value_t>> data;
};

using result_t = std::expected<value_t, evaluation_error_t>;
using done_t = std::expected<void, evaluation_error_t>;

/// How many blocks and statements one evaluation runs, and how deep its calls go, before it
/// stops.
constexpr std::uint64_t max_steps = 10'000'000;
constexpr std::size_t max_depth = 10'000;

template <typename Id>
std::size_t index(const Id id) {
    return std::to_underlying(id);
}

std::unexpected<evaluation_error_t> failure(std::string message,
                                            const std::optional<span_t> span = std::nullopt) {
    return std::unexpected(evaluation_error_t{ .message = std::move(message), .span = span });
}

[[nodiscard]] bool is_null(const const_value_t& value) {
    return std::holds_alternative<std::nullptr_t>(value.data);
}

/// One call that's running: its function, a value for each of its locals, and where it is. The
/// calls are a stack of these, so deep recursion doesn't use up the compiler's own stack.
struct frame_t {
    const mir_function_t* function = nullptr;
    std::vector<value_t> locals;
    std::size_t block = 0;
    std::size_t statement = 0;
    /// The call this one is waiting on, at `statement`, and where its result goes.
    const call_t* waiting = nullptr;
    const place_t* result = nullptr;
};

class evaluator_t {
public:
    explicit evaluator_t(const mir_module_t& module) : m_module(module), m_types(*module.types) {}

    /// Runs a function with its parameters' values and gives its result.
    result_t call(const function_id_t id, std::vector<value_t> arguments) {
        m_frames.push_back(frame_of(id));
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            m_frames.back().locals[i] = std::move(arguments[i]);
        }

        return run();
    }

private:
    const mir_module_t& m_module;
    const type_table_t& m_types;
    std::vector<frame_t> m_frames;
    std::uint64_t m_steps = 0;

    [[nodiscard]] frame_t frame_of(const function_id_t id) const {
        const mir_function_t& function = m_module.functions[index(id)];
        return { .function = &function, .locals = std::vector<value_t>(function.locals.size()) };
    }

    /// Runs the calls on the stack until the first one returns.
    result_t run() {
        while (true) {
            if (++m_steps > max_steps) {
                return failure(std::format("it runs more than {} steps", max_steps));
            }

            frame_t& frame = m_frames.back();
            const basic_block_t& current = frame.function->blocks[frame.block];
            if (frame.statement < current.statements.size()) {
                if (const done_t done = step(frame, current.statements[frame.statement]); !done) {
                    return std::unexpected(done.error());
                }

                continue;
            }

            const terminator_t& terminator = current.terminator;
            if (const auto* jump = std::get_if<goto_t>(&terminator)) {
                frame.block = index(jump->target);
                frame.statement = 0;
            } else if (const auto* branch = std::get_if<branch_t>(&terminator)) {
                const result_t condition = read(frame, branch->condition);
                if (!condition) {
                    return condition;
                }

                const bool taken = std::get<bool>(std::get<const_value_t>(condition->data).data);
                frame.block = index(taken ? branch->if_true : branch->if_false);
                frame.statement = 0;
            } else if (const auto* returned = std::get_if<return_t>(&terminator)) {
                result_t value = returned->value
                                     ? read(frame, *returned->value)
                                     : value_t{ const_value_t{ .type = type_id_t::NONE } };
                if (!value || m_frames.size() == 1) {
                    return value;
                }

                if (const done_t done = finish_call(std::move(*value)); !done) {
                    return std::unexpected(done.error());
                }
            } else if (const auto* panic = std::get_if<panic_t>(&terminator)) {
                return panicked(frame, *panic);
            } else {
                return failure("it reaches code that can't run");
            }
        }
    }

    /// Runs a statement. A call starts a new frame instead, and the statement finishes once it
    /// returns.
    done_t step(frame_t& frame, const statement_t& statement) {
        const call_t* call = nullptr;
        const place_t* result = nullptr;
        if (const auto* assign = std::get_if<assign_t>(&statement)) {
            call = std::get_if<call_t>(&assign->value);
            result = &assign->target;
        } else if (const auto* evaluated = std::get_if<evaluate_t>(&statement)) {
            call = std::get_if<call_t>(&evaluated->value);
        }

        if (call == nullptr) {
            ++frame.statement;
            return execute(frame, statement);
        }

        return start_call(frame, *call, result);
    }

    /// A new frame for a call, with its arguments.
    done_t start_call(frame_t& frame, const call_t& call, const place_t* result) {
        const mir_function_t& function = m_module.functions[index(call.function)];
        if (call.slot || call.interface != type_id_t::INVALID || function.blocks.empty()) {
            return failure("it calls a Method through a table, which only the running program has");
        }

        if (m_frames.size() >= max_depth) {
            return failure(std::format("its calls go more than {} deep", max_depth));
        }

        frame_t callee = frame_of(call.function);
        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
            result_t argument = read(frame, call.arguments[i].value);
            if (!argument) {
                return std::unexpected(argument.error());
            }

            callee.locals[i] = std::move(*argument);
        }

        frame.waiting = &call;
        frame.result = result;
        m_frames.push_back(std::move(callee));
        return {};
    }

    /// The top frame returned `value`: its caller gets it, and each ByRef argument, which is the
    /// caller's variable, gets the parameter's last value.
    done_t finish_call(value_t value) {
        const frame_t callee = std::move(m_frames.back());
        m_frames.pop_back();
        frame_t& caller = m_frames.back();
        const call_t& call = *caller.waiting;
        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
            if (call.arguments[i].by_ref) {
                const auto& place = std::get<place_t>(call.arguments[i].value);
                if (const done_t done = store(caller, place, callee.locals[i]); !done) {
                    return done;
                }
            }
        }

        const place_t* result = caller.result;
        caller.waiting = nullptr;
        caller.result = nullptr;
        ++caller.statement;
        if (result != nullptr) {
            return store(caller, *result, std::move(value));
        }

        return {};
    }

    result_t panicked(frame_t& frame, const panic_t& panic) {
        std::string message = panic.message;
        if (panic.text) {
            const result_t text = read(frame, *panic.text);
            if (!text) {
                return text;
            }

            message = std::get<std::string>(std::get<const_value_t>(text->data).data);
        }

        return failure(std::format("it panics: {}", message), panic.span);
    }

    done_t execute(frame_t& frame, const statement_t& statement) {
        if (const auto* assign = std::get_if<assign_t>(&statement)) {
            result_t value = compute(frame, assign->value);
            if (!value) {
                return std::unexpected(value.error());
            }

            return store(frame, assign->target, std::move(*value));
        }

        if (const auto* evaluated = std::get_if<evaluate_t>(&statement)) {
            if (const result_t value = compute(frame, evaluated->value); !value) {
                return std::unexpected(value.error());
            }
        }

        // Retains and releases count references, which values here don't have.
        return {};
    }

    result_t compute(frame_t& frame, const rvalue_t& value) {
        if (const auto* use = std::get_if<use_t>(&value)) {
            return read(frame, use->value);
        }

        if (const auto* unary = std::get_if<unary_t>(&value)) {
            const result_t operand = read(frame, unary->operand);
            if (!operand) {
                return operand;
            }

            return folded(fold_unary(unary->op, scalar(*operand)), unary->span);
        }

        if (const auto* binary = std::get_if<binary_t>(&value)) {
            return compute_binary(frame, *binary);
        }

        if (const auto* cast = std::get_if<cast_t>(&value)) {
            const result_t operand = read(frame, cast->value);
            if (!operand) {
                return operand;
            }

            return convert(*operand, cast->type, cast->span);
        }

        if (const auto* call = std::get_if<runtime_call_t>(&value)) {
            return run_runtime(frame, *call);
        }

        if (const auto* aggregate = std::get_if<aggregate_t>(&value)) {
            return parts_of(frame, aggregate->fields);
        }

        if (const auto* array = std::get_if<array_t>(&value)) {
            if (m_types.get(array->type).kind != type_kind_t::FIXED_ARRAY) {
                return failure("it makes a T[], which only the running program can");
            }

            return parts_of(frame, array->elements);
        }

        if (const auto* zero = std::get_if<zero_t>(&value)) {
            return zero_of(zero->type);
        }

        return failure("it uses an object, which only the running program has");
    }

    result_t parts_of(frame_t& frame, const std::vector<operand_t>& operands) {
        std::vector<value_t> parts;
        parts.reserve(operands.size());
        for (const operand_t& operand : operands) {
            result_t part = read(frame, operand);
            if (!part) {
                return part;
            }

            parts.push_back(std::move(*part));
        }

        return value_t{ std::move(parts) };
    }

    static const_value_t scalar(const value_t& value) {
        return std::get<const_value_t>(value.data);
    }

    static result_t folded(const fold_result_t& result, const span_t span) {
        if (!result) {
            return failure(result.error(), span);
        }

        return value_t{ *result };
    }

    /// An Enum's value as its integer type, and any other value as it is.
    [[nodiscard]] const_value_t as_integer(const_value_t value) const {
        if (value.type != type_id_t::INVALID && m_types.get(value.type).kind == type_kind_t::ENUM) {
            value.type = m_types.get(value.type).inner;
        }

        return value;
    }

    result_t compute_binary(frame_t& frame, const binary_t& node) {
        const result_t left_value = read(frame, node.left);
        if (!left_value) {
            return left_value;
        }

        const result_t right_value = read(frame, node.right);
        if (!right_value) {
            return right_value;
        }

        // Null on either side asks whether a T? holds a value.
        const const_value_t left = scalar(*left_value);
        const const_value_t right = scalar(*right_value);
        const bool equality = node.op == binary_op_t::EQUAL || node.op == binary_op_t::NOT_EQUAL;
        if (equality && (is_null(left) || is_null(right))) {
            const bool same = is_null(left) && is_null(right);
            return value_t{ make_bool(node.op == binary_op_t::EQUAL ? same : !same) };
        }

        const const_value_t a = as_integer(left);
        const const_value_t b = as_integer(right);
        // Folding fails just where the operation overflows or divides by zero.
        if (node.overflows) {
            return value_t{ make_bool(!fold_binary(node.op, a, b)) };
        }

        const bool arithmetic = node.op == binary_op_t::ADD || node.op == binary_op_t::SUBTRACT ||
                                node.op == binary_op_t::MULTIPLY;
        if (node.wrapping && arithmetic && is_integer(a.type)) {
            return value_t{ wrapped(node.op, a, b) };
        }

        result_t result = folded(fold_binary(node.op, a, b), node.span);
        // Bits of a @Flags Enum stay the Enum.
        if (result && left.type != a.type && scalar(*result).type == a.type) {
            std::get<const_value_t>(result->data).type = left.type;
        }

        return result;
    }

    /// + - * that wrap around: on the bits, cut down to the type.
    static const_value_t wrapped(const binary_op_t op, const const_value_t& a,
                                 const const_value_t& b) {
        const auto bits = [](const const_value_t& value) {
            if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
                return std::bit_cast<std::uint64_t>(*number);
            }

            return std::get<std::uint64_t>(value.data);
        };

        std::uint64_t raw = bits(a) * bits(b);
        if (op == binary_op_t::ADD) {
            raw = bits(a) + bits(b);
        } else if (op == binary_op_t::SUBTRACT) {
            raw = bits(a) - bits(b);
        }

        return *fold_cast({ .type = type_id_t::UINT64, .data = raw }, a.type);
    }

    /// A cast to `type`: between numbers, to and from an Enum's integer, and to a T?.
    result_t convert(const value_t& value, const type_id_t type, const span_t span) {
        const type_t& target = m_types.get(type);
        const auto* constant = std::get_if<const_value_t>(&value.data);
        if (constant == nullptr) {
            return value; // a Struct, tuple, or fixed array as a T? is itself
        }

        const_value_t from = *constant;
        if (from.type != type_id_t::INVALID &&
            m_types.get(from.type).kind == type_kind_t::NULLABLE) {
            from.type = m_types.get(from.type).inner;
        }

        if (target.kind == type_kind_t::NULLABLE) {
            if (is_null(from)) {
                return value_t{ const_value_t{ .type = type, .data = nullptr } };
            }

            result_t inner = convert(value_t{ from }, target.inner, span);
            if (inner) {
                std::get<const_value_t>(inner->data).type = type;
            }

            return inner;
        }

        if (target.kind == type_kind_t::ENUM) {
            result_t integer = folded(fold_cast(as_integer(from), target.inner), span);
            if (integer) {
                std::get<const_value_t>(integer->data).type = type;
            }

            return integer;
        }

        return folded(fold_cast(as_integer(from), type), span);
    }

    /// The runtime's String functions, which the compiler can do itself.
    result_t run_runtime(frame_t& frame, const runtime_call_t& call) {
        std::vector<const_value_t> arguments;
        for (const operand_t& operand : call.arguments) {
            const result_t argument = read(frame, operand);
            if (!argument) {
                return argument;
            }

            if (const auto* constant = std::get_if<const_value_t>(&argument->data)) {
                arguments.push_back(*constant);
            }
        }

        const auto text = [&](const std::size_t i) -> const std::string& {
            return std::get<std::string>(arguments[i].data);
        };

        switch (call.function) {
        case runtime_function_t::CONCAT:
            return value_t{ make_string(text(0) + text(1)) };
        case runtime_function_t::JOIN: {
            std::string joined;
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                joined += text(i);
            }

            return value_t{ make_string(std::move(joined)) };
        }
        case runtime_function_t::TO_TEXT:
            return value_t{ make_string(to_text(arguments[0])) };
        case runtime_function_t::EQUAL:
            return value_t{ make_bool(text(0) == text(1)) };
        case runtime_function_t::COMPARE: {
            const int order = text(0).compare(text(1));
            const std::int64_t sign =
                static_cast<std::int64_t>(order > 0) - static_cast<std::int64_t>(order < 0);
            return value_t{ const_value_t{ .type = type_id_t::INT32, .data = sign } };
        }
        case runtime_function_t::PRINT:
            return failure("it prints, which only the running program can");
        default:
            return failure("it uses objects or arrays, which only the running program has");
        }
    }

    /// What a variable of `type` starts with: zero, False, "", Null, or a Struct, tuple, or
    /// fixed array of those.
    [[nodiscard]] result_t zero_of(const type_id_t type) const {
        const type_t& info = m_types.get(type);
        if (type == type_id_t::BOOL) {
            return value_t{ make_bool(false) };
        }

        if (type == type_id_t::STRING) {
            return value_t{ make_string("") };
        }

        if (is_float(type)) {
            return value_t{ const_value_t{ .type = type, .data = 0.0 } };
        }

        if (is_integer(type)) {
            const const_value_t zero{ .type = type_id_t::UINT64, .data = std::uint64_t{ 0 } };
            return value_t{ *fold_cast(zero, type) };
        }

        switch (info.kind) {
        case type_kind_t::NULLABLE:
            return value_t{ const_value_t{ .type = type, .data = nullptr } };
        case type_kind_t::ENUM: {
            const_value_t zero = *fold_cast(
                { .type = type_id_t::UINT64, .data = m_types.enum_zero(type) }, info.inner);
            zero.type = type;
            return value_t{ zero };
        }
        case type_kind_t::STRUCT:
        case type_kind_t::TUPLE: {
            std::vector<value_t> parts;
            for (const field_t& field : info.fields) {
                result_t part = zero_of(field.type);
                if (!part) {
                    return part;
                }

                parts.push_back(std::move(*part));
            }

            return value_t{ std::move(parts) };
        }
        case type_kind_t::FIXED_ARRAY: {
            result_t element = zero_of(info.inner);
            if (!element) {
                return element;
            }

            return value_t{ std::vector<value_t>(m_types.element_count(type), *element) };
        }
        default:
            return failure("it uses an object, which only the running program has");
        }
    }

    /// Where a place's value is, and its type. A T? that holds a value is its value, so a step
    /// into one stays put.
    std::expected<std::pair<value_t*, type_id_t>, evaluation_error_t> locate(frame_t& frame,
                                                                             const place_t& place) {
        const auto* local = std::get_if<local_id_t>(&place.root);
        if (local == nullptr) {
            return failure("it uses a Global, which only the running program has");
        }

        value_t* value = &frame.locals[index(*local)];
        type_id_t type = frame.function->locals[index(*local)].type;
        for (const step_t& step : place.steps) {
            const type_t& info = m_types.get(type);
            if (!step.indices.empty()) {
                if (info.kind != type_kind_t::FIXED_ARRAY) {
                    return failure("it uses a T[], which only the running program has");
                }

                const std::expected<std::size_t, evaluation_error_t> at =
                    element_index(frame, type, step);
                if (!at) {
                    return std::unexpected(at.error());
                }

                value = &std::get<std::vector<value_t>>(value->data)[*at];
                type = info.inner;
                continue;
            }

            if (info.kind == type_kind_t::NULLABLE) {
                const auto* constant = std::get_if<const_value_t>(&value->data);
                if (constant != nullptr && is_null(*constant)) {
                    return failure("it reads Null's value", step.span);
                }

                type = info.inner;
                continue;
            }

            // A Struct that's set field by field has its parts made first.
            if (!std::holds_alternative<std::vector<value_t>>(value->data)) {
                value->data = std::vector<value_t>(info.fields.size());
            }

            value = &std::get<std::vector<value_t>>(value->data)[step.field];
            type = info.fields[step.field].type;
        }

        return std::pair{ value, type };
    }

    /// The element a step goes to: an index for each dimension, or one that counts every
    /// element.
    std::expected<std::size_t, evaluation_error_t>
    element_index(const frame_t& frame, const type_id_t type, const step_t& step) const {
        const std::vector<std::uint32_t>& sizes = m_types.get(type).sizes;
        std::uint64_t flat = 0;
        for (std::size_t i = 0; i < step.indices.size(); ++i) {
            const const_value_t number = scalar(frame.locals[index(step.indices[i])]);
            const std::uint64_t size =
                step.indices.size() == 1 ? m_types.element_count(type) : sizes[i];
            const auto* signed_number = std::get_if<std::int64_t>(&number.data);
            const bool negative = signed_number != nullptr && *signed_number < 0;
            const std::uint64_t at = signed_number != nullptr
                                         ? static_cast<std::uint64_t>(*signed_number)
                                         : std::get<std::uint64_t>(number.data);
            if (negative || at >= size) {
                return failure(
                    std::format("index {} is out of range for {} elements", to_text(number), size),
                    step.span);
            }

            flat = (flat * size) + at;
        }

        return static_cast<std::size_t>(flat);
    }

    result_t read(frame_t& frame, const operand_t& operand) {
        if (const auto* constant = std::get_if<const_value_t>(&operand)) {
            return value_t{ *constant };
        }

        const auto found = locate(frame, std::get<place_t>(operand));
        if (!found) {
            return std::unexpected(found.error());
        }

        // A T?'s value read through its "value" step has its T's type.
        value_t value = *found->first;
        if (auto* constant = std::get_if<const_value_t>(&value.data);
            constant != nullptr && !is_null(*constant)) {
            constant->type = found->second;
        }

        return value;
    }

    done_t store(frame_t& frame, const place_t& place, value_t value) {
        const auto found = locate(frame, place);
        if (!found) {
            return std::unexpected(found.error());
        }

        *found->first = std::move(value);
        return {};
    }
};

} // namespace

std::expected<const_value_t, evaluation_error_t>
evaluate(const mir_module_t& module, const function_id_t function,
         const std::vector<const_value_t>& arguments) {
    std::vector<value_t> values;
    values.reserve(arguments.size());
    for (const const_value_t& argument : arguments) {
        values.push_back({ argument });
    }

    const result_t result = evaluator_t(module).call(function, std::move(values));
    if (!result) {
        return std::unexpected(result.error());
    }

    const auto* constant = std::get_if<const_value_t>(&result->data);
    if (constant == nullptr) {
        return failure("it gives a value the compiler can't hold");
    }

    return *constant;
}

} // namespace fb
