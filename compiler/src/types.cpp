#include "fb/types.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace fb {
namespace {

struct named_type_t {
    std::string_view name;
    type_id_t type;
};

/// Every built-in type's own name, then the aliases ("Int" for Int32 and so on).
/// builtin_name() takes the first match, so the own names must come first.
constexpr auto type_names = std::to_array<named_type_t>({
    { .name = "Bool", .type = type_id_t::BOOL },
    { .name = "Int8", .type = type_id_t::INT8 },
    { .name = "Int16", .type = type_id_t::INT16 },
    { .name = "Int32", .type = type_id_t::INT32 },
    { .name = "Int64", .type = type_id_t::INT64 },
    { .name = "UInt8", .type = type_id_t::UINT8 },
    { .name = "UInt16", .type = type_id_t::UINT16 },
    { .name = "UInt32", .type = type_id_t::UINT32 },
    { .name = "UInt64", .type = type_id_t::UINT64 },
    { .name = "Float32", .type = type_id_t::FLOAT32 },
    { .name = "Float64", .type = type_id_t::FLOAT64 },
    { .name = "String", .type = type_id_t::STRING },
    { .name = "CString", .type = type_id_t::CSTRING },
    { .name = "Byte", .type = type_id_t::UINT8 },
    { .name = "Int", .type = type_id_t::INT32 },
    { .name = "Long", .type = type_id_t::INT64 },
    { .name = "Float", .type = type_id_t::FLOAT32 },
    { .name = "Double", .type = type_id_t::FLOAT64 },
});

} // namespace

std::string_view builtin_name(const type_id_t type) {
    if (type == type_id_t::INVALID) {
        return "<invalid>";
    }

    if (type == type_id_t::NONE) {
        return "nothing";
    }

    for (const named_type_t& entry : type_names) {
        if (entry.type == type) {
            return entry.name;
        }
    }

    std::unreachable();
}

std::optional<type_id_t> builtin_type(const std::string_view name) {
    for (const named_type_t& entry : type_names) {
        if (entry.name == name) {
            return entry.type;
        }
    }

    return std::nullopt;
}

// These ranges rely on type_id_t listing the signed integers, then the unsigned ones.
bool is_integer(const type_id_t type) {
    return type >= type_id_t::INT8 && type <= type_id_t::UINT64;
}

bool is_signed(const type_id_t type) {
    return (type >= type_id_t::INT8 && type <= type_id_t::INT64) || is_float(type);
}

bool is_float(const type_id_t type) {
    return type == type_id_t::FLOAT32 || type == type_id_t::FLOAT64;
}

bool is_numeric(const type_id_t type) {
    return is_integer(type) || is_float(type);
}

std::uint32_t bit_width(const type_id_t type) {
    switch (type) {
    case type_id_t::INT8:
    case type_id_t::UINT8:
        return 8;
    case type_id_t::INT16:
    case type_id_t::UINT16:
        return 16;
    case type_id_t::INT32:
    case type_id_t::UINT32:
    case type_id_t::FLOAT32:
        return 32;
    case type_id_t::INT64:
    case type_id_t::UINT64:
    case type_id_t::FLOAT64:
        return 64;
    default:
        return 0;
    }
}

bool converts_losslessly(const type_id_t from, const type_id_t to) {
    // INVALID was already reported. Accepting it avoids a second error.
    if (from == to || from == type_id_t::INVALID || to == type_id_t::INVALID) {
        return true;
    }

    if (is_integer(from) && is_integer(to)) {
        if (is_signed(from) == is_signed(to)) {
            return bit_width(to) >= bit_width(from);
        }

        // Unsigned to signed needs a wider type. Signed to unsigned loses the negatives.
        return !is_signed(from) && bit_width(to) > bit_width(from);
    }

    // A Float32 holds every integer of up to 24 bits exactly and a Float64 up to 53, so
    // 16-bit and 32-bit integers are the widest that fit.
    if (is_integer(from) && is_float(to)) {
        return bit_width(from) <= (to == type_id_t::FLOAT32 ? 16U : 32U);
    }

    return from == type_id_t::FLOAT32 && to == type_id_t::FLOAT64;
}

type_table_t::type_table_t() {
    for (std::uint32_t i = 0; i < std::to_underlying(type_id_t::BUILTIN_COUNT); ++i) {
        add({ .name = std::string(builtin_name(static_cast<type_id_t>(i))) });
    }
}

type_table_t::type_table_t(const type_table_t& other)
    : m_fixed_arrays(other.m_fixed_arrays), m_tuples(other.m_tuples),
      m_functions(other.m_functions), m_callbacks(other.m_callbacks),
      m_throwing_tasks(other.m_throwing_tasks), m_instances(other.m_instances),
      m_values(other.m_values), m_thread(other.m_thread), m_scheduler(other.m_scheduler) {
    m_entries.reserve(other.m_entries.size());
    for (const entry_t& entry : other.m_entries) {
        m_entries.push_back({
            .type = std::make_unique<type_t>(*entry.type),
            .nullable = entry.nullable,
            .array = entry.array,
            .weak = entry.weak,
            .atomic = entry.atomic,
            .mutex = entry.mutex,
            .task = entry.task,
            .channel = entry.channel,
            .pointer = entry.pointer,
        });
    }
}

type_table_t& type_table_t::operator=(const type_table_t& other) {
    if (this != &other) {
        type_table_t copy(other);
        *this = std::move(copy);
    }

    return *this;
}

const type_t& type_table_t::get(const type_id_t id) const {
    assert(std::to_underlying(id) < m_entries.size());
    return *m_entries[std::to_underlying(id)].type;
}

type_id_t type_table_t::nullable(const type_id_t inner) {
    return make(type_kind_t::NULLABLE, inner, std::format("{}?", suffixed(inner)));
}

type_id_t type_table_t::array(const type_id_t element) {
    return make(type_kind_t::ARRAY, element, std::format("{}[]", suffixed(element)));
}

type_id_t type_table_t::weak(const type_id_t inner) {
    if (inner != type_id_t::INVALID) {
        (void)nullable(inner);
    }

    return make(type_kind_t::WEAK, inner, std::format("Weak<{}>", name(inner)));
}

type_id_t type_table_t::atomic(const type_id_t inner) {
    return make(type_kind_t::ATOMIC, inner, std::format("Atomic<{}>", name(inner)));
}

type_id_t type_table_t::pointer(const type_id_t inner) {
    const std::string full_name =
        inner == type_id_t::NONE ? std::string("Ptr<Void>") : std::format("Ptr<{}>", name(inner));
    return make(type_kind_t::PTR, inner, full_name);
}

type_id_t type_table_t::mutex(const type_id_t inner) {
    return make(type_kind_t::MUTEX, inner, std::format("Mutex<{}>", name(inner)));
}

type_id_t type_table_t::task(const type_id_t result) {
    if (result == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    if (const type_id_t made = m_entries[std::to_underlying(result)].task;
        made != type_id_t::INVALID) {
        return made;
    }

    // fb_task_t's job and closure, then the result.
    const type_id_t work = function({}, result);
    type_t type{
        .kind = type_kind_t::TASK,
        .name =
            result == type_id_t::NONE ? std::string("Task") : std::format("Task<{}>", name(result)),
        .inner = result,
        .fields = { { .name = "job", .type = type_id_t::UINT64 } },
    };
    type.fields.push_back({ .name = "closure", .type = work });
    if (result != type_id_t::NONE) {
        type.fields.push_back({ .name = "result", .type = result });
    }

    const type_id_t made = add(std::move(type));
    m_entries[std::to_underlying(result)].task = made;
    return made;
}

type_id_t type_table_t::task(const type_id_t result, const std::vector<type_id_t>& throws,
                             const type_id_t error) {
    if (throws.empty() || result == type_id_t::INVALID) {
        return task(result);
    }

    for (const type_id_t made : m_throwing_tasks) {
        if (get(made).inner == result && get(made).throws == throws) {
            return made;
        }
    }

    type_t type = get(task(result));
    type.name += " Throws ";
    for (std::size_t i = 0; i < throws.size(); ++i) {
        type.name += std::format("{}{}", i == 0 ? "" : ", ", name(throws[i]));
    }

    type.fields.push_back({ .name = "error", .type = error });
    type.throws = throws;
    const type_id_t made = add(std::move(type));
    m_throwing_tasks.push_back(made);
    return made;
}

type_id_t type_table_t::thread() {
    if (m_thread == type_id_t::INVALID) {
        const type_id_t work = function({}, type_id_t::NONE);
        type_t type{
            .kind = type_kind_t::THREAD,
            .name = "Thread",
            .inner = type_id_t::NONE,
            .fields = { { .name = "job", .type = type_id_t::UINT64 } },
        };
        type.fields.push_back({ .name = "closure", .type = work });
        m_thread = add(std::move(type));
    }

    return m_thread;
}

type_id_t type_table_t::scheduler() {
    if (m_scheduler == type_id_t::INVALID) {
        m_scheduler = add({ .kind = type_kind_t::SCHEDULER, .name = "Scheduler" });
    }

    return m_scheduler;
}

type_id_t type_table_t::channel(const type_id_t element) {
    return make(type_kind_t::CHANNEL, element, std::format("Channel<{}>", name(element)));
}

type_id_t type_table_t::find_nullable(const type_id_t inner) const {
    return m_entries[std::to_underlying(inner)].nullable;
}

type_id_t type_table_t::make(const type_kind_t kind, const type_id_t inner, std::string full_name) {
    if (inner == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    // Found by index each time, since adding a type can move the entries.
    const auto made = [&] -> type_id_t& {
        entry_t& entry = m_entries[std::to_underlying(inner)];
        switch (kind) {
        case type_kind_t::NULLABLE:
            return entry.nullable;
        case type_kind_t::ARRAY:
            return entry.array;
        case type_kind_t::ATOMIC:
            return entry.atomic;
        case type_kind_t::MUTEX:
            return entry.mutex;
        case type_kind_t::CHANNEL:
            return entry.channel;
        case type_kind_t::PTR:
            return entry.pointer;
        default:
            return entry.weak;
        }
    };

    if (made() == type_id_t::INVALID) {
        type_t type{ .kind = kind, .name = std::move(full_name), .inner = inner };
        if (kind == type_kind_t::NULLABLE) {
            type.fields.push_back({ .name = "value", .type = inner });
        }

        // What a Ptr points at, which Ptr<Void> doesn't say.
        if (kind == type_kind_t::PTR && inner != type_id_t::NONE) {
            type.fields.push_back({ .name = "Value", .type = inner });
        }

        // fb_mutex_t, then the value.
        if (kind == type_kind_t::MUTEX) {
            type.fields.push_back({ .name = "state", .type = type_id_t::INT32 });
            type.fields.push_back({ .name = "owner", .type = type_id_t::UINT32 });
            type.fields.push_back({ .name = "value", .type = inner });
        }

        made() = add(std::move(type));
    }

    return made();
}

type_id_t type_table_t::fixed_array(const type_id_t element,
                                    const std::vector<std::uint32_t>& sizes,
                                    const std::vector<type_id_t>& dimensions) {
    if (element == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    // Every size known is no dimensions.
    std::vector<type_id_t> parameters = dimensions;
    if (std::ranges::all_of(parameters,
                            [](const type_id_t d) { return d == type_id_t::INVALID; })) {
        parameters.clear();
    }

    for (const type_id_t made : m_fixed_arrays) {
        if (get(made).inner == element && get(made).sizes == sizes &&
            get(made).parameters == parameters) {
            return made;
        }
    }

    std::string text = std::format("{}[", suffixed(element));
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        text += i == 0 ? "" : ", ";
        text += i < parameters.size() && parameters[i] != type_id_t::INVALID
                    ? std::string(name(parameters[i]))
                    : std::to_string(sizes[i]);
    }

    const type_id_t id = add({
        .kind = type_kind_t::FIXED_ARRAY,
        .name = text + "]",
        .inner = element,
        .sizes = sizes,
        .parameters = parameters,
    });
    m_fixed_arrays.push_back(id);
    return id;
}

type_id_t type_table_t::tuple(const std::vector<type_id_t>& elements) {
    if (std::ranges::contains(elements, type_id_t::INVALID)) {
        return type_id_t::INVALID;
    }

    for (const type_id_t made : m_tuples) {
        if (std::ranges::equal(get(made).fields, elements, {}, &field_t::type)) {
            return made;
        }
    }

    type_t type{ .kind = type_kind_t::TUPLE, .name = "(" };
    for (std::size_t i = 0; i < elements.size(); ++i) {
        type.name += std::format("{}{}", i == 0 ? "" : ", ", name(elements[i]));
        type.fields.push_back({ .name = std::to_string(i), .type = elements[i] });
    }

    type.name += ')';
    const type_id_t id = add(std::move(type));
    m_tuples.push_back(id);
    return id;
}

type_id_t type_table_t::function(const std::vector<type_id_t>& parameters, const type_id_t result) {
    if (result == type_id_t::INVALID || std::ranges::contains(parameters, type_id_t::INVALID)) {
        return type_id_t::INVALID;
    }

    for (const type_id_t made : m_functions) {
        if (get(made).parameters == parameters && get(made).inner == result) {
            return made;
        }
    }

    std::string text = "Func(";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        text += std::format("{}{}", i == 0 ? "" : ", ", name(parameters[i]));
    }

    text += ')';
    if (result != type_id_t::NONE) {
        text += std::format(": {}", name(result));
    }

    const type_id_t id = add({
        .kind = type_kind_t::FUNCTION,
        .name = std::move(text),
        .inner = result,
        .parameters = parameters,
    });
    m_functions.push_back(id);
    return id;
}

type_id_t type_table_t::callback(const std::vector<type_id_t>& parameters, const type_id_t result) {
    if (result == type_id_t::INVALID || std::ranges::contains(parameters, type_id_t::INVALID)) {
        return type_id_t::INVALID;
    }

    for (const type_id_t made : m_callbacks) {
        if (get(made).parameters == parameters && get(made).inner == result) {
            return made;
        }
    }

    std::string text = "Callback(";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        text += std::format("{}{}", i == 0 ? "" : ", ", name(parameters[i]));
    }

    text += ')';
    if (result != type_id_t::NONE) {
        text += std::format(": {}", name(result));
    }

    const type_id_t id = add({
        .kind = type_kind_t::CALLBACK,
        .name = std::move(text),
        .inner = result,
        .parameters = parameters,
    });
    m_callbacks.push_back(id);
    return id;
}

type_id_t type_table_t::add_parameter(std::string name) {
    return add({ .kind = type_kind_t::PARAMETER, .name = std::move(name) });
}

type_id_t type_table_t::add_value_parameter(std::string name, const type_id_t type) {
    return add({ .kind = type_kind_t::PARAMETER, .name = std::move(name), .inner = type });
}

type_id_t type_table_t::value(const type_id_t type, const std::int64_t number) {
    for (const type_id_t made : m_values) {
        if (get(made).inner == type && get(made).number == number) {
            return made;
        }
    }

    const type_id_t id = add({
        .kind = type_kind_t::VALUE,
        .name = std::to_string(number),
        .inner = type,
        .number = number,
    });
    m_values.push_back(id);
    return id;
}

type_id_t type_table_t::value_expression(const type_id_t type, const value_op_t op,
                                         const type_id_t left, const type_id_t right) {
    if (left == type_id_t::INVALID || right == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    const type_t& a = get(left);
    const type_t& b = get(right);
    if (a.kind == type_kind_t::VALUE && a.operation == value_op_t::NONE &&
        b.kind == type_kind_t::VALUE && b.operation == value_op_t::NONE) {
        return value(type, op == value_op_t::ADD ? a.number + b.number : a.number * b.number);
    }

    for (const type_id_t made : m_values) {
        if (get(made).inner == type && get(made).operation == op &&
            get(made).parameters == std::vector{ left, right }) {
            return made;
        }
    }

    // "(N + 1) * 2" keeps the sum together.
    const auto part = [this, op](const type_id_t side) {
        const bool sum = get(side).operation == value_op_t::ADD;
        return sum && op == value_op_t::MULTIPLY ? std::format("({})", name(side))
                                                 : std::string(name(side));
    };
    const type_id_t id = add({
        .kind = type_kind_t::VALUE,
        .name = std::format("{} {} {}", part(left), op == value_op_t::ADD ? "+" : "*", part(right)),
        .inner = type,
        .operation = op,
        .parameters = { left, right },
    });
    m_values.push_back(id);
    return id;
}

void type_table_t::set_parameters(const type_id_t type, std::vector<type_id_t> parameters) {
    assert(get(type).kind == type_kind_t::STRUCT || get(type).kind == type_kind_t::CLASS ||
           get(type).kind == type_kind_t::INTERFACE);
    m_entries[std::to_underlying(type)].type->parameters = std::move(parameters);
}

type_id_t type_table_t::instance(const type_id_t generic, const std::vector<type_id_t>& arguments) {
    // Instances that hold instances with ever deeper arguments, like a Struct S<T> with a
    // field of S<T[]>, would never end.
    constexpr std::uint32_t max_depth = 64;
    if (generic == type_id_t::INVALID || std::ranges::contains(arguments, type_id_t::INVALID) ||
        m_depth > max_depth) {
        return type_id_t::INVALID;
    }

    if (arguments == get(generic).parameters) {
        return generic;
    }

    for (const type_id_t made : m_instances) {
        if (get(made).generic == generic && get(made).arguments == arguments) {
            return made;
        }
    }

    // "Box<T>" makes "Box<Int32>".
    const std::string_view name = get(generic).name;
    std::string text(name.substr(0, name.find('<')));
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        text += std::format("{}{}", i == 0 ? "<" : ", ", this->name(arguments[i]));
    }

    const type_id_t id = add({
        .kind = get(generic).kind,
        .name = text + ">",
        .module = get(generic).module,
        .inner = get(generic).inner,
        .shared = get(generic).shared,
        .made = get(generic).made,
        .generic = generic,
        .arguments = arguments,
    });
    m_instances.push_back(id);
    fill(id);
    return id;
}

void type_table_t::fill(const type_id_t instance) {
    const type_id_t generic = get(instance).generic;
    const std::vector<type_id_t> parameters = get(generic).parameters;
    const std::vector<type_id_t> arguments = get(instance).arguments;
    const std::vector<field_t> generic_fields = get(generic).fields;
    const std::vector<type_id_t> generic_interfaces = get(generic).interfaces;
    ++m_depth;
    std::vector<field_t> fields;
    fields.reserve(generic_fields.size());
    for (const field_t& field : generic_fields) {
        fields.push_back(
            { .name = field.name, .type = substitute(field.type, parameters, arguments) });
    }

    // "Range<T> Implements Iterator<T>" makes Range<Int32> implement Iterator<Int32>, and
    // "Derived<T> Extends Base<T>" makes Derived<Int32> extend Base<Int32>.
    std::vector<type_id_t> interfaces;
    interfaces.reserve(generic_interfaces.size());
    for (const type_id_t interface : generic_interfaces) {
        interfaces.push_back(substitute(interface, parameters, arguments));
    }

    // What an Extend of this one instance lists stays. The generic type's Interfaces only grow,
    // so the ones it gave before are among the new ones.
    for (const type_id_t interface : get(instance).interfaces) {
        if (!std::ranges::contains(interfaces, interface)) {
            interfaces.push_back(interface);
        }
    }

    const type_id_t base = substitute(get(generic).base, parameters, arguments);
    --m_depth;
    type_t& entry = *m_entries[std::to_underlying(instance)].type;
    entry.fields = std::move(fields);
    entry.base = base;
    entry.interfaces = std::move(interfaces);
}

type_id_t type_table_t::substitute(const type_id_t type, const std::vector<type_id_t>& parameters,
                                   const std::vector<type_id_t>& arguments) {
    if (parameters.empty() || type == type_id_t::INVALID) {
        return type;
    }

    // A copy, since making types can move the entries.
    const type_t info = get(type);
    const auto each = [&](const std::vector<type_id_t>& types) {
        std::vector<type_id_t> result;
        result.reserve(types.size());
        for (const type_id_t part : types) {
            result.push_back(substitute(part, parameters, arguments));
        }

        return result;
    };

    switch (info.kind) {
    case type_kind_t::PARAMETER: {
        const auto found = std::ranges::find(parameters, type);
        return found == parameters.end()
                   ? type
                   : arguments[static_cast<std::size_t>(found - parameters.begin())];
    }
    case type_kind_t::NULLABLE:
        // T? with an Int32? for T is Int32??, which holds an Int32? that can be Null itself.
        return nullable(substitute(info.inner, parameters, arguments));
    case type_kind_t::ARRAY:
        return array(substitute(info.inner, parameters, arguments));
    case type_kind_t::FIXED_ARRAY: {
        // A size that's a Const type parameter takes its argument's value.
        std::vector<std::uint32_t> sizes = info.sizes;
        std::vector<type_id_t> dimensions = info.parameters;
        for (std::size_t i = 0; i < dimensions.size(); ++i) {
            if (dimensions[i] == type_id_t::INVALID) {
                continue;
            }

            dimensions[i] = substitute(dimensions[i], parameters, arguments);
            if (get(dimensions[i]).kind == type_kind_t::VALUE &&
                get(dimensions[i]).operation == value_op_t::NONE) {
                sizes[i] = static_cast<std::uint32_t>(get(dimensions[i]).number);
                dimensions[i] = type_id_t::INVALID;
            }
        }

        return fixed_array(substitute(info.inner, parameters, arguments), sizes, dimensions);
    }
    case type_kind_t::WEAK:
        return weak(substitute(info.inner, parameters, arguments));
    case type_kind_t::PTR:
        return pointer(substitute(info.inner, parameters, arguments));
    case type_kind_t::ATOMIC:
        return atomic(substitute(info.inner, parameters, arguments));
    case type_kind_t::MUTEX:
        return mutex(substitute(info.inner, parameters, arguments));
    case type_kind_t::TASK:
        if (!info.throws.empty()) {
            return task(substitute(info.inner, parameters, arguments), info.throws,
                        info.fields.back().type);
        }

        return task(substitute(info.inner, parameters, arguments));
    case type_kind_t::CHANNEL:
        return channel(substitute(info.inner, parameters, arguments));
    case type_kind_t::VALUE:
        // A computed one is known once what it's computed from is.
        if (info.operation == value_op_t::NONE) {
            return type;
        }

        return value_expression(info.inner, info.operation,
                                substitute(info.parameters[0], parameters, arguments),
                                substitute(info.parameters[1], parameters, arguments));
    case type_kind_t::TUPLE: {
        std::vector<type_id_t> elements;
        elements.reserve(info.fields.size());
        for (const field_t& field : info.fields) {
            elements.push_back(substitute(field.type, parameters, arguments));
        }

        return tuple(elements);
    }
    case type_kind_t::FUNCTION:
        return function(each(info.parameters), substitute(info.inner, parameters, arguments));
    case type_kind_t::CALLBACK:
        return callback(each(info.parameters), substitute(info.inner, parameters, arguments));
    case type_kind_t::STRUCT:
    case type_kind_t::CLASS:
    case type_kind_t::INTERFACE:
        if (info.generic != type_id_t::INVALID) {
            return instance(info.generic, each(info.arguments));
        }

        // A generic type used inside itself, like the Box<T> of Box's Methods.
        return info.parameters.empty() ? type : instance(type, each(info.parameters));
    default:
        return type;
    }
}

std::uint64_t type_table_t::element_count(const type_id_t type) const {
    std::uint64_t count = 1;
    for (const std::uint32_t size : get(type).sizes) {
        count *= size;
    }

    return count;
}

type_id_t type_table_t::add_struct(std::string name, std::string module) {
    return add({
        .kind = type_kind_t::STRUCT,
        .name = std::move(name),
        .module = std::move(module),
    });
}

type_id_t type_table_t::add_class(std::string name, std::string module) {
    return add({
        .kind = type_kind_t::CLASS,
        .name = std::move(name),
        .module = std::move(module),
    });
}

void type_table_t::set_fields(const type_id_t type, std::vector<field_t> fields) {
    assert(get(type).kind == type_kind_t::STRUCT || get(type).kind == type_kind_t::CLASS);
    m_entries[std::to_underlying(type)].type->fields = std::move(fields);

    // The instances made before a generic type's fields were known get them now.
    const std::vector<type_id_t> instances = m_instances;
    for (const type_id_t made : instances) {
        if (get(made).generic == type) {
            fill(made);
        }
    }
}

void type_table_t::set_made(const type_id_t type) {
    m_entries[std::to_underlying(type)].type->made = true;
}

type_id_t type_table_t::add_interface(std::string name, std::string module) {
    const type_id_t self = add_parameter("Self");
    return add({
        .kind = type_kind_t::INTERFACE,
        .name = std::move(name),
        .module = std::move(module),
        .inner = self,
    });
}

void type_table_t::set_interfaces(const type_id_t type, std::vector<type_id_t> interfaces) {
    m_entries[std::to_underlying(type)].type->interfaces = std::move(interfaces);
    const std::vector<type_id_t> instances = m_instances;
    for (const type_id_t made : instances) {
        if (get(made).generic == type) {
            fill(made);
        }
    }
}

type_id_t type_table_t::add_enum(std::string name, std::string module) {
    return add({
        .kind = type_kind_t::ENUM,
        .name = std::move(name),
        .module = std::move(module),
        .inner = type_id_t::INT32,
    });
}

void type_table_t::set_values(const type_id_t type, const type_id_t integer,
                              std::vector<enum_value_t> values, const bool flags) {
    assert(get(type).kind == type_kind_t::ENUM);
    type_t& entry = *m_entries[std::to_underlying(type)].type;
    entry.inner = integer;
    entry.values = std::move(values);
    entry.flags = flags;
}

std::uint64_t type_table_t::enum_zero(const type_id_t type) const {
    const type_t& entry = get(type);
    return entry.flags || entry.values.empty() ? 0 : entry.values.front().bits;
}

void type_table_t::set_shared(const type_id_t type) {
    assert(get(type).kind == type_kind_t::CLASS);
    m_entries[std::to_underlying(type)].type->shared = true;
}

void type_table_t::set_base(const type_id_t type, const type_id_t base) {
    assert(get(type).kind == type_kind_t::CLASS && get(base).kind == type_kind_t::CLASS);
    m_entries[std::to_underlying(type)].type->base = base;
    for (const type_id_t made : m_instances) {
        if (get(made).generic == type) {
            m_entries[std::to_underlying(made)].type->base = base;
        }
    }
}

bool type_table_t::extends(const type_id_t type, const type_id_t base) const {
    if (get(type).kind == type_kind_t::INTERFACE) {
        return type == base ||
               std::ranges::any_of(get(type).interfaces,
                                   [&](const type_id_t other) { return extends(other, base); });
    }

    for (type_id_t current = type; current != type_id_t::INVALID; current = get(current).base) {
        if (current == base) {
            return true;
        }
    }

    return false;
}

bool type_table_t::has_references(const type_id_t type) const {
    const type_kind_t kind = get(type).kind;
    if (type == type_id_t::STRING || kind == type_kind_t::CLASS || kind == type_kind_t::INTERFACE ||
        kind == type_kind_t::ARRAY || kind == type_kind_t::WEAK || kind == type_kind_t::FUNCTION ||
        kind == type_kind_t::TASK || kind == type_kind_t::THREAD || kind == type_kind_t::CHANNEL ||
        kind == type_kind_t::SCHEDULER) {
        return true;
    }

    if (kind == type_kind_t::FIXED_ARRAY) {
        return has_references(get(type).inner);
    }

    // What a Ptr points at isn't held in it.
    if (kind == type_kind_t::PTR) {
        return false;
    }

    return std::ranges::any_of(get(type).fields,
                               [this](const field_t& field) { return has_references(field.type); });
}

bool type_table_t::has_zero(const type_id_t type) const {
    // A type parameter might stand for any of them.
    const type_kind_t kind = get(type).kind;
    if (kind == type_kind_t::CLASS || kind == type_kind_t::INTERFACE ||
        kind == type_kind_t::ARRAY || kind == type_kind_t::FUNCTION ||
        kind == type_kind_t::PARAMETER || kind == type_kind_t::TASK ||
        kind == type_kind_t::THREAD || kind == type_kind_t::CHANNEL ||
        kind == type_kind_t::SCHEDULER) {
        return false;
    }

    if (kind == type_kind_t::FIXED_ARRAY) {
        return has_zero(get(type).inner);
    }

    // A T?'s zero is Null, whatever the T, and a Weak<T>'s refers to nothing. A Mutex<T>'s is
    // its T's.
    if (kind != type_kind_t::STRUCT && kind != type_kind_t::TUPLE && kind != type_kind_t::MUTEX) {
        return true;
    }

    return std::ranges::all_of(get(type).fields,
                               [this](const field_t& field) { return has_zero(field.type); });
}

std::string type_table_t::suffixed(const type_id_t type, const bool qualified,
                                   const std::string_view main) const {
    std::string text = qualified ? qualified_name(type, main) : std::string(name(type));
    if (type != type_id_t::INVALID &&
        (get(type).kind == type_kind_t::FUNCTION || get(type).kind == type_kind_t::CALLBACK) &&
        get(type).inner != type_id_t::NONE) {
        return std::format("({})", text);
    }

    return text;
}

std::string type_table_t::qualified_name(const type_id_t id, const std::string_view main) const {
    const type_t& type = get(id);
    // The types inside, each qualified, between commas.
    const auto list = [&](const std::vector<type_id_t>& types) {
        std::string text;
        for (std::size_t i = 0; i < types.size(); ++i) {
            text += std::format("{}{}", i == 0 ? "" : ", ", qualified_name(types[i], main));
        }

        return text;
    };

    // Each kind is spelled as the function that makes it names it, with the parts qualified.
    switch (type.kind) {
    case type_kind_t::STRUCT:
    case type_kind_t::CLASS:
    case type_kind_t::INTERFACE:
    case type_kind_t::ENUM: {
        const type_t& declared = type.generic != type_id_t::INVALID ? get(type.generic) : type;
        std::string_view module = declared.module.empty() ? main : declared.module;
        if (declared.made) {
            module = {};
        }

        std::string text = module.empty() ? std::string() : std::format("{}.", module);
        if (type.generic == type_id_t::INVALID) {
            return text + type.name;
        }

        const std::string_view generic = declared.name;
        return std::format("{}{}<{}>", text, generic.substr(0, generic.find('<')),
                           list(type.arguments));
    }
    case type_kind_t::NULLABLE:
        return std::format("{}?", suffixed(type.inner, true, main));
    case type_kind_t::ARRAY:
        return std::format("{}[]", suffixed(type.inner, true, main));
    case type_kind_t::FIXED_ARRAY: {
        std::string text = std::format("{}[", suffixed(type.inner, true, main));
        for (std::size_t i = 0; i < type.sizes.size(); ++i) {
            text += i == 0 ? "" : ", ";
            text += i < type.parameters.size() && type.parameters[i] != type_id_t::INVALID
                        ? std::string(name(type.parameters[i]))
                        : std::to_string(type.sizes[i]);
        }

        return text + "]";
    }
    case type_kind_t::WEAK:
        return std::format("Weak<{}>", qualified_name(type.inner, main));
    case type_kind_t::PTR:
        return type.inner == type_id_t::NONE
                   ? std::string("Ptr<Void>")
                   : std::format("Ptr<{}>", qualified_name(type.inner, main));
    case type_kind_t::ATOMIC:
        return std::format("Atomic<{}>", qualified_name(type.inner, main));
    case type_kind_t::MUTEX:
        return std::format("Mutex<{}>", qualified_name(type.inner, main));
    case type_kind_t::CHANNEL:
        return std::format("Channel<{}>", qualified_name(type.inner, main));
    case type_kind_t::TASK: {
        std::string text = type.inner == type_id_t::NONE
                               ? std::string("Task")
                               : std::format("Task<{}>", qualified_name(type.inner, main));
        if (!type.throws.empty()) {
            text += " Throws " + list(type.throws);
        }

        return text;
    }
    case type_kind_t::TUPLE: {
        std::vector<type_id_t> elements;
        elements.reserve(type.fields.size());
        for (const field_t& field : type.fields) {
            elements.push_back(field.type);
        }

        return std::format("({})", list(elements));
    }
    case type_kind_t::FUNCTION:
        return type.inner == type_id_t::NONE ? std::format("Func({})", list(type.parameters))
                                             : std::format("Func({}): {}", list(type.parameters),
                                                           qualified_name(type.inner, main));
    case type_kind_t::CALLBACK:
        return type.inner == type_id_t::NONE
                   ? std::format("Callback({})", list(type.parameters))
                   : std::format("Callback({}): {}", list(type.parameters),
                                 qualified_name(type.inner, main));
    default:
        return type.name;
    }
}

type_id_t type_table_t::add(type_t type) {
    const auto id = static_cast<type_id_t>(m_entries.size());
    m_entries.push_back({ .type = std::make_unique<type_t>(std::move(type)) });
    return id;
}

} // namespace fb
